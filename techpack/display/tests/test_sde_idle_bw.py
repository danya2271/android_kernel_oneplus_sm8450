#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real SDE idle bandwidth and LLCC vote aggregation."""

import unittest

from test_hotpath import DISPLAY, definition, run_c


SOURCE = DISPLAY / "msm/sde/sde_core_perf.c"

MOCK = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#pragma GCC diagnostic ignored "-Wunused-parameter"
typedef uint32_t u32;
typedef uint64_t u64;
enum { SDE_POWER_HANDLE_DBUS_ID_MNOC, SDE_POWER_HANDLE_DBUS_ID_LLCC,
       SDE_POWER_HANDLE_DBUS_ID_EBI, SDE_POWER_HANDLE_DBUS_ID_MAX };
enum sde_sys_cache_type { SDE_SYS_CACHE_DISP, SDE_SYS_CACHE_MAX };
enum { APPS_RSC_MODE, DISP_RSC_MODE, DISP_RSC_PRIMARY_MODE };
enum { SDE_PERF_MODE_NORMAL, SDE_PERF_MODE_FIXED };
enum sde_crtc_client_type { RT_CLIENT, RT_RSC_CLIENT };
enum { INTF_MODE_CMD = 1, INTF_MODE_VIDEO = 2 };
#define KKNX_PERF_LEVEL_UNKNOWN 0
#define KKNX_PERF_LEVEL_MINIMAL 1
#define GENMASK(h, l) (((~0U) >> (31 - (h))) & (~0U << (l)))
#define DRMID(crtc) ((crtc)->id)
#define SDE_EVT32(...) do {} while (0)
#define SDE_DEBUG(...) do {} while (0)
#define SDE_ERROR(...) do {} while (0)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define drm_for_each_crtc(crtc, dev) \
    for ((dev)->iter = 0; (dev)->iter < (dev)->count && \
         (((crtc) = (dev)->crtcs[(dev)->iter]), true); (dev)->iter++)
struct drm_crtc_state { int unused; };
struct sde_crtc_state {
    struct drm_crtc_state base;
    void *rsc_client;
};
struct drm_device {
    struct drm_crtc *crtcs[3];
    unsigned int count, iter;
    void *dev_private;
};
struct drm_crtc {
    struct drm_device *dev;
    struct drm_crtc_state *state;
    unsigned int id;
    int client_type;
    bool enabled;
};
struct sde_core_perf_params {
    u64 bw_ctl[SDE_POWER_HANDLE_DBUS_ID_MAX];
    u64 max_per_pipe_ib[SDE_POWER_HANDLE_DBUS_ID_MAX];
    bool llcc_active[SDE_SYS_CACHE_MAX];
};
struct rsc_client { u64 ab[SDE_POWER_HANDLE_DBUS_ID_MAX]; };
struct sde_crtc {
    struct drm_crtc base;
    struct sde_core_perf_params cur_perf;
    bool bw_vote_idle;
};
struct sde_sc_cfg { bool has_sys_cache; };
struct sde_perf_cfg {
    u32 max_bw_high, num_ddr_channels, dram_efficiency;
    struct sde_sc_cfg sc_cfg[SDE_SYS_CACHE_MAX];
};
struct sde_mdss_cfg { struct sde_perf_cfg perf; };
struct sde_core_perf {
    struct sde_perf_cfg *catalog;
    struct { u64 min_bus_vote; u32 mode; } perf_tune;
    u64 fix_core_ab_vote, fix_core_ib_vote;
    u32 bw_vote_mode;
    u32 kknx_load_level;
    bool sde_rsc_available, bw_vote_mode_updated;
};
struct sde_kms {
    struct drm_device *dev;
    struct sde_mdss_cfg *catalog;
    struct sde_core_perf perf;
};
struct sde_power_handle { int unused; };
struct msm_drm_private { struct sde_power_handle phandle; };
static struct drm_device device;
static struct sde_perf_cfg catalog;
static struct sde_mdss_cfg mdss;
static struct msm_drm_private priv;
static struct sde_kms kms;
static int sde_core_perf_lock;
static int active_intf_mode = INTF_MODE_CMD;
static u64 apps_ab[SDE_POWER_HANDLE_DBUS_ID_MAX];
static u64 apps_ib[SDE_POWER_HANDLE_DBUS_ID_MAX];
static unsigned int apps_updates;
static unsigned int llcc_updates, rsc_triggers;
static bool llcc_active[SDE_SYS_CACHE_MAX];
static struct sde_crtc crtcs[3];
static struct sde_crtc_state states[3];
static struct rsc_client rsc_clients[3];
#define to_sde_crtc(crtc) ((struct sde_crtc *)(crtc))
#define to_sde_crtc_state(state) \
    ((struct sde_crtc_state *)((char *)(state) - \
        offsetof(struct sde_crtc_state, base)))
static void mutex_lock(int *lock) { assert(!(*lock)++); }
static void mutex_unlock(int *lock) { assert(!--(*lock)); }
static bool sde_crtc_is_enabled(struct drm_crtc *crtc) { return crtc->enabled; }
static enum sde_crtc_client_type sde_crtc_get_client_type(struct drm_crtc *crtc)
{ return crtc->client_type; }
static int sde_crtc_get_intf_mode(struct drm_crtc *crtc,
                                  struct drm_crtc_state *state)
{ return active_intf_mode; }
static struct sde_kms *_sde_crtc_get_kms(struct drm_crtc *crtc) { return &kms; }
static void sde_power_data_bus_set_quota(struct sde_power_handle *handle,
                                         u32 bus, u64 ab, u64 ib)
{
    apps_ab[bus] = ab;
    apps_ib[bus] = ib;
    apps_updates++;
}
static int sde_rsc_client_vote(struct rsc_client *client, u32 bus,
                               u64 ab, u64 ib)
{
    client->ab[bus] = ab;
    return 0;
}
static int sde_rsc_client_trigger_vote(struct rsc_client *client, bool delta)
{
    rsc_triggers++;
    return 0;
}
static int _sde_core_perf_activate_llcc(struct sde_kms *sde_kms, int type,
                                        bool activate)
{
    llcc_active[type] = activate;
    llcc_updates++;
    return 0;
}
static u64 div_u64(u64 dividend, u32 divisor)
{ assert(divisor); return dividend / divisor; }
"""


class SdeIdleBandwidthTests(unittest.TestCase):
    def test_shared_bus_and_llcc_votes_restore_without_losing_other_crtcs(self):
        source = SOURCE.read_text()
        text = MOCK
        for signature in (
            "static bool _sde_core_perf_crtc_is_power_on(",
            "static inline bool _is_crtc_client_type_matches(",
            "static inline enum sde_crtc_client_type _get_sde_client_type(",
            "static void _sde_core_perf_crtc_set_llcc_cache_type(",
            "static void _sde_core_perf_crtc_update_bus(",
            "static void _sde_core_perf_crtc_set_idle_bw_locked(",
            "int sde_core_perf_crtc_set_idle_bw(",
        ):
            text += definition(source, signature) + "\n"
        text += r"""
int main(void)
{
    unsigned int i;
    struct sde_crtc *a = &crtcs[0], *b = &crtcs[1], *c = &crtcs[2];

    kms.dev = &device;
    kms.catalog = &mdss;
    kms.perf.catalog = &catalog;
    kms.perf.bw_vote_mode = APPS_RSC_MODE;
    kms.perf.kknx_load_level = KKNX_PERF_LEVEL_MINIMAL;
    catalog.max_bw_high = 10000;
    mdss.perf.max_bw_high = 10000;
    catalog.num_ddr_channels = 2;
    catalog.dram_efficiency = 50;
    mdss.perf.num_ddr_channels = 2;
    mdss.perf.dram_efficiency = 50;
    catalog.sc_cfg[SDE_SYS_CACHE_DISP].has_sys_cache = true;
    device.count = 3;
    device.dev_private = &priv;
    for (i = 0; i < 3; i++) {
        crtcs[i].base.dev = &device;
        crtcs[i].base.id = i + 1;
        crtcs[i].base.enabled = true;
        crtcs[i].base.client_type = RT_CLIENT;
        crtcs[i].base.state = &states[i].base;
        states[i].rsc_client = &rsc_clients[i];
        device.crtcs[i] = &crtcs[i].base;
        crtcs[i].cur_perf.llcc_active[SDE_SYS_CACHE_DISP] = true;
    }
    a->cur_perf.bw_ctl[0] = 100;
    b->cur_perf.bw_ctl[0] = 200;
    c->cur_perf.bw_ctl[0] = 500;
    c->base.enabled = false;

    kms.perf.kknx_load_level = KKNX_PERF_LEVEL_UNKNOWN;
    assert(sde_core_perf_crtc_set_idle_bw(&a->base, true) == -EAGAIN);
    assert(!a->bw_vote_idle);
    kms.perf.kknx_load_level = KKNX_PERF_LEVEL_MINIMAL;
    assert(!sde_core_perf_crtc_set_idle_bw(&a->base, true));
    assert(a->bw_vote_idle && apps_ab[0] == 200 && apps_ib[0] == 200);
    assert(llcc_active[SDE_SYS_CACHE_DISP]); /* B still uses the display slice. */
    assert(apps_updates == SDE_POWER_HANDLE_DBUS_ID_MAX);
    assert(!sde_core_perf_crtc_set_idle_bw(&a->base, true));
    assert(apps_updates == SDE_POWER_HANDLE_DBUS_ID_MAX);

    assert(!sde_core_perf_crtc_set_idle_bw(&b->base, true));
    assert(apps_ab[0] == 0 && apps_ib[0] == 0);
    assert(!llcc_active[SDE_SYS_CACHE_DISP]);
    kms.perf.perf_tune.min_bus_vote = 150;
    assert(!sde_core_perf_crtc_set_idle_bw(&a->base, false));
    assert(apps_ab[0] == 150 && apps_ib[0] == 150);
    assert(llcc_active[SDE_SYS_CACHE_DISP]);
    assert(!sde_core_perf_crtc_set_idle_bw(&b->base, false));
    assert(apps_ab[0] == 300 && apps_ib[0] == 300);

    kms.perf.perf_tune.min_bus_vote = 0;
    kms.perf.bw_vote_mode = DISP_RSC_PRIMARY_MODE;
    kms.perf.kknx_load_level = KKNX_PERF_LEVEL_MINIMAL;
    kms.perf.sde_rsc_available = true;
    b->base.client_type = RT_RSC_CLIENT;
    c->base.enabled = true;
    c->base.client_type = RT_RSC_CLIENT;
    b->cur_perf.bw_ctl[0] = 200;
    c->cur_perf.bw_ctl[0] = 50;
    assert(!sde_core_perf_crtc_set_idle_bw(&b->base, true));
    assert(rsc_clients[1].ab[0] == 50);
    assert(rsc_triggers == 1);
    assert(!sde_core_perf_crtc_set_idle_bw(&b->base, false));
    assert(rsc_clients[1].ab[0] == 250 && rsc_triggers == 2);

    active_intf_mode = INTF_MODE_VIDEO;
    assert(sde_core_perf_crtc_set_idle_bw(&a->base, true) == -EOPNOTSUPP);
    assert(!a->bw_vote_idle);
    assert(sde_core_perf_crtc_set_idle_bw(NULL, true) == -EINVAL);
    assert(!sde_core_perf_lock);
    puts("SDE idle BW: shared DDR/LLCC votes, min floor, RSC client isolation and restore");
    return 0;
}
"""
        run_c(text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
