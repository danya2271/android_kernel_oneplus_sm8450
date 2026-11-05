/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_KKNX_PERF_H
#define _LINUX_KKNX_PERF_H

#include <linux/kconfig.h>
#include <linux/types.h>

enum kknx_perf_load_level {
	KKNX_PERF_LEVEL_UNKNOWN,
	KKNX_PERF_LEVEL_MINIMAL,
	KKNX_PERF_LEVEL_MEDIUM,
	KKNX_PERF_LEVEL_HIGH,
};

struct kknx_perf_display_ops {
	int (*get_refresh_rate)(u32 *refresh_rate);
	void (*update_policy)(u32 load_level, bool probe_60hz);
};

enum kknx_perf_load_level kknx_perf_get_load_level(void);
bool kknx_perf_is_minimal_load(void);
bool kknx_perf_is_medium_load(void);
bool kknx_perf_is_high_load(void);
int kknx_perf_register_display_ops(const struct kknx_perf_display_ops *ops);
void kknx_perf_unregister_display_ops(const struct kknx_perf_display_ops *ops);

#if IS_ENABLED(CONFIG_KKNX_PERF)
bool kknx_perf_use_minimal_cpu_floor(void);
void kknx_perf_set_cpu_boost_active(bool active);
void kknx_perf_clear_cpu_max_cap_for_boost(void);
#else
static inline bool kknx_perf_use_minimal_cpu_floor(void)
{
	return false;
}

static inline void kknx_perf_set_cpu_boost_active(bool)
{
}

static inline void kknx_perf_clear_cpu_max_cap_for_boost(void)
{
}
#endif

#endif
