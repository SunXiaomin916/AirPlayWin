# Protocol boundary

Phase 3 implements bounded incremental RTSP/HTTP request parsing and RTSP response
construction. Phase 4 adds SDP/L16 and UDP record-transport parsing plus media-state handling
in `AirPlayControlService`. The service consumes `IControlConnectionHandler` events and calls
only the platform-neutral `IAudioTransportController`; it remains independent of Winsock,
IOCP, Windows audio APIs, RTP packet storage, and concrete decoders.

The parser is terminal after a framing error until reset. The connection owner responds with
400 and closes, preventing ambiguous bytes from being reinterpreted as a later request.
