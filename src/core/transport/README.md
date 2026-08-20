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
