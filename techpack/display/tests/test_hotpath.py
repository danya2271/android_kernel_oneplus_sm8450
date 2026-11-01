#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-side regression checks for SDE IRQ dispatch and commit lock scope."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


DISPLAY = Path(__file__).resolve().parents[1]


def definition(source, signature):
    """Extract a C definition so tests exercise the driver, not a duplicate."""
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


def run_c(source, *defines):
    with tempfile.TemporaryDirectory(prefix="sde-hotpath-") as directory:
        src = Path(directory) / "test.c"
        binary = Path(directory) / "test"
        src.write_text(source, encoding="ascii")
        compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
        flags = shlex.split(os.environ.get("HOSTCFLAGS", "-O2"))
        subprocess.run(
            compiler + flags + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
            + ["-D" + define for define in defines]
            + [str(src), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


IRQ_MOCK = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
struct sde_hw_blk_reg_map { int unused; };
static int lock_depth;
#define spin_lock_irqsave(lock, flags) do { \
    (void)(lock); (flags) = 0; assert(!lock_depth++); \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
    (void)(lock); (void)(flags); assert(!--lock_depth); \
} while (0)
"""

IRQ_IO = r"""
struct sde_hw_intr {
    struct sde_hw_blk_reg_map hw;
    u32 *cache_irq_mask;
    int sde_irq_size;
    struct sde_intr_reg *sde_irq_tbl;
    int sde_irq_map_size;
    struct sde_irq_type *sde_irq_map;
    int irq_lock;
};

static u32 status[3], cleared[3];
static int reads[3], callbacks[6], callback_count;

static u32 read_reg(struct sde_hw_blk_reg_map *hw, u32 offset)
{
    (void)hw;
    assert(lock_depth == 1 && offset < 3);
    reads[offset]++;
    return status[offset];
}

static void write_reg(struct sde_hw_blk_reg_map *hw, u32 offset, u32 value)
{
    (void)hw;
    assert(lock_depth == 1 && offset < 3);
    cleared[offset] |= value;
}

#define SDE_REG_READ read_reg
#define SDE_REG_WRITE write_reg

static void callback(void *arg, int index)
{
    struct sde_hw_intr *intr = arg;
    struct sde_irq_type *irq = &intr->sde_irq_map[index];

    assert(lock_depth == 1 && callback_count < 6);
    callbacks[callback_count++] = index;
    cleared[irq->reg_idx] |= irq->irq_mask;
}
"""

IRQ_TEST = r"""
int main(void)
{
    struct sde_intr_reg banks[3] = {
        { .clr_off = 0, .status_off = 0, .map_idx_start = 0, .map_idx_end = 2 },
        { .clr_off = 1, .status_off = 1, .map_idx_start = 2, .map_idx_end = 4 },
        { .clr_off = 2, .status_off = 2, .map_idx_start = 4, .map_idx_end = 6 },
    };
    /* Deliberately not in bit order: callback ordering must not change. */
    struct sde_irq_type map[6] = {
        { .irq_mask = 1U << 31, .reg_idx = 0 },
        { .irq_mask = 1U << 0,  .reg_idx = 0 },
        { .irq_mask = 1U << 4,  .reg_idx = 1 },
        { .irq_mask = 1U << 2,  .reg_idx = 1 },
        { .irq_mask = 1U << 1,  .reg_idx = 2 },
        { .irq_mask = 1U << 5,  .reg_idx = 2 },
    };
    u32 enabled[3] = { 0 };
    struct sde_hw_intr intr = {
        .cache_irq_mask = enabled, .sde_irq_size = 3,
        .sde_irq_tbl = banks, .sde_irq_map_size = 6, .sde_irq_map = map,
    };
    unsigned int mask, pending;
    int i, expected;

    sde_hw_intr_dispatch_irq(NULL, callback, &intr);
    for (mask = 0; mask < 64; mask++) {
        for (pending = 0; pending < 64; pending++) {
            memset(enabled, 0, sizeof(enabled));
            memset(status, 0, sizeof(status));
            memset(reads, 0, sizeof(reads));
            memset(cleared, 0, sizeof(cleared));
            callback_count = 0;
            for (i = 0; i < 6; i++) {
                if (mask & (1U << i))
                    enabled[map[i].reg_idx] |= map[i].irq_mask;
                if (pending & (1U << i))
                    status[map[i].reg_idx] |= map[i].irq_mask;
            }
            /* A stale, unmapped bit must neither dispatch nor be cleared. */
            status[0] |= 1U << 17;
            sde_hw_intr_dispatch_irq(&intr, callback, &intr);
            expected = 0;
            for (i = 0; i < 6; i++)
                if ((mask & pending) & (1U << i))
                    assert(callbacks[expected++] == i);
            assert(callback_count == expected);
            for (i = 0; i < 3; i++) {
                assert(reads[i] == !!enabled[i]);
                assert(cleared[i] == (status[i] & enabled[i]));
            }
            memset(cleared, 0, sizeof(cleared));
            sde_hw_intr_dispatch_irq(&intr, NULL, NULL);
            for (i = 0; i < 3; i++)
                assert(cleared[i] == (status[i] & enabled[i]));
        }
    }

    /* Invalid mapping ranges must be rejected before accessing MMIO. */
    banks[0].map_idx_end = 7;
    enabled[0] = 1;
    reads[0] = 0;
    sde_hw_intr_dispatch_irq(&intr, NULL, NULL);
    assert(reads[0] == 0 && lock_depth == 0);
    puts("IRQ: 4096 mask/status combinations, callback order and clear fallback OK");
    return 0;
}
"""

COMMIT_MOCK = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct msm_commit { unsigned int crtc_mask, plane_mask; };
struct drm_device { void *dev_private; };
struct drm_atomic_state { struct drm_device *dev; };
struct msm_kms_state {
    struct drm_atomic_state base;
    struct msm_commit commit;
};
struct msm_kms;
struct msm_kms_funcs {
    void (*prepare_commit)(struct msm_kms *, struct drm_atomic_state *);
    void (*complete_commit)(struct msm_kms *, struct drm_atomic_state *);
};
struct msm_kms { struct msm_kms_funcs *funcs; };
struct msm_drm_private { struct msm_kms *kms; int dspp_lock; };
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define DRM_PLANE_COMMIT_ACTIVE_ONLY 1
static int locked, step;
#ifdef OPLUS_FEATURE_DISPLAY
#define CP_LOCKED 1
#else
#define CP_LOCKED 0
#endif
#define mutex_lock(lock) do { (void)(lock); assert(!locked++); } while (0)
#define mutex_unlock(lock) do { (void)(lock); assert(!--locked); } while (0)

static void drm_atomic_helper_wait_for_fences(struct drm_device *dev,
        struct drm_atomic_state *state, bool pre_swap)
{
    assert(dev == state->dev && !pre_swap);
    assert(step++ == 0 && !locked);
}
static void prepare(struct msm_kms *kms, struct drm_atomic_state *state)
{
    (void)kms; (void)state;
    assert(step++ == 1 && locked == CP_LOCKED);
}
static void msm_atomic_helper_commit_modeset_disables(struct drm_device *dev,
        struct drm_atomic_state *state)
{
    assert(dev == state->dev && step++ == 2 && locked == CP_LOCKED);
}
static void drm_atomic_helper_commit_planes(struct drm_device *dev,
        struct drm_atomic_state *state, int flags)
{
    assert(flags == DRM_PLANE_COMMIT_ACTIVE_ONLY && dev == state->dev);
    assert(step++ == 3 && locked == CP_LOCKED);
}
static void msm_atomic_helper_commit_modeset_enables(struct drm_device *dev,
        struct drm_atomic_state *state)
{
    assert(dev == state->dev && step++ == 4 && !locked);
}
static void msm_atomic_wait_for_commit_done(struct drm_device *dev,
        struct drm_atomic_state *state)
{
    assert(dev == state->dev && step++ == 5 && !locked);
}
static void drm_atomic_helper_cleanup_planes(struct drm_device *dev,
        struct drm_atomic_state *state)
{
    assert(dev == state->dev && step++ == 6 && !locked);
}
static void complete(struct msm_kms *kms, struct drm_atomic_state *state)
{
    (void)kms; (void)state;
    assert(step++ == 7 && !locked);
}
static void end_atomic(struct msm_drm_private *priv, unsigned int crtcs,
        unsigned int planes)
{
    (void)priv;
    assert(crtcs == 1 && planes == 3 && step++ == 8 && !locked);
}
"""

COMMIT_TEST = r"""
int main(void)
{
    struct msm_kms_funcs funcs = { prepare, complete };
    struct msm_kms kms = { &funcs };
    struct msm_drm_private priv = { .kms = &kms };
    struct drm_device dev = { &priv };
    struct msm_kms_state state = {
        .base = { &dev }, .commit = { .crtc_mask = 1, .plane_mask = 3 },
    };

    complete_commit(&state.commit);
    assert(step == 9 && !locked);
    puts("commit: fence wait unlocked, programming locked, completion ordering OK");
    return 0;
}
"""

PROPERTY_MOCK = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define __user
#define unlikely(x) (x)
#define SDE_ERROR(...) do {} while (0)
#define SDE_ATRACE_BEGIN(...) do {} while (0)
struct msm_property_info { int unused; };
struct msm_drm_private { int dspp_lock; };
struct drm_device { struct msm_drm_private *dev_private; };
struct drm_crtc { struct drm_device *dev; };
struct drm_crtc_state { int unused; };
struct drm_property { int unused; };
struct sde_crtc { struct msm_property_info property_info; };
struct sde_crtc_state {
    struct drm_crtc_state base;
    int property_state;
};
static struct sde_crtc shared;
#define to_sde_crtc(crtc) ((void)(crtc), &shared)
#define to_sde_crtc_state(state) \
    ((struct sde_crtc_state *)((char *)(state) \
        - offsetof(struct sde_crtc_state, base)))
static int locked, known, cp_result, generic_result, cp_calls, generic_calls;
#ifdef OPLUS_FEATURE_DISPLAY
#define CP_LOCKED 1
#else
#define CP_LOCKED 0
#endif
#define mutex_lock(lock) do { (void)(lock); assert(!locked++); } while (0)
#define mutex_unlock(lock) do { (void)(lock); assert(!--locked); } while (0)

static int msm_property_index(struct msm_property_info *info,
        struct drm_property *property)
{
    (void)info; (void)property;
    return known ? 3 : -EINVAL;
}
static int sde_cp_crtc_set_property(struct drm_crtc *crtc,
        struct drm_crtc_state *state, struct drm_property *property, uint64_t val)
{
    (void)crtc; (void)state; (void)property; (void)val;
    assert(!known && locked == CP_LOCKED);
    cp_calls++;
    return cp_result;
}
static int msm_property_atomic_set(struct msm_property_info *info,
        int *state, struct drm_property *property, uint64_t val)
{
    (void)info; (void)state; (void)property; (void)val;
    /* A previous commit still owns the global lock. */
    assert(known && locked == 1);
    generic_calls++;
    return generic_result;
}
"""

PROPERTY_TEST = r"""
int main(void)
{
    struct msm_drm_private priv = { 0 };
    struct drm_device dev = { &priv };
    struct drm_crtc crtc = { &dev };
    struct sde_crtc_state state = { 0 };
    struct drm_property property = { 0 };
    const int errors[] = { 0, -ENOMEM, -ENOENT, -EINVAL };
    unsigned int i;

    known = 1;
    locked = 1;
    assert(!sde_crtc_atomic_set_property(&crtc, &state.base, &property, 123));
    assert(generic_calls == 1 && cp_calls == 0 && locked == 1);
    generic_result = -ENOMEM;
    assert(sde_crtc_atomic_set_property(&crtc, &state.base, &property, 123)
        == -ENOMEM);
    assert(generic_calls == 2 && cp_calls == 0 && locked == 1);

    known = 0;
    locked = 0;
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        cp_result = errors[i];
        assert(sde_crtc_atomic_set_property(&crtc, &state.base, &property, 123)
            == (cp_result == -ENOENT ? -EINVAL : cp_result));
        assert(!locked && generic_calls == 2);
    }
    assert(cp_calls == 4);
    puts("properties: ordinary path bypasses DSPP lock, CP errors unlock correctly");
    return 0;
}
"""


class HotpathTests(unittest.TestCase):
    def test_irq_dispatch(self):
        source = (DISPLAY / "msm/sde/sde_hw_interrupts.c").read_text()
        run_c(
            IRQ_MOCK
            + definition(source, "struct sde_intr_reg {") + ";\n"
            + definition(source, "struct sde_irq_type {") + ";\n"
            + IRQ_IO
            + definition(source, "static void sde_hw_intr_dispatch_irq(")
            + IRQ_TEST
        )

    def test_commit_order_with_and_without_oplus(self):
        source = (DISPLAY / "msm/msm_atomic.c").read_text()
        test = (
            COMMIT_MOCK
            + definition(source, "static void complete_commit(")
            + COMMIT_TEST
        )
        run_c(test, "OPLUS_FEATURE_DISPLAY")
        run_c(test)

    def test_property_dispatch(self):
        source = (DISPLAY / "msm/sde/sde_crtc.c").read_text()
        setter = definition(source, "static int sde_crtc_atomic_set_property(")
        # Exercise real dispatch/error handling; property-specific switches
        # need a DRM device and are intentionally outside this host test.
        prologue = setter[:setter.index("\tswitch (idx) {")]
        prologue += (
            "\nexit:\n"
            "    (void)fence_user_fd;\n"
            "    (void)prev_user_fd;\n"
            "    return ret;\n}\n"
        )
        test = PROPERTY_MOCK + prologue + PROPERTY_TEST
        run_c(test, "OPLUS_FEATURE_DISPLAY")
        run_c(test)

    def test_property_lock_scope(self):
        source = (DISPLAY / "msm/sde/sde_crtc.c").read_text()
        setter = definition(source, "static int sde_crtc_atomic_set_property(")
        cp = definition(setter, "if (idx < 0)")
        self.assertEqual(setter.count("mutex_lock(&priv->dspp_lock)"), 1)
        self.assertEqual(setter.count("mutex_unlock(&priv->dspp_lock)"), 1)
        self.assertNotIn("dspp_lock", setter.replace(cp, ""))
        self.assertLess(cp.index("mutex_lock("),
                        cp.index("sde_cp_crtc_set_property("))
        self.assertLess(cp.index("sde_cp_crtc_set_property("),
                        cp.index("mutex_unlock("))
        self.assertLess(cp.index("mutex_unlock("), cp.index("goto exit"))
        self.assertIn("if (ret == -ENOENT)\n\t\t\tret = -EINVAL;", cp)
        self.assertNotIn("msm_property_atomic_set(", cp)

    def test_irq_diagnostics_guard(self):
        source = (DISPLAY / "msm/sde/sde_encoder_phys_cmd.c").read_text()
        for name in ("pp_tx_done", "te_rd_ptr", "wr_ptr"):
            with self.subTest(irq=name):
                handler = definition(
                    source, "static void sde_encoder_phys_cmd_" + name + "_irq("
                )
                guard = definition(
                    handler,
                    "if (sde_evtlog_is_enabled(sde_dbg_base_evtlog, SDE_EVTLOG_IRQ))"
                )
                for call in ("get_scheduler_status", "get_pp_line_count",
                             "SDE_EVT32_IRQ"):
                    self.assertEqual(handler.count(call), guard.count(call))
                self.assertIn("SDE_EVT32_IRQ", guard)

    def test_vblank_allocation_pair(self):
        source = (DISPLAY / "msm/sde/sde_crtc.c").read_text()
        queue = definition(source, "static int vblank_ctrl_queue_work(")
        worker = definition(source, "static void vblank_ctrl_worker(")
        self.assertIn("kzalloc(sizeof(*cur_work), GFP_ATOMIC)", queue)
        self.assertIn("kfree(cur_work)", worker)
        self.assertNotIn("kmem_cache_free", worker)


if __name__ == "__main__":
    unittest.main(verbosity=2)
