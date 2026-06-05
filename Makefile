# Makefile — i2c_baikal_reset kernel module
#
# Two complementary solutions for sending CMD_RESET (0x52) to I2C slave
# 0x46 on bus 2 before the system shuts down or reboots:
#
#  1. Kernel-space reboot notifier (i2c_baikal_reset.ko)
#     Registers a notifier_block via register_reboot_notifier(). The
#     callback fires on SYS_RESTART / SYS_HALT / SYS_POWER_OFF inside
#     the kernel, acquiring the i2c-2 adapter and issuing the reset
#     before the system fully halts. No user-space dependency.
#
#  2. User-space systemd service (i2c_baikal_reset_shutdown.service)
#     A oneshot service with RemainAfterExit=yes whose ExecStop runs
#     /usr/local/bin/i2c-reset just before shutdown.target. Useful
#     when the kernel module is not loaded or as a belt-and-suspenders
#     safety net alongside the kernel notifier.

# Path to the kernel build directory for the running kernel.
KDIR	?= /lib/modules/$(shell uname -r)/build

# Out-of-tree module object.
obj-m	:= i2c_baikal_reset.o

# Default target: build the kernel module.
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Remove build artefacts.
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# install: build the module, install it, then set up the systemd service.
#
# Steps:
#   1. Build and install the .ko into the running kernel's module tree.
#   2. Copy the systemd unit file to /etc/systemd/system/.
#   3. Reload systemd so it picks up the new unit file.
#   4. Enable the service so it starts automatically on boot.
install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	install -m 644 i2c_baikal_reset_shutdown.service /etc/systemd/system/
	systemctl daemon-reload
	systemctl enable i2c_baikal_reset_shutdown.service

# uninstall: disable and remove the systemd service.
#
# Note: this does not remove the .ko from the module tree.
# Run 'modprobe -r i2c_baikal_reset' or reboot to unload the module.
uninstall:
	systemctl disable i2c_baikal_reset_shutdown.service
	rm -f /etc/systemd/system/i2c_baikal_reset_shutdown.service
	systemctl daemon-reload

.PHONY: all clean install uninstall
