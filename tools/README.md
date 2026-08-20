# Tools

The `AirPlayWin` executable doubles as the audio and receiver probe: it enumerates
endpoints, generates deterministic PCM, runs timed soaks, exercises guarded transitions, and
composes RTSP/RTP/L16 through WASAPI.

Phase 6 adds a read-only firewall status probe and the development-only `--lifecycle-smoke`
switch. The latter injects a coalesced suspend/resume pair into the real app composition so TCP
restart and DNS-SD republish can be validated without suspending the workstation.

Phase 7 adds the `--experimental-buffered-timing` receiver mode and
`--buffered-timing-ms <20..2000>`. Deterministic clock/wrap/drift simulation lives in the timing
unit suite; the CLI reports its lock, mapping, lateness, and sink scheduling metrics.

Phase 8 extends `--play --transition-cycles 1000` to rotate flush, seek, hard resync, and sender
replacement while retaining pause/resume, volume ramps, and stop/start. Runtime output includes
transition counters and post-gate click/pop detector metrics. The deterministic 48 kHz waveform
stress remains in the unit suite so it is independent of endpoint hardware.

Phase 9 adds `--low-latency`, `--exclusive`, `--strict-exclusive`, and
`--endpoint-offset-us`. The receiver reports the negotiated WASAPI path and every component of
its latency model. `Measure-LoopbackLatency.ps1` invokes the WAVE capture analyzer in either
dual-channel reference/return mode or known-stimulus-frame mode, with configurable onset
threshold and maximum latency. Use `--play --signal latency-pulse` for capture: unlike the
ordinary immediate impulse, its first pulse occurs after endpoint silent prewarm/fade-in.

`packet_replay/rtp_l16_replay.ps1` is a deterministic RTP/L16 440 Hz sender and network fault
injector. AirPlayWin analyzes completed PCM16/float32 WAVE captures but deliberately does not
pretend that QPC-only timing measures driver/DAC/analog latency; capture routing remains a
hardware or external-recorder setup step.
