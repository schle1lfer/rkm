// SPDX-License-Identifier: GPL-2.0
/*
 * i2c_baikal_reset.c — I2C Protocol v1.0 CMD_RESET, kernel reboot notifier
 *
 * Registers a reboot notifier that sends CMD_RESET (0x52) to a slave
 * device before the system restarts or shuts down.
 *
 * Wire transaction (I2C Protocol v1.0, 03/18/2026):
 *   START <addr>+W [0x52] REPEATED-START <addr>+R [STATUS] STOP
 *
 * STATUS codes: 0x00=OK  0x01=ERR  0x02=BUSY  0x03=INVAL
 *
 * Module parameters (set at insmod or via /sys/module/i2c_baikal_reset/parameters/):
 *   bus_num    - I2C adapter number (default: 2, corresponds to /dev/i2c-2)
 *   slave_addr - 7-bit slave address in hex (default: 0x46)
 *
 * Example:
 *   insmod i2c_baikal_reset.ko bus_num=2 slave_addr=0x46
 *
 * Design notes:
 *   - The I2C adapter is acquired once at module_init and released at
 *     module_exit.  Acquiring it inside the notifier callback is avoided
 *     because memory allocation during the shutdown path is unreliable.
 *   - i2c_transfer() takes an adapter directly; no i2c_client or
 *     i2c_new_dummy_device() is needed for a raw two-message transfer.
 *   - The notifier priority is left at 0 (default).  Raise it if this
 *     reset must run before other reboot notifiers.
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/reboot.h>

#define DRV_NAME "i2c_baikal_reset"

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

#define I2C_CMD_RESET        ((u8)0x52)

#define PROTO_STATUS_OK      ((u8)0x00)
#define PROTO_STATUS_ERR     ((u8)0x01)
#define PROTO_STATUS_BUSY    ((u8)0x02)
#define PROTO_STATUS_INVAL   ((u8)0x03)

/* -------------------------------------------------------------------------
 * Module globals
 * ---------------------------------------------------------------------- */

/* Adapter acquired at module_init, used by the notifier, released at exit. */
static struct i2c_adapter *i2c_baikal_reset_adap;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static const char *proto_status_str(u8 status)
{
	switch (status) {
	case PROTO_STATUS_OK:    return "OK";
	case PROTO_STATUS_ERR:   return "ERR (generic slave error)";
	case PROTO_STATUS_BUSY:  return "BUSY (retry later)";
	case PROTO_STATUS_INVAL: return "INVAL (unknown command)";
	default:                 return "UNKNOWN";
	}
}

static const char *reboot_action_str(unsigned long action)
{
	switch (action) {
	case SYS_RESTART:   return "SYS_RESTART";
	case SYS_HALT:      return "SYS_HALT";
	case SYS_POWER_OFF: return "SYS_POWER_OFF";
	default:            return "UNKNOWN";
	}
}

/* -------------------------------------------------------------------------
 * Protocol
 * ---------------------------------------------------------------------- */

/*
 * i2c_proto_reset - perform the CMD_RESET combined write+read transfer.
 * @adap: I2C adapter to use
 * @addr: 7-bit slave address
 *
 * Assembles two struct i2c_msg entries and calls i2c_transfer():
 *   msg[0]: write 1 byte  [0x52]
 *   msg[1]: read  1 byte  [STATUS]
 *
 * The kernel issues a repeated START between the two messages — no STOP
 * in between — which matches the protocol specification exactly.
 *
 * Returns 0 on PROTO_STATUS_OK, negative errno on any error.
 */
static int i2c_proto_reset(struct i2c_adapter *adap, u16 addr)
{
	u8 tx = I2C_CMD_RESET;
	u8 rx = 0xFF;
	struct i2c_msg msgs[2] = {
		{
			.addr  = addr,
			.flags = 0,        /* write */
			.len   = 1,
			.buf   = &tx,
		},
		{
			.addr  = addr,
			.flags = I2C_M_RD, /* read, repeated START */
			.len   = 1,
			.buf   = &rx,
		},
	};
	int ret;

	pr_info(DRV_NAME ": transfer start: bus=i2c-%d addr=0x%02x\n",
		bus_num, addr);
	pr_info(DRV_NAME ": TX [0x%02x] (CMD_RESET 'R')\n", tx);

	ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	if (ret < 0) {
		pr_err(DRV_NAME ": i2c_transfer failed (err=%d)\n", ret);
		return ret;
	}
	if (ret != ARRAY_SIZE(msgs)) {
		pr_err(DRV_NAME ": incomplete transfer: %d/%zu messages sent\n",
		       ret, ARRAY_SIZE(msgs));
		return -EIO;
	}

	pr_info(DRV_NAME ": RX [0x%02x] STATUS=%s\n", rx, proto_status_str(rx));

	if (rx != PROTO_STATUS_OK) {
		pr_err(DRV_NAME ": CMD_RESET rejected by slave: STATUS=0x%02x (%s)\n",
		       rx, proto_status_str(rx));
		return -EPROTO;
	}

	pr_info(DRV_NAME ": CMD_RESET accepted by slave\n");
	return 0;
}

/* -------------------------------------------------------------------------
 * Reboot notifier
 * ---------------------------------------------------------------------- */

static int i2c_baikal_reset_notify(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	int ret;

	switch (action) {
	case SYS_RESTART:
	case SYS_HALT:
	case SYS_POWER_OFF:
		break;
	default:
		pr_debug(DRV_NAME ": ignoring reboot event %lu\n", action);
		return NOTIFY_DONE;
	}

	pr_info(DRV_NAME ": reboot notifier fired: action=%s (%lu)\n",
		reboot_action_str(action), action);
	pr_info(DRV_NAME ": sending CMD_RESET to bus=i2c-%d addr=0x%02x\n",
		bus_num, slave_addr);

	ret = i2c_proto_reset(i2c_baikal_reset_adap, (u16)slave_addr);
	if (ret < 0)
		pr_err(DRV_NAME ": CMD_RESET failed (err=%d) — continuing shutdown\n",
		       ret);
	else
		pr_info(DRV_NAME ": CMD_RESET completed successfully\n");

	return NOTIFY_DONE;
}

static struct notifier_block i2c_baikal_reset_nb = {
	.notifier_call = i2c_baikal_reset_notify,
	/* .priority = 0 — default; raise to run before other notifiers */
};

/* -------------------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------------- */

static int __init i2c_baikal_reset_init(void)
{
	int ret;

	pr_info(DRV_NAME ": initializing (bus_num=%d slave_addr=0x%02x)\n",
		bus_num, slave_addr);

	if (slave_addr < 0x08 || slave_addr > 0x77) {
		pr_err(DRV_NAME ": invalid slave_addr=0x%02x "
		       "(valid 7-bit range: 0x08-0x77)\n", slave_addr);
		return -EINVAL;
	}

	pr_info(DRV_NAME ": requesting i2c-%d adapter\n", bus_num);
	i2c_baikal_reset_adap = i2c_get_adapter(bus_num);
	if (!i2c_baikal_reset_adap) {
		pr_err(DRV_NAME ": i2c-%d adapter not found — "
		       "is the bus driver loaded?\n", bus_num);
		return -ENODEV;
	}
	pr_info(DRV_NAME ": got adapter \"%s\"\n", i2c_baikal_reset_adap->name);

	pr_info(DRV_NAME ": registering reboot notifier\n");
	ret = register_reboot_notifier(&i2c_baikal_reset_nb);
	if (ret) {
		pr_err(DRV_NAME ": register_reboot_notifier failed (err=%d)\n",
		       ret);
		i2c_put_adapter(i2c_baikal_reset_adap);
		i2c_baikal_reset_adap = NULL;
		return ret;
	}

	pr_info(DRV_NAME ": ready — CMD_RESET will fire on shutdown/reboot "
		"(bus=i2c-%d addr=0x%02x)\n", bus_num, slave_addr);
	return 0;
}

static void __exit i2c_baikal_reset_exit(void)
{
	pr_info(DRV_NAME ": unloading — unregistering reboot notifier\n");
	unregister_reboot_notifier(&i2c_baikal_reset_nb);

	pr_info(DRV_NAME ": releasing i2c-%d adapter\n", bus_num);
	i2c_put_adapter(i2c_baikal_reset_adap);

	pr_info(DRV_NAME ": unloaded\n");
}

module_init(i2c_baikal_reset_init);
module_exit(i2c_baikal_reset_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Denis SHashunkin <shashunkin@opk-bulat.ru>");
MODULE_DESCRIPTION("I2C Protocol v1.0 CMD_RESET on shutdown/reboot");
MODULE_VERSION("2.0");
