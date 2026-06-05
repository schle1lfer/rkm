// SPDX-License-Identifier: GPL-2.0
/*
 * i2c_reset.c — I2C Protocol v1.0 CMD_RESET, kernel reboot notifier
 *
 * Registers a reboot notifier that sends CMD_RESET (0x52) to slave
 * address 0x46 on I2C bus 2 before the system restarts or shuts down.
 *
 * Wire transaction (I2C Protocol v1.0, 03/18/2026):
 *   START 0x46+W [0x52] REPEATED-START 0x46+R [STATUS] STOP
 *
 * STATUS codes: 0x00=OK  0x01=ERR  0x02=BUSY  0x03=INVAL
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
#include <linux/notifier.h>
#include <linux/reboot.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

#define I2C_RESET_BUS_NUM    2
#define I2C_RESET_SLAVE_ADDR ((u16)0x46)

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
static struct i2c_adapter *i2c_reset_adap;

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

/* -------------------------------------------------------------------------
 * Protocol
 * ---------------------------------------------------------------------- */

/*
 * i2c_proto_reset - perform the CMD_RESET combined write+read transfer.
 * @adap: I2C adapter to use (bus 2)
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
static int i2c_proto_reset(struct i2c_adapter *adap)
{
	u8 tx = I2C_CMD_RESET;
	u8 rx = 0xFF;
	struct i2c_msg msgs[2] = {
		{
			.addr  = I2C_RESET_SLAVE_ADDR,
			.flags = 0,        /* write */
			.len   = 1,
			.buf   = &tx,
		},
		{
			.addr  = I2C_RESET_SLAVE_ADDR,
			.flags = I2C_M_RD, /* read, repeated START */
			.len   = 1,
			.buf   = &rx,
		},
	};
	int ret;

	pr_info("i2c_reset: TX [0x%02x] (CMD_RESET)\n", tx);

	ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	if (ret < 0) {
		pr_err("i2c_reset: i2c_transfer error: %d\n", ret);
		return ret;
	}
	if (ret != ARRAY_SIZE(msgs)) {
		pr_err("i2c_reset: incomplete transfer (%d/%zu messages)\n",
		       ret, ARRAY_SIZE(msgs));
		return -EIO;
	}

	pr_info("i2c_reset: RX STATUS=0x%02x (%s)\n", rx, proto_status_str(rx));

	if (rx != PROTO_STATUS_OK) {
		pr_err("i2c_reset: slave rejected CMD_RESET: STATUS=0x%02x (%s)\n",
		       rx, proto_status_str(rx));
		return -EPROTO;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Reboot notifier
 * ---------------------------------------------------------------------- */

static int i2c_reset_notify(struct notifier_block *nb,
			    unsigned long action, void *data)
{
	int ret;

	switch (action) {
	case SYS_RESTART:
	case SYS_HALT:
	case SYS_POWER_OFF:
		break;
	default:
		return NOTIFY_DONE;
	}

	pr_info("i2c_reset: shutdown event %lu — sending CMD_RESET (bus=%d addr=0x%02x)\n",
		action, I2C_RESET_BUS_NUM, I2C_RESET_SLAVE_ADDR);

	ret = i2c_proto_reset(i2c_reset_adap);
	if (ret < 0)
		pr_err("i2c_reset: CMD_RESET failed: %d\n", ret);
	else
		pr_info("i2c_reset: CMD_RESET OK\n");

	return NOTIFY_DONE;
}

static struct notifier_block i2c_reset_nb = {
	.notifier_call = i2c_reset_notify,
	/* .priority = 0 — default; raise to run before other notifiers */
};

/* -------------------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------------- */

static int __init i2c_reset_init(void)
{
	int ret;

	i2c_reset_adap = i2c_get_adapter(I2C_RESET_BUS_NUM);
	if (!i2c_reset_adap) {
		pr_err("i2c_reset: i2c-%d adapter not found\n", I2C_RESET_BUS_NUM);
		return -ENODEV;
	}

	ret = register_reboot_notifier(&i2c_reset_nb);
	if (ret) {
		pr_err("i2c_reset: register_reboot_notifier failed: %d\n", ret);
		i2c_put_adapter(i2c_reset_adap);
		i2c_reset_adap = NULL;
		return ret;
	}

	pr_info("i2c_reset: ready — CMD_RESET will fire on shutdown/reboot "
		"(bus=%d addr=0x%02x)\n",
		I2C_RESET_BUS_NUM, I2C_RESET_SLAVE_ADDR);
	return 0;
}

static void __exit i2c_reset_exit(void)
{
	unregister_reboot_notifier(&i2c_reset_nb);
	i2c_put_adapter(i2c_reset_adap);
	pr_info("i2c_reset: unloaded\n");
}

module_init(i2c_reset_init);
module_exit(i2c_reset_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Denis SHashunkin <shashunkin@opk-bulat.ru>");
MODULE_DESCRIPTION("I2C Protocol v1.0 CMD_RESET on shutdown/reboot");
MODULE_VERSION("2.0");
