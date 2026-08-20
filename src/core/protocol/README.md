# Protocol boundary

Phase 3 implements bounded incremental RTSP/HTTP request parsing and RTSP response
construction. Phase 4 adds SDP/L16 and UDP record-transport parsing plus media-state handling
in `AirPlayControlService`. The service consumes `IControlConnectionHandler` events and calls
only the platform-neutral `IAudioTransportController`; it remains independent of Winsock,
IOCP, Windows audio APIs, RTP packet storage, and concrete decoders.

The parser is terminal after a framing error until reset. The connection owner responds with
400 and closes, preventing ambiguous bytes from being reinterpreted as a later request.

Phase 5 parses bounded single-stream `RTP-Info` anchors for RECORD and FLUSH, returns the
classic development-profile `Audio-Latency` response, and supports both setting and querying
the session volume through `text/parameters`. Protocol code still passes only typed timeline
and gain values through `IAudioTransportController`; it does not own packet or audio state.
