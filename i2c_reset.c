// SPDX-License-Identifier: GPL-2.0
/*
 * i2c_reset.c — I2C Protocol v1.0 kernel module
 *
 * Sends CMD_RESET (0x52) to slave 0x46 on I2C bus 2 on shutdown/reboot
 * via a reboot notifier. No reset is sent on module init.
 *
 * Protocol:
 *   Write: [0x52]
 *   Read:  [STATUS]  — 0x00 = OK
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/reboot.h>
#include <linux/notifier.h>

#define I2C_BUS		2
#define I2C_ADDR	0x46
#define CMD_RESET	0x52

#define PROTO_STATUS_OK		0x00

/* Protocol status constants */
static const struct {
	u8		code;
	const char	*str;
} proto_status_table[] = {
	{ PROTO_STATUS_OK, "OK" },
};

static const char *proto_status_str(u8 status)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(proto_status_table); i++) {
		if (proto_status_table[i].code == status)
			return proto_status_table[i].str;
	}
	return "UNKNOWN";
}

/**
 * i2c_proto_reset - Send CMD_RESET and read back STATUS byte.
 * @client: I2C client to communicate with.
 *
 * Performs a combined write+read transfer:
 *   write 1 byte  [CMD_RESET]
 *   read  1 byte  [STATUS]
 *
 * Returns 0 on success, negative errno on failure.
 */
static int i2c_proto_reset(struct i2c_client *client)
{
	u8 cmd = CMD_RESET;
	u8 status = 0xFF;
	struct i2c_msg msgs[2] = {
		{
			.addr	= client->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= &cmd,
		},
		{
			.addr	= client->addr,
			.flags	= I2C_M_RD,
			.len	= 1,
			.buf	= &status,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	pr_info("i2c_reset: CMD_RESET status=0x%02x (%s)\n",
		status, proto_status_str(status));

	if (status != PROTO_STATUS_OK)
		return -EPROTO;

	return 0;
}

/**
 * i2c_reset_notify - Reboot notifier callback.
 *
 * Fires on SYS_RESTART, SYS_HALT, and SYS_POWER_OFF.
 * Acquires the i2c-2 adapter, sends CMD_RESET to slave 0x46,
 * then releases the adapter.
 */
static int i2c_reset_notify(struct notifier_block *nb,
			    unsigned long action, void *data)
{
	struct i2c_adapter *adap;
	struct i2c_client *client;
	int ret;

	switch (action) {
	case SYS_RESTART:
	case SYS_HALT:
	case SYS_POWER_OFF:
		break;
	default:
		return NOTIFY_DONE;
	}

	pr_info("i2c_reset: reboot event %lu — sending CMD_RESET to bus=%d addr=0x%02x\n",
		action, I2C_BUS, I2C_ADDR);

	adap = i2c_get_adapter(I2C_BUS);
	if (!adap) {
		pr_err("i2c_reset: failed to get i2c-%d adapter\n", I2C_BUS);
		return NOTIFY_DONE;
	}

	client = i2c_new_dummy_device(adap, I2C_ADDR);
	if (IS_ERR(client)) {
		pr_err("i2c_reset: failed to create dummy client: %ld\n",
		       PTR_ERR(client));
		i2c_put_adapter(adap);
		return NOTIFY_DONE;
	}

	ret = i2c_proto_reset(client);
	if (ret < 0)
		pr_err("i2c_reset: CMD_RESET failed: %d\n", ret);
	else
		pr_info("i2c_reset: CMD_RESET succeeded\n");

	i2c_unregister_device(client);
	i2c_put_adapter(adap);

	return NOTIFY_DONE;
}

static struct notifier_block i2c_reset_nb = {
	.notifier_call	= i2c_reset_notify,
	.priority	= 0,
};

static int __init i2c_reset_init(void)
{
	int ret;

	ret = register_reboot_notifier(&i2c_reset_nb);
	if (ret) {
		pr_err("i2c_reset: failed to register reboot notifier: %d\n", ret);
		return ret;
	}

	pr_info("i2c_reset: registered reboot notifier — CMD_RESET will run on shutdown/reboot\n");
	return 0;
}

static void __exit i2c_reset_exit(void)
{
	unregister_reboot_notifier(&i2c_reset_nb);
	pr_info("i2c_reset: reboot notifier unregistered\n");
}

module_init(i2c_reset_init);
module_exit(i2c_reset_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("schle1lfer");
MODULE_DESCRIPTION("Send I2C CMD_RESET to 0x46 on bus 2 on shutdown/reboot (reboot notifier)");
MODULE_VERSION("2.0");
