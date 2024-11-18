# SPDX-License-Identifier: GPL-2.0
# Makefile for the Linux sound card driver
#
# Inlining optimization
KBUILD_CFLAGS  += -mllvm -inline-threshold=100
KBUILD_CFLAGS  += -mllvm -inlinehint-threshold=100
KBUILD_CFLAGS  += -mllvm -inline-instr-cost=100
KBUILD_CFLAGS  += -Oz
obj-$(CONFIG_SOUND) += soundcore.o
obj-$(CONFIG_DMASOUND) += oss/dmasound/
obj-$(CONFIG_SND) += core/ i2c/ drivers/ arm/ usb/ spi/ soc/
obj-$(CONFIG_SND_AOA) += aoa/

# This one must be compilable even if sound is configured out
obj-$(CONFIG_AC97_BUS) += ac97_bus.o
obj-$(CONFIG_AC97_BUS_NEW) += ac97/

ifeq ($(CONFIG_SND),y)
  obj-y += last.o
endif

soundcore-objs  := sound_core.o
