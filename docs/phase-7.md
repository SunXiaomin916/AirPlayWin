# Phase 7: buffered RTP timing experiment

Phase 7 implements sprint S7 from the development document: an experimental buffered/timing
path for a single audio stream. The goal is to make presentation time a first-class value and
exercise it through the existing RTP, decoder, AudioEngine, epoch, and WASAPI boundaries. This
phase does not claim complete AirPlay 2 interoperability or implement PTP.

## Explicit scope

The experiment is opt-in through `--experimental-buffered-timing`. When disabled, phase 6
arrival-driven behavior remains unchanged. When enabled:

- RECORD or an anchored resume binds its RTP timestamp to `QPC now + target buffer`;
- a RECORD without `RTP-Info` locks on the first continuous decoded frame;
- every decoded and loss-concealed frame receives a target QPC timestamp;
- the Windows sink waits on the non-real-time decoder thread until a safe submission window;
- FLUSH, anchored resume, stream reset, and session teardown create a new timing generation;
- pause/resume without a remote wall-clock re-anchors the first resumed frame locally.

The target buffer is configurable from 20 to 2,000 ms. The development default is 120 ms.
Changing it changes local safety reserve only; it is not advertised as a sender-negotiated
AirPlay latency.

## Architecture

```text
RECORD / FLUSH + optional RTP-Info(rtptime)
                       |
                       v
              RtpAudioStream
                 | Reset / Map
                 v
        core::timing::ITimingEngine
                 |
        BufferedRtpTimingEngine
          remote RTP timestamp
                 + local monotonic clock
                 v
              target_qpc
                 |
        DecodedAudioFrameView
                 v
       WindowsAudioStreamSink
     non-real-time submission wait
                 |
       WindowsAudioEngine / epoch
                 v
      AudioTransitionGuard -> WASAPI
```

`IMonotonicClock` keeps the core model independent of Windows. Production composition injects
`QpcClockSource`; deterministic tests inject a manually advanced clock. No platform header is
present in the core timing or transport modules.

## Mapping model

For one RTP audio stream:

```text
anchor_target_qpc = local_qpc_at_lock + target_buffer_ms * qpc_frequency / 1000

target_qpc(timestamp) = anchor_target_qpc
                      + wrap_delta(timestamp, anchor_timestamp)
                        * qpc_frequency / rtp_clock_rate
```

The 32-bit RTP delta uses signed modular subtraction. This accepts ordinary timestamp wrap while
preserving the half-range ordering rule already used by the phase 5 timeline gate. RFC 3550
defines an RTP timestamp as the sampling instant and specifies a payload-dependent clock rate;
it also explains why a separate reference-clock pairing is required for synchronization across
streams or hosts. S7 implements only the single-stream reconstruction part.

QPC is used as a high-resolution local interval clock, not as UTC or a remote master clock. The
model records late mappings and maximum lateness so simulated clock mismatch becomes visible,
but it does not change the sample rate or servo the clock.

## Submission scheduling

`WindowsAudioStreamSink` waits until `target_qpc - submission_lead` before writing a scheduled
frame to `AudioEngine`. The default lead is 30 ms and includes the existing prefill/WASAPI queue
budget. Waiting occurs on the RTP decoder worker, never on the WASAPI render thread. The render
thread remains event driven and keeps its no-allocation/no-file/no-network constraints.

The wait is cancellable through a monotonic scheduling generation. Configure, pause, resume,
flush, and stop invalidate an in-flight wait before it can submit PCM into a changed timeline.
The AudioEngine epoch remains the final stale-buffer barrier.

This simple worker-paced scheduler is appropriate for the S7 experiment and bounded target
buffers. A production scheduler with a preallocated multi-frame presentation queue can replace
it later without changing `DecodedAudioFrameView` or `ITimingEngine`.

## Diagnostics

Transport timing diagnostics expose:

- enabled/locked state and timing generation;
- RTP clock rate and configured target buffer;
- mapped packet and late-mapping counts;
- maximum mapping lateness;
- remote anchor, latest remote time, and latest target QPC;
- model error code.

Audio sink diagnostics expose scheduled frames, late scheduled frames, cumulative scheduling
wait, maximum submission lateness, and the latest target QPC. Existing epoch, queue depth,
underrun, output latency, device recovery, RTP, and jitter diagnostics remain intact.

## Automated acceptance

Tests cover:

- anchored and first-frame locking;
- exact RTP-to-QPC scale conversion;
- 32-bit RTP timestamp wrap;
- timing generation reset on pause/resume and FLUSH;
- deterministic +100 ppm clock simulation and accumulated lateness observation;
- target QPC propagation through decoder and `IAudioFrameSink`;
- controller composition with the Windows QPC source;
- actual non-real-time scheduling wait before an inspectable `IAudioOutput`;
- unchanged no-timing behavior for existing stream tests.

Debug and Release builds pass with `/W4 /WX`, and all 23 test modules pass in both
configurations. Fifty consecutive Release suite runs passed, followed by another 20 after
adding explicit in-flight scheduling-cancellation coverage. A real loopback
RTSP/RTP/L16-to-WASAPI smoke with a 500 ms reserve delivered 30 packets / 10,560 frames with
zero stale, rejected, or late scheduled frames and no protocol/output error. The measured
697,824 microseconds of cumulative submission wait confirms that the application exercised the
future-target scheduling path rather than falling back to arrival-driven writes.

An 80 ms trial driven by a deliberately slow PowerShell packet generator arrived after its
targets and correctly incremented both timing and sink lateness counters. This is expected
diagnostic behavior: the S7 reserve is configurable policy, not a negotiated or calibrated
latency guarantee.

## Deferred work

S7 does not parse the reserved timing UDP socket, implement NTP/PTP packet exchange, estimate
remote clock offset/drift/uncertainty, perform micro-resampling, enter holdover, execute clock
relock, calibrate endpoint latency, or coordinate multiple receivers. The development document
assigns low-latency IAudioClient3/adaptive-buffer work to S9, Windows PTP/ClockServo to S10, and
multi-room coordination to S11.

Primary technical references:

- [RFC 3550: RTP](https://www.rfc-editor.org/rfc/rfc3550.html)
- [Microsoft QueryPerformanceCounter](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter)
