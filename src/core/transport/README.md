# Transport boundary

`IControlConnectionHandler` is the platform-neutral phase 3 TCP/control callback. Windows IOCP
delivers opaque byte spans through this interface; the protocol layer returns complete writes
and a close-after-write decision.

Phase 4 adds the independent audio side: `RtpPacket`, fixed-capacity `RtpJitterBuffer`,
`RtpAudioStream`, `IUdpDatagramHandler`, `IAudioStream`, and
`IAudioTransportController`. UDP callbacks parse/filter/copy only. A dedicated stream worker
performs ordered delivery, loss concealment through `IAudioDecoder`, and float32 submission
through `IAudioFrameSink`. This directory contains no Winsock, IOCP, or WASAPI headers.

Phase 5 carries an `AudioTimelineAnchor` from RTSP `RTP-Info` through the controller into the
stream. RECORD, resumed RECORD, and FLUSH can reset the packet timeline; wrap-aware sequence
and timestamp gates reject older packets before jitter-buffer insertion. A four-byte RAOP
retransmission wrapper is removed before normal RTP validation. The control-port handler may
forward only those wrapped audio packets; timing traffic remains isolated.

Phase 7 can inject `ITimingEngine` into `RtpAudioStream`. In the explicit experimental mode,
every decoded or concealed frame maps its RTP sampling timestamp to a target QPC value before it
crosses `IAudioFrameSink`. With the mode disabled, the established arrival-time behavior is
unchanged. The reserved timing UDP socket is still isolated and counted; S7 does not parse or
claim PTP timing traffic.

Phase 9 optionally adapts the jitter target without changing sender timestamps or the negotiated
presentation timeline. Stable input reduces the target by one learned packet duration per
stable window; jitter p99, late/lost/overflow events, and downstream audio underruns increase it
quickly. The state machine is `Warmup -> Locked -> LowLatency`, with `Degraded -> Recovery` for
fault handling. Entry into `LowLatency` also requires timing lock when timing is enabled and a
measured decoder cost below half of packet duration. Packet/decode rolling costs and the
jitter/scheduled/output receiver-path estimate are diagnostics only and never alter negotiated
timestamps. The classic protocol-declared latency is transported and reported separately from
that receiver-path estimate. All storage, including the 128-sample jitter percentile window, is
fixed.

Phase 10 can inject `DisciplinedRtpTimingEngine` instead of the phase 7 local buffered engine.
The stream consumes its bounded rate correction through a preallocated `DriftResampler`, counts
inserted/dropped correction frames, and converts a pending clock discontinuity into
`IAudioFrameSink::HardResync`. The Windows sink then creates a new audio epoch and uses the
mandatory hard-resync fade sequence. The transport never calls WASAPI directly. Absolute
Phase 11 can additionally supply `RtpPtpPhaseTimeline`. In that mode an RTP frame must map
through the shared remote PTP phase and clock domain; an unavailable or master-mismatched phase
increments `timing_unmapped_dropped_frames` and the PCM is discarded. There is no fallback to a
receiver-local anchor. Multi-member policy remains in `GroupCoordinator`, outside transport.

Phase 12 adds `NetworkFaultInjector` as a deterministic test utility. A fixed seed produces
bounded delivery delay, random/burst loss, duplication, and reordering decisions with counters.
It is intentionally not wired into production packet receipt: test harnesses and replay tools
apply faults before the normal RTP parser/jitter-buffer boundary.
