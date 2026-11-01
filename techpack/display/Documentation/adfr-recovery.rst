.. SPDX-License-Identifier: GPL-2.0-only

Oplus ADFR request and idle recovery
==================================

Scope
-----

Samsung ADFR auto-mode, minimum-frequency and software-FPS state now belongs
to ``dsi_panel.adfr``. This does not add secondary-display ADFR support:
the existing global QSync/vsync compatibility interfaces remain primary-only.
The selected primary panel's DT configuration controls support, fake frames,
vsync switching and idle operation. Debug disable bits are respected.
The shipped Waipio Samsung panel configurations use ``oplus,adfr-config = 23``.

Frequency units
---------------

``min_fps`` and ``applied_min_fps`` contain panel extension codes, not Hz:
0 selects the native maximum, 1 represents 60 Hz, 2 represents 40 Hz, and
119 represents 1 Hz in the Samsung 120-Hz command family. The fixed-90-Hz
command and the 60-Hz mode's minimum code of 1 retain their existing handling.
The legacy HWC active manual-mode floor is code 2, not the literal value 40.
The autonomous governor uses the same validated manual command with lower
floors allowed, as in the existing manual-idle path.

``measured_fps`` and the HWC software-FPS hint are in Hz. The CRTC statistics
API can update the software-FPS fallback and fake-frame policy, but never
writes a measured Hz value into the minimum-frequency command. An explicit
HWC software-FPS hint takes precedence over that fallback. CRTC statistics
no longer emit minimum-frequency notifications as if a DSI update occurred.
The autonomous governor instead measures individual real kickoffs, converts
their cadence to supported extension codes, and applies them through the
same serialized DSI path.

Autonomous cadence and touch boost
---------------------------------

On supported primary command-mode SDC panels with both GLOBAL and IDLE DT
bits set, the kernel policy defaults to enabled with a 10-Hz idle minimum.
No Android/HWC minimum-FPS request is required. Explicit HWC AUTO_ON retains
the existing auto-mode implementation; autonomous manual control resumes
when HWC selects AUTO_OFF. Global and idle debug-disable bits stop the
governor. Unsupported panels, fixed-90-Hz modes and AOD/off are excluded.
Setting either policy-disable debug bit schedules native restoration even when
the panel is at an active low rate or a low-rate transfer is still in flight.

The active ladder is 10/20/30/40/60/120 Hz. By default, quiet content
remains at 10 Hz instead of dropping to 1 Hz; a 1-Hz idle floor remains
available through the sysfs control.
These are panel refresh minima, not a frame-rate limiter or a promise of
application FPS. There is no 50-Hz extension code in this command family.
The next supported rate above a 50-FPS workload is 60 Hz. No panel command
or advertised DRM mode is invented.

The governor tracks the real kickoff interval using a 3:1 EWMA. The faster
of the instantaneous and smoothed cadence selects the target. An increase
applies on the next kickoff without a hysteresis delay; a decrease requires
250 ms at the same candidate. A missing, reversed or more than 500-ms-old
sample restarts at native speed. The configured idle minimum also limits
how low active cadence may go.

Touch down, a new contact or coordinate movement immediately selects the
native minimum in the input callback and extends a 250-ms boost. The
callback does no DSI I/O and does not take sleeping locks. Only multitouch
position devices match the input handler; unrelated ABS/SYN events and
contact release do not extend the boost.

The input work uses the existing ADFR FIFO-priority-16 worker when present,
independently of display commits waiting on GPU fences. After resource
wakeup it restores native refresh without waiting for an Android/HWC
commit. VM ownership and encoder power state guard the transfer. A
nonblocking VM lock avoids deadlocking cancellation against a modeset or
trusted-UI transition; a skipped worker leaves the native request pending
for the normal kickoff path. Unregister and encoder destruction cancel
input work before releasing encoder resources. Repeated touch events at
native refresh extend the boost without redundant DSI transfers.

The existing delayed resource-off work retains clocks for at most two
smoothed frame periods (capped at 250 ms after the last frame), or until
the touch boost expires. It then requests the idle minimum before power
collapse. There is no extra timer that attempts DSI I/O after clocks are
off. Idle entry runs under the encoder resource lock, after checking for
pending frames; a touch arriving during the low-rate transfer postpones
collapse and the input worker restores native refresh. HBM and optical
fingerprint HBM block deep idle, and active HBM/FOD requests select native
refresh. Autonomous low-rate operation suppresses fake-frame injection.

Native means the current DRM mode's maximum: 120 Hz in the 120-Hz mode,
60 Hz in the 60-Hz mode. This governor does not perform an uncoordinated
DRM modeset, so Android must select the 120-Hz base mode to obtain a
120-Hz touch boost. Physical touch-to-TE latency still requires measurement.

The root-writable control is::

    /sys/kernel/oplus_display/adfr_idle_fps

Writing 1, 10, 20, 30, 40 or 60 selects the autonomous minimum; the default
is 10 Hz. Writing 0 opts out
and restores HWC policy on the next safe update. Other values, including
50, return ``-EINVAL``. Reads report configuration, requested and applied
codes, validity, idle/restoration state, last error, active target, measured
kickoff cadence and remaining touch boost. A successful low-rate command
can therefore be distinguished from a pending target.

Successful idle and active governor transitions emit rate-limited
``kVRR kernel idle enter/exit`` and ``kVRR kernel active update`` messages
with Hz and the actual applied code. For example, 1 Hz uses ``0x77`` and
10 Hz uses ``0x0b``. Per-frame status is also available through existing
ADFR trace counters without enabling unbounded printk in the hotpath.

Transactions and recovery
-------------------------

The connector property setter validates packed ADFR inputs without changing
panel state. Requests are captured by ``sde_kms_prepare_fence`` after the DRM
atomic state swap, even when the connector has no active CRTC. Consequently,
TEST_ONLY and rejected atomic transactions cannot leak policy into the
running panel. Requests captured during AOD/off are retained for resume.

A spinlock protects requested state and its generation. A separate mutex
serializes auto-mode and idle DSI transactions; it is never taken in IRQ
context. The DSI path keeps the existing display-lock/panel-lock order.
Applied values and pending flags change only after all controllers succeed.
Power deferral returns ``-EAGAIN``. Missing command sets and malformed
payloads return errors instead of success. A newer request arriving during
DSI I/O remains pending. Each commit attempts an update once; there is no
blocking retry loop. Permanent command-table errors defer further attempts
until the requested policy or timing changes; repeated identical HWC writes
do not create a new generation. Frames without pending policy changes avoid
the update mutex.

Auto-on filtering defers a request until TE clears the filter instead of
discarding the request. Panel enable/timing reset seeds the command tables'
known defaults while preserving requested policy that must be restored.

Manual idle is allowed only for supported 60/120-Hz SDC modes, with valid
applied state and no pending updates. Legacy HWC requests require an
extension code greater than the active floor and no greater than 119;
autonomous policy uses its independently configured minimum. Failed idle I/O
leaves the previous transition state intact and marks native-frequency restoration
pending, since a failed transfer may still have partially reached the panel.

Before the first kickoff after idle, the driver restores the mode's native
minimum: code 0 at 120 Hz or code 1 at 60 Hz. The following normal commit can
reapply the active floor. That first-frame restoration also runs when a
debug mask disables ADFR while the panel is already idle. The existing
60-Hz TE phase-alignment delay remains mandatory; its timestamp-list access
is now protected against the TE IRQ writer.

Notifications describe successful DSI updates. Dynamic TE interval arithmetic
uses nanoseconds and rejects missing, equal or reversed timestamps before
division. The legacy filtered dynamic-TE display value, including its idle
estimate, is not a substitute for a trace of raw panel TE edges.

Boot console, printk configuration and early display initialization messages
are not disabled by this change. Panel command tables, TE routing, refresh
mode tables and bandwidth votes are not replaced.

Validation
----------

Run the host tests using the actual driver function bodies with mocked DSI,
locks, timers and fault injection::

    PYTHONDONTWRITEBYTECODE=1 python3 techpack/display/tests/test_adfr.py
    PYTHONDONTWRITEBYTECODE=1 python3 techpack/display/tests/test_hotpath.py
    PYTHONDONTWRITEBYTECODE=1 python3 techpack/display/tests/test_adfr_input.py
    PYTHONDONTWRITEBYTECODE=1 \
        HOSTCFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
        python3 techpack/display/tests/test_adfr.py

Coverage includes 4096 configuration/debug combinations, 1536 min-FPS
mode/idle/code combinations, all 128 idle input codes, state isolation,
Hz/code separation, AOD deferral, missing commands, bad payloads, DSI errors,
partial multi-controller failure, request/reset injection during I/O,
auto-on filtering, first-frame native restoration and zero TE intervals.
Autonomous scenarios cover all supported cadence/floor combinations,
native touch boost, hysteresis, idle deadlines, feature opt-out,
HBM/FOD, input filtering, VM/RC failures, work cancellation and a touch
racing the idle command. Encoder tests verify the actual kickoff/input
functions as well as ordering around resource power collapse.
The existing hotpath regression checks must also continue to pass.

Build and link the configured target::

    ./clang.sh -j8 vmlinux

Before deployment, validate on the actual Waipio/wly phone with its selected
panel and vendor HWC. Trace requested codes, DSI completion, raw panel TE
intervals and kickoff order. Check static content, 1/10/20/30/40/60/120-FPS
workloads, touch after idle, AOD/FOD/HBM, brightness, power cycling and
FHD/WQHD mode changes. Compare first-frame and p95/p99 latency before/after.
Host tests and successful linking do not establish physical panel behavior
or a measured latency improvement.
