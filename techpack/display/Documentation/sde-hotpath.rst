.. SPDX-License-Identifier: GPL-2.0-only

SDE display latency audit
========================

Scope
-----

This tree builds the Waipio display techpack using
``config/gki_waipiodisp.conf`` and ``config/gki_waipiodispconf.h``, even though
the generic kernel configuration disables the in-tree DRM_MSM driver.
The frame path is ``msm_atomic_commit`` -> display kthread ->
``complete_commit`` -> SDE CRTC/encoder -> DSI. Read-pointer, write-pointer
and pingpong-done interrupts wake commit waiters and queue fence events.

The findings below are based on source inspection, not on hardware profiles.
They identify avoidable work and serialization; their relative contribution
to actual input-to-photon latency still needs measurement on the phone.

Changes
-------

* The IRQ dispatcher used to read the status register of every supported
  interrupt bank, including disabled interfaces, histogram and LTM blocks,
  while holding ``irq_lock`` with local interrupts disabled. It now skips
  banks with an empty cached enable mask and dispatches only enabled bits.
  The cache is updated under the same lock by IRQ enable/disable operations.
  Callback order, per-source acknowledgment and explicit timeout polling
  remain unchanged. A disabled source is cleared before it is enabled again.

* Command-mode IRQ handlers read CTL scheduler status solely for eventlog
  diagnostics even when logging was disabled or compiled out. Diagnostic
  collection is now conditional on the IRQ eventlog flag, including the
  line-count collection hook. Enabled eventlog diagnostics remain available.
  Scheduler reads used for actual commit/timeout decisions are unaffected.

* ``complete_commit`` held the device-wide Oplus DSPP mutex while waiting
  for implicit GPU fences. This also stalled CRTC property preparation for
  subsequent frames and other displays. Implicit fence waiting now precedes
  that mutex; the programming critical section remains protected.
  Color-property values and range payloads are stored in the transaction's
  ``sde_crtc_state``.

* The CRTC property setter held that same mutex across unrelated property
  processing, fence creation and userspace copies. Known MSM properties now
  bypass the color-processing lookup and DSPP mutex entirely. Only color
  properties take the mutex, releasing it after their callback, including
  error and not-found paths. Ordinary properties operate on the transaction's
  state. Explicit SDE acquire-fence waits remain in the existing CRTC flush
  path; they no longer serialize ordinary property preparation.

* Vblank work was allocated by ``kzalloc`` but freed through a cache pointer
  declared ``static`` in a shared header. Cache initialization in
  ``msm_drv.c`` did not initialize the distinct pointer in ``sde_crtc.c``.
  Use the matching ``kfree`` and remove the unused cache.

No panel commands, refresh rates, mandatory TE waits, GPU fence waits,
bandwidth votes, idle-power policies or timeout limits have been removed.
In particular, the Oplus 60 Hz idle-minfps delay documents a touchscreen
vsync constraint and must not be shortened without panel-specific testing.

Verification
------------

Run the host regression checks from the repository root::

    python3 techpack/display/tests/test_hotpath.py
    HOSTCFLAGS="-O1 -g -fsanitize=address,undefined" \
        python3 techpack/display/tests/test_hotpath.py

These compile the actual dispatcher and commit function bodies with mocked
MMIO/locking. The IRQ test exhausts 4096 enable/status combinations across
three banks, checks masked and unmapped sources, preserves callback order,
checks bit 31 and tests acknowledgment without callbacks. Commit and property
dispatch tests run with and without Oplus support. Property tests cover the
actual dispatch prologue, but not the property-specific switches requiring a
DRM device. Lock scope, diagnostic gating and vblank allocation pairing have
additional source-level checks. These tests do not emulate hardware register
ordering, concurrent IRQ delivery or a real DRM device.

Build the complete display subtree with the configured kernel toolchain::

    ./clang.sh techpack/display/

Before deploying, also link the complete kernel and test it on the device.
Record the same workload before and after, using existing ``sde`` trace
events and scheduler events in tracefs/Perfetto. Compare IRQ duration,
``complete_commit``/``plane_wait_input_fence``/``crtc_frame_event`` duration,
wakeup-to-run latency and missed frame deadlines at 60/120 Hz. Validate
screen off/on, AOD, fingerprint HBM, brightness changes, adaptive refresh,
secure display and external display modes. Measure median and tail
latency; a source-level reduction is not a measured millisecond saving.
