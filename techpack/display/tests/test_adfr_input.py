#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real input callback and worker with VM/RC fault injection."""

import unittest

from test_hotpath import DISPLAY, definition, run_c


SOURCE = DISPLAY / "msm/sde/sde_encoder.c"

MOCK = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define OPLUS_FEATURE_DISPLAY 1
#define EV_ABS 3
#define EV_KEY 1
#define ABS_MT_POSITION_X 53
#define ABS_MT_POSITION_Y 54
#define ABS_MT_TRACKING_ID 57
#define BTN_TOUCH 330
#define EINVAL 22
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define SDE_DEBUG_ENC(...) do {} while (0)
#define SDE_ERROR(...) do {} while (0)
#define SDE_EVT32_VERBOSE(...) do {} while (0)
#define SDE_ENC_RC_STATE_ON 1
#define SDE_ENC_RC_EVENT_EARLY_WAKEUP 4
struct kthread_work { int unused; };
struct kthread_worker { int unused; };
struct msm_drm_thread { struct kthread_worker worker; void *thread; };
struct msm_drm_private {
    struct msm_drm_thread disp_thread[2], adfr_thread[2];
};
struct drm_device { struct msm_drm_private *dev_private; };
struct drm_encoder { struct drm_device *dev; };
struct drm_crtc { size_t index; };
struct input_handler { void *private; };
struct input_handle { struct input_handler *handler; };
struct sde_encoder_virt {
    struct drm_encoder base;
    struct drm_crtc *crtc;
    struct input_handler *input_handler;
    struct kthread_work input_event_work;
    int rc_lock, rc_state;
};
struct sde_kms { int unused; };
static struct sde_kms kms;
static bool vm_available = true, owns_hw = true;
static int vm_held, rc, wakeups, boosts, policy_events, queues, cancels, unregistered;
static struct kthread_worker *queued_worker;
static struct sde_encoder_virt *active;
#define to_sde_encoder_virt(enc) ((struct sde_encoder_virt *)(enc))
static void mutex_lock(int *lock) { assert(!(*lock)++); }
static void mutex_unlock(int *lock) { assert(!--(*lock)); }
static struct sde_kms *sde_encoder_get_kms(struct drm_encoder *enc) { return &kms; }
static bool sde_vm_trylock(struct sde_kms *k)
{
    assert(!vm_held);
    if (!vm_available)
        return false;
    vm_held++;
    return true;
}
static void sde_vm_unlock(struct sde_kms *k) { assert(!--vm_held); }
static bool sde_vm_owns_hw(struct sde_kms *k) { assert(vm_held); return owns_hw; }
static int sde_encoder_resource_control(struct drm_encoder *enc, int event)
{
    assert(vm_held && owns_hw && event == SDE_ENC_RC_EVENT_EARLY_WAKEUP);
    wakeups++;
    return rc;
}
static void oplus_adfr_touch_event(void *enc)
{
    assert(enc == active && !vm_held && !active->rc_lock);
    policy_events++;
}
static void oplus_adfr_touch_work(void *enc)
{
    assert(enc == active && vm_held && active->rc_lock && owns_hw);
    assert(active->rc_state == SDE_ENC_RC_STATE_ON);
    boosts++;
}
static void kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
    assert(work == &active->input_event_work && policy_events);
    queued_worker = worker;
    queues++;
}
static void input_unregister_handler(struct input_handler *handler)
{
    assert(handler == active->input_handler && handler->private == active);
    unregistered++;
}
static void kthread_cancel_work_sync(struct kthread_work *work)
{
    assert(unregistered && work == &active->input_event_work);
    cancels++;
}
"""


class AdfrInputTests(unittest.TestCase):
    def test_touch_filter_worker_order_and_vm_safety(self):
        source = SOURCE.read_text()
        text = MOCK
        for signature in (
            "static void sde_encoder_input_event_handler(",
            "static void sde_encoder_input_event_work_handler(",
            "static void _sde_encoder_input_handler_unregister(\n",
        ):
            text += definition(source, signature) + "\n"
        text += r"""
int main(void)
{
    struct msm_drm_private priv = {0};
    struct drm_device dev = {.dev_private = &priv};
    struct drm_crtc crtc = {0};
    struct input_handler handler = {0};
    struct input_handle handle = {.handler = &handler};
    struct sde_encoder_virt enc = {
        .base = {.dev = &dev}, .crtc = &crtc, .input_handler = &handler,
        .rc_state = SDE_ENC_RC_STATE_ON,
    };
    int before;
    active = &enc;
    handler.private = &enc;
    priv.adfr_thread[0].thread = &enc;
    sde_encoder_input_event_handler(&handle, 0, 0, 0);
    sde_encoder_input_event_handler(&handle, EV_ABS, 0, 1);
    sde_encoder_input_event_handler(&handle, EV_ABS, ABS_MT_TRACKING_ID, -1);
    sde_encoder_input_event_handler(&handle, EV_KEY, BTN_TOUCH, 0);
    assert(!queues && !policy_events && !boosts);
    sde_encoder_input_event_handler(&handle, EV_ABS, ABS_MT_TRACKING_ID, 0);
    sde_encoder_input_event_handler(&handle, EV_ABS, ABS_MT_POSITION_X, 1);
    sde_encoder_input_event_handler(&handle, EV_ABS, ABS_MT_POSITION_Y, 1);
    sde_encoder_input_event_handler(&handle, EV_KEY, BTN_TOUCH, 1);
    assert(queues == 4 && policy_events == 4 && !boosts && !wakeups);
    assert(queued_worker == &priv.adfr_thread[0].worker);
    sde_encoder_input_event_work_handler(&enc.input_event_work);
    assert(wakeups == 1 && boosts == 1 && !vm_held && !enc.rc_lock);
    vm_available = false;
    sde_encoder_input_event_work_handler(&enc.input_event_work);
    assert(wakeups == 1 && boosts == 1 && !vm_held);
    vm_available = true;
    owns_hw = false;
    sde_encoder_input_event_work_handler(&enc.input_event_work);
    assert(wakeups == 1 && boosts == 1 && !vm_held);
    owns_hw = true;
    rc = -1;
    sde_encoder_input_event_work_handler(&enc.input_event_work);
    assert(wakeups == 2 && boosts == 1 && !vm_held);
    rc = 0;
    enc.rc_state = 0;
    sde_encoder_input_event_work_handler(&enc.input_event_work);
    assert(wakeups == 3 && boosts == 1 && !vm_held);
    enc.rc_state = SDE_ENC_RC_STATE_ON;
    priv.adfr_thread[0].thread = NULL;
    sde_encoder_input_event_handler(&handle, EV_KEY, BTN_TOUCH, 1);
    assert(queued_worker == &priv.disp_thread[0].worker);
    before = queues;
    crtc.index = 2;
    sde_encoder_input_event_handler(&handle, EV_KEY, BTN_TOUCH, 1);
    assert(queues == before);
    crtc.index = 0;
    _sde_encoder_input_handler_unregister(&enc.base);
    assert(unregistered == 1 && cancels == 1 && !handler.private);
    sde_encoder_input_event_work_handler(&enc.input_event_work);
    sde_encoder_input_event_handler(&handle, EV_KEY, BTN_TOUCH, 1);
    assert(wakeups == 3 && boosts == 1 && queues == before);
    puts("input: touch-only IRQ policy, RT worker, VM contention/ownership, RC failures, cancel");
    return 0;
}
"""
        run_c(text)

    def test_lifecycle_and_configuration_guards(self):
        source = SOURCE.read_text()
        destroy = definition(source, "void sde_encoder_destroy(")
        self.assertLess(destroy.index("_sde_encoder_input_handler_unregister("),
                        destroy.index("phys->ops.destroy("))
        disable = definition(source, "static void sde_encoder_virt_disable(")
        self.assertLess(disable.index("_sde_encoder_input_handler_unregister("),
                        disable.index("SDE_ENC_RC_EVENT_PRE_STOP"))
        self.assertIn("INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT",
                      source)
        setup = definition(source, "static int sde_encoder_setup_display(")
        self.assertIn("disp_info->display_type == SDE_CONNECTOR_PRIMARY", setup)
        self.assertIn("oplus_adfr_is_support()", setup)
        vm = (DISPLAY / "msm/sde/sde_vm.h").read_text()
        self.assertIn("mutex_trylock(&sde_kms->vm->vm_res_lock)", vm)


if __name__ == "__main__":
    unittest.main(verbosity=2)
