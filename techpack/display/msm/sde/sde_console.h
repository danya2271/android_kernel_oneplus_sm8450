/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SDE_CONSOLE_H__
#define __SDE_CONSOLE_H__

#include <linux/errno.h>

struct drm_device;

#if IS_ENABLED(CONFIG_DRM_MSM_SDE)
int sde_console_init_splash(struct drm_device *dev);
void sde_console_user_open(struct drm_device *dev);
void sde_console_fini(struct drm_device *dev);
#else
static inline int sde_console_init_splash(struct drm_device *dev)
{
	return -ENODEV;
}

static inline void sde_console_user_open(struct drm_device *dev)
{
}

static inline void sde_console_fini(struct drm_device *dev)
{
}
#endif

#endif
