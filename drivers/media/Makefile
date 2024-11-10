# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the kernel multimedia device drivers.
#
# Inlining optimization
KBUILD_CFLAGS  += -mllvm -inline-threshold=225
KBUILD_CFLAGS  += -mllvm -inlinehint-threshold=325
KBUILD_CFLAGS  += -mllvm -inline-instr-cost=60
KBUILD_CFLAGS  += -Oz
#
# I2C drivers should come before other drivers, otherwise they'll fail
# when compiled as builtin drivers
#
obj-y += i2c/ tuners/
obj-$(CONFIG_DVB_CORE)  += dvb-frontends/

#
# Now, let's link-in the media controller core
#
ifeq ($(CONFIG_MEDIA_CONTROLLER),y)
  obj-$(CONFIG_MEDIA_SUPPORT) += mc/
endif

obj-$(CONFIG_VIDEO_DEV) += v4l2-core/
obj-$(CONFIG_DVB_CORE)  += dvb-core/

# There are both core and drivers at RC subtree - merge before drivers
obj-y += rc/

obj-$(CONFIG_CEC_CORE) += cec/

#
# Finally, merge the drivers that require the core
#

obj-y += common/ platform/ pci/ usb/ mmc/ firewire/ spi/ test-drivers/
obj-$(CONFIG_VIDEO_DEV) += radio/

