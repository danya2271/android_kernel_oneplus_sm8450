#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual ADFR policy/DSI functions with host-side fault injection."""

import unittest

from test_hotpath import DISPLAY, definition, run_c


SOURCE = DISPLAY / "oplus/oplus_adfr.c"
HEADER = DISPLAY / "oplus/oplus_adfr.h"
KKNX_HEADER = DISPLAY.parents[1] / "include/linux/kknx_perf.h"

PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
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
struct kobject { int unused; };
struct kobj_attribute { int unused; };
#define OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define xchg(ptr, value) __sync_lock_test_and_set((ptr), (value))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000ULL
#define IRQ_HANDLED 1
#define HRTIMER_MODE_REL 0
#define SDE_MODE_DPMS_ON 0
#define SDE_MODE_DPMS_LP1 1
#define SDE_MODE_DPMS_OFF 5
#define SDE_ATRACE_BEGIN(...) do {} while (0)
#define SDE_ATRACE_END(...) do {} while (0)
#define SDE_ATRACE_INT(...) do {} while (0)
#define SDE_EVT32(...) do {} while (0)
#define SDE_ERROR(...) do {} while (0)
#define DSI_DEBUG(...) do {} while (0)
#define DSI_INFO(...) do {} while (0)
#define DSI_ERR(...) do {} while (0)
#define pr_err(...) do {} while (0)
#define pr_info_ratelimited(...) record_log(__VA_ARGS__)
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
#define INTF_MODE_CMD 1
#define DRM_MODE_CONNECTOR_DSI 16
#define CONNECTOR_PROP_HBM_ENABLE 0
#define OPLUS_OFP_PROPERTY_DIM_LAYER BIT(0)
#define OPLUS_OFP_PROPERTY_FINGERPRESS_LAYER BIT(1)
struct drm_connector { int connector_type; u64 *state; };
struct sde_connector {
    struct drm_connector base;
    int connector_type;
    struct dsi_display *display;
};
struct sde_encoder_phys { int intf_mode; struct drm_connector *connector; };
struct sde_encoder_virt { struct sde_encoder_phys *cur_master; };
#define to_sde_connector(conn) ((struct sde_connector *)(conn))
#define to_sde_encoder_phys_cmd(phys) ((struct sde_encoder_phys_cmd *)(phys))
static u64 sde_connector_get_property(u64 *state, int property) { return *state; }
void oplus_adfr_handle_idle_mode(void *sde_enc_v, int enter_idle);
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
static bool inject_touch;
static bool fixture_autonomous;
static u32 oplus_adfr_perf_level = 1;
static bool oplus_adfr_probe_60hz;
static bool global_hbm, ofp_supported, ofp_hbm;
static unsigned int log_count, log_hz, log_code;
static const char *log_owner, *log_action;
static struct dsi_panel *last_panel;
static struct dsi_display *current_display;
static struct dsi_display *oplus_display_get_current_display(void)
{ return current_display; }
static int oplus_display_panel_get_global_hbm_status(void) { return global_hbm; }
static bool oplus_ofp_is_supported(void) { return ofp_supported; }
static bool oplus_ofp_get_hbm_state(void) { return ofp_hbm; }
static void record_log(const char *fmt, ...)
{
    va_list args;
    assert(!spin_depth);
    assert(strstr(fmt, "kVRR"));
    va_start(args, fmt);
    log_owner = va_arg(args, const char *);
    log_action = va_arg(args, const char *);
    log_hz = va_arg(args, unsigned int);
    log_code = va_arg(args, unsigned int);
    log_count++;
    va_end(args);
}
static int kstrtou32(const char *text, unsigned int base, u32 *value)
{
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, base);
    if (errno || end == text || parsed > UINT32_MAX)
        return -EINVAL;
    if (*end == '\n')
        end++;
    if (*end)
        return -EINVAL;
    *value = parsed;
    return 0;
}
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
    if (inject_touch) {
        inject_touch = false;
        oplus_adfr_governor_touch(&panel->adfr, fake_now,
            panel->cur_mode->timing.refresh_rate);
    }
    return 0;
}

struct fixture {
    struct dsi_panel panel;
    struct dsi_display display;
    struct dsi_display_mode mode;
    struct dsi_display_mode_priv_info info;
    struct sde_connector connector;
    struct sde_encoder_phys phys;
    struct sde_encoder_virt encoder;
    u64 hbm_property;
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
    f->connector.base.connector_type = DRM_MODE_CONNECTOR_DSI;
    f->connector.connector_type = DRM_MODE_CONNECTOR_DSI;
    f->connector.base.state = &f->hbm_property;
    f->connector.display = &f->display;
    f->phys.intf_mode = INTF_MODE_CMD;
    f->phys.connector = &f->connector.base;
    f->encoder.cur_master = &f->phys;
    current_display = &f->display;
    for (i = 0; i < CMD_COUNT; i++) {
        f->info.cmd_sets[i].count = 3;
        f->info.cmd_sets[i].cmds = f->desc[i];
        for (j = 0; j < 3; j++) {
            f->desc[i][j].msg.tx_len = 2;
            f->desc[i][j].msg.tx_buf = f->payload[i][j];
        }
    }
    oplus_adfr_panel_init(&f->panel);
    f->panel.adfr.kernel_idle_enabled = fixture_autonomous;
    f->panel.adfr.kernel_active_min_fps = rate == 60 ? 1 : 0;
    f->panel.adfr.mode_valid = true;
    f->panel.adfr.min_fps_valid = true;
    f->panel.adfr.min_fps = rate == 60 ? 1 : 0;
    f->panel.adfr.applied_min_fps = f->panel.adfr.min_fps;
    f->panel.adfr.kernel_native_min_fps = rate == 60 ? 1 : 0;
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
    text += definition(KKNX_HEADER.read_text(),
                       "enum kknx_perf_load_level {") + ";\n"
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
        "static bool oplus_adfr_governor_enabled(",
        "static u32 oplus_adfr_rate_code(",
        "static u32 oplus_adfr_native_code(",
        "static void oplus_adfr_select_active(",
        "static void oplus_adfr_governor_touch(",
        "static void oplus_adfr_governor_frame(",
        "static u32 oplus_adfr_governor_idle_delay(",
        "ssize_t oplus_adfr_set_debug(",
        "void oplus_adfr_panel_init(",
        "static int oplus_adfr_set_kernel_idle_fps(",
        "ssize_t oplus_adfr_get_idle_fps(",
        "ssize_t oplus_adfr_set_idle_fps(",
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
        "static bool oplus_adfr_hbm_active(",
        "static struct dsi_display *oplus_adfr_encoder_display(",
        "void oplus_adfr_frame_start(",
        "void oplus_adfr_touch_event(",
        "void oplus_adfr_touch_work(",
        "u32 oplus_adfr_idle_delay_ms(",
        "bool oplus_adfr_is_kernel_idle(",
        "int oplus_adfr_get_current_refresh_rate(",
        "void oplus_adfr_kknx_policy_update(",
        "static u32 oplus_adfr_idle_target(",
        "static bool oplus_adfr_can_enter_idle(",
        "static int oplus_adfr_apply_idle(",
        "void oplus_adfr_handle_idle_mode(",
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
        struct dsi_panel fresh = {.type = "primary"};
        oplus_adfr_panel_init(&fresh);
        assert(fresh.adfr.kernel_idle_enabled && fresh.adfr.kernel_idle_min_fps == 11);
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
        f.panel.adfr.kernel_idle_enabled = false;
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

    def test_autonomous_idle_without_hwc_request(self):
        self.run_policy(r"""
        struct fixture f;
        struct sde_encoder_phys_cmd cmd = { 0 };
        int rate, before;
        fixture_autonomous = true;
        for (rate = 60; rate <= 120; rate += 60) {
            init_fixture(&f, rate);
            dsi_panel_adfr_status_reset(&f.panel);
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(!f.panel.adfr.has_request && !oplus_adfr_auto_update_counter);
            assert(f.panel.adfr.kernel_idle_enabled);
            before = attempts;
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
            assert(attempts == before + 1 && sent_min == 11);
            assert(f.panel.adfr.idle && f.panel.adfr.applied_min_fps == 11);
            assert(f.panel.adfr.min_fps == (rate == 60 ? 1U : 0U));
            assert(!strcmp(log_owner, "kernel") && !strcmp(log_action, "enter"));
            assert(log_hz == 10 && log_code == 11);
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, false));
            assert(sent_min == (rate == 60 ? 1U : 0U));
            assert(log_hz == (u32)rate && !strcmp(log_action, "exit"));
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(!f.panel.adfr.has_request && !oplus_adfr_auto_update_counter);
        }
        puts("autonomous: real reset -> idle 10Hz -> native FPS without any HWC request");
        """)

    def test_autonomous_target_sysfs_and_hwc_fallback(self):
        self.run_policy(r"""
        struct fixture f;
        struct sde_encoder_phys_cmd cmd = { 0 };
        char status[512];
        u64 sequence;
        int before;
        fixture_autonomous = true;
        init_fixture(&f, 120);
        assert(oplus_adfr_set_idle_fps(NULL, NULL, "10\n", 3) == 3);
        assert(f.panel.adfr.kernel_idle_min_fps == 11);
        sequence = f.panel.adfr.sequence;
        assert(oplus_adfr_set_idle_fps(NULL, NULL, "10", 2) == 2);
        assert(f.panel.adfr.sequence == sequence);
        assert(oplus_adfr_set_idle_fps(NULL, NULL, "5", 1) == -EINVAL);
        assert(oplus_adfr_set_idle_fps(NULL, NULL, "bad", 3) == -EINVAL);
        assert(f.panel.adfr.sequence == sequence && !attempts);
        assert(!dsi_display_auto_mode_update(&f.display));
        assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
        assert(sent_min == 11 && log_hz == 10 && !f.panel.adfr.has_request);
        assert(oplus_adfr_get_idle_fps(NULL, NULL, status) > 0);
        assert(strstr(status, "kernel=1 target_hz=10"));
        assert(strstr(status, "requested_code=0x00 applied_code=0x0b"));
        assert(strstr(status, "valid=1 idle=1"));
        assert(oplus_adfr_set_idle_fps(NULL, NULL, "0", 1) == 1);
        assert(!oplus_adfr_apply_idle(&f.display, &cmd, false));
        assert(!dsi_display_auto_mode_update(&f.display));
        before = attempts;
        assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
        assert(attempts == before && !f.panel.adfr.idle);
        assert(oplus_adfr_handle_auto_mode(&f.panel, MIN_REQUEST(119)) == 1);
        assert(!dsi_display_auto_mode_update(&f.display));
        assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
        assert(sent_min == 119 && !strcmp(log_owner, "hwc"));
        current_display = NULL;
        assert(oplus_adfr_get_idle_fps(NULL, NULL, status) == -ENODEV);
        assert(oplus_adfr_set_idle_fps(NULL, NULL, "1", 1) == -ENODEV);
        puts("sysfs: independent 10Hz target, truthful status, opt-out and HWC fallback");
        """)

    def test_autonomous_hbm_and_optical_fingerprint_guard(self):
        self.run_policy(r"""
        struct fixture f;
        struct sde_encoder_phys_cmd cmd = { 0 };
        fixture_autonomous = true;
        init_fixture(&f, 120);
        global_hbm = true;
        assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
        assert(!attempts && !f.panel.adfr.idle);
        global_hbm = false;
        ofp_supported = true;
        ofp_hbm = true;
        assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
        assert(!attempts && !f.panel.adfr.idle);
        ofp_hbm = false;
        assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
        assert(sent_min == 11 && f.panel.adfr.idle);
        ofp_hbm = true;
        assert(!oplus_adfr_apply_idle(&f.display, &cmd, false));
        assert(sent_min == 0 && !f.panel.adfr.idle);
        puts("HBM: autonomous entry blocked, native-FPS recovery remains allowed");
        """)

    def test_governor_cadence_ladder_and_immediate_upshift(self):
        self.run_policy(r"""
        struct fixture f;
        unsigned int rates[] = {10, 20, 30, 40, 60, 120};
        unsigned int codes[] = {11, 5, 3, 2, 1, 0};
        unsigned int r, i;
        u64 now, sequence;
        fixture_autonomous = true;
        assert(oplus_adfr_rate_code(1) == 119);
        assert(oplus_adfr_rate_code(50) == 1);
        for (r = 0; r < 6; r++) {
            init_fixture(&f, 120);
            now = NSEC_PER_SEC;
            oplus_adfr_governor_frame(&f.panel.adfr, now, 120, false);
            for (i = 0; i < rates[r] * 2; i++) {
                now += NSEC_PER_SEC / rates[r];
                oplus_adfr_governor_frame(&f.panel.adfr, now, 120, false);
                assert(!dsi_display_auto_mode_update(&f.display));
            }
            assert(f.panel.adfr.kernel_active_min_fps == codes[r]);
            assert(f.panel.adfr.applied_min_fps == codes[r]);
            assert(f.panel.adfr.cadence_fps == rates[r]);
            assert(!f.panel.adfr.min_fps && !f.panel.adfr.has_request);
            assert(!f.panel.adfr.fakeframe || rates[r] == 120);
            sequence = f.panel.adfr.sequence;
            oplus_adfr_governor_frame(&f.panel.adfr, now + 8333333, 120, false);
            assert(!f.panel.adfr.kernel_active_min_fps);
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(!f.panel.adfr.applied_min_fps);
            assert(f.panel.adfr.sequence == sequence + (rates[r] != 120));
        }
        assert(!oplus_adfr_auto_update_counter);
        puts("cadence: 10/20/30/40/60/120Hz converge; next fast frame upshifts immediately");
        """)

    def test_governor_touch_hysteresis_and_idle_hold(self):
        self.run_policy(r"""
        struct fixture f;
        unsigned int i;
        int before;
        u64 now = NSEC_PER_SEC, sequence;
        fixture_autonomous = true;
        init_fixture(&f, 120);
        oplus_adfr_governor_frame(&f.panel.adfr, now, 120, false);
        for (i = 1; i <= 7; i++)
            oplus_adfr_governor_frame(&f.panel.adfr,
                now + i * 33333333ULL, 120, false);
        assert(!f.panel.adfr.kernel_active_min_fps);
        for (i = 8; i <= 12; i++)
            oplus_adfr_governor_frame(&f.panel.adfr,
                now + i * 33333333ULL, 120, false);
        now += 12 * 33333333ULL;
        assert(f.panel.adfr.kernel_active_min_fps == 3);
        assert(!dsi_display_auto_mode_update(&f.display));
        before = attempts;
        oplus_adfr_governor_touch(&f.panel.adfr, now, 120);
        assert(!f.panel.adfr.kernel_active_min_fps);
        assert(attempts == before); /* IRQ path cannot send DSI. */
        assert(oplus_adfr_governor_idle_delay(&f.panel.adfr, now) == 250);
        assert(oplus_adfr_governor_idle_delay(&f.panel.adfr,
            now + 249 * NSEC_PER_MSEC + 1) == 1);
        assert(!oplus_adfr_governor_idle_delay(&f.panel.adfr,
            now + 250 * NSEC_PER_MSEC));
        assert(!dsi_display_auto_mode_update(&f.display));
        assert(!f.panel.adfr.applied_min_fps);
        sequence = f.panel.adfr.sequence;
        oplus_adfr_governor_touch(&f.panel.adfr, now + NSEC_PER_MSEC, 120);
        assert(f.panel.adfr.sequence == sequence);
        assert(!dsi_display_auto_mode_update(&f.display));
        assert(attempts == before + 1); /* No repeated native command per touch. */
        now += NSEC_PER_MSEC;
        for (i = 1; i <= 7; i++) {
            oplus_adfr_governor_frame(&f.panel.adfr,
                now + i * 33333333ULL, 120, false);
            assert(!f.panel.adfr.kernel_active_min_fps);
        }
        oplus_adfr_governor_frame(&f.panel.adfr,
            now + 2 * NSEC_PER_SEC, 120, false);
        assert(!f.panel.adfr.frame_period_ns &&
               !f.panel.adfr.kernel_active_min_fps);
        f.panel.adfr.frame_period_ns = NSEC_PER_SEC;
        f.panel.adfr.last_frame_ns = now;
        assert(oplus_adfr_governor_idle_delay(&f.panel.adfr, now) == 250);
        puts("touch: immediate native selection, 250ms boost, bounded idle hold, no redundant DSI");
        """)

    def test_governor_floor_hbm_and_legacy_optout(self):
        self.run_policy(r"""
        struct fixture f;
        struct sde_encoder_phys_cmd cmd = {0};
        unsigned int floors[] = {1, 10, 20, 30, 40, 60};
        unsigned int codes[] = {119, 11, 5, 3, 2, 1};
        unsigned int i, j;
        u64 now;
        char status[512];
        fixture_autonomous = true;
        for (i = 0; i < 6; i++) {
            init_fixture(&f, 60);
            assert(!oplus_adfr_set_kernel_idle_fps(&f.panel, floors[i]));
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, true));
            assert(sent_min == codes[i]);
            assert(!oplus_adfr_apply_idle(&f.display, &cmd, false));
            assert(sent_min == 1);
            now = NSEC_PER_SEC;
            for (j = 0; j < 20; j++) {
                now += NSEC_PER_SEC / 10;
                oplus_adfr_governor_frame(&f.panel.adfr, now, 60, false);
            }
            assert(f.panel.adfr.kernel_active_min_fps ==
                (codes[i] > 11 ? 11U : codes[i]));
            oplus_adfr_governor_frame(&f.panel.adfr, now + 8333333, 60, true);
            assert(f.panel.adfr.kernel_active_min_fps == 1);
            assert(!dsi_display_auto_mode_update(&f.display));
            assert(f.panel.adfr.applied_min_fps == 1);
        }
        assert(oplus_adfr_set_kernel_idle_fps(&f.panel, 50) == -EINVAL);
        oplus_adfr_governor_touch(&f.panel.adfr, now, 60);
        fake_now = now;
        assert(oplus_adfr_get_idle_fps(NULL, NULL, status) > 0);
        assert(strstr(status, "active_hz=60") && strstr(status, "boost_ms=250"));
        f.panel.adfr.auto_mode = OPLUS_ADFR_AUTO_ON;
        f.panel.adfr.kernel_active_min_fps = 11;
        oplus_adfr_governor_touch(&f.panel.adfr, now, 60);
        oplus_adfr_governor_frame(&f.panel.adfr, now, 60, true);
        assert(f.panel.adfr.kernel_active_min_fps == 11);
        f.panel.adfr.auto_mode = OPLUS_ADFR_AUTO_OFF;
        oplus_adfr_debug = OPLUS_ADFR_DEBUG_IDLE_MODE_DISABLE;
        oplus_adfr_governor_touch(&f.panel.adfr, now, 60);
        assert(f.panel.adfr.kernel_active_min_fps == 11);
        assert(!oplus_adfr_governor_idle_delay(&f.panel.adfr, now));
        puts("policy: all floors, 60Hz native cap, HBM native, explicit auto-on/debug opt-out");
        """)

    def test_governor_encoder_touch_idle_and_hbm_integration(self):
        self.run_policy(r"""
        struct fixture f;
        unsigned int i;
        int before;
        fixture_autonomous = true;
        init_fixture(&f, 120);
        dsi_panel_adfr_status_reset(&f.panel);
        fake_now = NSEC_PER_SEC;
        oplus_adfr_frame_start(&f.encoder);
        for (i = 0; i < 60; i++) {
            fake_now += 33333333;
            oplus_adfr_frame_start(&f.encoder);
        }
        assert(f.panel.adfr.applied_min_fps == 3);
        assert(!f.panel.adfr.has_request && !oplus_adfr_auto_update_counter);
        assert(attempts == 1); /* One rate change, no per-frame DSI traffic. */
        assert(!oplus_adfr_is_kernel_idle(&f.encoder));
        oplus_adfr_handle_idle_mode(&f.encoder, true);
        assert(!f.panel.adfr.idle && oplus_adfr_idle_delay_ms(&f.encoder) == 67);
        fake_now += 251 * NSEC_PER_MSEC;
        oplus_adfr_handle_idle_mode(&f.encoder, true);
        assert(f.panel.adfr.idle && sent_min == 11);
        assert(oplus_adfr_is_kernel_idle(&f.encoder));
        before = attempts;
        oplus_adfr_touch_event(&f.encoder);
        assert(attempts == before && !f.panel.adfr.kernel_active_min_fps);
        assert(oplus_adfr_idle_delay_ms(&f.encoder) == 250);
        oplus_adfr_touch_work(&f.encoder);
        assert(attempts == before + 1 && !sent_min && !f.panel.adfr.idle);
        assert(!f.panel.adfr.min_fps_updated && f.panel.adfr.min_fps_valid);
        oplus_adfr_touch_event(&f.encoder);
        oplus_adfr_touch_work(&f.encoder);
        assert(attempts == before + 1);
        fake_now += 300 * NSEC_PER_MSEC;
        oplus_adfr_handle_idle_mode(&f.encoder, true);
        assert(f.panel.adfr.idle && sent_min == 11);
        fake_now += NSEC_PER_SEC;
        oplus_adfr_frame_start(&f.encoder);
        oplus_adfr_handle_idle_mode(&f.encoder, false);
        assert(!sent_min && !f.panel.adfr.idle);
        assert(!oplus_adfr_is_kernel_idle(&f.encoder));
        for (i = 0; i < 60; i++) {
            fake_now += 33333333;
            oplus_adfr_frame_start(&f.encoder);
        }
        assert(sent_min == 3);
        f.hbm_property = OPLUS_OFP_PROPERTY_FINGERPRESS_LAYER;
        fake_now += 33333333;
        oplus_adfr_frame_start(&f.encoder);
        assert(!sent_min && !f.panel.adfr.kernel_active_min_fps);
        before = attempts;
        f.panel.power_mode = SDE_MODE_DPMS_LP1;
        oplus_adfr_touch_event(&f.encoder);
        oplus_adfr_touch_work(&f.encoder);
        oplus_adfr_frame_start(&f.encoder);
        assert(attempts == before && !oplus_adfr_idle_delay_ms(&f.encoder));
        f.panel.power_mode = SDE_MODE_DPMS_ON;
        f.mode.timing.refresh_rate = 90;
        oplus_adfr_touch_event(&f.encoder);
        oplus_adfr_touch_work(&f.encoder);
        oplus_adfr_frame_start(&f.encoder);
        assert(attempts == before && !oplus_adfr_idle_delay_ms(&f.encoder));
        puts("encoder: real frame/touch/idle paths, one transfer per change, FOD and AOD guards");
        """)

    def test_governor_touch_racing_idle_and_failed_boost(self):
        self.run_policy(r"""
        struct fixture f;
        int before;
        fixture_autonomous = true;
        init_fixture(&f, 120);
        fake_now = NSEC_PER_SEC;
        inject_touch = true;
        oplus_adfr_handle_idle_mode(&f.encoder, true);
        assert(f.panel.adfr.idle && sent_min == 11);
        assert(oplus_adfr_idle_delay_ms(&f.encoder) == 250);
        fail_at = attempts + 1;
        oplus_adfr_touch_work(&f.encoder);
        assert(f.panel.adfr.restore_pending && !f.panel.adfr.min_fps_valid);
        assert(f.panel.adfr.last_error == -EIO && sent_min == 11);
        fail_at = 0;
        oplus_adfr_touch_work(&f.encoder);
        assert(!sent_min && !f.panel.adfr.idle && !f.panel.adfr.restore_pending);
        assert(f.panel.adfr.min_fps_valid && !f.panel.adfr.last_error);
        before = attempts;
        oplus_adfr_handle_idle_mode(&f.encoder, true);
        assert(attempts == before); /* Boost prevents re-entry while clocks stay on. */
        f.panel.adfr.kernel_active_min_fps = 11;
        f.panel.adfr.min_fps_updated = true;
        assert(!dsi_display_auto_mode_update(&f.display) && sent_min == 11);
        oplus_adfr_touch_event(&f.encoder);
        fail_at = attempts + 1;
        oplus_adfr_touch_work(&f.encoder);
        assert(f.panel.adfr.min_fps_updated && !f.panel.adfr.min_fps_valid);
        fail_at = 0;
        oplus_adfr_frame_start(&f.encoder);
        assert(!sent_min && f.panel.adfr.min_fps_valid);
        puts("recovery: racing touch defers collapse; failed idle/active boost retries native");
        """)

    def test_governor_debug_disable_restores_active_and_idle(self):
        self.run_policy(r"""
        struct fixture f;
        fixture_autonomous = true;
        init_fixture(&f, 120);
        f.panel.adfr.kernel_active_min_fps = 11;
        f.panel.adfr.min_fps_updated = true;
        assert(!dsi_display_auto_mode_update(&f.display) && sent_min == 11);
        assert(oplus_adfr_set_debug(NULL, NULL, "1", 1) == 1);
        assert(f.panel.adfr.restore_pending && f.panel.adfr.min_fps_updated);
        oplus_adfr_frame_start(&f.encoder);
        oplus_adfr_handle_idle_mode(&f.encoder, false);
        assert(!sent_min && !f.panel.adfr.restore_pending);
        assert(oplus_adfr_set_debug(NULL, NULL, "0", 1) == 1);
        assert(!dsi_display_auto_mode_update(&f.display));
        oplus_adfr_handle_idle_mode(&f.encoder, true);
        assert(sent_min == 11 && f.panel.adfr.idle);
        assert(oplus_adfr_set_debug(NULL, NULL, "16", 2) == 2);
        oplus_adfr_handle_idle_mode(&f.encoder, false);
        assert(!sent_min && !f.panel.adfr.idle && !f.panel.adfr.restore_pending);
        puts("debug: global/idle policy disable restores native from active or idle low rates");
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
        self.assertLess(kickoff.index("oplus_adfr_frame_start("),
                        kickoff.index("oplus_adfr_handle_idle_mode("))
        self.assertLess(kickoff.index("oplus_adfr_handle_idle_mode("),
                        kickoff.index("_sde_encoder_kickoff_phys("))
        idle = definition(encoder, "static int _sde_encoder_rc_idle(")
        self.assertLess(idle.index("sde_crtc_frame_pending("),
                        idle.index("oplus_adfr_handle_idle_mode("))
        self.assertLess(idle.index("mutex_lock(&sde_enc->rc_lock)"),
                        idle.index("oplus_adfr_handle_idle_mode("))
        self.assertLess(idle.index("oplus_adfr_handle_idle_mode("),
                        idle.index("_sde_encoder_resource_control_helper("))
        self.assertEqual(idle.count("oplus_adfr_idle_delay_ms("), 2)
        self.assertLess(idle.index("oplus_adfr_handle_idle_mode("),
                        idle.index("oplus_adfr_is_kernel_idle("))
        self.assertLess(idle.index("oplus_adfr_is_kernel_idle("),
                        idle.index("sde_core_perf_crtc_set_idle_bw("))
        self.assertLess(idle.index("sde_core_perf_crtc_set_idle_bw("),
                        idle.index("_sde_encoder_resource_control_helper("))
        early_wakeup = definition(encoder, "static int _sde_encoder_rc_early_wakeup(")
        self.assertLess(early_wakeup.index("_sde_encoder_resource_control_helper("),
                        early_wakeup.index("sde_core_perf_crtc_set_idle_bw("))
        perf = (DISPLAY / "msm/sde/sde_core_perf.c").read_text()
        update_bus = definition(perf, "static void _sde_core_perf_crtc_update_bus(")
        self.assertIn("if (sde_crtc->bw_vote_idle)", update_bus)
        idle_vote = definition(perf, "int sde_core_perf_crtc_set_idle_bw(")
        self.assertIn("mutex_lock(&sde_core_perf_lock)", idle_vote)
        self.assertIn("_sde_core_perf_crtc_set_idle_bw_locked(kms, crtc, idle)",
                      idle_vote)
        idle_update = definition(perf,
            "static void _sde_core_perf_crtc_set_idle_bw_locked(")
        self.assertIn("_sde_core_perf_crtc_update_bus(kms, crtc, i)",
                      idle_update)
        perf_update = definition(perf, "void sde_core_perf_crtc_update(")
        self.assertIn("params_changed && sde_crtc->bw_vote_idle", perf_update)
        self.assertIn("sde_crtc->bw_vote_idle = false", perf_update)
        llcc = definition(perf, "static void _sde_core_perf_crtc_set_llcc_cache_type(")
        self.assertIn("if (!sde_crtc->bw_vote_idle)", llcc)
        self.assertIn("kms->perf.perf_tune.min_bus_vote", update_bus)
        crtc = (DISPLAY / "msm/sde/sde_crtc.c").read_text()
        fps = definition(crtc, "static void sde_crtc_calc_fps(")
        self.assertIn("oplus_adfr_update_sw_fps(", fps)
        self.assertNotIn("oplus_adfr_send_min_fps_event(", fps)
        self.assertNotIn("static u32 oplus_adfr_auto_", HEADER.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
