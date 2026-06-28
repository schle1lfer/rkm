##############################################################################
# Makefile — rtc-bcm Linux RTC driver
#
# Targets:
#   all       — build rtc-bcm.ko against the running kernel
#   clean     — remove build artefacts
#   install   — build + install module + udev rule + load/options configs
#   uninstall — remove udev rule and load/options configs
#
# Usage:
#   make
#   sudo make install
#   sudo insmod rtc-bcm.ko          # manual test load
#   dmesg | tail -30                # verify probe and RTC registration
#   ls -la /dev/rtc*                # confirm /dev/rtc_bcm0 symlink
#   hwclock -r -f /dev/rtc_bcm0    # read RTC
#   hwclock -w -f /dev/rtc_bcm0    # set RTC from system clock
#   hwclock -s -f /dev/rtc_bcm0    # set system clock from RTC
##############################################################################

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m := rtc-bcm.o

.PHONY: all clean install uninstall

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	# 1. Install the kernel module and rebuild the dependency map
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a
	# 2. udev rule: creates /dev/rtc_bcm0 symlink when the RTC is registered
	install -m 644 99-rtc-bcm.rules /etc/udev/rules.d/
	udevadm control --reload-rules
	# 3. Auto-load at boot
	install -m 644 rtc-bcm-load.conf /etc/modules-load.d/
	# 4. Module parameters
	install -m 644 rtc-bcm-options.conf /etc/modprobe.d/
	@echo
	@echo "Installation complete."
	@echo "On next boot the module will load automatically."
	@echo "To load now: modprobe rtc-bcm"

uninstall:
	rm -f /etc/udev/rules.d/99-rtc-bcm.rules
	rm -f /etc/modules-load.d/rtc-bcm-load.conf
	rm -f /etc/modprobe.d/rtc-bcm-options.conf
	udevadm control --reload-rules
	@echo "Uninstall complete. Run 'modprobe -r rtc-bcm' to unload."
