# Phase 5: sender controls and AudioEngine integration

Phase 5 implements sprint S5 from the development document: connect sender control semantics
to the real audio engine and complete volume, pause/resume, and flush behavior. Phase 4 already
introduced the basic decoded-PCM sink, so this phase strengthens that path with explicit RTP
timeline boundaries and an integration test that reaches the `IAudioOutput` boundary.

## Runtime path

```text
RECORD / FLUSH + optional RTP-Info(seq, rtptime)
                       |
                       v
            AirPlayControlService
                       |
             AudioTimelineAnchor
                       v
       WindowsRtpTransportController
           | audio UDP       | control UDP
           |                 | retransmitted wrapper only
           +--------+--------+
                    v
             RtpAudioStream
       timeline gate -> jitter buffer -> decoder worker
                    |
                    v
         WindowsAudioStreamSink
          prefill / pause / flush
                    |
                    v
        WindowsAudioEngine -> core AudioEngine
                    |               | epoch ownership
                    v
             IAudioOutput -> WASAPI
                    |
          AudioTransitionGuard render gate
```

Protocol and transport code still cannot reach WASAPI. Every decoded frame passes through the
same `IAudioFrameSink -> AudioEngine -> IAudioOutput` boundary used by the standalone engine.
The production composition creates `WasapiAudioOutput`; the injectable output constructor is
for deterministic integration testing and future composition roots.

## RTP timeline invariants

- `RTP-Info` accepts one stream with optional `url` plus at least one of `seq` or `rtptime`.
  Duplicate fields, unknown fields, signed/overflowing values, empty anchors, and comma-separated
  multi-stream values are rejected with RTSP 400.
- Initial RECORD installs the sender lower bound before the stream is armed.
- FLUSH always clears the jitter queue and decoder concealment history, advances the
  AudioEngine epoch through `WindowsAudioStreamSink::Flush`, and installs the new lower bound.
- A resumed RECORD with an explicit anchor is treated as a discontinuity: queue/history reset,
  guarded flush/epoch transition, prefill, then resume. A resume without an anchor preserves
  the paused packet timeline.
- Packet comparisons use signed differences in the native 16-bit sequence and 32-bit timestamp
  domains, so normal wraparound is accepted. Packets older than either installed lower bound
  are dropped before jitter-buffer insertion.
- The timeline gate and jitter reset share a short mutex on the network/worker side. The WASAPI
  render thread is not involved and performs no allocation, logging, file/network I/O, or long
  mutex hold.

These rules prevent buffered or late pre-FLUSH PCM from crossing into a new AudioEngine epoch.
The core output boundary remains the final stale-epoch guard.

## Sender controls

- RECORD returns `Audio-Latency: 11025` for this classic 44.1 kHz development profile.
- PAUSE reaches `AudioTransitionGuard` through `AudioEngine::Pause`; resume uses its guarded
  fade-in behavior.
- FLUSH reaches `AudioEngine::Flush`, which advances the epoch before later decoded PCM is
  submitted.
- SET_PARAMETER scans bounded `text/parameters` lines and accepts one valid `volume` field in
  the range -144 dB through 0 dB. Other parameter lines do not hide the volume field.
- GET_PARAMETER with an empty body remains a keep-alive. A `volume` query returns the stored
  session value as `text/parameters`.
- Linear gain remains applied by the bottom transition guard's sample ramp, never directly by
  protocol code.

The RECORD latency and four-byte retransmitted-audio wrapper behavior are informed by the
[open-source Shairport Sync](https://github.com/mikebrady/shairport-sync) classic AirPlay
implementation. The RTP header and wrap domains
remain governed by [RFC 3550](https://datatracker.ietf.org/doc/html/rfc3550.html).

## Inbound retransmitted audio

The negotiated control UDP port counts every received datagram but forwards only packets whose
outer payload type identifies the classic retransmitted-audio wrapper. `RtpAudioStream` removes
the four-byte wrapper and then applies the same RTP v2, inner payload type, peer IP, SSRC,
timeline, jitter, and decode checks as a packet received on the audio port.

The receiver does not yet generate resend requests. Loss detection already exists in the jitter
buffer, but request timing, retry windows, and sender control endpoints belong with the later
adaptive buffering/timing work rather than this AudioEngine integration phase.

## Diagnostics

Phase 5 adds:

- retransmitted audio packets accepted;
- packets rejected by the current sequence/timestamp boundary;
- timeline reset count;
- current optional sequence and RTP timestamp anchors.

The CLI prints these beside existing packet/loss/jitter/decoder and WASAPI/epoch metrics.

## Automated acceptance

Tests cover:

- valid, duplicate, malformed, signed, overflow, and multi-stream RTP-Info values;
- initial RECORD anchor propagation and `Audio-Latency` response;
- stale packet rejection after an anchored FLUSH;
- real loopback delivery of a wrapped retransmitted packet through the control UDP port;
- multiline volume SET and volume GET response;
- invalid resumed RECORD without altering the valid paused session;
- RTP/L16 decoding into `WindowsAudioStreamSink`, actual `WindowsAudioEngine` and core
  `AudioEngine`, including prefill/start, QPC stamping, volume, pause/resume, epoch advance,
  stale-timeline rejection, and deterministic stop/close.

## Interoperability boundary

This phase completes S5 for the repository's explicitly advertised open, unencrypted RTP/L16
development profile. It does not add pairing, FairPlay/MFi material, encrypted audio, ALAC/AAC,
sender-clock synchronization, PTP, multi-room, or WinUI. No private keys or unauthorized
proprietary implementation material are embedded.

Because current Apple devices may require those deferred capabilities, a physical iPhone,
iPad, or Mac end-to-end playback claim cannot be made from this phase alone. Hardware testing
is meaningful only for senders willing to negotiate the open L16 profile; modern Apple sender
acceptance remains a later milestone.

## Next stage

Per the development document, phase 6 should harden operational recovery and packaging:
disconnect/reconnect paths, sleep/wake behavior, network and device transitions, installer and
firewall handling, and long-running/manual hardware validation. Precise AirPlay 2 timing and
buffered playback remain later phases and should consume the optional `target_qpc` field rather
than coupling clocks into WASAPI.
