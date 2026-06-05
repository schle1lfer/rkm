##############################################################################
# Makefile — I2C Protocol v1.0 CMD_RESET kernel module
#
# Targets:
#   all    - Build the kernel module (i2c_reset.ko)
#   clean  - Remove build artefacts
#
# Usage:
#   make               # build against the running kernel
#   make KDIR=<path>   # build against a specific kernel source tree
#
# Loading / unloading:
#   sudo insmod i2c_reset.ko
#   sudo rmmod  i2c_reset
#   dmesg | tail        # inspect protocol log output
##############################################################################

obj-m := i2c_reset.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
