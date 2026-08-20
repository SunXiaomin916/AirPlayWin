# Protocol boundary

Phase 3 implements bounded incremental RTSP/HTTP request parsing, RTSP response construction,
and `AirPlayControlService`. The module consumes only `IControlConnectionHandler` events and
remains independent of Winsock, IOCP, Windows audio APIs, RTP, and codecs.

The parser is terminal after a framing error until reset. The connection owner responds with
400 and closes, preventing ambiguous bytes from being reinterpreted as a later request.
