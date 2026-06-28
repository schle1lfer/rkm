// SPDX-License-Identifier: GPL-2.0
/*
 * rtc-bcm.c — Linux RTC driver for BCM I2C slave (I2C Protocol v1.0)
 *
 * Implements a full Linux RTC driver backed by an I2C slave that speaks
 * I2C Protocol v1.0 (03/18/2026).  The driver plugs into the kernel RTC
 * subsystem so that standard tools (hwclock, timedatectl, adjtimex) and
 * the kernel's own hctosys mechanism all work out of the box.
 *
 * The RTC device appears as /dev/rtcN in the RTC subsystem.  A udev rule
 * (99-rtc-bcm.rules) creates /dev/rtc_bcm0 as a convenience symlink.
 *
 * -------------------------------------------------------------------------
 * Protocol summary (I2C Protocol v1.0, 03/18/2026)
 * -------------------------------------------------------------------------
 * Every exchange is a combined write+read transfer (repeated START):
 *
 *   START <addr>+W [request] REPEATED-START <addr>+R [response] STOP
 *
 * CMD_GET_TIME (0x54 'T'):
 *   Request:  [0x54]                    (1 byte)
 *   Response: [STATUS][T3][T2][T1][T0]  (5 bytes)
 *
 * CMD_SET_TIME (0x57 'W'):
 *   Request:  [0x57][T3][T2][T1][T0]   (5 bytes)
 *   Response: [STATUS]                 (1 byte)
 *
 * T3..T0: uint32_t big-endian, seconds since 2000-01-01 00:00:00 UTC.
 * STATUS: 0x00=OK  0x01=ERR  0x02=BUSY  0x03=INVAL
 *
 * -------------------------------------------------------------------------
 * Time base conversion
 * -------------------------------------------------------------------------
 * Slave epoch : 2000-01-01 00:00:00 UTC
 * Linux epoch : 1970-01-01 00:00:00 UTC
 * Offset      : BCM_EPOCH_OFFSET = 946684800 seconds
 *
 * unix_secs   = proto_secs + BCM_EPOCH_OFFSET
 * proto_secs  = unix_secs  - BCM_EPOCH_OFFSET
 *
 * Valid range : 2000-01-01 00:00:00 UTC  (proto_secs = 0)
 *           to 2136-02-07 06:28:15 UTC  (proto_secs = 0xFFFFFFFF)
 *
 * -------------------------------------------------------------------------
 * Module parameters
 * -------------------------------------------------------------------------
 *   bus_num    — I2C adapter number, default 2  (/dev/i2c-2)
 *   slave_addr — 7-bit slave address, default 0x46
 *
 * -------------------------------------------------------------------------
 * Usage
 * -------------------------------------------------------------------------
 *   insmod rtc-bcm.ko [bus_num=2] [slave_addr=0x46]
 *
 *   hwclock -r -f /dev/rtc_bcm0          # read RTC
 *   hwclock -w -f /dev/rtc_bcm0          # sync RTC <- system
 *   hwclock -s -f /dev/rtc_bcm0          # sync system <- RTC
 *   timedatectl set-local-rtc 0           # RTC stores UTC (recommended)
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rtc.h>
#include <linux/slab.h>

#define DRV_NAME "rtc-bcm"

/* -------------------------------------------------------------------------
 * Module parameters
 * ---------------------------------------------------------------------- */

static int bus_num    = 2;
static int slave_addr = 0x46;

module_param(bus_num,    int, 0444);
MODULE_PARM_DESC(bus_num,    "I2C adapter number (default: 2)");

module_param(slave_addr, int, 0444);
MODULE_PARM_DESC(slave_addr, "7-bit slave address (default: 0x46)");

/* -------------------------------------------------------------------------
 * I2C Protocol v1.0 constants
 * ---------------------------------------------------------------------- */

/*
 * Seconds between Unix epoch (1970-01-01) and the slave's time epoch
 * (2000-01-01 00:00:00 UTC).  Used for epoch conversion in both directions.
 */
#define BCM_EPOCH_OFFSET     946684800ULL

/* Maximum seconds value in the protocol (uint32_t saturation point) */
#define BCM_PROTO_SECS_MAX   0xFFFFFFFFUL

/* Protocol command bytes */
#define CMD_GET_TIME         ((u8)0x54)   /* ASCII 'T' */
#define CMD_SET_TIME         ((u8)0x57)   /* ASCII 'W' */

/* Response STATUS codes */
#define PROTO_STATUS_OK      ((u8)0x00)
#define PROTO_STATUS_ERR     ((u8)0x01)
#define PROTO_STATUS_BUSY    ((u8)0x02)
#define PROTO_STATUS_INVAL   ((u8)0x03)

/* Frame sizes */
#define GET_TIME_REQ_LEN     1U
#define GET_TIME_RESP_LEN    5U
#define SET_TIME_REQ_LEN     5U
#define SET_TIME_RESP_LEN    1U

/* -------------------------------------------------------------------------
 * Driver private data
 * ---------------------------------------------------------------------- */

struct bcm_rtc {
	struct i2c_client *client;
	struct rtc_device *rtc;
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static const char *bcm_status_str(u8 status)
{
	switch (status) {
	case PROTO_STATUS_OK:    return "OK";
	case PROTO_STATUS_ERR:   return "ERR (generic slave error)";
	case PROTO_STATUS_BUSY:  return "BUSY (retry later)";
	case PROTO_STATUS_INVAL: return "INVAL (unknown command)";
	default:                 return "UNKNOWN";
	}
}

/*
 * bcm_i2c_xfer - combined write+read I2C transfer (repeated START).
 *
 * Drives the wire transaction:
 *   START addr+W [tx_buf .. tx_len-1] REPEATED-START addr+R [rx_buf .. rx_len-1] STOP
 *
 * This is the only I2C function called by the protocol layer; all three
 * protocol commands share this helper.
 *
 * Returns 0 on success, negative errno on transport failure.
 */
static int bcm_i2c_xfer(struct i2c_client *client,
			const u8 *tx_buf, u16 tx_len,
			u8 *rx_buf, u16 rx_len)
{
	struct i2c_msg msgs[2] = {
		{
			.addr  = client->addr,
			.flags = 0,           /* write */
			.len   = tx_len,
			.buf   = (u8 *)(uintptr_t)tx_buf,
		},
		{
			.addr  = client->addr,
			.flags = I2C_M_RD,    /* read, repeated START */
			.len   = rx_len,
			.buf   = rx_buf,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;
	return 0;
}

/*
 * bcm_decode_be32 - decode four big-endian bytes into a uint32_t.
 */
static inline u32 bcm_decode_be32(const u8 *buf)
{
	return ((u32)buf[0] << 24) | ((u32)buf[1] << 16) |
	       ((u32)buf[2] <<  8) |  (u32)buf[3];
}

/*
 * bcm_encode_be32 - encode a uint32_t into four big-endian bytes.
 */
static inline void bcm_encode_be32(u8 *buf, u32 val)
{
	buf[0] = (val >> 24) & 0xFFu;
	buf[1] = (val >> 16) & 0xFFu;
	buf[2] = (val >>  8) & 0xFFu;
	buf[3] =  val        & 0xFFu;
}

/* -------------------------------------------------------------------------
 * RTC class operations
 * ---------------------------------------------------------------------- */

/*
 * bcm_rtc_read_time - read slave clock via CMD_GET_TIME (0x54).
 *
 * Wire frames:
 *   TX: [0x54]                       (1 byte)
 *   RX: [STATUS][T3][T2][T1][T0]     (5 bytes)
 *
 * On STATUS == OK the four time bytes are decoded as a big-endian uint32_t
 * (seconds since 2000-01-01 UTC), then shifted to the Unix epoch and
 * decomposed into an rtc_time struct via rtc_time64_to_tm().
 *
 * Called by: RTC subsystem on every read from /dev/rtcN, hwclock -r,
 *            timedatectl, and the kernel's hctosys path.
 */
static int bcm_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct bcm_rtc *priv = dev_get_drvdata(dev);
	u8 req = CMD_GET_TIME;
	u8 resp[GET_TIME_RESP_LEN];
	u32 proto_secs;
	time64_t unix_secs;
	int ret;

	dev_dbg(dev, "read_time: TX [0x%02x] (CMD_GET_TIME)\n", req);

	ret = bcm_i2c_xfer(priv->client,
			   &req, GET_TIME_REQ_LEN,
			   resp, GET_TIME_RESP_LEN);
	if (ret < 0) {
		dev_err(dev, "read_time: i2c transfer failed: %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "read_time: RX [%02x %02x %02x %02x %02x] STATUS=%s\n",
		resp[0], resp[1], resp[2], resp[3], resp[4],
		bcm_status_str(resp[0]));

	if (resp[0] != PROTO_STATUS_OK) {
		dev_err(dev, "read_time: slave STATUS=0x%02x (%s)\n",
			resp[0], bcm_status_str(resp[0]));
		return -EIO;
	}

	proto_secs = bcm_decode_be32(&resp[1]);
	unix_secs  = (time64_t)proto_secs + BCM_EPOCH_OFFSET;
	rtc_time64_to_tm(unix_secs, tm);

	dev_dbg(dev, "read_time: proto_secs=%u unix_secs=%lld "
		"%04d-%02d-%02d %02d:%02d:%02d UTC\n",
		proto_secs, (long long)unix_secs,
		tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		tm->tm_hour, tm->tm_min, tm->tm_sec);

	return 0;
}

/*
 * bcm_rtc_set_time - write slave clock via CMD_SET_TIME (0x57).
 *
 * Wire frames:
 *   TX: [0x57][T3][T2][T1][T0]       (5 bytes)
 *   RX: [STATUS]                     (1 byte)
 *
 * The rtc_time struct is converted to a Unix timestamp via rtc_tm_to_time64(),
 * then shifted from the Unix epoch to the slave epoch.  Returns -ERANGE for
 * dates before 2000-01-01 or after 2136-02-07 (outside uint32_t range).
 *
 * Called by: hwclock -w, timedatectl, and the kernel's systohc path.
 */
static int bcm_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct bcm_rtc *priv = dev_get_drvdata(dev);
	time64_t unix_secs;
	u32 proto_secs;
	u8 req[SET_TIME_REQ_LEN];
	u8 status;
	int ret;

	unix_secs = rtc_tm_to_time64(tm);

	if (unix_secs < (time64_t)BCM_EPOCH_OFFSET) {
		dev_err(dev, "set_time: date before 2000-01-01 not supported "
			"(unix_secs=%lld)\n", (long long)unix_secs);
		return -ERANGE;
	}

	if ((u64)(unix_secs - (time64_t)BCM_EPOCH_OFFSET) > BCM_PROTO_SECS_MAX) {
		dev_err(dev, "set_time: date after 2136-02-07 not supported "
			"(unix_secs=%lld)\n", (long long)unix_secs);
		return -ERANGE;
	}

	proto_secs = (u32)(unix_secs - (time64_t)BCM_EPOCH_OFFSET);

	req[0] = CMD_SET_TIME;
	bcm_encode_be32(&req[1], proto_secs);

	dev_dbg(dev, "set_time: %04d-%02d-%02d %02d:%02d:%02d UTC "
		"(proto_secs=%u)\n",
		tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		tm->tm_hour, tm->tm_min, tm->tm_sec, proto_secs);
	dev_dbg(dev, "set_time: TX [%02x %02x %02x %02x %02x]\n",
		req[0], req[1], req[2], req[3], req[4]);

	ret = bcm_i2c_xfer(priv->client,
			   req, SET_TIME_REQ_LEN,
			   &status, SET_TIME_RESP_LEN);
	if (ret < 0) {
		dev_err(dev, "set_time: i2c transfer failed: %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "set_time: RX [0x%02x] STATUS=%s\n",
		status, bcm_status_str(status));

	if (status != PROTO_STATUS_OK) {
		dev_err(dev, "set_time: slave STATUS=0x%02x (%s)\n",
			status, bcm_status_str(status));
		return -EIO;
	}

	return 0;
}

static const struct rtc_class_ops bcm_rtc_ops = {
	.read_time = bcm_rtc_read_time,
	.set_time  = bcm_rtc_set_time,
};

/* -------------------------------------------------------------------------
 * I2C driver probe
 * ---------------------------------------------------------------------- */

static int bcm_rtc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct bcm_rtc *priv;
	struct rtc_device *rtc;
	int ret;

	dev_info(dev, "probing on %s addr=0x%02x\n",
		 dev_name(&client->adapter->dev), client->addr);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "adapter does not support raw I2C (I2C_FUNC_I2C)\n");
		return -ENODEV;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	i2c_set_clientdata(client, priv);

	rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc)) {
		dev_err(dev, "failed to allocate RTC device: %ld\n", PTR_ERR(rtc));
		return PTR_ERR(rtc);
	}

	rtc->ops = &bcm_rtc_ops;

	/*
	 * Slave epoch starts at 2000-01-01 00:00:00 UTC.
	 * uint32_t max (0xFFFFFFFF seconds after 2000-01-01) lands on
	 * 2136-02-07 06:28:15 UTC.
	 */
	rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rtc->range_max = (time64_t)BCM_EPOCH_OFFSET + BCM_PROTO_SECS_MAX;

	/* No alarm hardware on this slave */
	set_bit(RTC_FEATURE_NO_ALARM, rtc->features);

	priv->rtc = rtc;

	ret = devm_rtc_register_device(rtc);
	if (ret) {
		dev_err(dev, "failed to register RTC: %d\n", ret);
		return ret;
	}

	dev_info(dev, "registered as %s (range 2000-01-01 to 2136-02-07)\n",
		 dev_name(&rtc->dev));
	return 0;
}

/* -------------------------------------------------------------------------
 * I2C driver descriptor
 * ---------------------------------------------------------------------- */

static const struct i2c_device_id bcm_rtc_id[] = {
	{ DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bcm_rtc_id);

static const struct of_device_id bcm_rtc_of_match[] = {
	{ .compatible = "bcm,i2c-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm_rtc_of_match);

static struct i2c_driver bcm_rtc_driver = {
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = bcm_rtc_of_match,
	},
	.probe    = bcm_rtc_probe,
	.id_table = bcm_rtc_id,
};

/* -------------------------------------------------------------------------
 * Module init / exit
 *
 * Because this hardware is not described in the device tree on the target
 * board, we manually instantiate the I2C client from module_init so that
 * the I2C core calls our probe function.
 *
 * Flow:
 *   insmod rtc-bcm.ko
 *     -> i2c_add_driver()          register the driver
 *     -> i2c_get_adapter(bus_num)  acquire i2c-N adapter
 *     -> i2c_new_client_device()   create the client  -> probe() fires
 *          -> devm_rtc_allocate_device()   allocate /dev/rtcN
 *          -> devm_rtc_register_device()   expose to userspace
 *     -> i2c_put_adapter()         release adapter reference
 *
 *   rmmod rtc-bcm
 *     -> i2c_unregister_device()   destroy client (devm cleanup removes RTC)
 *     -> i2c_del_driver()          unregister driver
 * ---------------------------------------------------------------------- */

static struct i2c_client *bcm_rtc_client;

static int __init bcm_rtc_module_init(void)
{
	struct i2c_adapter  *adap;
	struct i2c_board_info info = { };
	int ret;

	pr_info(DRV_NAME ": initializing (bus_num=%d slave_addr=0x%02x)\n",
		bus_num, slave_addr);

	if (slave_addr < 0x08 || slave_addr > 0x77) {
		pr_err(DRV_NAME ": invalid slave_addr=0x%02x "
		       "(valid 7-bit range: 0x08-0x77)\n", slave_addr);
		return -EINVAL;
	}

	pr_info(DRV_NAME ": registering I2C driver\n");
	ret = i2c_add_driver(&bcm_rtc_driver);
	if (ret) {
		pr_err(DRV_NAME ": i2c_add_driver failed: %d\n", ret);
		return ret;
	}

	pr_info(DRV_NAME ": requesting i2c-%d adapter\n", bus_num);
	adap = i2c_get_adapter(bus_num);
	if (!adap) {
		pr_err(DRV_NAME ": i2c-%d adapter not found — "
		       "is the bus driver loaded?\n", bus_num);
		i2c_del_driver(&bcm_rtc_driver);
		return -ENODEV;
	}
	pr_info(DRV_NAME ": got adapter \"%s\"\n", adap->name);

	strscpy(info.type, DRV_NAME, sizeof(info.type));
	info.addr = (unsigned short)slave_addr;

	pr_info(DRV_NAME ": instantiating I2C client (addr=0x%02x)\n",
		info.addr);
	bcm_rtc_client = i2c_new_client_device(adap, &info);
	i2c_put_adapter(adap);

	if (IS_ERR(bcm_rtc_client)) {
		ret = PTR_ERR(bcm_rtc_client);
		pr_err(DRV_NAME ": i2c_new_client_device failed: %d\n", ret);
		i2c_del_driver(&bcm_rtc_driver);
		return ret;
	}

	pr_info(DRV_NAME ": ready\n");
	return 0;
}

static void __exit bcm_rtc_module_exit(void)
{
	pr_info(DRV_NAME ": unloading\n");
	i2c_unregister_device(bcm_rtc_client);
	i2c_del_driver(&bcm_rtc_driver);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(bcm_rtc_module_init);
module_exit(bcm_rtc_module_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Denis SHashunkin <shashunkin@opk-bulat.ru>");
MODULE_DESCRIPTION("Linux RTC driver for BCM I2C slave (I2C Protocol v1.0)");
MODULE_VERSION("1.0");
