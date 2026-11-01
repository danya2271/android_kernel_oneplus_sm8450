/***************************************************************
** Copyright (C), 2022, OPLUS Mobile Comm Corp., Ltd
** File : oplus_adfr.h
** Description : ADFR kernel module
** Version : 1.0
** Date : 2022/08/01
** Author : Display
******************************************************************/
#include "sde_trace.h"
#include "msm_drv.h"
#include "sde_kms.h"
#include "sde_connector.h"
#include "sde_crtc.h"
#include "sde_encoder_phys.h"
#include <linux/ratelimit.h>
#include <uapi/linux/sched/types.h>

#include "dsi_display.h"
#include "dsi_panel.h"
#include "dsi_parser.h"
#include "dsi_drm.h"
#include "dsi_defs.h"

#include "oplus_adfr.h"
#include "oplus_bl.h"
#include "oplus_dsi_support.h"
#ifdef OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT
#include "oplus_onscreenfingerprint.h"
#endif

#define OPLUS_ADFR_CONFIG_GLOBAL (1<<0)
#define OPLUS_ADFR_CONFIG_FAKEFRAME (1<<1)
#define OPLUS_ADFR_CONFIG_VSYNC_SWITCH (1<<2)
#define OPLUS_ADFR_CONFIG_VSYNC_SWITCH_MODE (1<<3)
#define OPLUS_ADFR_CONFIG_IDLE_MODE (1<<4)
#define OPLUS_ADFR_CONFIG_TEMPERATURE_DETECTION (1<<5)

#define OPLUS_ADFR_DEBUG_GLOBAL_DISABLE (1<<0)
#define OPLUS_ADFR_DEBUG_FAKEFRAME_DISABLE (1<<1)
#define OPLUS_ADFR_DEBUG_VSYNC_SWITCH_DISABLE (1<<2)
#define OPLUS_ADFR_DEBUG_IDLE_MODE_DISABLE (1<<4)
#define OPLUS_ADFR_DEBUG_TEMPERATURE_DETECTION_DISABLE (1<<5)

#define ADFR_GET_GLOBAL_CONFIG(config) ((config) & OPLUS_ADFR_CONFIG_GLOBAL)
#define ADFR_GET_FAKEFRAME_CONFIG(config) ((config) & OPLUS_ADFR_CONFIG_FAKEFRAME)
#define ADFR_GET_VSYNC_SWITCH_CONFIG(config) ((config) & OPLUS_ADFR_CONFIG_VSYNC_SWITCH)
#define ADFR_GET_VSYNC_SWITCH_MODE(config) ((config) & OPLUS_ADFR_CONFIG_VSYNC_SWITCH_MODE)
#define ADFR_GET_IDLE_MODE_CONFIG(config) ((config) & OPLUS_ADFR_CONFIG_IDLE_MODE)
#define ADFR_GET_TEMPERATURE_DETECTION_CONFIG(config) ((config) & OPLUS_ADFR_CONFIG_TEMPERATURE_DETECTION)

#define OPLUS_ADFR_AUTO_MAGIC 0X00800000
#define OPLUS_ADFR_AUTO_MODE_MAGIC 0X00400000
#define OPLUS_ADFR_AUTO_MODE_VALUE(auto_value) (((auto_value)&0X003F0000)>>16)
#define OPLUS_ADFR_AUTO_FAKEFRAME_MAGIC 0X00008000
#define OPLUS_ADFR_AUTO_FAKEFRAME_VALUE(auto_value) (((auto_value)&0X00007F00)>>8)
#define OPLUS_ADFR_AUTO_MIN_FPS_MAGIC 0X00000080
#define OPLUS_ADFR_AUTO_MIN_FPS_VALUE(auto_value) ((auto_value)&0X0000007F)

#define SDC_AUTO_MIN_FPS_CMD_OFFSET 2
#define SDC_MANUAL_MIN_FPS_CMD_OFFSET 2
#define SDC_MIN_FPS_CMD_SIZE 2
#define ADFR_TOUCH_BOOST_NS (250ULL * NSEC_PER_MSEC)
#define ADFR_DOWNSHIFT_NS (250ULL * NSEC_PER_MSEC)
#define ADFR_IDLE_HOLD_MAX_NS (250ULL * NSEC_PER_MSEC)
#define ADFR_CADENCE_RESET_NS (500ULL * NSEC_PER_MSEC)

#define to_dsi_bridge(x)  container_of((x), struct dsi_bridge, base)
#define to_sde_encoder_phys_cmd(x) container_of(x, struct sde_encoder_phys_cmd, base)
#define to_dsi_display(x) container_of(x, struct dsi_display, host)

static u32 oplus_adfr_config = 0;
static u32 oplus_adfr_debug = 0;
static bool need_deferred_fakeframe = false;
/* add for adfr hardware revision compatibility */
static bool oplus_adfr_compatibility_mode = false;

/* qsync mode minfps */
bool oplus_adfr_qsync_mode_minfps_updated = false;
static u32 oplus_adfr_qsync_mode_minfps = 0;
/* disable qsync when backlight updated */
bool oplus_adfr_need_filter_backlight_cmd = false;

/* Samsung auto-mode state is owned by each panel. */
static u64 oplus_adfr_auto_update_counter = 0;
/* Panel extension code for a 40 Hz active floor, not a value in Hz. */
static const u32 oplus_adfr_exit_idle_minfps = OPLUS_ADFR_AUTO_MIN_FPS_40HZ;
static bool oplus_adfr_skip_min_fps_cmd = false;

struct oplus_te_refcount te_refcount = {0, 0, 0, 0};
/* dynamic te detect */
struct oplus_adfr_dynamic_te oplus_adfr_dynamic_te = {0};
extern int oplus_dsi_log_type;
DEFINE_MUTEX(dynamic_te_lock);

/* --------------- adfr misc ---------------*/

static struct dsi_panel *
oplus_adfr_panel_from_connector(struct drm_connector *connector)
{
	struct sde_connector *c_conn;
	struct dsi_display *display;

	if (!connector || connector->connector_type != DRM_MODE_CONNECTOR_DSI)
		return NULL;

	c_conn = to_sde_connector(connector);
	display = c_conn->display;
	return display ? display->panel : NULL;
}

static bool oplus_adfr_panel_supported(struct dsi_panel *panel)
{
	return panel && ADFR_GET_GLOBAL_CONFIG(panel->adfr.config) &&
	       oplus_adfr_is_support();
}

static bool oplus_adfr_governor_enabled(const struct oplus_adfr_state *adfr)
{
	return adfr->kernel_idle_enabled &&
	       adfr->auto_mode == OPLUS_ADFR_AUTO_OFF &&
	       oplus_adfr_idle_mode_is_enable();
}

static u32 oplus_adfr_rate_code(u32 fps)
{
	if (fps <= 1)
		return OPLUS_ADFR_AUTO_MIN_FPS_1HZ;
	if (fps <= 10)
		return OPLUS_ADFR_AUTO_MIN_FPS_10HZ;
	if (fps <= 20)
		return OPLUS_ADFR_AUTO_MIN_FPS_20HZ;
	if (fps <= 30)
		return OPLUS_ADFR_AUTO_MIN_FPS_30HZ;
	if (fps <= 40)
		return OPLUS_ADFR_AUTO_MIN_FPS_40HZ;
	/* 50 Hz has no extension code in this command family: use 60 Hz. */
	if (fps <= 60)
		return OPLUS_ADFR_AUTO_MIN_FPS_60HZ;
	return OPLUS_ADFR_AUTO_MIN_FPS_MAX;
}

static u32 oplus_adfr_native_code(u32 refresh_rate)
{
	return refresh_rate == 60 ? OPLUS_ADFR_AUTO_MIN_FPS_60HZ :
				   OPLUS_ADFR_AUTO_MIN_FPS_MAX;
}

/* Called under adfr->lock; smaller extension codes mean faster refresh. */
static void oplus_adfr_select_active(struct oplus_adfr_state *adfr, u32 code)
{
	if (adfr->kernel_active_min_fps == code)
		return;
	adfr->kernel_active_min_fps = code;
	adfr->min_fps_updated = true;
	adfr->fakeframe_updated = true;
	adfr->sequence++;
}

static void oplus_adfr_governor_touch(struct oplus_adfr_state *adfr, u64 now,
				      u32 refresh_rate)
{
	u32 native = oplus_adfr_native_code(refresh_rate);

	if (!oplus_adfr_governor_enabled(adfr))
		return;
	adfr->boost_until_ns = now + ADFR_TOUCH_BOOST_NS;
	adfr->kernel_down_candidate = native;
	adfr->down_since_ns = now;
	oplus_adfr_select_active(adfr, native);
}

static void oplus_adfr_governor_frame(struct oplus_adfr_state *adfr, u64 now,
				      u32 refresh_rate, bool require_native)
{
	u32 native = oplus_adfr_native_code(refresh_rate);
	u32 code, instant;
	u64 delta;

	if (!oplus_adfr_governor_enabled(adfr))
		return;
	if (!adfr->last_frame_ns || now <= adfr->last_frame_ns ||
	    now - adfr->last_frame_ns > ADFR_CADENCE_RESET_NS) {
		adfr->last_frame_ns = now;
		adfr->frame_period_ns = 0;
		adfr->cadence_fps = refresh_rate;
		adfr->kernel_down_candidate = native;
		adfr->down_since_ns = now;
		oplus_adfr_select_active(adfr, native);
		return;
	}
	delta = now - adfr->last_frame_ns;
	adfr->last_frame_ns = now;
	if (!adfr->frame_period_ns)
		adfr->frame_period_ns = delta;
	else
		adfr->frame_period_ns =
			(adfr->frame_period_ns * 3 + delta) / 4;
	adfr->cadence_fps = div64_u64(NSEC_PER_SEC + adfr->frame_period_ns / 2,
				      adfr->frame_period_ns);
	code = oplus_adfr_rate_code(adfr->cadence_fps);
	instant = oplus_adfr_rate_code(div64_u64(NSEC_PER_SEC + delta / 2, delta));
	if (instant < code)
		code = instant;
	/* The idle target is also the lowest allowed autonomous active rate. */
	if (code > adfr->kernel_idle_min_fps)
		code = adfr->kernel_idle_min_fps;
	if (code < native)
		code = native;
	if (require_native || now < adfr->boost_until_ns) {
		adfr->kernel_down_candidate = native;
		adfr->down_since_ns = now;
		oplus_adfr_select_active(adfr, native);
		return;
	}
	if (code <= adfr->kernel_active_min_fps) {
		adfr->kernel_down_candidate = code;
		adfr->down_since_ns = now;
		oplus_adfr_select_active(adfr, code);
	} else if (code != adfr->kernel_down_candidate) {
		adfr->kernel_down_candidate = code;
		adfr->down_since_ns = now;
	} else if (now - adfr->down_since_ns >= ADFR_DOWNSHIFT_NS) {
		oplus_adfr_select_active(adfr, code);
	}
}

static u32 oplus_adfr_governor_idle_delay(const struct oplus_adfr_state *adfr,
					  u64 now)
{
	u64 deadline, hold;

	if (!oplus_adfr_governor_enabled(adfr))
		return 0;
	hold = adfr->frame_period_ns * 2;
	if (hold > ADFR_IDLE_HOLD_MAX_NS)
		hold = ADFR_IDLE_HOLD_MAX_NS;
	deadline = adfr->last_frame_ns + hold;
	if (deadline < adfr->boost_until_ns)
		deadline = adfr->boost_until_ns;
	if (now >= deadline)
		return 0;
	return div64_u64(deadline - now + NSEC_PER_MSEC - 1, NSEC_PER_MSEC);
}

void oplus_adfr_panel_init(void *dsi_panel)
{
	struct dsi_panel *panel = dsi_panel;
	struct oplus_adfr_state *adfr = &panel->adfr;

	spin_lock_init(&adfr->lock);
	mutex_init(&adfr->update_lock);
	adfr->fakeframe_requested = true;
	adfr->kernel_idle_min_fps = OPLUS_ADFR_AUTO_MIN_FPS_1HZ;
	if (panel->type && !strcmp(panel->type, "primary"))
		of_property_read_u32(panel->panel_of_node, "oplus,adfr-config",
				     &adfr->config);
	adfr->kernel_idle_enabled = ADFR_GET_GLOBAL_CONFIG(adfr->config) &&
		ADFR_GET_IDLE_MODE_CONFIG(adfr->config);
}

void oplus_adfr_panel_deinit(void *dsi_panel)
{
	struct dsi_panel *panel = dsi_panel;

	if (ADFR_GET_GLOBAL_CONFIG(panel->adfr.config))
		hrtimer_cancel(&oplus_adfr_dynamic_te.timer);
}

void oplus_adfr_init(void *panel_node)
{
	static bool inited = false;
	u32 config = 0;
	int rc = 0;
	struct device_node *of_node = panel_node;

	pr_info("kVRR oplus_adfr_init now.");

	if (!of_node) {
		pr_err("kVRR oplus_adfr_init: the param is null.");
		return;
	}

	if (inited) {
		pr_warn("kVRR adfr config = %#X already!", oplus_adfr_config);
		return;
	}

	rc = of_property_read_u32(of_node, "oplus,adfr-config", &config);
	if (rc == 0) {
		oplus_adfr_config = config;
	} else {
		oplus_adfr_config = 0;
	}

	/* add for adfr hardware revision compatibility */
	if (oplus_adfr_is_support()) {
		/* if adfr-compatibility-mode is define, should not do the vsync switch, just set to TE vsync always */
		oplus_adfr_compatibility_mode = of_property_read_bool(of_node, "oplus,adfr-compatibility-mode");
		/* add for dynamic te check */
		hrtimer_init(&oplus_adfr_dynamic_te.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		oplus_adfr_dynamic_te.timer.function = oplus_adfr_dynamic_te_timer_handler;
	}

	inited = true;

	pr_info("kVRR adfr config = %#X, adfr compatibility mode = %d\n", oplus_adfr_config, oplus_adfr_compatibility_mode);
}

ssize_t oplus_adfr_get_debug(struct kobject *obj,
	struct kobj_attribute *attr, char *buf)
{
	struct dsi_display *display = oplus_display_get_current_display();
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	u32 mode = 0, fakeframe = 0, min_fps = 0;

	if (display && display->panel) {
		adfr = &display->panel->adfr;
		spin_lock_irqsave(&adfr->lock, flags);
		mode = adfr->auto_mode;
		fakeframe = adfr->fakeframe;
		min_fps = adfr->min_fps;
		spin_unlock_irqrestore(&adfr->lock, flags);
	}
	pr_err("kVRR get adfr config %#X debug %#X \n", oplus_adfr_config, oplus_adfr_debug);
	return sprintf(buf,
		       "debug:0x%08X config:0x%08X auto_mode:0x%08X fakeframe:0x%08X auto_minfps:0x%08X auto_counter:%llu\n",
		       oplus_adfr_debug, oplus_adfr_config, mode, fakeframe, min_fps,
		       oplus_adfr_auto_update_counter);
}

ssize_t oplus_adfr_set_debug(struct kobject *obj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct dsi_display *display;
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	u32 policy_mask = OPLUS_ADFR_DEBUG_GLOBAL_DISABLE |
			  OPLUS_ADFR_DEBUG_IDLE_MODE_DISABLE;
	u32 previous = READ_ONCE(oplus_adfr_debug);
	u32 debug, rate;
	int rc = kstrtou32(buf, 0, &debug);

	if (rc)
		return rc;
	WRITE_ONCE(oplus_adfr_debug, debug);
	display = oplus_display_get_current_display();
	if (((previous ^ debug) & policy_mask) && display && display->panel &&
	    display->panel->cur_mode &&
	    (display->panel->cur_mode->timing.refresh_rate == 60 ||
	     display->panel->cur_mode->timing.refresh_rate == 120)) {
		rate = display->panel->cur_mode->timing.refresh_rate;
		adfr = &display->panel->adfr;
		spin_lock_irqsave(&adfr->lock, flags);
		if (adfr->kernel_idle_enabled && adfr->auto_mode == OPLUS_ADFR_AUTO_OFF) {
			/* Also recover an unacknowledged low-rate transfer in flight. */
			if (debug & policy_mask)
				adfr->restore_pending = true;
			adfr->kernel_active_min_fps = oplus_adfr_native_code(rate);
			adfr->last_frame_ns = 0;
			adfr->frame_period_ns = 0;
			adfr->boost_until_ns = 0;
			adfr->min_fps_updated = true;
			adfr->fakeframe_updated = true;
			adfr->sequence++;
		}
		spin_unlock_irqrestore(&adfr->lock, flags);
	}
	pr_err("kVRR get adfr config %#X debug %#X \n", oplus_adfr_config, oplus_adfr_debug);

	return count;
}

static int oplus_adfr_set_kernel_idle_fps(struct dsi_panel *panel, u32 fps)
{
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	u32 code;
	bool enabled = fps != 0;

	if (fps != 0 && fps != 1 && fps != 10 && fps != 20 &&
	    fps != 30 && fps != 40 && fps != 60)
		return -EINVAL;
	if (!panel || !ADFR_GET_GLOBAL_CONFIG(panel->adfr.config) ||
	    !ADFR_GET_IDLE_MODE_CONFIG(panel->adfr.config))
		return -EOPNOTSUPP;
	adfr = &panel->adfr;
	code = oplus_adfr_rate_code(fps);
	spin_lock_irqsave(&adfr->lock, flags);
	if (adfr->kernel_idle_enabled != enabled ||
	    (enabled && adfr->kernel_idle_min_fps != code)) {
		adfr->kernel_idle_enabled = enabled;
		if (enabled)
			adfr->kernel_idle_min_fps = code;
		adfr->kernel_active_min_fps = panel->cur_mode ?
			oplus_adfr_native_code(panel->cur_mode->timing.refresh_rate) :
			OPLUS_ADFR_AUTO_MIN_FPS_MAX;
		adfr->last_frame_ns = 0;
		adfr->frame_period_ns = 0;
		adfr->boost_until_ns = 0;
		adfr->cadence_fps = 0;
		adfr->min_fps_updated = true;
		adfr->fakeframe_updated = true;
		adfr->sequence++;
	}
	spin_unlock_irqrestore(&adfr->lock, flags);
	return 0;
}

ssize_t oplus_adfr_get_idle_fps(struct kobject *obj,
				struct kobj_attribute *attr, char *buf)
{
	struct dsi_display *display = oplus_display_get_current_display();
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	u32 target, requested, applied, active, cadence, boost_ms;
	u64 now = ktime_get_ns();
	bool enabled, valid, idle, restore;
	int error;

	if (!display || !display->panel)
		return -ENODEV;
	adfr = &display->panel->adfr;
	spin_lock_irqsave(&adfr->lock, flags);
	enabled = adfr->kernel_idle_enabled;
	target = enabled ? 120 / (adfr->kernel_idle_min_fps + 1) : 0;
	requested = adfr->min_fps;
	applied = adfr->applied_min_fps;
	valid = adfr->min_fps_valid;
	idle = adfr->idle;
	restore = adfr->restore_pending;
	error = adfr->last_error;
	active = 120 / (adfr->kernel_active_min_fps + 1);
	cadence = adfr->cadence_fps;
	boost_ms = now < adfr->boost_until_ns ?
		div64_u64(adfr->boost_until_ns - now + NSEC_PER_MSEC - 1,
			  NSEC_PER_MSEC) : 0;
	spin_unlock_irqrestore(&adfr->lock, flags);

	return sprintf(buf,
		       "kernel=%u target_hz=%u requested_code=0x%02x applied_code=0x%02x valid=%u idle=%u restore=%u last_error=%d active_hz=%u cadence_fps=%u boost_ms=%u\n",
		       enabled, target, requested, applied, valid, idle, restore,
		       error, active, cadence, boost_ms);
}

ssize_t oplus_adfr_set_idle_fps(struct kobject *obj,
				struct kobj_attribute *attr, const char *buf,
				size_t count)
{
	struct dsi_display *display = oplus_display_get_current_display();
	u32 fps;
	int rc;

	rc = kstrtou32(buf, 0, &fps);
	if (rc)
		return rc;
	if (!display || !display->panel)
		return -ENODEV;
	rc = oplus_adfr_set_kernel_idle_fps(display->panel, fps);
	if (rc)
		return rc;
	return count;
}

static inline bool oplus_adfr_fakeframe_is_enable(struct dsi_panel *panel)
{
	if (panel && READ_ONCE(panel->adfr.kernel_idle_enabled) &&
	    READ_ONCE(panel->adfr.auto_mode) == OPLUS_ADFR_AUTO_OFF &&
	    (READ_ONCE(panel->adfr.idle) ||
	     READ_ONCE(panel->adfr.kernel_active_min_fps) >
		     OPLUS_ADFR_AUTO_MIN_FPS_MAX))
		return false;
	return (bool)(oplus_adfr_panel_supported(panel) &&
		      ADFR_GET_FAKEFRAME_CONFIG(oplus_adfr_config) &&
		      !(oplus_adfr_debug &
			OPLUS_ADFR_DEBUG_FAKEFRAME_DISABLE) &&
		      READ_ONCE(panel->adfr.fakeframe));
}

static bool oplus_adfr_fakeframe_for_connector(struct drm_connector *connector)
{
	struct dsi_panel *panel = oplus_adfr_panel_from_connector(connector);

	return oplus_adfr_fakeframe_is_enable(panel);
}

bool oplus_adfr_vsync_switch_is_enable(void)
{
	return (bool)(oplus_adfr_is_support() &&
		      ADFR_GET_VSYNC_SWITCH_CONFIG(oplus_adfr_config) &&
		      !(oplus_adfr_debug &
			OPLUS_ADFR_DEBUG_VSYNC_SWITCH_DISABLE));
}

enum oplus_vsync_mode oplus_adfr_get_vsync_mode(void)
{
	if (!oplus_adfr_vsync_switch_is_enable()) {
		return OPLUS_INVALID_VSYNC;
	}

	return (enum oplus_vsync_mode)ADFR_GET_VSYNC_SWITCH_MODE(oplus_adfr_config);
}

bool oplus_adfr_idle_mode_is_enable(void)
{
	return oplus_adfr_is_support() &&
	       ADFR_GET_IDLE_MODE_CONFIG(READ_ONCE(oplus_adfr_config)) &&
	       !(READ_ONCE(oplus_adfr_debug) &
		 OPLUS_ADFR_DEBUG_IDLE_MODE_DISABLE);
}

bool oplus_adfr_is_support(void)
{
	return ADFR_GET_GLOBAL_CONFIG(READ_ONCE(oplus_adfr_config)) &&
	       !(READ_ONCE(oplus_adfr_debug) & OPLUS_ADFR_DEBUG_GLOBAL_DISABLE);
}


inline bool oplus_adfr_temperature_detection_is_enable(void)
{
	return oplus_adfr_is_support() &&
		ADFR_GET_TEMPERATURE_DETECTION_CONFIG(oplus_adfr_config) &&
		!(oplus_adfr_debug & OPLUS_ADFR_DEBUG_TEMPERATURE_DETECTION_DISABLE);
}

/* --------------- msm_drv ---------------*/

static void oplus_adfr_thread_priority_worker(struct kthread_work *work)
{
	int ret = 0;
	struct sched_param param = { 0 };
	struct task_struct *task = current->group_leader;

	/**
	 * this priority was found during empiric testing to have appropriate
	 * realtime scheduling to process display updates and interact with
	 * other real time and normal priority task
	 */
	param.sched_priority = 16;
	ret = sched_setscheduler(task, SCHED_FIFO, &param);
	if (ret)
		pr_warn("pid:%d name:%s priority update failed: %d\n",
			current->tgid, task->comm, ret);
}

int oplus_adfr_thread_create(void *msm_priv, void *msm_ddev, void *msm_dev)
{
	struct msm_drm_private *priv;
	struct drm_device *ddev;
	struct device *dev;
	int i = 0;

	priv = msm_priv;
	ddev = msm_ddev;
	dev = msm_dev;

	for (i = 0; i < priv->num_crtcs; i++) {
		/* initialize adfr thread */
		priv->adfr_thread[i].crtc_id = priv->crtcs[i]->base.id;
		kthread_init_worker(&priv->adfr_thread[i].worker);
		priv->adfr_thread[i].dev = ddev;
		priv->adfr_thread[i].thread =
			kthread_run(kthread_worker_fn,
				&priv->adfr_thread[i].worker,
				"adfr:%d", priv->adfr_thread[i].crtc_id);
		kthread_init_work(&priv->thread_priority_work, oplus_adfr_thread_priority_worker);
		kthread_queue_work(&priv->adfr_thread[i].worker, &priv->thread_priority_work);
		kthread_flush_work(&priv->thread_priority_work);

		if (IS_ERR(priv->adfr_thread[i].thread)) {
			dev_err(dev, "kVRR failed to create adfr_commit kthread\n");
			priv->adfr_thread[i].thread = NULL;
		}

		if ((!priv->adfr_thread[i].thread)) {
			/* clean up previously created threads if any */
			for (; i >= 0; i--) {
				if (priv->adfr_thread[i].thread) {
					kthread_stop(
						priv->adfr_thread[i].thread);
					priv->adfr_thread[i].thread = NULL;
				}
			}
			return -EINVAL;
		}
	}
	pr_info("kVRR adfr thread create successfully\n");

	return 0;
}

void oplus_adfr_thread_destroy(void *msm_priv)
{
	struct msm_drm_private *priv;
	int i;

	priv = msm_priv;

	for (i = 0; i < priv->num_crtcs; i++) {
		if (priv->adfr_thread[i].thread) {
			kthread_flush_worker(&priv->adfr_thread[i].worker);
			kthread_stop(priv->adfr_thread[i].thread);
			priv->adfr_thread[i].thread = NULL;
		}
	}
}

/* ------------ sde_connector ------------ */
/* handle it early since qsync min fps dirty will disappeared with high probabilities */
int oplus_adfr_handle_qsync_mode_minfps(u32 propval)
{
	int handled = 0;
	SDE_INFO("kVRR update qsync mode minfps %u[%08X]\n", propval, propval);

	SDE_ATRACE_BEGIN("oplus_adfr_handle_qsync_mode_minfps");

	oplus_adfr_qsync_mode_minfps_updated = true;
	oplus_adfr_qsync_mode_minfps = propval;
	handled = 1;

	SDE_ATRACE_INT("oplus_adfr_qsync_mode_minfps", oplus_adfr_qsync_mode_minfps);
	SDE_ATRACE_END("oplus_adfr_handle_qsync_mode_minfps");

	SDE_INFO("kVRR qsync mode minfps %u[%d]\n",
		oplus_adfr_qsync_mode_minfps, oplus_adfr_qsync_mode_minfps_updated);

	return handled;
}

bool oplus_adfr_qsync_mode_minfps_is_updated(void) {
	bool updated = oplus_adfr_qsync_mode_minfps_updated;
	oplus_adfr_qsync_mode_minfps_updated = false;
	return updated;
}

u32 oplus_adfr_get_qsync_mode_minfps(void) {
	SDE_INFO("kVRR get qsync mode minfps %u\n", oplus_adfr_qsync_mode_minfps);
	return oplus_adfr_qsync_mode_minfps;
}

/* qsync mode timer */
enum hrtimer_restart oplus_adfr_qsync_mode_timer_handler(struct hrtimer *timer)
{
	struct sde_connector *c_conn =
			from_timer(c_conn, timer, qsync_mode_timer);

	/* qsync status reset */
	c_conn->qsync_mode_recovery = true;
	oplus_adfr_qsync_mode_minfps_updated = true;
	SDE_DEBUG("kVRR qsync_mode_timer handler\n");

	return HRTIMER_NORESTART;
}

void oplus_adfr_qsync_mode_timer_start(void *sde_connector, int deferred_ms)
{
	struct sde_connector *c_conn = sde_connector;

	SDE_DEBUG("kVRR qsync_mode_timer start\n");
	hrtimer_start(&c_conn->qsync_mode_timer, ms_to_ktime(deferred_ms), HRTIMER_MODE_REL);
}

/* --------------- sde_crtc ---------------*/

void sde_crtc_adfr_handle_frame_event(void *crt, void *event)
{
	struct drm_crtc *crtc = crt;
	struct sde_crtc *sde_crtc = to_sde_crtc(crtc);
	struct sde_crtc_frame_event *fevent = event;
	struct drm_encoder *encoder;

	/* cancel deferred adfr fakeframe timer */
	if (oplus_adfr_fakeframe_for_connector(fevent->connector) &&
	    (fevent->event & SDE_ENCODER_FRAME_EVENT_SIGNAL_RETIRE_FENCE)) {
		mutex_lock(&sde_crtc->crtc_lock);
		list_for_each_entry(encoder, &crtc->dev->mode_config.encoder_list, head) {
			if (encoder->crtc != crtc)
				continue;

			sde_encoder_adfr_cancel_fakeframe(encoder);
		}
		mutex_unlock(&sde_crtc->crtc_lock);
	}
}


/* --------------- sde_encoder ---------------*/

static inline struct dsi_display_mode_priv_info *oplus_get_current_mode_priv_info(struct drm_connector * drm_conn)
{
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	struct dsi_display *dsi_display;
	struct dsi_panel *panel;

	if (!drm_conn) {
		SDE_ERROR("kVRR adfr drm_conn is null.\n");
		return NULL;
	}

	priv = drm_conn->dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);

	if (!sde_kms) {
		SDE_ERROR("kVRR adfr sde_kms is null.\n");
		return NULL;
	}

	if (sde_kms->dsi_display_count && sde_kms->dsi_displays) {
		/* only use primary dsi */
		dsi_display = sde_kms->dsi_displays[0];
	} else {
		SDE_ERROR("kVRR adfr sde_kms's dsi_display is null.\n");
		return NULL;
	}

	panel = dsi_display->panel;

	if (!panel || !panel->cur_mode) {
		SDE_ERROR("kVRR adfr dsi_display's panel is null.\n");
		return NULL;
	}

	return panel->cur_mode->priv_info;
}

void sde_encoder_adfr_prepare_commit(void *crt, void *enc, void *conn) {
	struct dsi_display_mode_priv_info *priv_info;
	struct drm_crtc *crtc = crt;
	struct drm_connector *drm_conn = conn;
	struct sde_connector *sde_conn = NULL;
	struct dsi_display *dsi_display = NULL;

	/* when power on, disable deferred fakeframe
	* after power on and before first frame flush
	* if panel get a fakeframe then refresh itself (with a dirty buffer), tearing happen
	* so for power on case, set need_deferred_fakeframe false
	* this can avoid deferred fakeframe tearing issue (eg. AOD)
	* power off --> sde_encoder_virt_disable set "sde_enc->cur_master = NULL"
	* power on  --> sde_encoder_virt_enable  set "sde_enc->cur_master = XXX"
	* prepare_commit need cur_master is not null but it is before than sde_encoder_virt_enable
	* so use prepare_commit(NULL, NULL, NULL) to imply this commit is first commit after power on
	*/
	if (!crt && !enc && !conn) {
		need_deferred_fakeframe = false;
		/* SDE_ATRACE_INT("need_deferred_fakeframe", need_deferred_fakeframe); */
		return;
	}
	if (!oplus_adfr_fakeframe_for_connector(drm_conn))
		return;

	if (!crt || !enc || !conn) {
		SDE_ERROR("kVRR sde_encoder_adfr_prepare_commit error: %p %p %p",
			crt, enc, conn);
		return;
	}

	sde_conn = to_sde_connector(drm_conn);
	if (!sde_conn) {
		SDE_ERROR("kVRR sde_encoder_adfr_prepare_commit error: %p", sde_conn);
		return;
	}

	if (!sde_conn->display) {
		SDE_ERROR("kVRR sde_encoder_adfr_prepare_commit error: %p", sde_conn->display);
		return;
	}
	dsi_display = sde_conn->display;
	if (!dsi_display->panel) {
		SDE_ERROR("kVRR sde_encoder_adfr_prepare_commit error: %p", dsi_display->panel);
		return;
	}
	/* after power on, enable deferred fakeframe */
	/* if (get_oplus_display_power_status() == OPLUS_DISPLAY_POWER_ON) { */
	if (dsi_display->panel->power_mode == SDE_MODE_DPMS_ON) {
		need_deferred_fakeframe = true;
	} else {
		need_deferred_fakeframe = false;
		/* SDE_DEBUG("kVRR display stats: %d , skip fakeframe", dsi_display->panel->power_mode); */
		return;
	}

	priv_info = oplus_get_current_mode_priv_info(drm_conn);

	/* check 1st bit */
	if (!priv_info || !(priv_info->fakeframe_config & 0X00000001)) {
		return;
	}

	/* before commit send a fakeframe to triger the panel flush
	* but if pre-frame is pending, ignore this time
	* because pre-frame is a real frame, Not Need fakeframe
	* SDE_ATRACE_INT("frame_pending", sde_crtc_frame_pending(sde_enc->crtc));
	*/
	if ((sde_crtc_frame_pending(crtc) == 0)) {
		sde_encoder_adfr_trigger_fakeframe(enc);
		need_deferred_fakeframe = true;
	}
}

void sde_encoder_adfr_kickoff(void *crt, void *enc, void *conn) {
	struct dsi_display_mode_priv_info *priv_info;
	struct drm_connector *drm_conn = conn;
	int deferred_ms = -1;

	if (!oplus_adfr_fakeframe_for_connector(drm_conn))
		return;

	/* SDE_ATRACE_INT("need_deferred_fakeframe", need_deferred_fakeframe); */
	if (!need_deferred_fakeframe) {
		/* SDE_ERROR("kVRR sde_encoder_adfr_kickoff skip, need_deferred_fakeframe is false."); */
		return;
	}

	if (!crt || !enc || !conn) {
		SDE_ERROR("kVRR sde_encoder_adfr_kickoff error:  %p %p %p",
			crt, enc, conn);
		return;
	}

	priv_info = oplus_get_current_mode_priv_info(drm_conn);

	/* check 2st bit */
	if (!priv_info || !(priv_info->fakeframe_config & 0X00000002)) {
		return;
	}
	deferred_ms = priv_info->deferred_fakeframe_time;

	oplus_adfr_fakeframe_timer_start(enc, deferred_ms);
	need_deferred_fakeframe = false;
}

/* --------------- sde_encoder_phys_cmd ---------------*/
/* Add for qsync tearing issue */
/* indicates whether filter backlight cmd is need */
bool oplus_adfr_backlight_cmd_filter_set(bool enable)
{
	oplus_adfr_need_filter_backlight_cmd = enable;
	return oplus_adfr_need_filter_backlight_cmd;
}

bool oplus_adfr_backlight_cmd_filter_get(void)
{
	return oplus_adfr_need_filter_backlight_cmd;
}

/* if force_qsync_mode_off is true, close qsync window immediately */
void oplus_adfr_force_qsync_mode_off(void *drm_connector)
{
	struct drm_connector *connector = drm_connector;
	struct sde_connector *c_conn;
	struct drm_bridge *temp_bridge;
	struct dsi_bridge *c_bridge;
	struct dsi_display *display;

	if (!connector || !connector->encoder)
		return;
	if (!drm_bridge_chain_get_first_bridge(connector->encoder))
		return;

	c_conn = to_sde_connector(connector);
	temp_bridge = drm_bridge_chain_get_first_bridge(c_conn->encoder);
	c_bridge = to_dsi_bridge(temp_bridge);
	display = c_bridge->display;

	if (!display)
		return;

	if (display->force_qsync_mode_off) {
		SDE_INFO("kVRR force qsync mode update %d -> %d\n",
				c_conn->qsync_mode, SDE_RM_QSYNC_DISABLED);
		c_conn->qsync_updated = true;
		c_conn->qsync_mode = SDE_RM_QSYNC_DISABLED;
		/* qsync disable need change min fps */
		c_conn->qsync_curr_dynamic_min_fps = 0;
		c_conn->qsync_deferred_window_status = SET_WINDOW_IMMEDIATELY;
		display->force_qsync_mode_off = false;
	} else if ((c_conn->qsync_mode != SDE_RM_QSYNC_DISABLED) &&
		(c_conn->qsync_deferred_window_status != DEFERRED_WINDOW_START) && c_conn->oplus_adfr_backlight_updated) {
		/* if qsync is enable and backlight status update, close qsync immediately */
		SDE_INFO("kVRR backlight update, force qsync mode update %d -> %d\n",
				c_conn->qsync_mode, SDE_RM_QSYNC_DISABLED);
		c_conn->qsync_updated = true;
		c_conn->qsync_mode = SDE_RM_QSYNC_DISABLED;
		/* qsync disable need change min fps */
		c_conn->qsync_curr_dynamic_min_fps = 0;
		c_conn->qsync_deferred_window_status = SET_WINDOW_IMMEDIATELY;

		/* send qsync off cmd */
		sde_connector_prepare_commit(connector);
		/* start timer to delay qsync status recovery */
		oplus_adfr_qsync_mode_timer_start(c_conn, 1000);
	} else if (c_conn->qsync_mode != SDE_RM_QSYNC_DISABLED && (c_conn->qsync_deferred_window_status != DEFERRED_WINDOW_START)) {
		/* if backlight cmd is set after qsync window setting and qsync is enable, filter it
		otherwise tearing issue happen */
		oplus_adfr_backlight_cmd_filter_set(true);
	}

	c_conn->oplus_adfr_backlight_updated = false;
	return;
}

int oplus_adfr_adjust_tearcheck_for_dynamic_qsync(void *sde_phys_enc)
{
	struct sde_encoder_phys *phys_enc = sde_phys_enc;
	struct sde_hw_tear_check tc_cfg = {0};
	struct sde_connector *sde_conn = NULL;
	int ret = 0;

	if (!phys_enc || !phys_enc->connector) {
		SDE_ERROR("kVRR invalid encoder parameters\n");
		return -EINVAL;
	}

	sde_conn = to_sde_connector(phys_enc->connector);

	if (sde_connector_get_qsync_mode(phys_enc->connector) == 0 ||
		sde_connector_get_qsync_dynamic_min_fps(phys_enc->connector) == 0) {
		/* Fixed qsync window and panel min fps nonsynchronous issue */
		phys_enc->current_sync_threshold_start = phys_enc->qsync_sync_threshold_start;
		return ret;
	}

	SDE_ATRACE_BEGIN("adjust_tearcheck_for_qsync");
	SDE_ATRACE_INT("frame_state", atomic_read(&phys_enc->frame_state));
	SDE_DEBUG("frame_state = %d\n", atomic_read(&phys_enc->frame_state));

	/* this time maybe remain in qsync window, so shrink qsync window */
	/* to avoid tearing and keep qsync enable for this frame */
	if (atomic_read(&phys_enc->frame_state) != 0) {
		/* 300 is a estimated value */
		tc_cfg.sync_threshold_start = 300;
	} else {
		/* remain use original qsync window */
		tc_cfg.sync_threshold_start = phys_enc->qsync_sync_threshold_start;
	}

	if(phys_enc->current_sync_threshold_start != tc_cfg.sync_threshold_start) {
		SDE_ATRACE_BEGIN("update_qsync");

		if (phys_enc->has_intf_te &&
			phys_enc->hw_intf->ops.update_tearcheck)
			phys_enc->hw_intf->ops.update_tearcheck(
				phys_enc->hw_intf, &tc_cfg);
		else if (phys_enc->hw_pp->ops.update_tearcheck)
			phys_enc->hw_pp->ops.update_tearcheck(
				phys_enc->hw_pp, &tc_cfg);
		SDE_EVT32(DRMID(phys_enc->parent), tc_cfg.sync_threshold_start);
		phys_enc->current_sync_threshold_start = tc_cfg.sync_threshold_start;
		/* trigger AP update qsync flush */
		sde_conn->qsync_updated = true;

		SDE_ATRACE_END("update_qsync");
	}

	SDE_DEBUG("kVRR threshold_lines %d\n", phys_enc->current_sync_threshold_start);
	SDE_ATRACE_INT("threshold_lines", phys_enc->current_sync_threshold_start);
	SDE_ATRACE_END("adjust_tearcheck_for_qsync");

	return ret;
}

/* --------------- dsi_connector ---------------*/

/* fake frame */
int sde_connector_send_fakeframe(void *conn)
{
	struct drm_connector *connector = conn;
	struct sde_connector *c_conn;
	int rc;

	if (!connector) {
		SDE_ERROR("kVRR invalid argument\n");
		return -EINVAL;
	}

	c_conn = to_sde_connector(connector);
	if (!c_conn->display) {
		SDE_ERROR("kVRR invalid connector display\n");
		return -EINVAL;
	}

	rc = dsi_display_send_fakeframe(c_conn->display);

	SDE_EVT32(connector->base.id, rc);
	return rc;
}

/* --------------- dsi_display ---------------*/

/* qsync enhance */
/* update qsync min fps */
int dsi_display_qsync_update_min_fps(void *dsi_display, void *dsi_params)
{
	struct dsi_display *display = dsi_display;
	struct msm_display_conn_params *params = dsi_params;
	int i;
	int rc = 0;

	if (!params->qsync_update) {
		return 0;
	}

	/* allow qsync off but update qsync min fps only */
	SDE_ATRACE_BEGIN("dsi_display_qsync_update_min_fps #3");

	mutex_lock(&display->display_lock);

	display_for_each_ctrl(i, display) {
		/* send the commands to updaet qsync min fps */
		pr_info("dsi_panel_send_qsync_min_fps_dcs #2");
		rc = dsi_panel_send_qsync_min_fps_dcs(display->panel, i, params->qsync_dynamic_min_fps);
		if (rc) {
			DSI_ERR("kVRR fail qsync UPDATE cmds rc:%d\n", rc);
			goto exit;
		}
	}

exit:
	SDE_EVT32(params->qsync_mode, params->qsync_dynamic_min_fps, rc);
	mutex_unlock(&display->display_lock);

	SDE_ATRACE_END("dsi_display_qsync_update_min_fps");

	return rc;
}

/* save qsync info, then restore qsync status after panel enable*/
int dsi_display_qsync_restore(void *dsi_display)
{
	struct msm_display_conn_params params;
	struct dsi_display *display = dsi_display;
	int rc = 0;

	if (display->need_qsync_restore) {
		display->need_qsync_restore = false;
	} else {
		return 0;
	}

	params.qsync_update = display->current_qsync_mode ||
						  display->current_qsync_dynamic_min_fps;

	if (!params.qsync_update) {
		DSI_DEBUG("kVRR %s:INFO: qsync status is clean.\n", __func__);
		return 0;
	}

	params.qsync_mode = display->current_qsync_mode;
	params.qsync_dynamic_min_fps = 10;

	SDE_ATRACE_BEGIN("dsi_display_qsync_restore");

	DSI_INFO("kVRR qsync restore mode %d minfps %d \n",
	         params.qsync_mode, params.qsync_dynamic_min_fps);
	rc = dsi_display_pre_commit(display, &params);
	SDE_EVT32(params.qsync_mode, params.qsync_dynamic_min_fps, rc);

	SDE_ATRACE_END("dsi_display_qsync_restore");

	return rc;
}

/* fake frame */
int dsi_display_send_fakeframe(void *disp)
{
	struct dsi_display *display = (struct dsi_display *)disp;
	int i, rc = 0;

	if (!display) {
		pr_err("kVRR Invalid params\n");
		return -EINVAL;
	}

	SDE_ATRACE_BEGIN("dsi_display_send_fakeframe");
	display_for_each_ctrl(i, display) {
		/* send the commands to simulate a frame transmission */
		rc = dsi_panel_send_fakeframe_dcs(display->panel, i);
		if (rc) {
			DSI_ERR("kVRR fail fake frame cmds rc:%d\n", rc);
			goto exit;
		}
	}

exit:
	SDE_ATRACE_END("dsi_display_send_fakeframe");
	SDE_EVT32(rc);

	return rc;
}

void oplus_adfr_set_dynamic_te_config(int config)
{
	mutex_lock(&dynamic_te_lock);
	oplus_adfr_dynamic_te.config = config;
	mutex_unlock(&dynamic_te_lock);
}

/* dynamic te timer */
enum hrtimer_restart oplus_adfr_dynamic_te_timer_handler(struct hrtimer *timer)
{
	struct dsi_display *display = oplus_display_get_current_display();

	/* update report rate if enter idle mode */
	if (display && display->panel && READ_ONCE(display->panel->adfr.idle) &&
	    READ_ONCE(display->panel->adfr.min_fps_valid))
		oplus_adfr_dynamic_te.refresh_rate =
			120 /
			(READ_ONCE(display->panel->adfr.applied_min_fps) + 1);
	if (oplus_adfr_dynamic_te.config == OPLUS_ADFR_DYNAMIC_TE_ENABLE_WITCH_LOG) {
		DSI_INFO("kVRR dynamic te: enter idle mode, refresh_rate=%d\n", oplus_adfr_dynamic_te.refresh_rate);
	}

	return HRTIMER_NORESTART;
}

/* dynamic te detect */
irqreturn_t oplus_adfr_dynamic_te_handler(int irq, void *data)
{
	static int mid_refresh_rate_count = 0;
	static int high_refresh_rate_count = 0;
	int temp_refresh_rate = 0;
	struct dsi_display *display = (struct dsi_display *)data;
	struct oplus_adfr_state *adfr;
	struct dsi_mode_info timing;
	u64 delta_ns;
	bool native;

	if (!display || !display->panel || !display->panel->cur_mode)
		return IRQ_HANDLED;
	adfr = &display->panel->adfr;
	native =
		READ_ONCE(adfr->sw_fps) != 60 ||
		READ_ONCE(adfr->applied_min_fps) == OPLUS_ADFR_AUTO_MIN_FPS_MAX;

	if (oplus_adfr_dynamic_te.config != OPLUS_ADFR_DYNAMIC_TE_DISABLE) {
		timing = display->panel->cur_mode->timing;

		/* check the te interval to calculate framerate */
		oplus_adfr_dynamic_te.last_te_timestamp = oplus_adfr_dynamic_te.current_te_timestamp;
		oplus_adfr_dynamic_te.current_te_timestamp = ktime_get_ns();
		if (!oplus_adfr_dynamic_te.last_te_timestamp ||
		    oplus_adfr_dynamic_te.current_te_timestamp <=
			    oplus_adfr_dynamic_te.last_te_timestamp)
			return IRQ_HANDLED;
		delta_ns = oplus_adfr_dynamic_te.current_te_timestamp -
			   oplus_adfr_dynamic_te.last_te_timestamp;
		temp_refresh_rate = div64_u64(NSEC_PER_SEC, delta_ns);

		if (!strcmp(display->panel->oplus_priv.vendor_name, "NT37705") || !strcmp(display->panel->oplus_priv.vendor_name, "BOE_NT37705")) {
			if (oplus_dsi_log_type & BIT(4)) {
				if (timing.h_skew == SDC_ADFR || timing.h_skew == SDC_MFR) {
					if (timing.refresh_rate == 120) {
						if (temp_refresh_rate > 90)
							oplus_adfr_dynamic_te.refresh_rate = 120;
						else
							oplus_adfr_dynamic_te.refresh_rate = 60;
					} else if (timing.refresh_rate == 90) {
						if (temp_refresh_rate > 60)
							oplus_adfr_dynamic_te.refresh_rate = 90;
						else
							oplus_adfr_dynamic_te.refresh_rate = 45;
					} else {
						oplus_adfr_dynamic_te.refresh_rate = 60;
					}
				} else if (timing.h_skew == OPLUS_ADFR || timing.h_skew == OPLUS_MFR) {
					oplus_adfr_dynamic_te.refresh_rate = 120;
				} else {
					oplus_adfr_dynamic_te.refresh_rate = 0;
				}
			} else {
				oplus_adfr_dynamic_te.refresh_rate = timing.refresh_rate;
			}
			goto log;
		}

		if ((!strcmp(display->panel->oplus_priv.vendor_name, "TM_NT37705"))
			|| (!strcmp(display->panel->oplus_priv.vendor_name, "TM_NT37705_DVT"))) {
			if (timing.h_skew == SDC_ADFR || timing.h_skew == SDC_MFR) {
				if (timing.refresh_rate == 120) {
					if (temp_refresh_rate > 75) {
						high_refresh_rate_count++;
						/* update refresh rate if 4 continous temp_refresh_rate are greater than 55 */
						if (high_refresh_rate_count == 4) {
							oplus_adfr_dynamic_te.refresh_rate = 120;
							high_refresh_rate_count--;
						}
					} else {
						high_refresh_rate_count = 0;
						oplus_adfr_dynamic_te.refresh_rate = 60;
					}
				} else if (timing.refresh_rate == 90) {
					if (temp_refresh_rate > 60) {
						high_refresh_rate_count++;
						if (high_refresh_rate_count == 4) {
							oplus_adfr_dynamic_te.refresh_rate = 90;
							high_refresh_rate_count--;
						}
					} else {
						high_refresh_rate_count = 0;
						oplus_adfr_dynamic_te.refresh_rate = 45;
					}
				} else if (timing.refresh_rate == 144) {
					if (temp_refresh_rate > 96) {
                                                high_refresh_rate_count++;
                                                /* update refresh rate if 4 continous temp_refresh_rate are greater than 55 */
                                                if (high_refresh_rate_count == 4) {
							oplus_adfr_dynamic_te.refresh_rate = 144;
							high_refresh_rate_count--;
						}
					} else {
                                                high_refresh_rate_count = 0;
						oplus_adfr_dynamic_te.refresh_rate = 72;
					}
				} else {
					oplus_adfr_dynamic_te.refresh_rate = 60;
				}
			} else if (timing.h_skew == OPLUS_ADFR || timing.h_skew == OPLUS_MFR) {
				oplus_adfr_dynamic_te.refresh_rate = 120;
			} else {
				oplus_adfr_dynamic_te.refresh_rate = 0;
			}
			goto log;
                }

		/* filtering algorithm */
		if (timing.h_skew == SDC_ADFR || timing.h_skew == SDC_MFR) {
			if (timing.refresh_rate == 90) {
				mid_refresh_rate_count = 0;
				high_refresh_rate_count = 0;
				/* fix frame rate */
				oplus_adfr_dynamic_te.refresh_rate = 90;
			} else if (timing.refresh_rate == 144) {
				if (temp_refresh_rate <= 90) {
					mid_refresh_rate_count++;
					high_refresh_rate_count = 0;
					if (mid_refresh_rate_count == 1) {
						oplus_adfr_dynamic_te.refresh_rate = 72;
						mid_refresh_rate_count--;
					}
				} else {
                                	mid_refresh_rate_count = 0;
                                	high_refresh_rate_count = 0;
                                	/* fix frame rate */
                                	oplus_adfr_dynamic_te.refresh_rate = 144;
				}
			} else if (timing.refresh_rate == 120 || timing.refresh_rate == 60) {
				if (temp_refresh_rate > 55) {
					mid_refresh_rate_count = 0;
					high_refresh_rate_count++;
					if (timing.refresh_rate == 120) {
						/* update refresh rate if 4 continous temp_refresh_rate are greater than 55 */
						if (high_refresh_rate_count == 4) {
							oplus_adfr_dynamic_te
								.refresh_rate =
								native ? 120 :
									 60;
							high_refresh_rate_count--;
						}
					} else {
							/* update refresh rate if 4 continous temp_refresh_rate are greater than 55 */
							if (high_refresh_rate_count >= 4) {
								oplus_adfr_dynamic_te.refresh_rate = 60;
								high_refresh_rate_count = 3;
							}
					}
				} else if (temp_refresh_rate > 16 && temp_refresh_rate <= 55) {
					mid_refresh_rate_count++;
					high_refresh_rate_count = 0;
					/* update refresh rate if 1 continous temp_refresh_rate are greater than 16 and less than or equal to 55 */
					if (mid_refresh_rate_count == 1) {
						oplus_adfr_dynamic_te.refresh_rate = 30;
						mid_refresh_rate_count--;
					}
				} else {
					mid_refresh_rate_count = 0;
					high_refresh_rate_count = 0;
					/* update report value if refresh rate is less than or equal to 16 */
					oplus_adfr_dynamic_te.refresh_rate = temp_refresh_rate;
				}
			} else {
					mid_refresh_rate_count = 0;
					high_refresh_rate_count = 0;
					oplus_adfr_dynamic_te.refresh_rate = 0;
			}
		} else if (timing.h_skew == OPLUS_ADFR || timing.h_skew == OPLUS_MFR) {
				mid_refresh_rate_count = 0;
				high_refresh_rate_count = 0;
				oplus_adfr_dynamic_te.refresh_rate = 120;
		} else {
				mid_refresh_rate_count = 0;
				high_refresh_rate_count = 0;
				oplus_adfr_dynamic_te.refresh_rate = 0;
		}

log:
		if (oplus_adfr_dynamic_te.config == OPLUS_ADFR_DYNAMIC_TE_ENABLE_WITCH_LOG) {
			DSI_INFO("kVRR dynamic te: temp_refresh_rate=%d\n", temp_refresh_rate);
			/* print key information every te interval */
			DSI_INFO("kVRR dynamic te: last_te_timestamp=%lu, current_te_timestamp=%lu, refresh_rate=%d\n",
				oplus_adfr_dynamic_te.last_te_timestamp, oplus_adfr_dynamic_te.current_te_timestamp,
					oplus_adfr_dynamic_te.refresh_rate);
			DSI_INFO("kVRR hactive=%d,vactive=%d,fps=%d,h_skew=%d,auto_mode=%d,auto_minfps=%d,sw_fps=%d,fakeframe=%d,idle_mode=%d,qsync_mode=%d,qsync_minfps=%d\n",
				 timing.h_active, timing.v_active,
				 timing.refresh_rate, timing.h_skew,
				 READ_ONCE(adfr->applied_auto_mode),
				 READ_ONCE(adfr->applied_min_fps),
				 READ_ONCE(adfr->sw_fps),
				 READ_ONCE(adfr->fakeframe),
				 READ_ONCE(adfr->idle),
				 display->current_qsync_mode,
				 display->current_qsync_dynamic_min_fps);
		}
	}

	return IRQ_HANDLED;
}

void oplus_adfr_register_dynamic_te_irq(void *dsi_display)
{
	struct dsi_display *display = dsi_display;
	int rc = 0;
	struct platform_device *pdev;
	struct device *dev;
	unsigned int dynamic_te_irq;

	pdev = display->pdev;
	if (!pdev) {
		DSI_ERR("invalid platform device\n");
		return;
	}

	dev = &pdev->dev;
	if (!dev) {
		DSI_ERR("invalid device\n");
		return;
	}

	if (display->trusted_vm_env) {
		DSI_INFO("GPIO's are not enabled in trusted VM\n");
		return;
	}

	if (!gpio_is_valid(display->panel->dynamic_te_gpio)) {
		rc = -EINVAL;
		goto error;
	}

	dynamic_te_irq = gpio_to_irq(display->panel->dynamic_te_gpio);

	/* Avoid deferred spurious irqs with disable_irq() */
	irq_set_status_flags(dynamic_te_irq, IRQ_DISABLE_UNLAZY);

	/* detect TE rising edge */
	rc = devm_request_irq(dev, dynamic_te_irq, oplus_adfr_dynamic_te_handler,
			      IRQF_TRIGGER_RISING | IRQF_ONESHOT,
			      "DYNAMIC_TE_GPIO", display);
	if (rc) {
		DSI_ERR("dynamic TE request_irq failed rc:%d\n", rc);
		irq_clear_status_flags(dynamic_te_irq, IRQ_DISABLE_UNLAZY);
		goto error;
	}

	disable_irq(dynamic_te_irq);

	DSI_DEBUG("kVRR register dynamic te irq successfully\n");

	return;

error:
	DSI_WARN("Unable to register for dynamic TE IRQ\n");
}

ssize_t oplus_adfr_get_dynamic_te(struct kobject *obj,
	struct kobj_attribute *attr, char *buf)
{
	struct dsi_display *display = oplus_display_get_current_display();
	struct dsi_mode_info timing;
	int refresh_rate = 0;

	if (display == NULL) {
		DSI_ERR("%s error :NULL display", __func__);
		return -EINVAL;
	}

	if (display->panel == NULL) {
		DSI_ERR("%s error :NULL panel", __func__);
		return -EINVAL;
	}

	if (display->panel->cur_mode == NULL) {
		DSI_ERR("%s error :NULL cur_mode", __func__);
		return -EINVAL;
	}

	if (!gpio_is_valid(display->panel->dynamic_te_gpio)) {
		timing = display->panel->cur_mode->timing;
		refresh_rate = timing.refresh_rate;
		return sprintf(buf, "%d\n", refresh_rate);
	}

	if ((!strcmp(display->panel->oplus_priv.vendor_name, "TM_NT37705"))
		|| (!strcmp(display->panel->oplus_priv.vendor_name, "TM_NT37705_DVT"))) {
		if ((oplus_adfr_config == 0) || (oplus_adfr_dynamic_te.refresh_rate == 1)
			|| (oplus_adfr_dynamic_te.refresh_rate == 0)) {
			timing = display->panel->cur_mode->timing;
			refresh_rate = timing.refresh_rate;
			return sprintf(buf, "%d\n", refresh_rate);
		}
	}

	if (!strcmp(display->display_type, "primary")) {
		refresh_rate = oplus_adfr_dynamic_te.refresh_rate;
		DSI_INFO("kVRR dynamic te refresh rate is %d\n", refresh_rate);
	}
	if (!strcmp(display->display_type, "secondary")) {
		timing = display->panel->cur_mode->timing;
		refresh_rate = timing.refresh_rate;
	}
	return sprintf(buf, "%d\n", refresh_rate);
}

ssize_t oplus_adfr_set_dynamic_te(struct kobject *obj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct dsi_display *display = oplus_display_get_current_display();
	unsigned int dynamic_te_irq;
	int config = 0;

	if (display == NULL) {
		DSI_ERR("%s error :NULL display", __func__);
		return -EINVAL;
	}

	if (display->panel == NULL) {
		DSI_ERR("%s error :NULL panel", __func__);
		return -EINVAL;
	}

	if (!gpio_is_valid(display->panel->dynamic_te_gpio)) {
		DSI_ERR("invalid dynamic te gpio\n");
		return count;
	}

	sscanf(buf, "%du", &config);
	oplus_adfr_set_dynamic_te_config(config);

	dynamic_te_irq = gpio_to_irq(display->panel->dynamic_te_gpio);

	if (oplus_adfr_dynamic_te.config != OPLUS_ADFR_DYNAMIC_TE_DISABLE) {
		enable_irq(dynamic_te_irq);
		DSI_INFO("kVRR dynamic te detect enable\n");
	} else {
		disable_irq(dynamic_te_irq);
		DSI_INFO("kVRR dynamic te detect disable\n");
	}

	return count;
}

int oplus_display_set_dynamic_te(void *buf)
{
	struct dsi_display *display = oplus_display_get_current_display();
	unsigned int dynamic_te_irq;
	uint32_t *buf_temp = buf;
	uint32_t config = *buf_temp;

	if (display == NULL) {
		DSI_ERR("%s error :NULL display", __func__);
		return -EINVAL;
	}

	if (display->panel == NULL) {
		DSI_ERR("%s error :NULL panel", __func__);
		return -EINVAL;
	}

	if (!gpio_is_valid(display->panel->dynamic_te_gpio)) {
		DSI_ERR("invalid dynamic te gpio\n");
		return -EINVAL;
	}

	oplus_adfr_set_dynamic_te_config(config);

	dynamic_te_irq = gpio_to_irq(display->panel->dynamic_te_gpio);

	if (oplus_adfr_dynamic_te.config != OPLUS_ADFR_DYNAMIC_TE_DISABLE) {
		enable_irq(dynamic_te_irq);
		DSI_INFO("kVRR dynamic te detect enable\n");
	} else {
		disable_irq(dynamic_te_irq);
		DSI_INFO("kVRR dynamic te detect disable\n");
	}

	return 0;
}

int oplus_display_get_dynamic_te(void *buf)
{
	struct dsi_display *display = oplus_display_get_current_display();
	struct dsi_mode_info timing;
	uint32_t *refresh_rate = buf;

	if (display == NULL) {
		DSI_ERR("%s error :NULL display", __func__);
		return -EINVAL;
	}

	if (display->panel == NULL) {
		DSI_ERR("%s error :NULL panel", __func__);
		return -EINVAL;
	}

	if (display->panel->cur_mode == NULL) {
		DSI_ERR("%s error :NULL cur_mode", __func__);
		return -EINVAL;
	}

	if (!gpio_is_valid(display->panel->dynamic_te_gpio)) {
		timing = display->panel->cur_mode->timing;
		*refresh_rate = timing.refresh_rate;
		return 0;
	}

	if (!strcmp(display->display_type, "primary")) {
		*refresh_rate = oplus_adfr_dynamic_te.refresh_rate;
		DSI_INFO("kVRR dynamic te refresh rate is %d\n", *refresh_rate);
	}
	if (!strcmp(display->display_type, "secondary")) {
		timing = display->panel->cur_mode->timing;
		*refresh_rate = timing.refresh_rate;
	}

	return 0;
}

/* --------------- dsi_panel ---------------*/

/* qsync enhance */
const char *qsync_min_fps_set_map[DSI_CMD_QSYNC_MIN_FPS_COUNTS] = {
	"qcom,mdss-dsi-qsync-min-fps-0",
	"qcom,mdss-dsi-qsync-min-fps-1",
	"qcom,mdss-dsi-qsync-min-fps-2",
	"qcom,mdss-dsi-qsync-min-fps-3",
	"qcom,mdss-dsi-qsync-min-fps-4",
	"qcom,mdss-dsi-qsync-min-fps-5",
	"qcom,mdss-dsi-qsync-min-fps-6",
	"qcom,mdss-dsi-qsync-min-fps-7",
	"qcom,mdss-dsi-qsync-min-fps-8",
	"qcom,mdss-dsi-qsync-min-fps-9",
};

/* qsync enhance */
int dsi_panel_send_qsync_min_fps_dcs(void *dsi_panel,
		int ctrl_idx, uint32_t min_fps)
{
	struct dsi_panel *panel = dsi_panel;
	struct dsi_display_mode_priv_info *priv_info;
	int rc = 0;
	int i = 0;

	if (!panel || !panel->cur_mode) {
		DSI_ERR("kVRR Invalid params\n");
		return -EINVAL;
	}

	priv_info = panel->cur_mode->priv_info;

	mutex_lock(&panel->panel_lock);

	/* select a best fps to fit min_fps */
	for(i = priv_info->qsync_min_fps_sets_size - 1; i >= 0; i--) {
		if(priv_info->qsync_min_fps_sets[i] <= min_fps) {
			DSI_DEBUG("kVRR ctrl:%d qsync find min fps %d\n", ctrl_idx, priv_info->qsync_min_fps_sets[i]);
			break;
		}
	}

	if(i >= 0 && i < priv_info->qsync_min_fps_sets_size) {
		DSI_INFO("kVRR ctrl:%d qsync update min fps %d use \n", ctrl_idx, min_fps);
		SDE_ATRACE_INT("oplus_adfr_qsync_mode_minfps_cmd", min_fps);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_QSYNC_MIN_FPS_0+i);
		if (rc)
			DSI_ERR("kVRR [%s] failed to send DSI_CMD_QSYNC_MIN_FPS cmds rc=%d\n",
				panel->name, rc);
		pr_info("oplus_adfr_send_min_fps_event #3");
		oplus_adfr_send_min_fps_event(OPLUS_ADFR, min_fps);
	} else {
		DSI_ERR("kVRR ctrl:%d failed to sets qsync min fps %u, %d\n", ctrl_idx, min_fps, i);
	}
	mutex_unlock(&panel->panel_lock);
	return rc;
}

/* fake frame */
int dsi_panel_send_fakeframe_dcs(void *dsi_panel,
		int ctrl_idx)
{
	struct dsi_panel *panel = dsi_panel;
	int rc = 0;

	/* SDC's auto, fakeframe and minfps are available only after power on */

	if (!panel) {
		DSI_ERR("kVRR invalid params\n");
		return -EINVAL;
	}

	/* if (get_oplus_display_power_status() != OPLUS_DISPLAY_POWER_ON) { */
	if (panel->power_mode != SDE_MODE_DPMS_ON) {
		DSI_INFO("kVRR ignore %s when power is %d", __FUNCTION__, panel->power_mode);
		return 0;
	}

	if (!oplus_adfr_fakeframe_is_enable(panel)) {
		DSI_INFO("kVRR fakeframe canceled\n");
		return 0;
	}
	mutex_lock(&panel->panel_lock);

	DSI_DEBUG("kVRR ctrl:%d fake frame\n", ctrl_idx);
	if (get_oplus_display_power_status() == OPLUS_DISPLAY_POWER_ON) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_FAKEFRAME);
		if (rc)
			DSI_ERR("kVRR [%s] failed to send DSI_CMD_FAKEFRAME cmds rc=%d\n",
			       panel->name, rc);
	}

	mutex_unlock(&panel->panel_lock);
	return rc;
}


/* qsync enhance */
static int dsi_panel_parse_qsync_min_fps(
		struct dsi_display_mode_priv_info *priv_info,
		struct dsi_parser_utils *utils)
{
	int rc = 0;
	u32 i;

	if (!priv_info) {
		DSI_ERR("kVRR dsi_panel_parse_qsync_min_fps err: invalid mode priv info\n");
		return -EINVAL;
	}

	priv_info->qsync_min_fps_sets_size = 0;

	for (i = 0; i < DSI_CMD_QSYNC_MIN_FPS_COUNTS; i++) {
		rc = utils->read_u32(utils->data, qsync_min_fps_set_map[i],
			&priv_info->qsync_min_fps_sets[i]);
		if (rc) {
			DSI_DEBUG("kVRR failed to parse qsync min fps set %u\n", i);
			break;
		}
		else {
			priv_info->qsync_min_fps_sets_size++;
			DSI_DEBUG("kVRR parse qsync min fps set %u = %u\n",
			priv_info->qsync_min_fps_sets_size - 1, priv_info->qsync_min_fps_sets[i]);
		}
	}

	return rc;
}

/* fake frame */
static int dsi_panel_parse_fakeframe(
		struct dsi_display_mode_priv_info *priv_info,
		struct dsi_parser_utils *utils)
{
	int rc = 0;

	if (!priv_info) {
		DSI_ERR("kVRR dsi_panel_parse_fakeframe err: invalid mode priv info\n");
		return -EINVAL;
	}

	priv_info->fakeframe_config = 0;
	priv_info->deferred_fakeframe_time = 0;

	rc = utils->read_u32(utils->data, "oplus,adfr-fakeframe-config",
			&priv_info->fakeframe_config);
	if (rc) {
		DSI_DEBUG("kVRR failed to parse fakeframe.\n");
	}

	rc = utils->read_u32(utils->data, "oplus,adfr-fakeframe-deferred-time",
			&priv_info->deferred_fakeframe_time);
	if (rc) {
		DSI_DEBUG("kVRR failed to parse deferred_fakeframe_time.\n");
	}

	DSI_DEBUG("kVRR adfr fakeframe_config: %u, deferred_fakeframe_time: %u \n",
		priv_info->fakeframe_config, priv_info->deferred_fakeframe_time);

	return rc;
}

int dsi_panel_parse_adfr(void *dsi_mode, void *dsi_utils)
{
	struct dsi_display_mode *mode = dsi_mode;
	struct dsi_parser_utils *utils = dsi_utils;
	struct dsi_display_mode_priv_info *priv_info = mode->priv_info;

	/* qsync enhance */
	if (dsi_panel_parse_qsync_min_fps(priv_info, utils)) {
		DSI_DEBUG("kVRR adfr failed to parse qsyn min fps\n");
	}
	/* fake frame */
	if (dsi_panel_parse_fakeframe(priv_info, utils)) {
		DSI_DEBUG("kVRR adfr failed to parse fakeframe\n");
	}

	return 0;
}

/* update fakeframe status according to different situation */
int oplus_adfr_fakeframe_status_update(void *dsi_panel, bool force_disable)
{
	struct dsi_panel *panel = dsi_panel;
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	int refresh_rate = 120;

	if (!panel || !panel->cur_mode) {
		DSI_ERR("kVRR invalid params\n");
		return -EINVAL;
	}

	refresh_rate = panel->cur_mode->timing.refresh_rate;
	adfr = &panel->adfr;
	spin_lock_irqsave(&adfr->lock, flags);

	if (force_disable || !adfr->fakeframe_requested ||
	    (oplus_adfr_governor_enabled(adfr) &&
	     (adfr->idle || adfr->kernel_active_min_fps >
			      oplus_adfr_native_code(refresh_rate)))) {
		adfr->fakeframe = OPLUS_ADFR_FAKEFRAME_OFF;
	} else {
		/* if fakeframe is sent after resolution switch, local garbage issue will happen in low probability */
		if (panel->cur_h_active != panel->cur_mode->timing.h_active) {
			adfr->fakeframe = OPLUS_ADFR_FAKEFRAME_OFF;
		} else {
			if (refresh_rate == 120 || refresh_rate == 90) {
				if (adfr->sw_fps == 60) {
					/* if oplus_adfr_auto_sw_fps is 60hz, no need to send fakeframe */
					adfr->fakeframe =
						OPLUS_ADFR_FAKEFRAME_OFF;
				} else {
					adfr->fakeframe =
						OPLUS_ADFR_FAKEFRAME_ON;
				}
			} else {
				adfr->fakeframe = OPLUS_ADFR_FAKEFRAME_OFF;
			}
		}
	}
	spin_unlock_irqrestore(&adfr->lock, flags);
	SDE_ATRACE_INT("oplus_adfr_auto_fakeframe", READ_ONCE(adfr->fakeframe));

	return 0;
}

/* Add for adfr status reset */
/* reset auto mode status as panel power on and timing switch to SM */
void dsi_panel_adfr_status_reset(void *dsi_panel)
{
	struct dsi_panel *panel = dsi_panel;
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	u32 min_fps;
	u32 refresh_rate = 120;
	u32 h_skew = SDC_ADFR;
	u32 oplus_adfr_auto_min_fps_cmd = OPLUS_ADFR_AUTO_MIN_FPS_MAX;

	if ((panel == NULL) || (panel->cur_mode == NULL)) {
		DSI_ERR("kVRR Invalid params");
		return;
	}

	h_skew = panel->cur_mode->timing.h_skew;
	refresh_rate = panel->cur_mode->timing.refresh_rate;
	adfr = &panel->adfr;
	min_fps = refresh_rate == 60 ? OPLUS_ADFR_AUTO_MIN_FPS_60HZ :
				       OPLUS_ADFR_AUTO_MIN_FPS_MAX;

	if ((h_skew == SDC_ADFR) || (h_skew == SDC_MFR)) {
		/* after auto off cmd was sent, auto on cmd filter start */
		oplus_adfr_auto_on_cmd_filter_set(panel, true);

		if (refresh_rate == 90) {
			/* should +9 in auto off mode */
			if (strcmp(panel->oplus_priv.vendor_name, "TM_NT37705") && strcmp(panel->oplus_priv.vendor_name, "NT37705")
				&& strcmp(panel->oplus_priv.vendor_name, "TM_NT37705_DVT") && strcmp(panel->oplus_priv.vendor_name, "BOE_NT37705"))
				oplus_adfr_auto_min_fps_cmd = OPLUS_ADFR_AUTO_MIN_FPS_MAX + 9;
		} else {
			oplus_adfr_auto_min_fps_cmd = min_fps;
		}

		spin_lock_irqsave(&adfr->lock, flags);
		adfr->sequence++;
		adfr->kernel_active_min_fps = min_fps;
		adfr->kernel_down_candidate = min_fps;
		adfr->last_frame_ns = 0;
		adfr->frame_period_ns = 0;
		adfr->boost_until_ns = 0;
		adfr->cadence_fps = 0;
		adfr->idle = false;
		adfr->restore_pending = false;
		adfr->applied_auto_mode = OPLUS_ADFR_AUTO_OFF;
		adfr->applied_min_fps = oplus_adfr_auto_min_fps_cmd;
		adfr->mode_valid = true;
		adfr->min_fps_valid = true;
		if (!adfr->has_request) {
			adfr->auto_mode = OPLUS_ADFR_AUTO_OFF;
			adfr->min_fps = min_fps;
		}
		adfr->auto_mode_updated =
			adfr->auto_mode != OPLUS_ADFR_AUTO_OFF;
		adfr->min_fps_updated =
			adfr->auto_mode_updated ||
			((!oplus_adfr_governor_enabled(adfr) ||
			  (refresh_rate != 60 && refresh_rate != 120)) &&
			 adfr->min_fps != min_fps);
		adfr->fakeframe_updated = true;
		spin_unlock_irqrestore(&adfr->lock, flags);
		oplus_adfr_fakeframe_status_update(panel, false);
		oplus_adfr_send_min_fps_event(h_skew, min_fps);

		/* update auto mode and qsync para when timing switch or panel enable for debug */
		SDE_ATRACE_INT("oplus_adfr_auto_mode",
			       READ_ONCE(adfr->auto_mode));
		SDE_ATRACE_INT("oplus_adfr_auto_min_fps",
			       READ_ONCE(adfr->min_fps));
		SDE_ATRACE_INT("oplus_adfr_auto_mode_cmd", OPLUS_ADFR_AUTO_OFF);
		SDE_ATRACE_INT("oplus_adfr_auto_min_fps_cmd", oplus_adfr_auto_min_fps_cmd);
		SDE_ATRACE_INT("oplus_adfr_qsync_mode_minfps_cmd", 0);
	} else {
		spin_lock_irqsave(&adfr->lock, flags);
		adfr->sequence++;
		adfr->idle = false;
		adfr->restore_pending = false;
		adfr->mode_valid = false;
		adfr->min_fps_valid = false;
		spin_unlock_irqrestore(&adfr->lock, flags);
		oplus_adfr_send_min_fps_event(OPLUS_ADFR, refresh_rate);

		SDE_ATRACE_INT("oplus_adfr_auto_mode_cmd", 0);
		SDE_ATRACE_INT("oplus_adfr_auto_min_fps_cmd", 0);
		SDE_ATRACE_INT("oplus_adfr_qsync_mode_minfps_cmd", refresh_rate);
		DSI_INFO("kVRR oplus_adfr_qsync_mode_minfps_cmd %d\n", refresh_rate);
	}
	SDE_ATRACE_INT("qsync_mode_cmd", 0);
	SDE_ATRACE_INT("h_skew", h_skew);

	return;
}

/* --------------- vsync switch ---------------*/

/* ------------- mux switch ------------ */
static int oplus_dsi_display_enable_and_waiting_for_next_te_irq(struct dsi_display *display)
{
	int const switch_te_timeout = msecs_to_jiffies(1100);

	dsi_display_adfr_change_te_irq_status(display, true);
	DSI_INFO("kVRR Waiting for the next TE to switch\n");

	display->panel->vsync_switch_pending = true;
	reinit_completion(&display->switch_te_gate);

	if (!wait_for_completion_timeout(&display->switch_te_gate, switch_te_timeout)) {
		DSI_ERR("kVRR vsync switch TE check failed\n");
		dsi_display_adfr_change_te_irq_status(display, false);
		return -EINVAL;
	}

	return 0;
}

/*GPIO SWITCH: 0-TP Vsync    1-TE Vsync*/
static int oplus_dsi_display_vsync_switch_check_te(struct dsi_display *display, int level)
{
	int rc = 0;

	if ((display == NULL) || (display->panel == NULL)) {
		DSI_ERR("kVRR Invalid params");
		return -EINVAL;
	}

	if (level == display->panel->vsync_switch_gpio_level) {
		DSI_INFO("kVRR vsync_switch_gpio is already %d\n", level);
		return 0;
	}

	/* add for Filter out all vsync switch */
	if (display->panel->force_te_vsync == true) {
		DSI_INFO("kVRR force te vsync, filter other vsync switch\n");
		return 0;
	}

	if (!gpio_is_valid(display->panel->vsync_switch_gpio)) {
		DSI_ERR("kVRR vsync_switch_gpio is invalid\n");
		return -EINVAL;
	}

	oplus_dsi_display_enable_and_waiting_for_next_te_irq(display);

	if (oplus_adfr_compatibility_mode == false) {
		if (level) {
			rc = gpio_direction_output(display->panel->vsync_switch_gpio, 1);
			if (rc) {
				DSI_ERR("kVRR unable to set dir for vsync_switch_gpio, rc=%d\n", rc);
				dsi_display_adfr_change_te_irq_status(display, false);
				return rc;
			} else {
				DSI_INFO("kVRR set vsync_switch_gpio to 1\n");
			}
		} else {
			gpio_set_value(display->panel->vsync_switch_gpio, 0);
			DSI_INFO("kVRR set vsync_switch_gpio to 0\n");
		}
	}

	dsi_display_adfr_change_te_irq_status(display, false);

	display->panel->vsync_switch_gpio_level = level;
	SDE_ATRACE_INT("vsync_switch_gpio_level", display->panel->vsync_switch_gpio_level);

	return rc;
}

static int oplus_dsi_display_set_vsync_switch_gpio(struct dsi_display *display, int level)
{
	struct dsi_panel *panel = NULL;
	int rc = 0;

	/* only support in mux switch */
	if (oplus_adfr_get_vsync_mode() != OPLUS_EXTERNAL_TE_TP_VSYNC) {
		DSI_ERR("kVRR %s is not supported\n", __func__);
		return -EINVAL;
	}

	if ((display == NULL) || (display->panel == NULL))
		return -EINVAL;

	panel = display->panel;

	mutex_lock(&display->display_lock);

	if (!panel->panel_initialized) {
		if (gpio_is_valid(panel->vsync_switch_gpio)) {
			if (level) {
				rc = gpio_direction_output(panel->vsync_switch_gpio, 1);	/*TE Vsync */
				if (rc) {
					DSI_ERR("kVRR unable to set dir for vsync_switch_gpio gpio rc=%d\n", rc);
				} else {
					DSI_INFO("kVRR set vsync_switch_gpio to 1\n");
				}
			} else {
				gpio_set_value(panel->vsync_switch_gpio, 0);	/* TP Vsync */
				DSI_INFO("kVRR set vsync_switch_gpio to 0\n");
			}
			panel->vsync_switch_gpio_level = level;
			SDE_ATRACE_INT("vsync_switch_gpio_level", panel->vsync_switch_gpio_level);
		}
	} else {
		oplus_dsi_display_vsync_switch_check_te(display, level);
	}

	mutex_unlock(&display->display_lock);
	return rc;
}

static int oplus_dsi_display_get_vsync_switch_gpio(struct dsi_display *display)
{
	if ((display == NULL) || (display->panel == NULL))
		return -EINVAL;

	return display->panel->vsync_switch_gpio_level;
}


/*GPIO SWITCH: 0-TP Vsync    1-TE Vsync*/
ssize_t oplus_set_vsync_switch(struct kobject *obj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct dsi_display *display = get_main_display();
	int ret = 0;
	int vsync_switch_gpio = 0;

	if (display == NULL) {
		DSI_ERR("%s error :NULL display", __func__);
		return -EINVAL;
	}

	sscanf(buf, "%du", &vsync_switch_gpio);

	printk(KERN_INFO "kVRR %s oplus_set_vsync_switch = %d\n", __func__, vsync_switch_gpio);

	ret = oplus_dsi_display_set_vsync_switch_gpio(display, vsync_switch_gpio);
	if (ret)
		pr_err("kVRR oplus_dsi_display_set_vsync_switch_gpio(%d) fail\n", vsync_switch_gpio);

	return count;
}

ssize_t oplus_get_vsync_switch(struct kobject *obj,
	struct kobj_attribute *attr, char *buf)
{
	struct dsi_display *display = get_main_display();
	int vsync_switch_gpio = OPLUS_VSYNC_SWITCH_TE;

	if (display == NULL) {
		DSI_ERR("%s error :NULL display", __func__);
		return -EINVAL;
	}

	vsync_switch_gpio = oplus_dsi_display_get_vsync_switch_gpio(display);

	return sprintf(buf, "%d\n", vsync_switch_gpio);
}

void oplus_dsi_display_vsync_switch(void *disp, bool force_te_vsync)
{
	struct dsi_display *display = disp;
	int level = OPLUS_VSYNC_SWITCH_TE;
	int h_skew = SDC_ADFR;
	int rc = 0;

	if (!oplus_adfr_vsync_switch_is_enable()) {
		SDE_EVT32(0);
		return;
	}

	if ((display == NULL) || (display->panel == NULL) || (display->panel->cur_mode == NULL)) {
		DSI_ERR("kVRR Invalid params");
		return;
	}

	if (force_te_vsync == true) {
		if (oplus_adfr_get_vsync_mode() == OPLUS_EXTERNAL_TE_TP_VSYNC) {
			if (display->panel->vsync_switch_gpio_level == OPLUS_VSYNC_SWITCH_TP) {
				level = OPLUS_VSYNC_SWITCH_TE;
				oplus_dsi_display_vsync_switch_check_te(display, level);

				display->panel->force_te_vsync = true;
			}
		}
	} else {
		/* disable fake frame before vsync switch */
		oplus_adfr_fakeframe_status_update(display->panel, true);

		mutex_lock(&display->panel->panel_lock);
		rc = dsi_panel_tx_cmd_set(display->panel, DSI_CMD_ADFR_PRE_SWITCH);
		mutex_unlock(&display->panel->panel_lock);
		if (rc)
			DSI_ERR("kVRR [%s] failed to send DSI_CMD_ADFR_PRE_SWITCH cmds rc=%d\n", display->panel, rc);

		if (oplus_adfr_get_vsync_mode() != OPLUS_EXTERNAL_TE_TP_VSYNC) {
			DSI_DEBUG("kVRR OPLUS_EXTERNAL_TE_TP_VSYNC is not supported\n");
			return;
		}

		h_skew = display->panel->cur_mode->timing.h_skew;

		if (h_skew == OPLUS_ADFR) {
			level = OPLUS_VSYNC_SWITCH_TE;
		} else {
			level = OPLUS_VSYNC_SWITCH_TP;
		}

		oplus_dsi_display_vsync_switch_check_te(display, level);
	}
}

/* add for vsync switch in resolution switch and aod scene */
void sde_encoder_adfr_vsync_switch(void *enc) {
	struct drm_encoder *drm_enc = enc;
	struct sde_encoder_virt *sde_enc = to_sde_encoder_virt(drm_enc);
	struct drm_connector *drm_conn;
	struct drm_bridge *temp_bridge;
	struct dsi_bridge *c_bridge;
	struct dsi_display *display;
	struct dsi_panel *panel;

	if ((drm_enc == NULL) || (sde_enc->cur_master == NULL) || (sde_enc->cur_master->connector == NULL)) {
		SDE_DEBUG("kVRR : invalid drm encoder parameters\n");
		return;
	}

	drm_conn = sde_enc->cur_master->connector;
	if ((drm_conn == NULL) || (drm_conn->encoder == NULL)) {
		SDE_ERROR("kVRR : invalid drm connector parameters\n");
		return;
	}

	if (drm_bridge_chain_get_first_bridge(drm_conn->encoder) == NULL) {
		SDE_ERROR("kVRR : invalid drm connector parameters\n");
		return;
	}

	temp_bridge = drm_bridge_chain_get_first_bridge(drm_conn->encoder);
	c_bridge = to_dsi_bridge(temp_bridge);
	display = c_bridge->display;

	if ((display == NULL) || (display->panel == NULL)) {
		SDE_ERROR("kVRR : invalid dsi display parameters\n");
		return;
	}
	panel = display->panel;

	SDE_ATRACE_BEGIN("sde_encoder_adfr_vsync_switch");

	if (panel->need_vsync_switch) {
		/* wait for idle */
		sde_encoder_wait_for_event(drm_enc, MSM_ENC_TX_COMPLETE);
		/* after resolution switch and aod off , change back to tp vsync */
		/* if oplus_adfr_compatibility_mode is true, could not switch to tp vsync because hardware is not supported */
		if (oplus_adfr_compatibility_mode == false) {
			if (gpio_is_valid(panel->vsync_switch_gpio)) {
				gpio_set_value(panel->vsync_switch_gpio, 0);
				DSI_INFO("kVRR set vsync_switch_gpio to 0\n");
				panel->vsync_switch_gpio_level = OPLUS_VSYNC_SWITCH_TP;
			}
		}
		panel->need_vsync_switch = false;
		SDE_DEBUG("kVRR : vsync switch to %d\n", panel->vsync_switch_gpio_level);
		SDE_ATRACE_INT("vsync_switch_gpio_level", panel->vsync_switch_gpio_level);

		/* update fakeframe setting */
		oplus_adfr_fakeframe_status_update(panel, false);
	}

	SDE_ATRACE_END("sde_encoder_adfr_vsync_switch");
}

void sde_kms_adfr_vsync_switch(void *m_kms,
		void *d_crtc)
{
	struct msm_kms *kms = m_kms;
	struct drm_crtc *crtc = d_crtc;
	struct drm_encoder *encoder;
	struct drm_device *dev;

	if (!kms || !crtc || !crtc->state) {
		SDE_ERROR("kVRR invalid params\n");
		return;
	}

	dev = crtc->dev;

	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		if (encoder->crtc != crtc)
			continue;
		sde_encoder_adfr_vsync_switch(encoder);
	}
}

/*
 if use TP when timing switch (resolution switch), tearing happen
 it seems like DDIC does not support MIPI offset writes after resolution switching
 TE is official, so do the TE switch after timing switch because MIPI will be reset after that
 if current use TE, do nothing
*/
void oplus_adfr_resolution_vsync_switch(void *dsi_panel)
{
	int rc = 0;
	struct dsi_panel *panel = dsi_panel;

	if ((panel == NULL) || (panel->cur_mode == NULL)) {
		DSI_ERR("kVRR Invalid params");
		return;
	}

	/* just do switch when use tp vsync and resolution change */
	if ((panel->cur_h_active != panel->cur_mode->timing.h_active) && (panel->vsync_switch_gpio_level == OPLUS_VSYNC_SWITCH_TP)) {
		if (gpio_is_valid(panel->vsync_switch_gpio)) {
			rc = gpio_direction_output(panel->vsync_switch_gpio, 1);
			if (rc) {
				DSI_ERR("kVRR unable to set dir for vsync_switch_gpio gpio rc=%d\n", rc);
			} else {
				DSI_INFO("kVRR set vsync_switch_gpio to 1\n");
			}
			panel->vsync_switch_gpio_level = OPLUS_VSYNC_SWITCH_TE;
		}

		/* after one frame commit completed, change back to current mode vsync */
		panel->need_vsync_switch = true;
		SDE_ATRACE_INT("vsync_switch_gpio_level", panel->vsync_switch_gpio_level);
	}
	panel->cur_h_active = panel->cur_mode->timing.h_active;
}

/* vsync switch Entry and exit */
void oplus_adfr_aod_fod_vsync_switch(void *dsi_panel, bool force_te_vsync)
{
	struct dsi_panel *panel = dsi_panel;
	int h_skew = SDC_ADFR;
	int rc = 0;

	if (!oplus_adfr_vsync_switch_is_enable()) {
		SDE_EVT32(0);
		return;
	}

	if (panel == NULL) {
		DSI_ERR("kVRR Invalid params");
		return;
	}

	/* force switch to te vsync as tp vsync will change in aod and fod mode */
	if (force_te_vsync == true) {
		if (panel->vsync_switch_gpio_level == OPLUS_VSYNC_SWITCH_TP) {
			if (gpio_is_valid(panel->vsync_switch_gpio)) {
				rc = gpio_direction_output(panel->vsync_switch_gpio, 1);
				if (rc) {
					DSI_ERR("kVRR unable to set dir for vsync_switch_gpio gpio rc=%d\n", rc);
				} else {
					DSI_INFO("kVRR set vsync_switch_gpio to 1\n");
				}
				panel->vsync_switch_gpio_level = OPLUS_VSYNC_SWITCH_TE;
				panel->force_te_vsync = true;
				SDE_ATRACE_INT("vsync_switch_gpio_level", panel->vsync_switch_gpio_level);
			}
		}
	} else {
		/* change back to tp vysnc since aod/fod mode is off */
		if ((panel->force_te_vsync == true) && (panel->vsync_switch_gpio_level == OPLUS_VSYNC_SWITCH_TE)) {
			h_skew = panel->cur_mode->timing.h_skew;
			/* maybe change to OA in aod/fod mode */
			if (h_skew == SDC_ADFR || h_skew == SDC_MFR || h_skew == OPLUS_MFR) {
				panel->need_vsync_switch = true;
			}
			panel->force_te_vsync = false;
		}
	}
}

/* Add for vsync switch status reset */
/* switch to tp vsync since panel is no longer in aod mode after power on */
void oplus_adfr_vsync_switch_reset(void *dsi_panel)
{
	struct dsi_panel *panel = dsi_panel;
	u32 h_skew = SDC_ADFR;

	if ((panel == NULL) || (panel->cur_mode == NULL)) {
		DSI_ERR("kVRR Invalid params");
		return;
	}

	h_skew = panel->cur_mode->timing.h_skew;

	if (oplus_adfr_get_vsync_mode() == OPLUS_EXTERNAL_TE_TP_VSYNC) {
		/* reset to tp vsync after power on */
		if (panel->panel_initialized == false) {
			if (panel->force_te_vsync == true) {
				/* maybe change to OA in aod/fod mode */
				if (h_skew == SDC_ADFR || h_skew == SDC_MFR || h_skew == OPLUS_MFR) {
					/* could not change vsync gpio if the machine is incompatible with adfr */
					if (oplus_adfr_compatibility_mode == false) {
						if (gpio_is_valid(panel->vsync_switch_gpio)) {
							gpio_set_value(panel->vsync_switch_gpio, 0);
							DSI_INFO("kVRR set vsync_switch_gpio to 0\n");
							panel->vsync_switch_gpio_level = OPLUS_VSYNC_SWITCH_TP;
						}
					}
				}
				panel->force_te_vsync = false;
				SDE_ATRACE_INT("vsync_switch_gpio_level", panel->vsync_switch_gpio_level);
			}
		}
	}
}

/* ---------- te source switch --------- */
int oplus_adfr_get_vsync_source(void *dsi_panel) {
	struct dsi_panel *panel = dsi_panel;
	u32 h_skew = SDC_ADFR;

	if (!panel) {
		DSI_ERR("invalid panel params\n");
		return -EINVAL;
	}

	h_skew = panel->cur_mode->timing.h_skew;
	DSI_INFO("kVRR %s h_skew = %d", __func__, h_skew);

	if (panel->power_mode == SDE_MODE_DPMS_LP1 ||
			panel->power_mode == SDE_MODE_DPMS_LP2) {
		return OPLUS_TE_SOURCE_TE;
	}

	if ((h_skew == OPLUS_ADFR) || (h_skew == OPLUS_MFR)) {
		return OPLUS_TE_SOURCE_TE;
	} else if ((h_skew == SDC_ADFR) || (h_skew == SDC_MFR)) {
		return OPLUS_TE_SOURCE_TP;
	} else {
		DSI_ERR("kVRR %s error value", __func__);
		return -1;
	}
}

int oplus_adfr_vsync_source_switch(void *dsi_panel, u8 v_source) {
	struct dsi_panel *panel = dsi_panel;
	struct dsi_display *d_display = NULL;
	struct drm_encoder *drm_enc = NULL;
	struct sde_encoder_virt *sde_enc = NULL;

	if (!panel) {
		DSI_ERR("invalid panel params\n");
		return -EINVAL;
	}

	d_display = to_dsi_display(panel->host);
	if (!d_display) {
		DSI_ERR("invalid display params\n");
		return -EINVAL;
	}

	drm_enc = d_display->bridge->base.encoder;
	if (!drm_enc) {
		DSI_ERR("invalid encoder params\n");
		return -EINVAL;
	}

	sde_enc = to_sde_encoder_virt(drm_enc);
	if (sde_enc == NULL) {
		SDE_ERROR("invalid pointer sde_enc");
		return -EFAULT;
	}

	if (sde_enc->cur_master == NULL) {
		SDE_ERROR("invalid pointer cur_master, vsync switch failed, need switch again");
		panel->need_te_source_switch = true;
		return -EFAULT;
	}

	SDE_ATRACE_BEGIN("oplus_adfr_vsync_source_switch");

	sde_enc->te_source = v_source;
	sde_encoder_helper_switch_vsync(drm_enc, false);
	SDE_ATRACE_INT("te_source", sde_enc->te_source);

	SDE_ATRACE_END("oplus_adfr_vsync_source_switch");

	return 0;
}

int oplus_adfr_vsync_source_reset(void *enc) {
	struct drm_encoder *drm_enc = enc;
	struct sde_encoder_virt *sde_enc = NULL;
	struct drm_connector *drm_conn;
	struct sde_connector *sde_conn;
	struct drm_bridge *temp_bridge;
	struct dsi_bridge *c_bridge;
	struct dsi_display *display;
	struct dsi_panel *panel;
	u8 v_source;

	if (drm_enc == NULL) {
		SDE_ERROR("kVRR : invalid drm encoder parameters\n");
		return 0;
	}

	sde_enc = to_sde_encoder_virt(drm_enc);
	if ((sde_enc->cur_master == NULL) || (sde_enc->cur_master->connector == NULL)) {
		SDE_ERROR("kVRR : invalid sde encoder parameters\n");
		return 0;
	}

	drm_conn = sde_enc->cur_master->connector;
	if ((drm_conn == NULL) || (drm_conn->encoder == NULL)) {
		SDE_ERROR("kVRR : invalid drm connector parameters\n");
		return 0;
	}

	sde_conn = to_sde_connector(drm_conn);
	if (sde_conn == NULL) {
		SDE_ERROR("kVRR : invalid sde connector parameters\n");
		return 0;
	}
	if (sde_conn->connector_type != DRM_MODE_CONNECTOR_DSI) {
		SDE_DEBUG("kVRR : Only reset when display is dsi_display\n");
		return 0;
	}

	if (drm_bridge_chain_get_first_bridge(drm_conn->encoder) == NULL) {
		SDE_ERROR("kVRR : invalid drm bridge parameters\n");
		return 0;
	}

	temp_bridge = drm_bridge_chain_get_first_bridge(drm_conn->encoder);
	c_bridge = to_dsi_bridge(temp_bridge);
	display = c_bridge->display;

	if ((display == NULL) || (display->panel == NULL)) {
		SDE_ERROR("kVRR : invalid dsi display parameters\n");
		return 0;
	}
	panel = display->panel;

	if (panel->is_switching) {
		panel->is_switching = false;
		SDE_INFO("kVRR Don't reset TE when timing switch");
		return 0;
	}

	if (panel->power_mode == SDE_MODE_DPMS_LP1 ||
			panel->power_mode == SDE_MODE_DPMS_LP2) {
		SDE_INFO("kVRR Don't reset TE in AOD mode");
		return 0;
	}

	SDE_ATRACE_BEGIN("oplus_adfr_vsync_source_reset");

	v_source = oplus_adfr_get_vsync_source(panel);
	SDE_INFO("kVRR : vsync source switched to %d when resume\n", v_source);
	panel->need_te_source_switch = false;
	oplus_adfr_vsync_source_switch(panel, v_source);

	SDE_ATRACE_END("oplus_adfr_vsync_source_reset");

	return 0;
}

int oplus_adfr_timing_vsync_source_switch(void *dsi_panel) {
	struct dsi_panel *panel = dsi_panel;
	struct dsi_display *d_display = NULL;
	struct drm_encoder *drm_enc = NULL;
	struct sde_encoder_virt *sde_enc = NULL;
	u8 v_source;

	if (!panel) {
		DSI_ERR("invalid panel params\n");
		return -EINVAL;
	}

	d_display = to_dsi_display(panel->host);
	if (!d_display) {
		DSI_ERR("invalid display params\n");
		return -EINVAL;
	}

	if (panel->power_mode == SDE_MODE_DPMS_LP1 ||
			panel->power_mode == SDE_MODE_DPMS_LP2) {
		return 0;
	}

	drm_enc = d_display->bridge->base.encoder;
	if (!drm_enc) {
		DSI_ERR("invalid encoder params\n");
		return -EINVAL;
	}

	sde_enc = to_sde_encoder_virt(drm_enc);
	if (sde_enc == NULL) {
		SDE_ERROR("invalid pointer sde_enc");
		return -EFAULT;
	}

	SDE_ATRACE_BEGIN("oplus_adfr_timing_vsync_source_switch");

	if (panel->cur_h_active != panel->cur_mode->timing.h_active) {
		v_source = OPLUS_TE_SOURCE_TE;
		SDE_INFO("kVRR : vsync source switched to %d before resolution switch\n", v_source);
		oplus_adfr_vsync_source_switch(panel, v_source);
		panel->need_te_source_switch = true;
		SDE_INFO("kVRR : need to switch to normal after resolution switch\n");
	} else {
		v_source = oplus_adfr_get_vsync_source(panel);
		if (sde_enc->te_source != v_source) {
			SDE_INFO("kVRR : vsync source switched to %d before game mode switch\n", v_source);
			oplus_adfr_vsync_source_switch(panel, v_source);
		}
	}

	SDE_ATRACE_END("oplus_adfr_timing_vsync_source_switch");
	return 0;
}

void sde_encoder_adfr_vsync_source_switch(void *enc) {
	struct drm_encoder *drm_enc = enc;
	struct sde_encoder_virt *sde_enc = to_sde_encoder_virt(drm_enc);
	struct drm_connector *drm_conn;
	struct sde_connector *sde_conn;
	struct drm_bridge *temp_bridge;
	struct dsi_bridge *c_bridge;
	struct dsi_display *display;
	struct dsi_panel *panel;
	u8 v_source;

	if ((drm_enc == NULL) || (sde_enc->cur_master == NULL) || (sde_enc->cur_master->connector == NULL)) {
		SDE_ERROR("kVRR : invalid drm encoder parameters\n");
		return;
	}

	drm_conn = sde_enc->cur_master->connector;
	if ((drm_conn == NULL) || (drm_conn->encoder == NULL)) {
		SDE_ERROR("kVRR : invalid drm connector parameters\n");
		return;
	}

	sde_conn = to_sde_connector(drm_conn);
	if (sde_conn == NULL) {
		SDE_ERROR("kVRR : invalid sde connector parameters\n");
		return;
	}
	if (sde_conn->connector_type != DRM_MODE_CONNECTOR_DSI) {
		SDE_DEBUG("kVRR : Only switch when display is dsi_display\n");
		return;
	}

	if (drm_bridge_chain_get_first_bridge(drm_conn->encoder) == NULL) {
		SDE_ERROR("kVRR : invalid drm connector parameters\n");
		return;
	}

	temp_bridge = drm_bridge_chain_get_first_bridge(drm_conn->encoder);
	c_bridge = to_dsi_bridge(temp_bridge);
	display = c_bridge->display;

	if ((display == NULL) || (display->panel == NULL)) {
		SDE_ERROR("kVRR : invalid dsi display parameters\n");
		return;
	}
	panel = display->panel;

	SDE_ATRACE_BEGIN("sde_encoder_adfr_vsync_source_switch");

	if (panel->power_mode == SDE_MODE_DPMS_LP1 ||
			panel->power_mode == SDE_MODE_DPMS_LP2) {
		panel->need_te_source_switch = false;
	}

	if (panel->need_te_source_switch) {
		/* wait for idle */
		v_source = oplus_adfr_get_vsync_source(panel);
		sde_encoder_wait_for_event(drm_enc, MSM_ENC_TX_COMPLETE);
		SDE_INFO("kVRR : vsync source switched to %d when needed\n", v_source);
		panel->need_te_source_switch = false;
		oplus_adfr_vsync_source_switch(panel, v_source);

		/* update fakeframe setting after resolution switch */
		oplus_adfr_fakeframe_status_update(panel, false);
	}

	SDE_ATRACE_INT("te_source", sde_enc->te_source);
	SDE_ATRACE_END("sde_encoder_adfr_vsync_source_switch");
}

/* double TE */
void sde_kms_adfr_vsync_source_switch(void *m_kms,
		void *d_crtc)
{
	struct msm_kms *kms = m_kms;
	struct drm_crtc *crtc = d_crtc;
	struct drm_encoder *encoder;
	struct drm_device *dev;

	if (!kms || !crtc || !crtc->state) {
		SDE_ERROR("kVRR invalid params\n");
		return;
	}

	dev = crtc->dev;

	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		if (encoder->crtc != crtc)
			continue;
		sde_encoder_adfr_vsync_source_switch(encoder);
	}
}

/*return value*/
/*1: switch to panel TE*/
/*0: don't need to switch to panel TE*/
int dsi_panel_aod_need_vsync_source_switch(void *dsi_panel)
{
	struct dsi_panel *panel = dsi_panel;
	u32 h_skew = SDC_ADFR;

	if ((panel == NULL) || (panel->cur_mode == NULL)) {
		DSI_ERR("kVRR Invalid params");
		return 0;
	}

	h_skew = panel->cur_mode->timing.h_skew;
	DSI_INFO("kVRR %s h_skew = %d", __func__, h_skew);

	if ((h_skew == OPLUS_ADFR) || (h_skew == OPLUS_MFR)) {
		return 0;
	} else if ((h_skew == SDC_ADFR) || (h_skew == SDC_MFR)) {
		return 1;
	} else {
		DSI_ERR("kVRR %s error value", __func__);
		return 0;
	}
}

/* te_source:                    */
/* OPLUS_TE_SOURCE_TE = 0,  TE0  */
/* OPLUS_TE_SOURCE_TP = 1,  TE1  */
void sde_encoder_adfr_aod_fod_source_switch(void *dsi_display, int te_source) {
	struct drm_encoder *drm_enc = NULL;
	struct sde_encoder_virt *sde_enc = NULL;
	struct dsi_display *d_display = dsi_display;

	if (!d_display || !d_display->bridge) {
		SDE_ERROR("kVRR error: %s NULL Pointer\n", __func__);
		return;
	}

	drm_enc = d_display->bridge->base.encoder;
	sde_enc = to_sde_encoder_virt(drm_enc);
	SDE_INFO("kVRR : [from %d change to %d] in fod aod scenes\n", sde_enc->te_source, te_source);
	if(!dsi_panel_aod_need_vsync_source_switch(d_display->panel)) {
		SDE_INFO("kVRR : don't need to change te\n");
		return;
	}

	SDE_ATRACE_BEGIN("sde_encoder_adfr_aod_fod_source_switch");
	if (te_source != sde_enc->te_source) {
		sde_enc->te_source = te_source;
		if (!d_display->panel->panel_initialized) {
			/* no need to wait for idle for panel not initialized*/
			SDE_INFO("kVRR : vsync source switched to %d before panel initialized\n", sde_enc->te_source);
			sde_encoder_helper_switch_vsync(drm_enc, false);
			SDE_ATRACE_INT("aod_te_source", sde_enc->te_source);
		} else {
			/* wait for idle */
			sde_encoder_wait_for_event(drm_enc, MSM_ENC_TX_COMPLETE);
			SDE_INFO("kVRR : vsync source switched to %d after wait for idle\n", sde_enc->te_source);
			sde_encoder_helper_switch_vsync(drm_enc, false);
			SDE_ATRACE_INT("aod_te_source", sde_enc->te_source);
		}
	}
	SDE_ATRACE_END("sde_encoder_adfr_aod_fod_source_switch");
}

/* --------------- auto mode ---------------*/
/* Add for auto on cmd filter */
/* if auto on command is to be sent within the same frame, filter it out */
bool oplus_adfr_auto_on_cmd_filter_set(void *dsi_panel, bool enable)
{
	struct dsi_panel *panel = dsi_panel;

	if (panel)
		WRITE_ONCE(panel->adfr.filter_auto_on, enable);
	return enable;
}

bool oplus_adfr_auto_on_cmd_filter_get(void *dsi_panel)
{
	struct dsi_panel *panel = dsi_panel;

	return panel && READ_ONCE(panel->adfr.filter_auto_on);
}

bool oplus_adfr_has_auto_mode(u32 value)
{
	return (value & OPLUS_ADFR_AUTO_MAGIC);
}

int oplus_adfr_validate_auto_mode(u32 propval)
{
	u32 mode = OPLUS_ADFR_AUTO_MODE_VALUE(propval);

	if (!oplus_adfr_has_auto_mode(propval))
		return 0;
	if ((propval & OPLUS_ADFR_AUTO_MODE_MAGIC) &&
	    mode > OPLUS_ADFR_AUTO_IDLE)
		return -EINVAL;
	if ((propval & OPLUS_ADFR_AUTO_FAKEFRAME_MAGIC) &&
	    OPLUS_ADFR_AUTO_FAKEFRAME_VALUE(propval) > OPLUS_ADFR_FAKEFRAME_ON)
		return -EINVAL;
	if (propval & OPLUS_ADFR_AUTO_MIN_FPS_MAGIC) {
		u32 value = OPLUS_ADFR_AUTO_MIN_FPS_VALUE(propval);

		if ((propval & OPLUS_ADFR_AUTO_MODE_MAGIC) &&
		    mode == OPLUS_ADFR_AUTO_IDLE) {
			if (value > 120)
				return -EINVAL;
		} else if (value > OPLUS_ADFR_AUTO_MIN_FPS_1HZ) {
			return -EINVAL;
		}
	}
	return 0;
}

int oplus_adfr_handle_auto_mode(void *dsi_panel, u32 propval)
{
	struct dsi_panel *panel = dsi_panel;
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	u32 mode = OPLUS_ADFR_AUTO_MODE_VALUE(propval);
	bool changed = false;
	int rc;

	if (!oplus_adfr_has_auto_mode(propval))
		return 0;
	rc = oplus_adfr_validate_auto_mode(propval);
	if (rc)
		return rc;
	if (!oplus_adfr_panel_supported(panel))
		return -EOPNOTSUPP;

	adfr = &panel->adfr;
	spin_lock_irqsave(&adfr->lock, flags);
	oplus_adfr_auto_update_counter++;
	if ((propval & OPLUS_ADFR_AUTO_MODE_MAGIC) &&
	    mode == OPLUS_ADFR_AUTO_IDLE) {
		/* HWC's software FPS hint is in Hz, never a panel command code. */
		if (propval & OPLUS_ADFR_AUTO_MIN_FPS_MAGIC) {
			if (!adfr->hwc_sw_fps ||
			    adfr->sw_fps != OPLUS_ADFR_AUTO_MIN_FPS_VALUE(propval))
				adfr->sequence++;
			adfr->sw_fps = OPLUS_ADFR_AUTO_MIN_FPS_VALUE(propval);
			adfr->hwc_sw_fps = true;
			adfr->fakeframe_updated = true;
		}
		goto unlock;
	}

	changed = !adfr->has_request;
	adfr->has_request = true;
	if (propval & OPLUS_ADFR_AUTO_MODE_MAGIC) {
		if (mode != adfr->auto_mode || !adfr->mode_valid) {
			changed |= mode != adfr->auto_mode;
			adfr->auto_mode = mode;
			adfr->auto_mode_updated = true;
			adfr->min_fps_updated = true;
		}
	}
	if (propval & OPLUS_ADFR_AUTO_FAKEFRAME_MAGIC) {
		changed |= adfr->fakeframe_requested !=
			OPLUS_ADFR_AUTO_FAKEFRAME_VALUE(propval);
		adfr->fakeframe_requested =
			OPLUS_ADFR_AUTO_FAKEFRAME_VALUE(propval);
		adfr->fakeframe_updated = true;
	}
	if (propval & OPLUS_ADFR_AUTO_MIN_FPS_MAGIC) {
		u32 min_fps = OPLUS_ADFR_AUTO_MIN_FPS_VALUE(propval);

		if (min_fps != adfr->min_fps || !adfr->min_fps_valid) {
			changed |= min_fps != adfr->min_fps;
			adfr->min_fps = min_fps;
			adfr->min_fps_updated = true;
		}
	}
	if (changed)
		adfr->sequence++;
unlock:
	spin_unlock_irqrestore(&adfr->lock, flags);
	SDE_EVT32(propval);
	return 1;
}

static void oplus_adfr_update_panel_sw_fps(struct dsi_panel *panel, u32 fps)
{
	struct oplus_adfr_state *adfr = &panel->adfr;
	unsigned long flags;

	spin_lock_irqsave(&adfr->lock, flags);
	adfr->measured_fps = fps;
	if (!adfr->hwc_sw_fps && adfr->sw_fps != fps) {
		adfr->sw_fps = fps;
		adfr->fakeframe_updated = true;
		adfr->sequence++;
	}
	spin_unlock_irqrestore(&adfr->lock, flags);
}

void oplus_adfr_update_sw_fps(void *drm_crtc, u32 fps)
{
	struct drm_crtc *crtc = drm_crtc;
	struct drm_encoder *encoder;

	if (!crtc || !crtc->state || !oplus_adfr_is_support())
		return;

	drm_for_each_encoder_mask(encoder, crtc->dev, crtc->state->encoder_mask) {
		struct sde_encoder_virt *enc = to_sde_encoder_virt(encoder);
		struct dsi_panel *panel;

		if (!enc->cur_master)
			continue;
		panel = oplus_adfr_panel_from_connector(enc->cur_master->connector);
		if (!oplus_adfr_panel_supported(panel))
			continue;
		oplus_adfr_update_panel_sw_fps(panel, fps);
	}
}

static int dsi_panel_send_auto_on_dcs(struct dsi_panel *panel,
		int ctrl_idx)
{
	int rc = 0;

	/* SDC's auto, fakeframe and minfps are available only after power on */

	if (!panel) {
		DSI_ERR("kVRR invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);
	if (panel->power_mode != SDE_MODE_DPMS_ON) {
		rc = -EAGAIN;
		goto unlock;
	}
	if (!panel->cur_mode || !panel->cur_mode->priv_info ||
	    !panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_QSYNC_ON].count ||
	    !panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_QSYNC_ON].cmds) {
		rc = -EOPNOTSUPP;
		goto unlock;
	}

	DSI_INFO("kVRR ctrl:%d auto on\n", ctrl_idx);
	SDE_ATRACE_INT("oplus_adfr_auto_mode_cmd", OPLUS_ADFR_AUTO_ON);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_ON);
	if (rc)
		DSI_ERR("kVRR [%s] failed to send DSI_CMD_SET_AUTO_ON cmds rc=%d\n",
		       panel->name, rc);

unlock:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int dsi_panel_send_auto_off_dcs(struct dsi_panel *panel,
		int ctrl_idx)
{
	int rc = 0;
	/* SDC's auto, fakeframe and minfps are available only after power on */

	if (!panel) {
		DSI_ERR("kVRR invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);
	if (panel->power_mode != SDE_MODE_DPMS_ON) {
		rc = -EAGAIN;
		goto unlock;
	}
	if (!panel->cur_mode || !panel->cur_mode->priv_info ||
	    !panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_QSYNC_OFF].count ||
	    !panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_QSYNC_OFF].cmds) {
		rc = -EOPNOTSUPP;
		goto unlock;
	}

	DSI_INFO("kVRR ctrl:%d auto off\n", ctrl_idx);
	SDE_ATRACE_INT("oplus_adfr_auto_mode_cmd", OPLUS_ADFR_AUTO_OFF);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_OFF);
	if (rc) {
		DSI_ERR("kVRR [%s] failed to send DSI_CMD_SET_AUTO_OFF cmds rc=%d\n",
		       panel->name, rc);
	} else {
		/* after auto off cmd was sent, auto on cmd filter start */
		oplus_adfr_auto_on_cmd_filter_set(panel, true);
	}

unlock:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int dsi_display_auto_mode_enable(struct dsi_display *display, bool enable)
{
	int i;
	int rc = 0;

	if (!display->ctrl_count)
		return -EINVAL;
	mutex_lock(&display->display_lock);

	display_for_each_ctrl(i, display) {
		if (enable) {
			/* send the commands to enable auto mode */
			rc = dsi_panel_send_auto_on_dcs(display->panel, i);
			if (rc) {
				DSI_ERR("kVRR fail auto ON cmds rc:%d\n", rc);
				goto exit;
			}
		} else {
			/* send the commands to disbale auto mode */
			rc = dsi_panel_send_auto_off_dcs(display->panel, i);
			if (rc) {
				DSI_ERR("kVRR fail auto OFF cmds rc:%d\n", rc);
				goto exit;
			}
		}
	}

exit:
	SDE_EVT32(enable, rc);
	mutex_unlock(&display->display_lock);
	return rc;
}

/* prevent the wrong min fps setting */
static int dsi_panel_auto_minfps_check(struct dsi_panel *panel,
				       u32 extend_frame, bool idle)
{
	int h_skew = panel->cur_mode->timing.h_skew;
	int refresh_rate = panel->cur_mode->timing.refresh_rate;
	u32 minfps_exidle = oplus_adfr_exit_idle_minfps;
	bool auto_mode =
		READ_ONCE(panel->adfr.applied_auto_mode) != OPLUS_ADFR_AUTO_OFF;
	const char *vendor = panel->oplus_priv.vendor_name;

	if (h_skew == SDC_ADFR) {
		if (refresh_rate == 90) {
			if (auto_mode)
				return OPLUS_ADFR_AUTO_MIN_FPS_MAX;
			if (vendor && (!strcmp(vendor, "TM_NT37705") ||
				       !strcmp(vendor, "NT37705") ||
				       !strcmp(vendor, "TM_NT37705_DVT") ||
				       !strcmp(vendor, "BOE_NT37705")))
				return extend_frame;
			/* Preserve the panel's special fixed-90-Hz command. */
			return OPLUS_ADFR_AUTO_MIN_FPS_MAX + 9;
		}
		if (refresh_rate == 120) {
			if (extend_frame > OPLUS_ADFR_AUTO_MIN_FPS_1HZ)
				return OPLUS_ADFR_AUTO_MIN_FPS_MAX;
			if (!auto_mode && !idle && extend_frame > minfps_exidle)
				return minfps_exidle;
		}
	} else if (h_skew == SDC_MFR) {
		if (extend_frame < OPLUS_ADFR_AUTO_MIN_FPS_60HZ ||
		    extend_frame > OPLUS_ADFR_AUTO_MIN_FPS_1HZ)
			return OPLUS_ADFR_AUTO_MIN_FPS_60HZ;
		if (!idle && extend_frame > minfps_exidle)
			return minfps_exidle;
	}
	return extend_frame;
}

static int dsi_panel_send_auto_minfps_dcs(struct dsi_panel *panel, int ctrl_idx,
					  u32 extend_frame, bool idle,
					  u32 *applied)
{
	int rc = 0;
	struct dsi_display_mode *mode;
	struct dsi_cmd_desc *cmds;
	size_t tx_len;
	size_t k;
	u8 *tx_buf;
	u32 count;

	/* SDC's auto, fakeframe and minfps are available only after power on */

	if (!panel || !applied) {
		DSI_ERR("kVRR invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);
	if (panel->power_mode != SDE_MODE_DPMS_ON) {
		DSI_INFO("kVRR ignore %s %u when power is %d", __FUNCTION__, extend_frame, panel->power_mode);
		rc = -EAGAIN;
		goto exit;
	}
	if (!panel->cur_mode || !panel->cur_mode->priv_info) {
		rc = -EINVAL;
		goto exit;
	}

	mode = panel->cur_mode;

	/*check minfps*/
	DSI_DEBUG("kVRR %s min fps %u .\n", __func__, extend_frame);
	extend_frame = dsi_panel_auto_minfps_check(panel, extend_frame, idle);

	/*update the sdc min fps cmds*/
	if (READ_ONCE(panel->adfr.applied_auto_mode) == OPLUS_ADFR_AUTO_OFF) {
		cmds = mode->priv_info->cmd_sets[DSI_CMD_QSYNC_MIN_FPS_0].cmds;
		count = mode->priv_info->cmd_sets[DSI_CMD_QSYNC_MIN_FPS_0].count;

		if (count == 0) {
			DSI_INFO("kVRR [%s] No commands to be sent for manual min fps.\n", panel->name);
			rc = -EOPNOTSUPP;
			goto exit;
		}

		if (!cmds || count <= SDC_MANUAL_MIN_FPS_CMD_OFFSET) {
			DSI_ERR("kVRR [%s] No commands to be sent for manual min fps, wrong cmds count.\n", panel->name);
			rc = -EINVAL;
			goto exit;
		}

		/*update manual min fps*/
		tx_len = cmds[SDC_MANUAL_MIN_FPS_CMD_OFFSET].msg.tx_len;
		tx_buf = (u8 *)cmds[SDC_MANUAL_MIN_FPS_CMD_OFFSET].msg.tx_buf;
		if (!tx_buf || tx_len != SDC_MIN_FPS_CMD_SIZE) {
			DSI_ERR("kVRR [%s] No commands to be sent for manual min fps, wrong cmds size %u.\n", panel->name, tx_len);
			rc = -EINVAL;
			goto exit;
		}

		tx_buf[SDC_MIN_FPS_CMD_SIZE-1] = extend_frame;
		DSI_DEBUG("kVRR send manual min fps %u .\n", extend_frame);
		for (k = 0; k < tx_len; k++) {
			DSI_DEBUG("kVRR manual min fps %02x", tx_buf[k]);
		}

		DSI_DEBUG("kVRR ctrl:%d manual min fps\n", ctrl_idx);
		SDE_ATRACE_INT("oplus_adfr_auto_min_fps_cmd", extend_frame);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_QSYNC_MIN_FPS_0);
		if (rc)
			DSI_ERR("kVRR [%s] failed to send DSI_CMD_QSYNC_MIN_FPS_0 cmds rc=%d\n",
				panel->name, rc);
	} else {
		cmds = mode->priv_info->cmd_sets[DSI_CMD_QSYNC_MIN_FPS_1].cmds;
		count = mode->priv_info->cmd_sets[DSI_CMD_QSYNC_MIN_FPS_1].count;

		if (count == 0) {
			DSI_DEBUG("kVRR [%s] No commands to be sent for auto min fps.\n", panel->name);
			rc = -EOPNOTSUPP;
			goto exit;
		}

		if (!cmds || count <= SDC_AUTO_MIN_FPS_CMD_OFFSET) {
			DSI_ERR("kVRR [%s] No commands to be sent for auto min fps, wrong cmds count.\n", panel->name);
			rc = -EINVAL;
			goto exit;
		}
		/*update auto min fps*/
		tx_len = cmds[SDC_AUTO_MIN_FPS_CMD_OFFSET].msg.tx_len;
		tx_buf = (u8 *)cmds[SDC_AUTO_MIN_FPS_CMD_OFFSET].msg.tx_buf;
		if (!tx_buf || tx_len != SDC_MIN_FPS_CMD_SIZE) {
			DSI_ERR("kVRR [%s] No commands to be sent for auto min fps, wrong cmds size %u.\n", panel->name, tx_len);
			rc = -EINVAL;
			goto exit;
		}

		tx_buf[SDC_MIN_FPS_CMD_SIZE-1] = extend_frame;
		DSI_INFO("kVRR send auto min fps %u .\n", extend_frame);
		for (k = 0; k < tx_len; k++) {
			DSI_DEBUG("kVRR auto min fps %02x", tx_buf[k]);
		}

		DSI_DEBUG("kVRR ctrl:%d auto min fps\n", ctrl_idx);
		SDE_ATRACE_INT("oplus_adfr_auto_min_fps_cmd", extend_frame);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_QSYNC_MIN_FPS_1);
		if (rc)
			DSI_ERR("kVRR [%s] failed to send DSI_CMD_QSYNC_MIN_FPS_1 cmds rc=%d\n",
				panel->name, rc);
	}

	if (!rc)
		*applied = extend_frame;

exit:
	SDE_EVT32(extend_frame, rc);
	mutex_unlock(&panel->panel_lock);
	return rc;
}

static int dsi_display_auto_mode_min_fps(struct dsi_display *display,
					 u32 extend_frame, bool idle,
					 u32 *applied)
{
	int i;
	int rc = 0;

	if (!display->ctrl_count)
		return -EINVAL;
	mutex_lock(&display->display_lock);

	display_for_each_ctrl(i, display) {
		/* send the commands to set auto mode min fps */
		rc = dsi_panel_send_auto_minfps_dcs(display->panel, i, extend_frame, idle, applied);
		if (rc) {
			DSI_ERR("kVRR fail auto Min Fps cmds rc:%d\n", rc);
			goto exit;
		}
	}

exit:
	SDE_EVT32(extend_frame, rc);
	mutex_unlock(&display->display_lock);
	return rc;
}

static bool oplus_adfr_retry_blocked(const struct oplus_adfr_state *adfr, const void *timing)
{
	return (adfr->last_error == -EOPNOTSUPP || adfr->last_error == -EINVAL) &&
		adfr->failed_timing == timing && adfr->failed_sequence == adfr->sequence;
}

int dsi_display_auto_mode_update(void *dsi_display)
{
	struct dsi_display *display = dsi_display;
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	u64 sequence;
	u32 mode, min_fps, applied = 0;
	bool update_mode, update_min, update_fakeframe, kernel_active;
	bool notify = false;
	const void *timing;
	int h_skew = SDC_ADFR;
	int rc = 0;

	if (!display || !display->panel || !display->panel->cur_mode) {
		DSI_ERR("kVRR dsi_display_auto_mode_update Invalid params\n");
		return -EINVAL;
	}
	if (!oplus_adfr_panel_supported(display->panel))
		return 0;

	adfr = &display->panel->adfr;
	timing = display->panel->cur_mode;
	h_skew = display->panel->cur_mode->timing.h_skew;
	if ((h_skew != SDC_ADFR) && (h_skew != SDC_MFR)) {
		/* DSI_ERR("kVRR OPLUS ADFR does not support auto mode setting\n"); */
		return 0;
	}

	/* Most frames have no panel policy update: avoid the sleeping mutex. */
	spin_lock_irqsave(&adfr->lock, flags);
	if (oplus_adfr_retry_blocked(adfr, timing)) {
		spin_unlock_irqrestore(&adfr->lock, flags);
		return -EAGAIN;
	}
	if (adfr->idle || adfr->restore_pending ||
	    (adfr->mode_valid && adfr->min_fps_valid &&
	     !adfr->auto_mode_updated && !adfr->min_fps_updated &&
	     !adfr->fakeframe_updated)) {
		spin_unlock_irqrestore(&adfr->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&adfr->lock, flags);

	SDE_ATRACE_BEGIN("dsi_display_auto_mode_update");
	mutex_lock(&adfr->update_lock);
	spin_lock_irqsave(&adfr->lock, flags);
	sequence = adfr->sequence;
	mode = adfr->auto_mode;
	kernel_active = oplus_adfr_governor_enabled(adfr) &&
		(display->panel->cur_mode->timing.refresh_rate == 120 ||
		 display->panel->cur_mode->timing.refresh_rate == 60);
	min_fps = kernel_active ? adfr->kernel_active_min_fps : adfr->min_fps;
	update_mode = adfr->auto_mode_updated || !adfr->mode_valid;
	update_min = adfr->min_fps_updated || !adfr->min_fps_valid;
	update_fakeframe = adfr->fakeframe_updated;
	/* The first frame after idle restores native FPS before kickoff. */
	if (adfr->idle || adfr->restore_pending ||
	    oplus_adfr_retry_blocked(adfr, timing)) {
		spin_unlock_irqrestore(&adfr->lock, flags);
		goto exit;
	}
	spin_unlock_irqrestore(&adfr->lock, flags);

	if (update_mode) {
		if (mode == OPLUS_ADFR_AUTO_ON &&
		    oplus_adfr_auto_on_cmd_filter_get(display->panel)) {
			rc = -EAGAIN;
			goto exit;
		}
		rc = dsi_display_auto_mode_enable(display, mode);
		if (rc) {
			spin_lock_irqsave(&adfr->lock, flags);
			adfr->mode_valid = false;
			adfr->auto_mode_updated = true;
			adfr->last_error = rc;
			adfr->failed_timing = timing;
			adfr->failed_sequence = sequence;
			spin_unlock_irqrestore(&adfr->lock, flags);
			goto exit;
		}
		spin_lock_irqsave(&adfr->lock, flags);
		if (sequence != adfr->sequence) {
			adfr->mode_valid = false;
			adfr->auto_mode_updated = true;
			spin_unlock_irqrestore(&adfr->lock, flags);
			rc = -EAGAIN;
			goto exit;
		}
		adfr->applied_auto_mode = mode;
		adfr->mode_valid = true;
		adfr->auto_mode_updated = false;
		adfr->last_error = 0;
		spin_unlock_irqrestore(&adfr->lock, flags);
		SDE_ATRACE_INT("oplus_adfr_applied_auto_mode", mode);
	}

	if (update_min) {
		/* A wakeup may already have restored exactly this native code. */
		spin_lock_irqsave(&adfr->lock, flags);
		if (kernel_active && !update_mode && adfr->min_fps_valid &&
		    adfr->applied_min_fps == min_fps && sequence == adfr->sequence) {
			adfr->min_fps_updated = false;
			update_min = false;
		}
		spin_unlock_irqrestore(&adfr->lock, flags);
	}
	if (update_min) {
		if (oplus_adfr_skip_min_fps_cmd) {
			DSI_INFO("kVRR skip min fps %u setting\n", min_fps);
		} else {
			rc = dsi_display_auto_mode_min_fps(display, min_fps,
							   kernel_active, &applied);
			if (rc) {
				spin_lock_irqsave(&adfr->lock, flags);
				adfr->min_fps_valid = false;
				adfr->min_fps_updated = true;
				adfr->last_error = rc;
				adfr->failed_timing = timing;
				adfr->failed_sequence = sequence;
				spin_unlock_irqrestore(&adfr->lock, flags);
				goto exit;
			}
			spin_lock_irqsave(&adfr->lock, flags);
			if (sequence != adfr->sequence) {
				adfr->min_fps_valid = false;
				adfr->min_fps_updated = true;
				spin_unlock_irqrestore(&adfr->lock, flags);
				rc = -EAGAIN;
				goto exit;
			}
			adfr->applied_min_fps = applied;
			adfr->min_fps_valid = true;
			adfr->min_fps_updated = false;
			adfr->last_error = 0;
			spin_unlock_irqrestore(&adfr->lock, flags);
			SDE_ATRACE_INT("oplus_adfr_applied_min_fps", applied);
			if (kernel_active)
				pr_info_ratelimited("kVRR %s active %s: %u Hz (code=0x%02x)\n",
						    "kernel", "update",
						    120 / (applied + 1), applied);
			notify = true;
		}
	}

	if (update_fakeframe) {
		rc = oplus_adfr_fakeframe_status_update(display->panel, false);
		if (rc)
			goto exit;
		spin_lock_irqsave(&adfr->lock, flags);
		if (sequence == adfr->sequence)
			adfr->fakeframe_updated = false;
		spin_unlock_irqrestore(&adfr->lock, flags);
	}
exit:
	mutex_unlock(&adfr->update_lock);
	if (notify)
		oplus_adfr_send_min_fps_event(h_skew, applied);
	SDE_ATRACE_INT("oplus_adfr_update_rc", rc);
	SDE_ATRACE_END("dsi_display_auto_mode_update");

	return rc;
}

int oplus_adfr_send_min_fps_event(unsigned int h_skew, unsigned int min_fps)
{
	unsigned int data = min_fps;

	DSI_DEBUG("KVRR start\n");

	if (h_skew == OPLUS_ADFR) {
		if (min_fps >= 120) {
			data = OPLUS_ADFR_AUTO_MIN_FPS_MAX;
			pr_info("Using 120HZ refresh rate");
		} else if (min_fps >= 60) {
			data = OPLUS_ADFR_AUTO_MIN_FPS_60HZ;
			pr_info("Using 60HZ refresh rate");
		} else if (min_fps >= 40) {
			data = OPLUS_ADFR_AUTO_MIN_FPS_40HZ;
			pr_info("Using 40HZ refresh rate");
		} else if (min_fps >= 30) {
			data = OPLUS_ADFR_AUTO_MIN_FPS_30HZ;
			pr_info("Using 30HZ refresh rate");
		} else if (min_fps >= 24) {
			data = OPLUS_ADFR_AUTO_MIN_FPS_24HZ;
			pr_info("Using 24HZ refresh rate");
		} else if (min_fps >= 20) {
			data = OPLUS_ADFR_AUTO_MIN_FPS_20HZ;
			pr_info("Using 20HZ refresh rate");
		} else if (min_fps >= 10) {
			data = OPLUS_ADFR_AUTO_MIN_FPS_10HZ;
			pr_info("Using 20HZ refresh rate");
		} else {
			/* generally greater than 20 */
			data = OPLUS_ADFR_AUTO_MIN_FPS_1HZ;
			pr_info("Using 1HZ refresh rate");
		}
	}

	oplus_event_data_notifier_trigger(DRM_PANEL_EVENT_ADFR_MIN_FPS, data, true);

	DSI_DEBUG("kVRR DRM_PANEL_EVENT_ADFR_MIN_FPS:%u\n", data);

	DSI_DEBUG("KVRR end\n");

	return 0;
}

/* the highest min fps setting is required when the temperature meets certain conditions, otherwise recovery it */
int oplus_adfr_temperature_detection_handle(void *dsi_display, int ntc_temp, int shell_temp)
{
	static bool last_oplus_adfr_skip_min_fps_cmd = false;
	struct dsi_display *display = dsi_display;
	struct oplus_adfr_state *adfr;
	unsigned long flags;

	if (!oplus_adfr_temperature_detection_is_enable()) {
		return 0;
	}

	if (!display || !display->panel || !display->panel->cur_mode)
		return -EINVAL;
	adfr = &display->panel->adfr;
	oplus_adfr_skip_min_fps_cmd = false;
	if (last_oplus_adfr_skip_min_fps_cmd) {
		spin_lock_irqsave(&adfr->lock, flags);
		adfr->min_fps_updated = true;
		adfr->sequence++;
		spin_unlock_irqrestore(&adfr->lock, flags);
		DSI_INFO("KVRR ntc_temp:%d,shell_temp:%d,need to recover min fps\n",
			 ntc_temp, shell_temp);
	}

	last_oplus_adfr_skip_min_fps_cmd = oplus_adfr_skip_min_fps_cmd;

	return 0;
}

/* --------------- idle mode ---------------*/
void oplus_adfr_idle_mode_minfps_delay(void *sde_encoder_phys_cmd)
{
	struct sde_encoder_phys_cmd *cmd_enc = sde_encoder_phys_cmd;
	s64 us_per_frame = 0;
	struct sde_encoder_phys_cmd_te_timestamp *te_timestamp = NULL;
	ktime_t last_te_timestamp = 0;
	s64 delay = 0;
	s64 elapsed_us;
	unsigned long flags;

	if (!cmd_enc) {
		SDE_ERROR("kVRR : invalid sde_encoder_phys_cmd parameters\n");
		return;
	}

	SDE_ATRACE_BEGIN("oplus_adfr_idle_mode_minfps_delay");

	/* for tp vysnc shift issue */
	us_per_frame = 1000000 / 60;
	spin_lock_irqsave(cmd_enc->base.enc_spinlock, flags);
	if (!list_empty(&cmd_enc->te_timestamp_list)) {
		te_timestamp = list_last_entry(&cmd_enc->te_timestamp_list,
					       struct sde_encoder_phys_cmd_te_timestamp, list);
		last_te_timestamp = te_timestamp->timestamp;
	}
	spin_unlock_irqrestore(cmd_enc->base.enc_spinlock, flags);
	elapsed_us = ktime_us_delta(ktime_get(), last_te_timestamp);
	if (!last_te_timestamp || elapsed_us < 0)
		goto exit;

	delay = (us_per_frame >> 1) - (elapsed_us % us_per_frame);
	SDE_DEBUG("kVRR time interval since the last rd_ptr is %lu\n", ktime_to_us(ktime_sub(ktime_get(), last_te_timestamp)));

	if (delay > 0) {
		/* make sure to send min fps in the next 8.3ms period */
		delay = delay + 1000;
		usleep_range(delay, delay + 100);
		SDE_DEBUG("kVRR delay %lu us for idle min fps setting\n", delay);
	}

exit:
	SDE_ATRACE_END("oplus_adfr_idle_mode_minfps_delay");

	return;
}

static bool oplus_adfr_hbm_active(void)
{
	if (oplus_display_panel_get_global_hbm_status())
		return true;
#ifdef OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT
	return oplus_ofp_is_supported() && oplus_ofp_get_hbm_state();
#else
	return false;
#endif
}

static struct dsi_display *oplus_adfr_encoder_display(void *sde_enc_v)
{
	struct sde_encoder_virt *enc = sde_enc_v;
	struct sde_encoder_phys *phys;
	struct dsi_panel *panel;
	struct dsi_display *display;
	struct sde_connector *connector;
	u32 rate, skew;

	if (!enc || !enc->cur_master)
		return NULL;
	phys = enc->cur_master;
	if (phys->intf_mode != INTF_MODE_CMD || !phys->connector ||
	    phys->connector->connector_type != DRM_MODE_CONNECTOR_DSI)
		return NULL;
	connector = to_sde_connector(phys->connector);
	display = connector->display;
	if (!display || !display->panel || !display->panel->cur_mode)
		return NULL;
	panel = display->panel;
	rate = panel->cur_mode->timing.refresh_rate;
	skew = panel->cur_mode->timing.h_skew;
	if (!oplus_adfr_panel_supported(panel) ||
	    !oplus_adfr_idle_mode_is_enable() ||
	    !READ_ONCE(panel->adfr.kernel_idle_enabled) ||
	    READ_ONCE(panel->adfr.auto_mode) != OPLUS_ADFR_AUTO_OFF ||
	    !ADFR_GET_IDLE_MODE_CONFIG(panel->adfr.config) ||
	    (rate != 60 && rate != 120) || (skew != SDC_ADFR && skew != SDC_MFR) ||
	    READ_ONCE(panel->power_mode) != SDE_MODE_DPMS_ON)
		return NULL;
	return display;
}

void oplus_adfr_frame_start(void *sde_enc_v)
{
	struct dsi_display *display = oplus_adfr_encoder_display(sde_enc_v);
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	bool native;

	if (!display)
		return;
	adfr = &display->panel->adfr;
	native = oplus_adfr_hbm_active();
#ifdef OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT
	{
		struct sde_encoder_virt *enc = sde_enc_v;
		struct drm_connector *conn = enc->cur_master->connector;
		u64 hbm = 0;

		if (conn->state)
			hbm = sde_connector_get_property(conn->state, CONNECTOR_PROP_HBM_ENABLE);
		if (hbm & (OPLUS_OFP_PROPERTY_DIM_LAYER | OPLUS_OFP_PROPERTY_FINGERPRESS_LAYER))
			native = true;
	}
#endif
	spin_lock_irqsave(&adfr->lock, flags);
	oplus_adfr_governor_frame(adfr, ktime_get_ns(),
				  display->panel->cur_mode->timing.refresh_rate,
				  native || adfr->idle || adfr->restore_pending);
	spin_unlock_irqrestore(&adfr->lock, flags);
	dsi_display_auto_mode_update(display);
}

/* Input callbacks only update policy; all DSI I/O stays in process context. */
void oplus_adfr_touch_event(void *sde_enc_v)
{
	struct dsi_display *display = oplus_adfr_encoder_display(sde_enc_v);
	struct oplus_adfr_state *adfr;
	unsigned long flags;

	if (!display)
		return;
	adfr = &display->panel->adfr;
	spin_lock_irqsave(&adfr->lock, flags);
	oplus_adfr_governor_touch(adfr, ktime_get_ns(),
				  display->panel->cur_mode->timing.refresh_rate);
	spin_unlock_irqrestore(&adfr->lock, flags);
}

void oplus_adfr_touch_work(void *sde_enc_v)
{
	struct dsi_display *display = oplus_adfr_encoder_display(sde_enc_v);
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	bool enabled;

	if (!display)
		return;
	adfr = &display->panel->adfr;
	spin_lock_irqsave(&adfr->lock, flags);
	enabled = oplus_adfr_governor_enabled(adfr);
	spin_unlock_irqrestore(&adfr->lock, flags);
	if (!enabled)
		return;
	oplus_adfr_handle_idle_mode(sde_enc_v, false);
	dsi_display_auto_mode_update(display);
}

u32 oplus_adfr_idle_delay_ms(void *sde_enc_v)
{
	struct dsi_display *display = oplus_adfr_encoder_display(sde_enc_v);
	struct oplus_adfr_state *adfr;
	unsigned long flags;
	u32 delay;

	if (!display)
		return 0;
	adfr = &display->panel->adfr;
	spin_lock_irqsave(&adfr->lock, flags);
	delay = oplus_adfr_governor_idle_delay(adfr, ktime_get_ns());
	spin_unlock_irqrestore(&adfr->lock, flags);
	return delay;
}

static u32 oplus_adfr_idle_target(const struct oplus_adfr_state *adfr)
{
	return adfr->kernel_idle_enabled ? adfr->kernel_idle_min_fps : adfr->min_fps;
}

static bool oplus_adfr_can_enter_idle(const struct oplus_adfr_state *adfr)
{
	u32 target = oplus_adfr_idle_target(adfr);

	return adfr->mode_valid && adfr->min_fps_valid &&
	       !adfr->auto_mode_updated && !adfr->min_fps_updated &&
	       !adfr->restore_pending && !adfr->idle &&
	       adfr->auto_mode == OPLUS_ADFR_AUTO_OFF &&
	       adfr->applied_auto_mode == OPLUS_ADFR_AUTO_OFF &&
	       (adfr->kernel_idle_enabled ||
		target > oplus_adfr_exit_idle_minfps) &&
	       target <= OPLUS_ADFR_AUTO_MIN_FPS_1HZ;
}

static int oplus_adfr_apply_idle(struct dsi_display *display,
				 struct sde_encoder_phys_cmd *cmd_enc,
				 bool enter_idle)
{
	struct dsi_panel *panel = display->panel;
	struct oplus_adfr_state *adfr = &panel->adfr;
	unsigned long flags;
	u32 h_skew = panel->cur_mode->timing.h_skew;
	u32 refresh_rate = panel->cur_mode->timing.refresh_rate;
	u32 requested, applied = 0;
	u64 sequence;
	bool notify = false;
	bool kernel_idle;
	int rc = 0;

	if ((h_skew != SDC_ADFR && h_skew != SDC_MFR) ||
	    (refresh_rate != 120 && refresh_rate != 60))
		return 0;
	/* Do not pay the 60-Hz phase delay for a command that cannot be sent. */
	if (READ_ONCE(panel->power_mode) != SDE_MODE_DPMS_ON)
		return -EAGAIN;
	if (enter_idle && oplus_adfr_hbm_active())
		return 0;
	mutex_lock(&adfr->update_lock);
	spin_lock_irqsave(&adfr->lock, flags);
	sequence = adfr->sequence;
	kernel_idle = adfr->kernel_idle_enabled;
	if (enter_idle ? !oplus_adfr_can_enter_idle(adfr) :
			 (!adfr->idle && !adfr->restore_pending)) {
		spin_unlock_irqrestore(&adfr->lock, flags);
		goto exit;
	}
	if (enter_idle && oplus_adfr_governor_idle_delay(adfr, ktime_get_ns())) {
		spin_unlock_irqrestore(&adfr->lock, flags);
		goto exit;
	}
	/* Restore native FPS for the first frame, then reapply the active floor. */
	requested = enter_idle ?
			    oplus_adfr_idle_target(adfr) :
			    (refresh_rate == 60 ? OPLUS_ADFR_AUTO_MIN_FPS_60HZ :
						  OPLUS_ADFR_AUTO_MIN_FPS_MAX);
	spin_unlock_irqrestore(&adfr->lock, flags);
	if (refresh_rate == 60)
		oplus_adfr_idle_mode_minfps_delay(cmd_enc);
	rc = dsi_display_auto_mode_min_fps(display, requested, enter_idle,
					   &applied);
	spin_lock_irqsave(&adfr->lock, flags);
	if (rc || sequence != adfr->sequence) {
		adfr->restore_pending = true;
		adfr->min_fps_valid = false;
		adfr->min_fps_updated = true;
		adfr->last_error = rc ? rc : -EAGAIN;
		if (!rc)
			rc = -EAGAIN;
	} else {
		adfr->applied_min_fps = applied;
		adfr->min_fps_valid = true;
		adfr->idle = enter_idle;
		adfr->restore_pending = false;
		adfr->last_error = 0;
		if (!enter_idle)
			adfr->min_fps_updated = true;
		notify = true;
	}
	spin_unlock_irqrestore(&adfr->lock, flags);
exit:
	mutex_unlock(&adfr->update_lock);
	if (notify) {
		pr_info_ratelimited("kVRR %s idle %s: %u Hz (code=0x%02x)\n",
				    kernel_idle ? "kernel" : "hwc",
				    enter_idle ? "enter" : "exit",
				    applied ? 120 / (applied + 1) : refresh_rate,
				    applied);
		SDE_ATRACE_INT("oplus_adfr_applied_min_fps", applied);
		oplus_adfr_send_min_fps_event(h_skew, applied);
		if (enter_idle &&
		    oplus_adfr_dynamic_te.config !=
			    OPLUS_ADFR_DYNAMIC_TE_DISABLE &&
		    applied >= OPLUS_ADFR_AUTO_MIN_FPS_10HZ)
			hrtimer_start(&oplus_adfr_dynamic_te.timer,
				      ms_to_ktime(10), HRTIMER_MODE_REL);
	}
	return rc;
}

void oplus_adfr_handle_idle_mode(void *sde_enc_v, int enter_idle)
{
	struct sde_encoder_virt *sde_enc = sde_enc_v;
	struct sde_encoder_phys *phys;
	struct sde_connector *connector;
	struct dsi_display *display;
	int rc;

	if (!sde_enc)
		return;
	phys = sde_enc->cur_master;
	if (!phys || phys->intf_mode != INTF_MODE_CMD || !phys->connector)
		return;
	connector = to_sde_connector(phys->connector);
	if (connector->connector_type != DRM_MODE_CONNECTOR_DSI)
		return;
	display = connector->display;
	if (!display || !display->panel || !display->panel->cur_mode)
		return;
	if (enter_idle &&
	    (!oplus_adfr_panel_supported(display->panel) ||
	     !oplus_adfr_idle_mode_is_enable() ||
	     !ADFR_GET_IDLE_MODE_CONFIG(display->panel->adfr.config) ||
	     READ_ONCE(oplus_adfr_skip_min_fps_cmd)))
		return;

	SDE_ATRACE_BEGIN("oplus_adfr_handle_idle_mode");
	rc = oplus_adfr_apply_idle(display, to_sde_encoder_phys_cmd(phys),
				   enter_idle);
	if (rc && rc != -EAGAIN)
		DSI_ERR("kVRR %s idle min fps failed: %d\n",
			enter_idle ? "enter" : "exit", rc);
	SDE_ATRACE_INT("oplus_adfr_idle_mode",
		       READ_ONCE(display->panel->adfr.idle));
	SDE_ATRACE_END("oplus_adfr_handle_idle_mode");
}

int oplus_enable_te_refcount(void *data)
{
	unsigned int *te_enable =  (unsigned int *)data;
	struct dsi_display *display = NULL;

	if (!te_enable)
		return -EINVAL;
	DSI_INFO("%s te_enable = %d", __func__, (*te_enable));

	display = get_main_display();
	if (display == NULL) {
		pr_err("%s error :NULL display", __func__);
		return -1;
	}

	if ((*te_enable) == 1) {
		te_refcount.te_calculate_enable = true;
		te_refcount.start_timeline = ktime_get();
		te_refcount.te_refcount = 0;
	} else if ((*te_enable) == 0) {
		te_refcount.te_calculate_enable = false;
		te_refcount.end_timeline = ktime_get();
	}

	dsi_display_adfr_change_te_irq_status(display, te_refcount.te_calculate_enable);

	return 0;
}

int oplus_get_te_fps(void *data)
{
	unsigned int *te_fps =  (unsigned int *)data;
	s64 elapsed_ns;
	u64 count;

	if (!te_fps)
		return -EINVAL;
	elapsed_ns =
		ktime_to_ns(ktime_sub(READ_ONCE(te_refcount.end_timeline),
				      READ_ONCE(te_refcount.start_timeline)));
	if (elapsed_ns <= 0) {
		pr_err("%s error :out of time", __func__);
		return -EAGAIN;
	}
	count = READ_ONCE(te_refcount.te_refcount);
	*te_fps = div64_u64(count * NSEC_PER_SEC, elapsed_ns);
	DSI_INFO("%s te count = %llu, interval = %lld ns, te fps = %u",
		 __func__, count, elapsed_ns, *te_fps);

	return 0;
}

