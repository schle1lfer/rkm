/**
 * Copyright (C) 2026 Denis SHashunkin, LLC Bulat
 * <shashunkin@opk-bulat.ru>
 *
 * @file i2c_reset.c
 * @brief Kernel module — I2C Protocol v1.0 CMD_RESET
 *
 * Replicates the user-space CMD_RESET logic from the i2c project in kernel
 * space.  On module load it performs a single combined I2C transfer to bus 2
 * at slave address 0x46:
 *
 *   START  0x46+W  [0x52]  REPEATED-START  0x46+R  [STATUS]  STOP
 *
 * Protocol (I2C Protocol v1.0 from 03/18/2026):
 *   CMD_RESET (0x52 'R'):
 *     Request:  [CMD]      (1 byte write)
 *     Response: [STATUS]   (1 byte read)
 *
 *   STATUS codes:
 *     0x00  OK    — reset accepted
 *     0x01  ERR   — generic slave-side error
 *     0x02  BUSY  — slave temporarily busy; retry later
 *     0x03  INVAL — unknown command
 *
 * Equivalent user-space call (for reference):
 *   i2c_proto_reset(fd, 0x46, NULL);  // i2c_proto.c
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

/** I2C bus number — corresponds to /dev/i2c-2 in user space. */
#define I2C_RESET_BUS_NUM  2

/** Slave device address (7-bit). */
#define I2C_RESET_SLAVE_ADDR  ((u16)0x46)

/* -------------------------------------------------------------------------
 * I2C Protocol v1.0 constants
 * ---------------------------------------------------------------------- */

/** CMD_RESET command byte (ASCII 'R'). */
#define I2C_CMD_RESET    ((u8)0x52)

/** Protocol STATUS codes returned in the response frame. */
#define PROTO_STATUS_OK    ((u8)0x00)
#define PROTO_STATUS_ERR   ((u8)0x01)
#define PROTO_STATUS_BUSY  ((u8)0x02)
#define PROTO_STATUS_INVAL ((u8)0x03)

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/**
 * proto_status_str - return a human-readable label for a STATUS byte.
 * @status: one of the PROTO_STATUS_* constants
 */
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
 * Protocol implementation
 * ---------------------------------------------------------------------- */

/**
 * i2c_proto_reset - send CMD_RESET to the slave over I2C.
 * @adap: kernel I2C adapter (bus)
 * @addr: 7-bit slave address
 *
 * Issues a combined write+read transfer identical to the user-space
 * i2c_proto_reset() function:
 *
 *   START addr+W [0x52] REPEATED-START addr+R [STATUS] STOP
 *
 * Return:
 *   0       success (PROTO_STATUS_OK received)
 *  -EIO     protocol error (non-OK STATUS from slave)
 *  negative kernel error code on transport failure
 */
static int i2c_proto_reset(struct i2c_adapter *adap, u16 addr)
{
	u8 tx = I2C_CMD_RESET;
	u8 rx = 0xFF;
	int ret;

	struct i2c_msg msgs[2] = {
		{
			.addr  = addr,
			.flags = 0,        /* write */
			.len   = 1,
			.buf   = &tx,
		},
		{
			.addr  = addr,
			.flags = I2C_M_RD, /* read */
			.len   = 1,
			.buf   = &rx,
		},
	};

	pr_info("i2c_reset: TX [0x%02x]  (CMD_RESET)\n", tx);

	ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	if (ret < 0) {
		pr_err("i2c_reset: i2c_transfer error: %d\n", ret);
		return ret;
	}
	if (ret != ARRAY_SIZE(msgs)) {
		pr_err("i2c_reset: incomplete transfer: %d/%zu messages\n",
		       ret, ARRAY_SIZE(msgs));
		return -EIO;
	}

	pr_info("i2c_reset: RX [0x%02x]  (STATUS=%s)\n",
		rx, proto_status_str(rx));

	if (rx != PROTO_STATUS_OK) {
		pr_err("i2c_reset: slave rejected CMD_RESET, STATUS=0x%02x (%s)\n",
		       rx, proto_status_str(rx));
		return -EIO;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------------- */

static int __init i2c_reset_init(void)
{
	struct i2c_adapter *adap;
	int ret;

	pr_info("i2c_reset: loading — bus=%d  slave=0x%02x\n",
		I2C_RESET_BUS_NUM, I2C_RESET_SLAVE_ADDR);

	adap = i2c_get_adapter(I2C_RESET_BUS_NUM);
	if (!adap) {
		pr_err("i2c_reset: i2c-%d adapter not found\n", I2C_RESET_BUS_NUM);
		return -ENODEV;
	}

	ret = i2c_proto_reset(adap, I2C_RESET_SLAVE_ADDR);
	i2c_put_adapter(adap);

	if (ret < 0) {
		pr_err("i2c_reset: CMD_RESET failed: %d\n", ret);
		return ret;
	}

	pr_info("i2c_reset: CMD_RESET completed successfully\n");
	return 0;
}

static void __exit i2c_reset_exit(void)
{
	pr_info("i2c_reset: unloaded\n");
}

module_init(i2c_reset_init);
module_exit(i2c_reset_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis SHashunkin <shashunkin@opk-bulat.ru>");
MODULE_DESCRIPTION("I2C Protocol v1.0 CMD_RESET — kernel-space implementation");
MODULE_VERSION("1.0");
