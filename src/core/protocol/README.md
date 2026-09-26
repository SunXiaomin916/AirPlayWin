# Protocol boundary

Phase 3 implements bounded incremental RTSP/HTTP request parsing and RTSP response
construction. Phase 4 adds SDP/L16 and UDP record-transport parsing plus media-state handling
in `AirPlayControlService`. The service consumes `IControlConnectionHandler` events and calls
only the platform-neutral `IAudioTransportController`; it remains independent of Winsock,
IOCP, Windows audio APIs, RTP packet storage, and concrete decoders.

The classic RAOP compatibility path also parses Apple Lossless `fmtp` into the 24-byte ALAC
specific configuration and accepts paired `rsaaeskey`/`aesiv` attributes. Protocol code asks
the injected cryptographic provider for typed AES material and never retains those SDP values
in request diagnostics.

The parser is terminal after a framing error until reset. The connection owner responds with
400 and closes, preventing ambiguous bytes from being reinterpreted as a later request.

Phase 5 parses bounded single-stream `RTP-Info` anchors for RECORD and FLUSH, returns the
classic development-profile `Audio-Latency` response, and supports both setting and querying
the session volume through `text/parameters`. Protocol code still passes only typed timeline
and gain values through `IAudioTransportController`; it does not own packet or audio state.

Compatibility diagnostics retain only the latest 16 request summaries: method, bounded target,
protocol version, content type, body byte count, connection ID, and response status. Request
bodies and authorization/key headers are never copied into the trace. Classic RAOP diagnostics
record only whether an Apple challenge was present and whether an Apple response header was sent;
the cryptographic values are never retained.

AirPlay 2 `POST /command` metadata updates are accepted only with the binary-plist content type
and `bplist00` envelope. Their bodies are deliberately not retained or interpreted by the audio
pipeline; a successful empty response lets media negotiation continue independently.

Classic RAOP `SET_PARAMETER` accepts validated volume updates plus progress, DMAP metadata, and
JPEG/PNG/empty artwork notifications. Metadata and artwork bodies are acknowledged but are not
retained, logged, or forwarded into the audio path.
