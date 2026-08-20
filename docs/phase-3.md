# Phase 3: control session foundation

Phase 3 implements the development document's P2 Session / Sprint S3 boundary: defensive
RTSP/HTTP framing, observable per-sender sessions, fixture tests, and Windows control sockets.
It intentionally does not implement Sprint S4 RTP transport, jitter buffering, decoding, or
the pairing/encryption work that requires a separately reviewed security design.

## Runtime flow

```text
TCP 5000 / 7000
       |
       v
IocpTcpServer (Winsock2 overlapped I/O)
       |
       | opaque bytes / complete response writes
       v
IControlConnectionHandler
       |
       v
IncrementalRtspParser -> AirPlayControlService -> SessionManager
                                  |
                         ISessionAuthenticator
```

No control-session code includes WASAPI headers or writes PCM. There is no connection between
the control service and `AudioEngine` in this phase.

## Parser invariants

- Accept `RTSP/1.0`, `HTTP/1.0`, and `HTTP/1.1` request lines.
- Parse arbitrary TCP fragmentation and up to 16 pipelined requests per feed call.
- Require CRLF framing; reject header folding, NUL in the request head, invalid token names,
  duplicate/invalid `Content-Length`, and every `Transfer-Encoding`.
- Limit request lines to 4 KiB, individual headers to 8 KiB, the request head to 64 KiB,
  header count to 100, body to 1 MiB, and buffered data to 2 MiB.
- Treat a parser error as terminal for that connection. The service sends 400 and closes.
- Preserve binary request bodies, including embedded NUL bytes.

RTSP requests require `CSeq`; responses mirror it and always include an explicit
`Content-Length`. The response builder rejects newline injection through generated headers.

## Implemented control behavior

| Request | Phase 3 behavior |
| --- | --- |
| `OPTIONS` | 200 with the actually supported control methods |
| SDP `ANNOUNCE` | Validate content type/body, reserve the one active sender, return a session ID |
| empty `GET_PARAMETER` | 200 keep-alive response |
| volume `SET_PARAMETER` | Validate and store finite -144..0 dB session volume |
| `TEARDOWN` | 200, release on disconnect, close after the queued response |
| `SETUP` | 461 because RTP transport is phase 4 |
| `RECORD` / `PAUSE` / `FLUSH` | 455 because no media stream exists |
| pairing paths | 501; no fake trust or key material |
| other methods | 501 |

The default `RejectNew` policy gives the first valid `ANNOUNCE` ownership of the active slot.
A second sender receives 453. `PreemptExisting` exists and is unit-tested, but enabling it must
later coordinate `AudioEpoch::SenderReplace` and a guarded fade-out before ownership changes.

## IOCP lifecycle

The listener accepts TCP sockets and hands them to one completion-port worker. Each connected
socket has one outstanding receive and at most one outstanding send. Responses are queued in
order, bounded by 2 MiB per connection. The default server limit is 32 connections and the
default idle timeout is 30 seconds.

Shutdown stops the listener, closes active sockets, and continues draining completion packets
until all canceled overlapped operations have completed. Handler exceptions and Winsock errors
close only the affected connection and increment diagnostics.

## Diagnostics

Protocol snapshots expose active/tracked sessions, playback owner, request/parse/rejection/
unsupported/pairing counters, created/closed/preempted counts, and last protocol error.
Transport snapshots expose bound port, active/accepted/rejected connections, received/sent
bytes, idle timeouts, handler/transport errors, and last Windows error.

Per-session snapshots expose connection/session IDs, peer and sender identity, state, request
count, last method, stored volume, ownership, and last activity.

## Verification

Automated tests cover:

- fragmented, pipelined, binary-body, duplicate-length, chunked, and oversized parser cases;
- reject and preempt single-active-session policies;
- fixture `OPTIONS` and SDP `ANNOUNCE`, volume, deferred transport, pairing rejection, and
  `TEARDOWN` state/diagnostics;
- real loopback Winsock/IOCP fragmented request/response, close-after-write, and idle timeout;
- all phase 1 audio and phase 2 discovery regression suites.

Run the receiver probe:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --serve --name "Living Room PC" --duration 300
```

This publishes `_raop._tcp.local` and `_airplay._tcp.local` only after both matching TCP ports
have bound successfully. No firewall rule is added automatically.

## Manual acceptance remaining

- Connect a second machine to both LAN ports and confirm fragmented keep-alive requests.
- Confirm idle clients close after 30 seconds and diagnostics increment exactly once.
- Confirm a second SDP `ANNOUNCE` receives 453 while the first owns the session.
- Confirm `TEARDOWN` allows a later sender to acquire the active slot.
- Exercise malformed traffic and connection churn from another host.

Modern Apple-device audio is not a phase 3 acceptance item: pairing, binary plist capability
exchange, RTP ports, decoder setup, and media timing are deliberately absent.
