# Security policy

AirPlayWin 1.0 is a classic RAOP receiver intended for a trusted Windows Private network.

## Network boundary

- The v1.0 receiver does not implement AirPlay pairing, a PIN, or per-sender authorization.
- Any compatible sender that can reach TCP port 5000 and the negotiated UDP ports may attempt
  to start or control the single active audio session.
- The installer creates program-scoped inbound rules for the Windows Private profile only.
- Do not enable those rules on Public/Domain profiles, forward the ports through a router, or
  expose the receiver directly to the Internet.
- Stop the receiver or remove its firewall rules when using an untrusted LAN.

The parser, queues, and connection counts are bounded, but those defensive limits are not an
authentication mechanism. Full AirPlay 2 pairing and FairPlay remain outside the v1.0 scope.

## Package integrity

Officially prepared archives have an adjacent `.sha256` file. Verify it before installation.
An unsigned local build can trigger Windows SmartScreen; public distributors should sign
`AirPlayWin.exe` with a production Authenticode certificate and timestamp the signature.

## Reporting a vulnerability

Do not publish exploit details before the maintainer has had an opportunity to reproduce and
fix the issue. Include the AirPlayWin version, Windows version, reproduction steps, relevant
logs, and whether the receiver was reachable outside a Private network.
