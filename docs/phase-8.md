# Phase 8: anti-pop validation and waveform regression

Phase 8 implements sprint S8 from the development document. Earlier phases already placed
`AudioTransitionGuard` beneath `AudioEngine`, attached every PCM buffer to an audio epoch, and
implemented fade/gate, underrun-to-zero, numeric safety, and endpoint prewarm. S8 preserves
those proven paths and closes the remaining verification and observability gaps: post-gate
click/pop detection, safety-command arbitration, complete transition counters, and repeatable
1,000-cycle waveform regression.

This phase does not implement the S9 `IAudioClient3`, exclusive-mode, adaptive-jitter, or
loopback-latency work.

## Bottom-of-stack invariant

```text
protocol / local probe
          |
          v
     AudioEngine ---- audio_epoch_id
          |
          v
      IAudioOutput
          |
          v
 fixed SPSC PCM ring
          |
          v
 AudioTransitionGuard
   | numeric safety
   | volume sample ramp
   | fade / gate / silent prewarm
   | underrun ramp-to-zero
   v
 ClickPopDetector (post-gate observation)
          |
          v
 WASAPI event-driven ReleaseBuffer
```

Every sample released to WASAPI still passes through `AudioTransitionGuard`. The detector is
inside that final path and observes the actual guarded waveform, but never modifies it. It uses
fixed per-channel history and atomics only: no allocation, file/network I/O, logging, or mutex is
introduced on the render thread.

## Transition behavior

| Event | Guarded behavior | Terminal gate |
|---|---|---|
| Start / Resume | silent prewarm -> smooth fade-in | audible |
| Stop | last released sample -> fade-out | stopped |
| Pause | last released sample -> fade-out | paused |
| Flush / Seek | fade-out -> epoch purge -> prewarm/fade-in on refill | SAFE_MUTE |
| Hard resync | fade-out -> epoch jump -> prewarm/fade-in | SAFE_MUTE |
| Underrun | ramp-to-zero -> explicit zero fill -> refill/prewarm | SAFE_MUTE |
| Device switch | old endpoint fade-out -> close/reopen -> silent prewarm | device-muted |
| Volume step | per-frame gain ramp | unchanged |

A start or resume that arrives while a fade-out is active is deferred until the old tail reaches
zero. A device switch is stricter: it remains device-muted until recovery explicitly starts the
new endpoint. This prevents a control-plane race from reopening the old waveform before a safety
transition completes.

Pending commands are merged by safety priority:

```text
DeviceSwitch > HardResync > Flush/Seek > Stop > Pause > Start/Resume > None
```

Consequently a racing start/resume cannot overwrite a queued flush, hard resync, or endpoint
switch before the render thread observes it.

## Click/pop detector

`ClickPopDetector` compares each finite output sample with the preceding sample on the same
channel. One event is recorded for a frame when any channel exceeds the configured absolute
step threshold. Production uses a 0.35 full-scale threshold; the deterministic sine stress uses
the stricter 0.20 threshold.

The detector publishes:

- analyzed frame count;
- threshold-crossing event count;
- last event frame;
- maximum step since open;
- peak step in the most recently rendered block.

The first sample after detector initialization establishes history and cannot create a false
event. Non-finite values are treated as zero by the standalone observer; in production they are
already sanitized by the transition guard before observation.

This is a deterministic discontinuity regression oracle, not a psychoacoustic classifier. A
deliberate full-scale impulse may legitimately cross the threshold, and physical driver/DAC
clicks occurring after the application render buffer still require loopback or external capture
in S9/S12.

## Epoch coverage

The epoch regression rotates all required reasons: flush, seek, format change, sender replace,
hard resync, and session reset. Every advance is monotonic and the previous epoch becomes stale
immediately. The output boundary continues to perform both a pre-write and post-publish epoch
check around the fixed ring, preventing an old producer racing a timeline reset from leaking PCM.

## Runtime diagnostics

Local probe and receiver diagnostics now include:

- transition state and total transition requests;
- fade-in and fade-out counts;
- SAFE_MUTE entries;
- hard-resync and underrun-transition counts;
- click/pop analyzed frames, events, maximum step, recent block peak, and last event frame;
- existing invalid numeric, clipped sample, DC-offset, underrun, latency, epoch, and device
  recovery metrics.

The diagnostics snapshot performs no render-thread formatting. Values are formatted only by the
application/control thread.

## Automated acceptance

The S8 test matrix includes:

- direct detector tests with a continuous sine and an injected full-scale discontinuity;
- safety-command priority and start-during-fade deferral;
- 1,000 lightweight guard state iterations retained from phase 1;
- a 1,000-cycle 48 kHz stereo waveform test over 1,920,720 frames;
- stop, pause, flush, seek, hard resync, device switch, underrun, rapid volume steps, and
  deliberately discontinuous post-reset source phases;
- 1,000 audio-epoch changes rotating every required reason;
- AudioEngine format-change restart and stale-buffer rejection.

The waveform test fails if any output step exceeds 0.20 full scale. It uses a continuous 440 Hz
source so transitions are judged on a known, bounded derivative rather than silence alone.

Debug and Release builds pass with `/W4 /WX`, and all 25 test modules pass in both
configurations. The waveform stress analyzed exactly 1,920,720 frames with zero threshold
crossings. During repeated-suite validation, an existing pause/worker race was reproduced: the
worker could pass its outer recording check, wait behind the pause transition lock, then pop a
packet while the sink was paused. A second running/recording check under that lock now leaves the
packet queued for resume. After the fix, 50 consecutive complete Release runs passed in 47.68
seconds.

## Hardware acceptance boundary

`--transition-cycles 1000` exercises the real event-driven WASAPI path and now rotates flush,
seek, hard resync, and sender replacement in addition to pause/resume, stop/start, and volume
ramps. Use `--signal silence` for a non-audible lifecycle run or `--signal 440` with a physical
loopback/external recorder for waveform analysis.

Automated tests cannot physically remove a USB DAC, change the Windows default endpoint, or
observe analog clicks created inside a driver or DAC after `ReleaseBuffer`. Those remain explicit
hardware acceptance checks; the application-side device-switch fade, mute, silent prewarm, and
recovery counters are implemented and regression-covered at their deterministic boundaries.

On the development endpoint, the real Release probe completed 1,000 silent transition cycles,
rendered 10,737,153 frames, advanced epoch 1 to 1,001, and executed 250 hard resyncs. It reported
zero click/pop events, zero stale epochs, and zero numeric/clipping/DC events. All 31 underruns
caused by the intentionally aggressive submission/transition loop were paired with 31 guarded
underrun transitions. The endpoint latency estimate was approximately 23 ms and the process
exited normally.

## Deferred work

S9 owns `IAudioClient3`, exclusive mode, adaptive jitter buffering, endpoint period selection,
and a physical/loopback receiver-added-latency harness. S10 owns remote-clock/PTP servo and
holdover/relock. S12 expands the detector and fault injection into long-duration synchronized
group regression.
