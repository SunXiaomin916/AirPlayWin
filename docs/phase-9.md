# Phase 9: low-latency output, adaptive buffering, and loopback measurement

Phase 9 implements sprint S9 from the development document. It adds opt-in `IAudioClient3`
Shared and event-driven Exclusive rendering, an adaptive RTP jitter target, a decomposed endpoint
latency model, and a capture-based impulse latency analyzer. Shared event-driven output remains
the safe default, and every output path retains the phase 1/8 audio epoch, transition guard, and
click/pop invariants.

This phase does not implement remote-clock/PTP servo, pairing, encryption, Apple codecs,
multi-room, or WinUI. Those boundaries remain unchanged.

## Output negotiation

| Request | Negotiation | Failure behavior |
|---|---|---|
| default | legacy Shared float32/event-driven | endpoint recovery loop |
| `--low-latency` | `IAudioClient3`, minimum supported Shared engine period | fresh-client legacy Shared fallback with HRESULT |
| `--exclusive` | Exclusive float32, then Exclusive PCM16 | Shared fallback with HRESULT |
| `--exclusive --strict-exclusive` | Exclusive float32, then Exclusive PCM16 | fail without silently changing mode |

The Exclusive initializer handles aligned-buffer renegotiation. The endpoint render format is
reported independently of the protocol/core float32 format. PCM16 conversion occurs only in the
platform backend, after the final transition gate, using preallocated storage.

Event-driven Exclusive differs materially from Shared: each render event supplies the complete
endpoint buffer, while Shared calculates writable frames from current padding. The Exclusive
render worker is waiting before `IAudioClient::Start`; a 250 ms first-render watchdog prevents a
driver from reporting a successful start while never delivering usable render progress.

## Preserved real-time boundary

```text
RTP / local generator
        |
        v
   AudioEngine + epoch rejection
        |
        v
 fixed float32 SPSC ring
        |
        v
 AudioTransitionGuard
        |
        v
 ClickPopDetector
        |
        +---- float32 Shared/Exclusive ReleaseBuffer
        |
        `---- preallocated float32 -> PCM16 conversion -> Exclusive ReleaseBuffer
```

Neither low-latency path can bypass the guard. The active render callback performs no allocation,
file/network I/O, log formatting, or long control-mutex acquisition. Endpoint reinitialization
may allocate its PCM16 scratch buffer while output remains in device-mute/prewarm state.

## Adaptive jitter buffer

Adaptive mode is enabled by `--serve --low-latency`; the pre-existing fixed-target behavior
remains the default. Its bounded target is expressed in learned packet durations:

```text
Warmup -> Locked -> LowLatency
                    |
             fault / underrun
                    v
                 Degraded -> Recovery -> Locked
```

Stable traffic contracts by one packet only after a complete stable window. Jitter p99, packet
loss, late/overflow drops, and downstream WASAPI underrun deltas expand the target immediately.
A fixed 128-entry interarrival-jitter window supplies p95/p99 telemetry. Flush resets adaptive
timeline state and the live target but retains cumulative diagnostic counters. No adaptive
decision changes negotiated presentation timestamps or creates a parallel audio timeline.

## Latency model and physical harness

The runtime estimate is intentionally decomposed:

```text
software PCM queue
  + current WASAPI endpoint padding
  + IAudioClient stream latency (engine-period fallback)
  + signed endpoint calibration offset
```

`--endpoint-offset-us` records an endpoint-specific correction but never turns the estimate into
a physical measurement. Driver, USB/HDMI transport, DAC, analog, and capture latency occur after
application buffer release.

`--analyze-loopback` reads PCM16 or float32 RIFF/WAVE, including extensible PCM/float subtypes.
The preferred fixture has the emitted impulse on one channel and the returned physical/virtual
loopback on another:

```powershell
.\build\vs2022-x64\Release\AirPlayWin.exe --analyze-loopback .\capture.wav `
    --reference-channel 0 --output-channel 1
```

For a recording aligned to an independently known stimulus frame:

```powershell
.\build\vs2022-x64\Release\AirPlayWin.exe --analyze-loopback .\capture.wav `
    --stimulus-frame 4800 --output-channel 0
```

The analyzer reports status, onset frames, frame/microsecond latency, and both peaks. It uses a
0.25 full-scale onset threshold and rejects negative or greater-than-one-second results. The
PowerShell wrapper in `tools/Measure-LoopbackLatency.ps1` supplies the same two modes.

## Diagnostics

Output diagnostics now include requested/active mode, `legacy`/`IAudioClient3`, float32/PCM16,
low-latency active, fallback and HRESULT, exclusive start timeouts, wakeups, endpoint buffer,
engine period, queue target, and latency components. Receiver diagnostics add adaptive state,
minimum/current/maximum targets, learned packet duration, jitter p95/p99, target increases/
decreases, degradation events, and downstream underruns.

No render-thread text formatting was added; CLI rendering consumes atomic/control-thread
snapshots.

## Automated validation

- Debug and Release x64 compile with MSVC `/W4 /WX`.
- All 29 modules pass in both configurations.
- Adaptive tests cover stable contraction, jitter-p99 expansion, and downstream-underrun
  expansion/state degradation.
- Endpoint-model tests cover queue/padding/engine/calibration composition, engine-period
  fallback, and negative clamping.
- Loopback tests cover dual-channel 10 ms, known-frame 5 ms, missing onset, invalid channel
  mapping, and maximum-latency rejection.
- The WAVE reader test creates and reads a PCM16 stereo capture and validates sample conversion
  and missing-file failure.
- The complete 29-module Release suite passed 50 consecutive runs; final Debug and Release CTest
  also pass.

## Development-machine hardware results

Strict Exclusive on the default Realtek endpoint selected PCM16, a 160-frame/3.33 ms period,
and a 480-frame/10 ms software queue target. A 10-second 440 Hz run rendered 482,080 frames with
zero underruns, stale buffers, numeric faults, or click/pop events. Its live model reported 13 ms
(10 ms software queue + 3.33 ms engine). A 100-cycle run covering pause/resume, volume down/up,
flush/seek/hard-resync/sender replacement, and stop/start ended with zero underruns and zero
click/pop events.

On the active NVIDIA HDMI endpoint, Shared `IAudioClient3` selected a 480-frame/10 ms period and
480-frame queue target. A 10-second silent run completed with zero underruns and no fallback. Its
uncalibrated live model reported 32 ms: 10 ms software queue + 12 ms padding + 10 ms engine.

The default Realtek endpoint rejected the float32 `IAudioClient3` period request with
`0x88890008`; the tested fallback reopened legacy Shared and ran without underruns. This is an
endpoint capability result, not a hidden low-latency success.

CPack produced `AirPlayWin-0.9.0-windows-x64.zip`, and its Release executable, README, installer
guide, and installation scripts were inspected.

## Acceptance boundary and next stage

Application stability, negotiation, transition safety, adaptive fault response, and measurement
machinery are implemented and regression-covered. The development run did not have a synchronized
physical loopback/external-capture WAVE fixture, so neither the Shared <=30 ms nor Exclusive <=15
ms receiver-added physical target is signed off from QPC/model estimates alone. Exclusive's 13 ms
model is encouraging but is not substituted for the required measurement.

S10 should add the platform-neutral TimingService/ClockServo and Windows PTP transport, including
lock, holdover, bounded slew, discontinuity-to-hard-resync policy, and deterministic drift/jump
tests. It must consume the existing epoch/guard boundary rather than modifying the S9 output or
jitter timeline in place.
