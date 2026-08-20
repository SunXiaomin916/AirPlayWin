# Phase 1: Project skeleton and WASAPI audio engine

## Frozen invariants

1. Protocol, discovery, transport, and future UI code can submit PCM only through `AudioEngine`; no such module may include WASAPI headers.
2. `WasapiAudioOutput::RenderOnce` always passes endpoint PCM through `AudioTransitionGuard` before `ReleaseBuffer`.
3. All PCM is interleaved float32 and carries `epoch_id`, QPC timestamp, frame count, sample rate, and channel count.
4. An epoch mismatch is rejected before the PCM ring. Epoch-changing operations reset the queued timeline and synthesize a continuous short tail to zero before prewarm/fade-in.
5. The steady-state render callback performs no heap allocation, file/network I/O, log formatting, or mutex acquisition.
6. Underrun memory is explicitly zeroed; the last arbitrary sample or stale memory is never repeated.
7. Shared/event-driven WASAPI is the phase-1 default. Exclusive mode is a later backend strategy, not a protocol concern.

## Transition behavior

| Operation | Guard path |
|---|---|
| Start / Resume | silent prewarm -> smooth fade-in |
| Stop / Pause | continuous last-sample tail -> smooth fade-out -> stopped/paused gate |
| Flush / Seek | new epoch -> queue discard -> fade-out -> prewarm -> fade-in |
| Hard resync | new epoch -> queue discard -> safe tail -> timeline jump -> prewarm -> fade-in |
| Volume change | per-frame gain ramp |
| Underrun | ramp-to-zero -> zero fill -> safe mute; resume after a safe refill |
| Device change | device-switch fade/mute -> endpoint close/open -> silent prime -> fade-in |

The synthetic tail is bounded by the configured fade-out duration and starts from the last sample already released to WASAPI. This preserves continuity even when an epoch change must immediately discard every queued old frame.

## Automated tests

The unit target covers:

- epoch monotonicity and reasons;
- ring wraparound, reset, capacity, and concurrent SPSC ordering;
- start/fade, volume ramp, NaN/Inf removal, amplitude clamp, underrun mute, and hard resync;
- all five test signal modes;
- `AudioEngine` metadata and epoch-to-output propagation.

## Hardware acceptance checklist

These checks require an actual Windows endpoint and therefore are run with `AirPlayWin.exe`, not in the hardware-independent unit target.

- Enumerate active, disabled, absent, and unplugged render endpoints.
- Play 440 Hz on the default endpoint and on a copied explicit endpoint ID.
- Run `--duration 1800`; verify no crash and review underrun/latency counters.
- Run `--transition-cycles 100`; listen or loop back the output and check for repeatable click/pop/burst.
- During default output playback, change the Windows default endpoint and verify recovery.
- During selected USB DAC playback, remove the DAC, wait, reconnect it, and verify the same endpoint is reopened.

Phase 8 adds an in-process post-gate transient detector and a 1,000-cycle known-waveform
regression. Loopback/external capture remains necessary for clicks created inside a driver or
physical endpoint after WASAPI buffer submission. Human listening is useful but is not a
sufficient regression oracle.

## Recorded phase-1 verification

- MSVC 19.44 / Windows SDK 10.0.26100, x64 Debug and Release: clean build under `/W4 /WX`.
- Debug and Release CTest: all suites passed, including 1,000 offline transition iterations and concurrent SPSC ordering.
- Default endpoint 440 Hz smoke: five seconds, zero underruns/stale epochs.
- Explicit HDMI endpoint silent smoke: three seconds, zero underruns/stale epochs.
- Combined transition probe: 100 cycles, zero underruns/stale epochs, epoch 1 -> 101.
- Silent soak: 30 minutes, 86,401,953 frames, zero underruns/stale epochs, exit code 0.

The observed shared-mode estimate alternated between roughly 27 and 37 ms on the test endpoint. The `IAudioClient3` low-period path remains deferred, so the document's consistent shared-mode `<= 30 ms` target is not yet claimed as passed.

## Deferred by scope

AirPlay pairing, authentication/encryption, RTSP, RTP, mDNS/DNS-SD, AirPlay 2 PTP, multi-room coordination, exclusive WASAPI, and WinUI 3 are intentionally absent from this phase.
