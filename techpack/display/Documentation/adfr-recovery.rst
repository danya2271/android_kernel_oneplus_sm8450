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
The active manual-mode floor is code 2, not the literal value 40.

``measured_fps`` and the HWC software-FPS hint are in Hz. The CRTC statistics
API can update the software-FPS fallback and fake-frame policy, but never
writes a measured Hz value into the minimum-frequency command. An explicit
HWC software-FPS hint takes precedence over that fallback. CRTC statistics
no longer emit minimum-frequency notifications as if a DSI update occurred.

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
applied state, no pending updates, and a requested extension code greater
than the active floor and no greater than 119. Failed idle I/O leaves the
previous transition state intact and marks native-frequency restoration
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
    PYTHONDONTWRITEBYTECODE=1 \
        HOSTCFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
        python3 techpack/display/tests/test_adfr.py

Coverage includes 4096 configuration/debug combinations, 1536 min-FPS
mode/idle/code combinations, all 128 idle input codes, state isolation,
Hz/code separation, AOD deferral, missing commands, bad payloads, DSI errors,
partial multi-controller failure, request/reset injection during I/O,
auto-on filtering, first-frame native restoration and zero TE intervals.
The existing hotpath regression checks must also continue to pass.

Build and link the configured target::

    ./clang.sh -j8 vmlinux

Before deployment, validate on the actual Waipio/wly phone with its selected
panel and vendor HWC. Trace requested codes, DSI completion, raw panel TE
intervals and kickoff order. Check static content, 24/30/40/60/120-FPS
workloads, touch after idle, AOD/FOD/HBM, brightness, power cycling and
FHD/WQHD mode changes. Compare first-frame and p95/p99 latency before/after.
Host tests and successful linking do not establish physical panel behavior
or a measured latency improvement.
