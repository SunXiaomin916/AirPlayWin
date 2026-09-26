# Phase 10: PTP/QPC clock discipline and drift correction

Phase 10 implements sprint S10 from the development document: a Windows timing service, a
platform-neutral clock servo, holdover/relock behavior, and bounded audio-rate correction.
It extends the existing S7 `ITimingEngine` boundary and preserves the S8/S9 epoch, transition,
and real-time invariants.

The mode is experimental and opt-in. It does not advertise additional AirPlay capabilities.
Pairing, encryption, Apple codecs, absolute RTP-to-PTP phase association, grandmaster election,
multi-room group coordination, and WinUI remain outside this phase.

## Architecture

```text
PTPv2 multicast/unicast UDP 319 + 320
                  |
                  v
       WindowsPtpTimingService
       (exclusive IOCP sockets)
                  |
                  v
           PtpClockDomain
     one-step / two-step completion
                  |
                  v
             ClockServo
       remote nanoseconds -> QPC
          drift / state / slew
                  |
                  v
     DisciplinedRtpTimingEngine
 RTP timestamp -> target QPC + rate correction
                  |
                  v
 RtpAudioStream -> DriftResampler -> IAudioFrameSink
                                      |
                            discontinuity only
                                      v
                       HardResync -> new epoch
                         fade-out / jump / fade-in
```

Winsock and QPC are restricted to the Windows platform layer. PTP parsing, clock estimation,
RTP mapping, resampling, and diagnostics are core modules. Neither the timing service nor the
transport can call WASAPI directly.

## PTP receive path

`PtpPacket` validates the PTPv2 version, declared message length, common header fields, signed
fixed-point correction field, source port identity, sequence ID, and 48-bit-seconds timestamp.
The clock domain accepts:

- one-step Sync, using the Sync origin timestamp;
- two-step Sync + Follow_Up with exact domain/source/port/sequence matching;
- Announce for observability only;
- other valid message types as counted, ignored input.

Malformed lengths, timestamps whose nanoseconds field is outside `[0, 1e9)`, correction
overflow, and unmatched Follow_Up messages never enter the servo.

The Windows service uses event-driven IOCP receivers on UDP 319 and 320. The addresses are
exclusive so a port owner conflict fails startup visibly. By default it joins `224.0.1.129` and
`224.0.0.107`; `--ptp-interface <IPv4>` chooses the membership interface on a multi-NIC host.
Normal receiver startup does not modify Windows Firewall.

## Clock servo

The local clock is always QPC. Wall-clock APIs are not used for presentation decisions. A sample
contains remote nanoseconds, receive QPC, optional RTT, and source clock identity. The model:

- subtracts half of a supplied RTT when one is available;
- estimates oscillator slope over the acquisition window so adjacent receive-scheduling jitter
  is not amplified into drift;
- low-pass filters bounded drift and phase residuals;
- rejects non-monotonic, excessive-RTT, residual-outlier, and implausible-slope samples;
- limits rate-correction movement in ppm per second;
- resets acquisition on a source-clock change;
- creates a one-shot hard-resync request for a large discontinuity after lock.

The state machine is:

```text
Unlocked -> Acquiring -> Locked -> Holdover -> Relocking -> Locked
                           |                         |
                           +---- long timeout ------+----> Unlocked
```

Holdover preserves the last bounded model for short packet loss. A returning valid stream
relocks over multiple samples instead of stepping the audio rate. Once the unlock timeout is
exceeded, mapping is disabled until a new acquisition succeeds.

## Audio correction and hard resynchronization

The disciplined RTP engine retains a stream-local RTP anchor and applies the servo's bounded
rate correction to both target-QPC mapping and decoded frame count. `DriftResampler` uses caller
owned, preallocated storage and a fractional-frame accumulator; it never grows memory while the
stream worker is active. Runtime counters expose input, output, inserted, and dropped frames.

A normal offset/drift change never rewrites the audio timeline abruptly. A discontinuity beyond
the hard threshold is consumed once by `RtpAudioStream`, which calls
`IAudioFrameSink::HardResync`. The Windows sink cancels pending scheduling, creates a new audio
epoch, resets prefill, and invokes the existing guarded fade-out/timeline-jump/fade-in sequence.
Old-epoch PCM therefore cannot cross the clock jump into WASAPI.

## CLI and diagnostics

Start the experimental service with:

```powershell
.\build\vs2022-x64\Release\AirPlayWin.exe --serve --experimental-ptp-timing `
    --duration 300
```

The PTP diagnostic line reports service state, bound ports, multicast/interface state, event and
general datagram counts, message/parser counts, PTP domain, source clock identity, accepted/
rejected/outlier samples, offset residual, drift ppm, rate correction, uncertainty, sync age,
holdover/relock/hard-resync counts, receive errors, and the last Windows error. Media diagnostics
add resampler input/output/insert/drop and timing hard-resync counters.

`tools/timing_probe/ptp_sync_replay.ps1` produces one-step or two-step Sync/Follow_Up traffic,
optional synthetic drift, and an optional clock jump. It is a packet-path smoke tool; PowerShell
scheduling is not treated as calibration or servo-accuracy evidence.

## Automated validation

- Debug and Release x64 build with MSVC `/W4 /WX`.
- All 33 test modules pass in both configurations.
- The complete Release suite passes 50 consecutive runs.
- Servo tests cover +100 ppm drift, 125 ms cadence with alternating receive jitter, mapping,
  bounded correction, outliers, lock, holdover, relock, hard resync, unlock, and reacquisition.
- PTP tests cover one-step/two-step completion, correction application, malformed/unmatched
  input, +50 ppm mapping, and disciplined RTP timing.
- Resampler tests cover unity, positive/negative correction, fractional accumulation, bounds,
  and finite output.
- Windows tests use real IOCP UDP sockets for both message classes, verify exclusive port
  conflicts and invalid multicast configuration, and keep the service restart-safe.
- The Windows audio integration test proves that timing hard resync increments the audio epoch
  and reaches `AudioTransition::HardResync`.

The final Release repetition passed 50/50 runs in 48.47 seconds. A local application smoke bound
UDP 319/320 exclusively, joined both multicast groups on the default interface, and received 12
Sync plus 12 Follow_Up datagrams from the PowerShell probe with zero parser, socket, or service
errors. The probe's user-mode scheduling jitter produced rejected servo samples as expected, so
that run is packet-path evidence only; the native deterministic tests are the drift/lock evidence.

CPack produced `AirPlayWin-0.10.0-windows-x64.zip`. Its Release executable, project README,
installer guide, and install/uninstall scripts were inspected. The archive is 242,928 bytes with
SHA-256 `3DD7922F78E4B0B25E5836EAD1491B49579210B3A52C1C0C1BFA9EC0B265E249`.

## Acceptance boundary and next stage

The deterministic completion criterion for S10 is met: a remote clock can be mapped to QPC, a
stable drift estimate drives bounded correction, short loss enters holdover, returning timing
relocks without a step, and large discontinuities cross the protected epoch/transition path.

The Windows service is receive-only in this phase. RTT remains zero unless a future active-delay
exchange supplies it; no Delay_Req is sent, path asymmetry is not estimated, and Announce does
not elect a grandmaster. A physical external PTP master, firewall policy, and multiple real NICs
remain hardware/manual acceptance items.

S11 should add `GroupCoordinator`: sender/master selection, a defined RTP-to-PTP phase anchor,
join/leave transitions, per-endpoint calibration exchange, and group resynchronization policy.
It should reuse this clock domain and must continue to route discontinuities through the audio
epoch and transition guard.
