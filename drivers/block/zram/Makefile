# SPDX-License-Identifier: GPL-2.0-only

# Inlining optimization
KBUILD_CFLAGS  += -mllvm -inline-instr-cost=1

zram-y	:=	zcomp.o zram_drv.o

obj-$(CONFIG_ZRAM)	+=	zram.o
