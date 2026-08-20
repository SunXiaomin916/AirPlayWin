# Phase 4: RTP transport, jitter buffer, and decoder boundary

Phase 4 implements sprint S4 from the development document: receive RTP/stream packets,
reorder them, account for loss, decode them off the IOCP callback, and deliver a continuous
float32 frame stream to the existing audio engine. The runnable development profile is
unencrypted RTP/L16. ALAC, AAC, FairPlay, PTP, retransmission, and adaptive low-latency
control remain later phases.

## Runtime path

```text
RTSP ANNOUNCE (SDP L16 format)
        |
RTSP SETUP -> three ephemeral UDP ports (audio/control/timing)
        |
IOCP WSARecvFrom audio callbacks
        | parse/filter/fixed-size copy only
        v
fixed-capacity RtpJitterBuffer
        | ordered packet or explicit missing packet
        v
RtpAudioStream worker -> IAudioDecoder -> float32 DecodedAudioFrameView
        |
        v
IAudioFrameSink -> WindowsAudioStreamSink -> AudioEngine -> IAudioOutput -> WASAPI
```

Protocol code only sees `IAudioTransportController`; it cannot call WASAPI. The media worker
only sees `IAudioFrameSink`; it cannot reach a render buffer. Consequently all PCM still
passes through `AudioEngine`, its audio epoch handling, and the unskippable
`AudioTransitionGuard`.

## RTP and buffering invariants

- The parser validates RTP v2, CSRC length, extension length, padding, and a 4096-byte fixed
  payload limit before the packet reaches the jitter buffer.
- Packet payload storage is preallocated with the jitter-buffer slots. IOCP completion does
  not decode, perform file/network I/O, or allocate per packet.
- 16-bit sequence numbers are extended across wrap. Packets are classified as received,
  reordered, duplicate, late, lost, or overflowed.
- Startup waits for a fixed target packet depth. A gap below the highest received sequence is
  emitted as an explicit missing frame; the decoder currently conceals it with the nominal or
  most recently decoded frame count of zeros.
- Interarrival jitter follows the RTP smoothed estimator and is reported in RTP timestamp
  units and microseconds.
- `FLUSH` clears the buffered timeline and decoder history, and the audio sink starts another
  guarded prefill. A stale packet that arrives below the new sequence cursor is rejected.

The packet field and sequence/timestamp rules follow
[RFC 3550](https://datatracker.ietf.org/doc/html/rfc3550.html). RTP/L16 payload samples are
signed, network-byte-order 16-bit values as specified by
[RFC 3551](https://datatracker.ietf.org/doc/html/rfc3551).

## Windows UDP receiver

`IocpUdpReceiver` creates an overlapped UDP socket, associates it with a private IO completion
port, and keeps a bounded set of preallocated `WSARecvFrom` operations outstanding. Shutdown
closes the socket, drains cancelled completions, joins the worker, and releases Winsock/handle
resources deterministically. This matches the Windows completion-port model documented by
[Microsoft](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports) and
the overlapped receive contract of
[`WSARecvFrom`](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecvfrom).

The negotiated control and timing UDP ports are reserved and counted in this phase. RTCP
semantics, retransmission requests, timing responses, and PTP discipline are intentionally not
implemented yet.

## RTSP development profile

- `ANNOUNCE` accepts SDP dynamic L16 mappings or static payload types 10/11.
- `SETUP` accepts unicast `RTP/AVP[/UDP]` record transport, opens audio/control/timing ports,
  and returns them in the `Transport` response header.
- `RECORD`, `PAUSE`, `FLUSH`, volume `SET_PARAMETER`, and `TEARDOWN` drive the media stream.
- The CLI `--serve` path composes the control service, RTP controller, audio stream sink, and
  WASAPI output. `--device` selects an endpoint; omitting it follows the default endpoint.

This profile is a local transport milestone. Current Apple devices normally require pairing,
encryption, Apple codec negotiation, and later timing work, so phase 4 does not claim physical
iPhone/iPad/Mac interoperability.

## Diagnostics and replay

The snapshot includes UDP bytes/packets, invalid/source/payload rejects, decoded and concealed
frames, decoder/backpressure errors, jitter-buffer depth/loss/late/duplicate/reorder/overflow,
control/timing datagram counts, negotiated ports, and existing WASAPI depth/underrun/latency
metrics.

The deterministic replay tool is documented in
[`tools/packet_replay/README.md`](../tools/packet_replay/README.md). Automated tests cover RTP
headers, extension/padding validation, reorder/loss/wrap behavior, L16 conversion, SDP and
transport parsing, worker-thread continuity, real IOCP UDP loopback, and an end-to-end
`ANNOUNCE -> SETUP -> RECORD -> RTP -> FLUSH/PAUSE/TEARDOWN` flow.

## Deferred work

- ALAC/AAC decoder implementations and encrypted payload handling.
- Retransmission control packets and active RTCP/timing responses.
- Adaptive jitter target, latency controller, clock-drift resampling, hard resync policy, and
  PTP master selection.
- Pairing/FairPlay, multi-room, WinUI, and physical Apple-device acceptance.
