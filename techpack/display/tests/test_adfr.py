#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual ADFR policy/DSI functions with host-side fault injection."""

import unittest

from test_hotpath import DISPLAY, definition, run_c


SOURCE = DISPLAY / "oplus/oplus_adfr.c"
HEADER = DISPLAY / "oplus/oplus_adfr.h"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#pragma GCC diagnostic ignored "-Wunused-parameter"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef s64 ktime_t;
typedef int spinlock_t;
typedef int irqreturn_t;
struct mutex { int held; };
struct hrtimer { int unused; };
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define NSEC_PER_SEC 1000000000ULL
#define IRQ_HANDLED 1
#define HRTIMER_MODE_REL 0
#define SDE_MODE_DPMS_ON 0
#define SDE_MODE_DPMS_LP1 1
#define SDE_MODE_DPMS_OFF 5
#define SDE_ATRACE_BEGIN(...) do {} while (0)
#define SDE_ATRACE_END(...) do {} while (0)
#define SDE_ATRACE_INT(...) do {} while (0)
#define SDE_EVT32(...) do {} while (0)
#define DSI_DEBUG(...) do {} while (0)
#define DSI_INFO(...) do {} while (0)
#define DSI_ERR(...) do {} while (0)
#define pr_err(...) do {} while (0)
#define BIT(n) (1U << (n))
#define spin_lock_init(lock) (*(lock) = 0)
#define mutex_init(lock) ((lock)->held = 0)
static int spin_depth;
#define spin_lock_irqsave(lock, flags) do { \
    (flags) = 0; assert(!spin_depth++); assert(!(*(lock))++); \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
    (void)(flags); assert(!--(*(lock))); assert(!--spin_depth); \
} while (0)
static void mutex_lock(struct mutex *lock)
{ assert(!spin_depth && !lock->held); lock->held = 1; }
static void mutex_unlock(struct mutex *lock)
{ assert(!spin_depth && lock->held); lock->held = 0; }
static u64 div64_u64(u64 n, u64 d) { assert(d); return n / d; }
static s64 ktime_sub(s64 a, s64 b) { return a - b; }
static s64 ktime_to_ns(s64 t) { return t; }
static s64 ms_to_ktime(s64 t) { return t * 1000000; }
static u64 fake_now;
static u64 ktime_get_ns(void) { return fake_now; }
"""

MOCK = r"""
enum { DSI_CMD_SET_QSYNC_ON, DSI_CMD_SET_QSYNC_OFF,
       DSI_CMD_QSYNC_MIN_FPS_0, DSI_CMD_QSYNC_MIN_FPS_1, CMD_COUNT };
struct dsi_cmd_desc {
    struct { size_t tx_len; const void *tx_buf; } msg;
};
struct dsi_panel_cmd_set { u32 count; struct dsi_cmd_desc *cmds; };
struct dsi_display_mode_priv_info { struct dsi_panel_cmd_set cmd_sets[CMD_COUNT]; };
struct dsi_mode_info { int h_skew, refresh_rate; u32 h_active, v_active; };
struct dsi_display_mode {
    struct dsi_mode_info timing;
    struct dsi_display_mode_priv_info *priv_info;
};
struct dsi_panel {
    struct oplus_adfr_state adfr;
    struct mutex panel_lock;
    const char *name, *type;
    void *panel_of_node;
    struct dsi_display_mode *cur_mode;
    struct { const char *vendor_name; } oplus_priv;
    u32 cur_h_active;
    int power_mode;
};
struct dsi_display {
    struct dsi_panel *panel;
    struct mutex display_lock;
    u32 ctrl_count;
};
struct sde_encoder_phys_cmd { int unused; };
#define display_for_each_ctrl(i, display) \
    for ((i) = 0; (u32)(i) < (display)->ctrl_count; (i)++)
static u32 oplus_adfr_config = 23, oplus_adfr_debug, dt_config = 23;
static const u32 oplus_adfr_exit_idle_minfps = OPLUS_ADFR_AUTO_MIN_FPS_40HZ;
static u64 oplus_adfr_auto_update_counter;
static bool oplus_adfr_skip_min_fps_cmd;
static struct oplus_adfr_dynamic_te oplus_adfr_dynamic_te;
static struct oplus_te_refcount te_refcount;
static int oplus_dsi_log_type;
static int attempts, fail_at, failure = -EIO, notices, delays, timers;
static u32 inject_request, notified, sent_min, sent_mode;
static bool inject_reset;
static struct dsi_panel *last_panel;
static int dsi_panel_tx_cmd_set(struct dsi_panel *panel, int command);
static int oplus_adfr_send_min_fps_event(unsigned int h_skew, unsigned int value)
{
    assert(!spin_depth);
    notices++;
    notified = value;
    return 0;
}
static int of_property_read_u32(void *node, const char *key, u32 *value)
{
    assert(!strcmp(key, "oplus,adfr-config"));
    *value = dt_config;
    return 0;
}
static void oplus_adfr_idle_mode_minfps_delay(void *cmd)
{ assert(!spin_depth); delays++; }
static void hrtimer_start(struct hrtimer *timer, ktime_t time, int mode)
{ assert(!spin_depth); timers++; }
"""

FIXTURE = r"""
static int dsi_panel_tx_cmd_set(struct dsi_panel *panel, int command)
{
    const u8 *payload;

    assert(!spin_depth && panel->panel_lock.held && panel->adfr.update_lock.held);
    last_panel = panel;
    attempts++;
    if (attempts == fail_at)
        return failure;
    if (command == DSI_CMD_SET_QSYNC_ON)
        sent_mode = OPLUS_ADFR_AUTO_ON;
    else if (command == DSI_CMD_SET_QSYNC_OFF)
        sent_mode = OPLUS_ADFR_AUTO_OFF;
    else {
        payload = panel->cur_mode->priv_info->cmd_sets[command].cmds[2].msg.tx_buf;
        sent_min = payload[1];
    }
    if (inject_request) {
        u32 request = inject_request;
        inject_request = 0;
        assert(oplus_adfr_handle_auto_mode(panel, request) == 1);
    }
    if (inject_reset) {
        inject_reset = false;
        dsi_panel_adfr_status_reset(panel);
    }
    return 0;
}

struct fixture {
    struct dsi_panel panel;
    struct dsi_display display;
    struct dsi_display_mode mode;
    struct dsi_display_mode_priv_info info;
    struct dsi_cmd_desc desc[CMD_COUNT][3];
    u8 payload[CMD_COUNT][3][2];
};
static void init_fixture(struct fixture *f, int rate)
{
    int i, j;

    memset(f, 0, sizeof(*f));
    f->panel.name = "mock S6E3HC3";
    f->panel.type = "primary";
    f->panel.oplus_priv.vendor_name = "S6E3HC3";
    f->panel.cur_mode = &f->mode;
    f->panel.cur_h_active = 1440;
    f->mode.timing.h_active = 1440;
    f->mode.timing.h_skew = rate == 60 ? SDC_MFR : SDC_ADFR;
    f->mode.timing.refresh_rate = rate;
    f->mode.priv_info = &f->info;
    f->display.panel = &f->panel;
    f->display.ctrl_count = 1;
    for (i = 0; i < CMD_COUNT; i++) {
        f->info.cmd_sets[i].count = 3;
        f->info.cmd_sets[i].cmds = f->desc[i];
        for (j = 0; j < 3; j++) {
            f->desc[i][j].msg.tx_len = 2;
            f->desc[i][j].msg.tx_buf = f->payload[i][j];
        }
    }
    oplus_adfr_panel_init(&f->panel);
    f->panel.adfr.mode_valid = true;
    f->panel.adfr.min_fps_valid = true;
    f->panel.adfr.min_fps = rate == 60 ? 1 : 0;
    f->panel.adfr.applied_min_fps = f->panel.adfr.min_fps;
    assert(!spin_depth);
}
#define MIN_REQUEST(v) (OPLUS_ADFR_AUTO_MAGIC | OPLUS_ADFR_AUTO_MIN_FPS_MAGIC | (v))
#define MODE_REQUEST(v) (OPLUS_ADFR_AUTO_MAGIC | OPLUS_ADFR_AUTO_MODE_MAGIC | ((v) << 16))
"""


def program(body):
    source = SOURCE.read_text()
    header = HEADER.read_text()
    text = PREFIX
    text += source[source.index("#define OPLUS_ADFR_CONFIG_GLOBAL"):
                   source.index("#define to_dsi_bridge")]
    text += definition(header, "struct oplus_adfr_state {") + ";\n"
    for name in (
        "h_skew_type", "oplus_adfr_auto_mode_value",
        "oplus_adfr_auto_fakeframe_value", "oplus_adfr_auto_min_fps_value",
        "oplus_adfr_idle_mode_value", "oplus_adfr_dynamic_te_config",
    ):
        text += definition(header, "enum " + name + " {") + ";\n"
    for name in ("oplus_adfr_dynamic_te", "oplus_te_refcount"):
        text += definition(header, "struct " + name + " {") + ";\n"
    text += MOCK
    for signature in (
        "bool oplus_adfr_is_support(",
        "bool oplus_adfr_idle_mode_is_enable(",
        "static bool oplus_adfr_panel_supported(",
        "void oplus_adfr_panel_init(",
        "bool oplus_adfr_auto_on_cmd_filter_set(",
        "bool oplus_adfr_auto_on_cmd_filter_get(",
        "bool oplus_adfr_has_auto_mode(",
        "int oplus_adfr_validate_auto_mode(",
        "int oplus_adfr_handle_auto_mode(",
        "static void oplus_adfr_update_panel_sw_fps(",
        "int oplus_adfr_fakeframe_status_update(",
        "void dsi_panel_adfr_status_reset(",
        "static int dsi_panel_send_auto_on_dcs(",
        "static int dsi_panel_send_auto_off_dcs(",
        "static int dsi_display_auto_mode_enable(",
        "static int dsi_panel_auto_minfps_check(",
        "static int dsi_panel_send_auto_minfps_dcs(",
        "static int dsi_display_auto_mode_min_fps(",
        "static bool oplus_adfr_retry_blocked(",
        "int dsi_display_auto_mode_update(",
        "static bool oplus_adfr_can_enter_idle(",
        "static int oplus_adfr_apply_idle(",
        "int oplus_get_te_fps(",
        "irqreturn_t oplus_adfr_dynamic_te_handler(",
    ):
        text += definition(source, signature) + "\n"
    # All real functions are available to each scenario, including diagnostics.
    text += FIXTURE + "\nint main(void)\n{\n" + body
    text += "\nassert(!spin_depth); return 0;\n}\n"
    return text


class AdfrTests(unittest.TestCase):
    def run_policy(self, body):
        # Some scenarios intentionally exercise only a subset of static helpers.
        run_c('#pragma GCC diagnostic ignored "-Wunused-function"\n' + program(body))

    def test_config_and_input_validation(self):
        self.run_policy(r"""
        unsigned int config, debug, raw;
        struct fixture f;
        for (config = 0; config < 64; config++)
            for (debug = 0; debug < 64; debug++) {
                bool supported = (config & 1) && !(debug & 1);
                oplus_adfr_config = config;
                oplus_adfr_debug = debug;
                assert(oplus_adfr_is_support() == supported);
                assert(oplus_adfr_idle_mode_is_enable() ==
                    (supported && (config & 16) && !(debug & 16)));
            }
        oplus_adfr_config = 23;
        oplus_adfr_debug = 0;
        init_fixture(&f, 120);
        for (raw = 0; raw < 128; raw++) {
            assert(oplus_adfr_validate_auto_mode(MIN_REQUEST(raw)) ==
                (raw <= 119 ? 0 : -EINVAL));
            assert(oplus_adfr_validate_auto_mode(
                MODE_REQUEST(OPLUS_ADFR_AUTO_IDLE) |
                OPLUS_ADFR_AUTO_MIN_FPS_MAGIC | raw) ==
                (raw <= 120 ? 0 : -EINVAL));
        }
        assert(oplus_adfr_validate_auto_mode(MODE_REQUEST(3)) == -EINVAL);
        assert(!oplus_adfr_validate_auto_mode(24));
        assert(!f.panel.adfr.sequence && !attempts);
        dt_config = 0;
        init_fixture(&f, 120);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) ==
            -EOPNOTSUPP);
        puts("config: 4096 combinations; invalid encodings rejected");
        """)

    def test_min_fps_matrix(self):
        self.run_policy(r"""
        struct fixture f;
        int rates[] = { 60, 90, 120 };
        unsigned int r, mode, idle, raw, valid = 0;
        for (r = 0; r < 3; r++) {
            init_fixture(&f, rates[r]);
            for (mode = 0; mode < 2; mode++)
                for (idle = 0; idle < 2; idle++)
                    for (raw = 0; raw < 128; raw++) {
                        unsigned int expected = raw;
                        f.panel.adfr.applied_auto_mode = mode;
                        if (rates[r] == 90)
                            expected = mode ? 0 : 9;
                        else if (rates[r] == 60)
                            expected = raw < 1 || raw > 119 ? 1 :
                                (!idle && raw > 2 ? 2 : raw);
                        else
                            expected = raw > 119 ? 0 :
                                (!mode && !idle && raw > 2 ? 2 : raw);
                        assert((u32)dsi_panel_auto_minfps_check(
                            &f.panel, raw, idle) == expected);
                        valid++;
                    }
        }
        init_fixture(&f, 90);
        f.panel.oplus_priv.vendor_name = "NT37705";
        assert(dsi_panel_auto_minfps_check(&f.panel, 11, false) == 11);
        init_fixture(&f, 120);
        for (raw = 0; raw < 128; raw++) {
            f.panel.adfr.min_fps = raw;
            assert(oplus_adfr_can_enter_idle(&f.panel.adfr) ==
                (raw > 2 && raw <= 119));
        }
        printf("min-FPS: %u cases; active 40Hz code=2, 90Hz quirk preserved\n", valid);
        """)

    def test_state_isolation_and_fps_units(self):
        self.run_policy(r"""
        struct fixture a, b;
        init_fixture(&a, 120);
        init_fixture(&b, 120);
        assert(oplus_adfr_handle_auto_mode(&a.panel, MIN_REQUEST(119)) == 1);
        assert(oplus_adfr_handle_auto_mode(&b.panel, MIN_REQUEST(3)) == 1);
        assert(!dsi_display_auto_mode_update(&a.display));
        assert(a.panel.adfr.min_fps == 119 && a.panel.adfr.applied_min_fps == 2);
        assert(b.panel.adfr.min_fps == 3 && b.panel.adfr.applied_min_fps == 0);
        assert(!dsi_display_auto_mode_update(&b.display));
        oplus_adfr_update_panel_sw_fps(&a.panel, 60);
        assert(a.panel.adfr.sw_fps == 60 && a.panel.adfr.min_fps == 119);
        assert(a.panel.adfr.fakeframe_updated);
        assert(oplus_adfr_handle_auto_mode(&a.panel,
            MODE_REQUEST(OPLUS_ADFR_AUTO_IDLE) |
            OPLUS_ADFR_AUTO_MIN_FPS_MAGIC | 30) == 1);
        oplus_adfr_update_panel_sw_fps(&a.panel, 120);
        assert(a.panel.adfr.sw_fps == 30 && a.panel.adfr.measured_fps == 120);
        assert(a.panel.adfr.min_fps == 119 && b.panel.adfr.min_fps == 3);
        puts("state: panels isolated; measured/HWC Hz never overwrite panel codes");
        """)

    def test_dsi_failures_and_retry(self):
        self.run_policy(r"""
        struct fixture f;
        init_fixture(&f, 120);
        assert(oplus_adfr_handle_auto_mode(&f.panel,
            MODE_REQUEST(OPLUS_ADFR_AUTO_ON) |
            OPLUS_ADFR_AUTO_MIN_FPS_MAGIC | 119) == 1);
        fail_at = 1;
        assert(dsi_display_auto_mode_update(&f.display) == -EIO);
        assert(f.panel.adfr.auto_mode_updated && f.panel.adfr.min_fps_updated);
        assert(!f.panel.adfr.mode_valid && !notices);
        fail_at = 3;
        assert(dsi_display_auto_mode_update(&f.display) == -EIO);
        assert(!f.panel.adfr.auto_mode_updated && f.panel.adfr.mode_valid);
        assert(f.panel.adfr.min_fps_updated && !f.panel.adfr.min_fps_valid);
        assert(!notices && f.panel.adfr.applied_min_fps == 0);
        fail_at = 0;
        assert(!dsi_display_auto_mode_update(&f.display));
        assert(f.panel.adfr.applied_auto_mode == 1);
        assert(f.panel.adfr.applied_min_fps == 119 && notified == 119);
        assert(!f.panel.adfr.min_fps_updated && f.panel.adfr.min_fps_valid);
        puts("DSI: mode/min-FPS failures retained, retry applies and notifies once");
        """)

    def test_missing_commands_and_power_deferral(self):
        self.run_policy(r"""
        struct fixture f;
        struct dsi_panel_cmd_set *set;
        init_fixture(&f, 120);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) == 1);
        f.panel.power_mode = SDE_MODE_DPMS_LP1;
        assert(dsi_display_auto_mode_update(&f.display) == -EAGAIN);
        assert(!attempts && f.panel.adfr.min_fps_updated);
        f.panel.power_mode = SDE_MODE_DPMS_ON;
        set = &f.info.cmd_sets[DSI_CMD_QSYNC_MIN_FPS_0];
        set->count = 0;
        assert(dsi_display_auto_mode_update(&f.display) == -EOPNOTSUPP);
        assert(dsi_display_auto_mode_update(&f.display) == -EAGAIN);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) == 1);
        assert(dsi_display_auto_mode_update(&f.display) == -EAGAIN);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(118)) == 1);
        set->count = 2;
        assert(dsi_display_auto_mode_update(&f.display) == -EINVAL);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) == 1);
        set->count = 3;
        f.desc[DSI_CMD_QSYNC_MIN_FPS_0][2].msg.tx_len = 1;
        assert(dsi_display_auto_mode_update(&f.display) == -EINVAL);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(118)) == 1);
        f.desc[DSI_CMD_QSYNC_MIN_FPS_0][2].msg.tx_len = 2;
        f.desc[DSI_CMD_QSYNC_MIN_FPS_0][2].msg.tx_buf = NULL;
        assert(dsi_display_auto_mode_update(&f.display) == -EINVAL);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) == 1);
        f.desc[DSI_CMD_QSYNC_MIN_FPS_0][2].msg.tx_buf =
            f.payload[DSI_CMD_QSYNC_MIN_FPS_0][2];
        set->cmds = NULL;
        assert(dsi_display_auto_mode_update(&f.display) == -EINVAL);
        assert(!notices && f.panel.adfr.min_fps_updated);
        set->cmds = f.desc[DSI_CMD_QSYNC_MIN_FPS_0];
        f.display.ctrl_count = 0;
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(118)) == 1);
        assert(dsi_display_auto_mode_update(&f.display) == -EINVAL);
        f.display.ctrl_count = 1;
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) == 1);
        assert(!dsi_display_auto_mode_update(&f.display) && notified == 2);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MODE_REQUEST(1)) == 1);
        f.info.cmd_sets[DSI_CMD_SET_QSYNC_ON].count = 0;
        assert(dsi_display_auto_mode_update(&f.display) == -EOPNOTSUPP);
        assert(f.panel.adfr.auto_mode_updated && !f.panel.adfr.mode_valid);
        puts("defer: AOD, absent commands and malformed payloads keep pending state");
        """)

    def test_new_request_during_io(self):
        self.run_policy(r"""
        struct fixture f;
        init_fixture(&f, 120);
        assert(oplus_adfr_handle_auto_mode(&f.panel,
            MODE_REQUEST(1) | OPLUS_ADFR_AUTO_MIN_FPS_MAGIC | 119) == 1);
        inject_request = MIN_REQUEST(1);
        assert(dsi_display_auto_mode_update(&f.display) == -EAGAIN);
        assert(f.panel.adfr.min_fps == 1 && f.panel.adfr.auto_mode_updated);
        assert(!notices);
        assert(!dsi_display_auto_mode_update(&f.display));
        assert(f.panel.adfr.applied_min_fps == 1);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) == 1);
        inject_request = MIN_REQUEST(3);
        assert(dsi_display_auto_mode_update(&f.display) == -EAGAIN);
        assert(f.panel.adfr.min_fps == 3 && f.panel.adfr.min_fps_updated);
        assert(notices == 1);
        assert(!dsi_display_auto_mode_update(&f.display));
        assert(f.panel.adfr.applied_min_fps == 3 && notified == 3);
        puts("generation: an in-flight update cannot clear a newer request");
        """)

    def test_idle_native_first_frame_and_retry(self):
        self.run_policy(r"""
        struct fixture f;
        struct sde_encoder_phys_cmd cmd = { 0 };
        int rate, previous;
        for (rate = 60; rate <= 120; rate += 60) {
            last_panel = NULL;
            init_fixture(&f, rate);
            assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) == 1);
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(f.panel.adfr.applied_min_fps == 2);
            assert(oplus_adfr_can_enter_idle(&f.panel.adfr));
            previous = delays;
            f.panel.power_mode = SDE_MODE_DPMS_LP1;
            assert(oplus_adfr_apply_idle(&f.display, &cmd, true) == -EAGAIN);
            assert(!f.panel.adfr.idle && !f.panel.adfr.restore_pending);
            assert(delays == previous);
            f.panel.power_mode = SDE_MODE_DPMS_ON;
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
            assert(f.panel.adfr.idle && sent_min == 119);
            previous = attempts;
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
            assert(attempts == previous);
            oplus_adfr_debug = OPLUS_ADFR_DEBUG_GLOBAL_DISABLE;
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, false));
            assert(!f.panel.adfr.idle && !f.panel.adfr.restore_pending);
            assert(sent_min == (rate == 60 ? 1U : 0U));
            assert(f.panel.adfr.min_fps == 119 && f.panel.adfr.min_fps_updated);
            oplus_adfr_debug = 0;
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(f.panel.adfr.applied_min_fps == 2);
            fail_at = attempts + 1;
            assert(oplus_adfr_apply_idle(&f.display, &cmd, true) == -EIO);
            assert(!f.panel.adfr.idle && f.panel.adfr.restore_pending);
            assert(!f.panel.adfr.min_fps_valid);
            fail_at = 0;
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, false));
            assert(!f.panel.adfr.restore_pending && !f.panel.adfr.idle);
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
            fail_at = attempts + 1;
            assert(oplus_adfr_apply_idle(&f.display, &cmd, false) == -EIO);
            assert(f.panel.adfr.idle && f.panel.adfr.restore_pending);
            fail_at = 0;
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, false));
            assert(!f.panel.adfr.idle);
        }
        assert(delays > 0);
        puts("idle: 60/120Hz, native first frame, failed entry/exit and recovery OK");
        """)

    def test_reset_filter_and_multiple_controllers(self):
        self.run_policy(r"""
        struct fixture f;
        init_fixture(&f, 120);
        assert(oplus_adfr_handle_auto_mode(&f.panel,
            MODE_REQUEST(1) | OPLUS_ADFR_AUTO_MIN_FPS_MAGIC | 119) == 1);
        dsi_panel_adfr_status_reset(&f.panel);
        assert(f.panel.adfr.min_fps == 119 && f.panel.adfr.auto_mode == 1);
        assert(f.panel.adfr.min_fps_updated && f.panel.adfr.auto_mode_updated);
        assert(dsi_display_auto_mode_update(&f.display) == -EAGAIN && !attempts);
        oplus_adfr_auto_on_cmd_filter_set(&f.panel, false);
        f.display.ctrl_count = 2;
        fail_at = 2;
        assert(dsi_display_auto_mode_update(&f.display) == -EIO);
        assert(!f.panel.adfr.mode_valid && f.panel.adfr.auto_mode_updated);
        fail_at = 0;
        assert(!dsi_display_auto_mode_update(&f.display));
        assert(f.panel.adfr.mode_valid && f.panel.adfr.min_fps_valid);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(11)) == 1);
        inject_reset = true;
        assert(dsi_display_auto_mode_update(&f.display) == -EAGAIN);
        assert(f.panel.adfr.min_fps_updated);
        assert(f.panel.adfr.min_fps == 11);
        puts("reset: requests preserved; auto-on deferred until TE; partial send retries");
        """)

    def test_te_zero_interval_and_nanoseconds(self):
        self.run_policy(r"""
        struct fixture f;
        unsigned int fps = 777;
        int i;
        assert(oplus_get_te_fps(NULL) == -EINVAL);
        assert(oplus_get_te_fps(&fps) == -EAGAIN && fps == 777);
        te_refcount.start_timeline = 100;
        te_refcount.end_timeline = 100 + 8333333;
        te_refcount.te_refcount = 1;
        assert(!oplus_get_te_fps(&fps) && fps == 120);
        te_refcount.end_timeline = 99;
        assert(oplus_get_te_fps(&fps) == -EAGAIN);
        init_fixture(&f, 120);
        oplus_adfr_dynamic_te.config = OPLUS_ADFR_DYNAMIC_TE_ENABLE;
        fake_now = NSEC_PER_SEC;
        assert(oplus_adfr_dynamic_te_handler(0, &f.display) == IRQ_HANDLED);
        assert(oplus_adfr_dynamic_te_handler(0, &f.display) == IRQ_HANDLED);
        for (i = 0; i < 4; i++) {
            fake_now += 8333333;
            assert(oplus_adfr_dynamic_te_handler(0, &f.display) == IRQ_HANDLED);
        }
        assert(oplus_adfr_dynamic_te.refresh_rate == 120);
        puts("TE: equal/reversed timestamps safe; 8.333ms measured as 120Hz");
        """)

    def test_atomic_and_kickoff_integration(self):
        connector = (DISPLAY / "msm/sde/sde_connector.c").read_text()
        setter = definition(connector, "static int sde_connector_atomic_set_property(")
        self.assertNotIn("oplus_adfr_handle_auto_mode(", setter)
        self.assertIn("oplus_adfr_validate_auto_mode(", setter)
        prepare = definition(connector, "void sde_connector_prepare_adfr(")
        self.assertIn("oplus_adfr_handle_auto_mode(", prepare)
        kms = (DISPLAY / "msm/sde/sde_kms.c").read_text()
        prepare_fence = definition(kms, "static void sde_kms_prepare_fence(")
        self.assertIn("for_each_new_connector_in_state(", prepare_fence)
        self.assertLess(prepare_fence.index("sde_connector_prepare_adfr("),
                        prepare_fence.index("sde_crtc_prepare_commit("))
        atomic = (DISPLAY / "msm/msm_atomic.c").read_text()
        commit = definition(atomic, "int msm_atomic_commit(")
        self.assertLess(commit.index("drm_atomic_helper_swap_state("),
                        commit.index("priv->kms->funcs->prepare_fence("))
        encoder = (DISPLAY / "msm/sde/sde_encoder.c").read_text()
        kickoff = definition(encoder, "void sde_encoder_kickoff(")
        self.assertLess(kickoff.index("oplus_adfr_handle_idle_mode("),
                        kickoff.index("_sde_encoder_kickoff_phys("))
        crtc = (DISPLAY / "msm/sde/sde_crtc.c").read_text()
        fps = definition(crtc, "static void sde_crtc_calc_fps(")
        self.assertIn("oplus_adfr_update_sw_fps(", fps)
        self.assertNotIn("oplus_adfr_send_min_fps_event(", fps)
        self.assertNotIn("static u32 oplus_adfr_auto_", HEADER.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
