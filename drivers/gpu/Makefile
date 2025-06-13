# SPDX-License-Identifier: GPL-2.0-only
# drm/tegra depends on host1x, so if both drivers are built-in care must be
# taken to initialize them in the correct order. Link order is the only way
# to ensure this currently.
KBUILD_CFLAGS += -Ofast
KBUILD_CFLAGS  += -mllvm -inline-threshold=18480
KBUILD_CFLAGS  += -mllvm -inlinehint-threshold=18350
KBUILD_CFLAGS  += -mllvm -unroll-threshold=20000
KBUILD_CFLAGS  += -mllvm -vectorize-memory-check-threshold=1
KBUILD_CFLAGS  += -mllvm -aggressive-ext-opt
KBUILD_CFLAGS  += -fno-strict-overflow

obj-$(CONFIG_TEGRA_HOST1X)	+= host1x/
obj-y			+= drm/ vga/
obj-$(CONFIG_IMX_IPUV3_CORE)	+= ipu-v3/
obj-$(CONFIG_QCOM_KGSL) += msm/
obj-$(CONFIG_TRACE_GPU_MEM)		+= trace/
