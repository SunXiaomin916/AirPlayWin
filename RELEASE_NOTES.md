# AirPlayWin 1.0.0

AirPlayWin 1.0.0 is the first supported Classic RAOP release for Windows 11 x64.

## Supported

- iPhone/iPad/Mac discovery through the native Windows DNS-SD API.
- Classic RAOP realtime audio using Apple Lossless or L16.
- Legacy RSA-AES session-key handling and AES-128-CBC audio decryption.
- Event-driven WASAPI Shared output to the default or a selected Windows endpoint.
- iPhone sender-volume synchronization with the `AirPlayWin.exe` Windows mixer session.
- Pause, resume, flush, sender replacement, epoch isolation, anti-pop transitions, underrun
  protection, and default-device recovery.
- Per-user ZIP installation, optional startup, Private-profile firewall rules, live timestamped logs,
  and an orderly stop command/shortcut.

## Compatibility boundary

This release intentionally advertises only `_raop._tcp` by default. It is not a complete
AirPlay 2 implementation: pairing/FairPlay, modern buffered sessions, complete bidirectional
PTP, networked multi-room coordination, video/screen mirroring, and WinUI are not included.

Authentication is open on the local network. Read `SECURITY.md` before enabling the receiver.

## Verified baseline

- MSVC Debug and Release x64 builds with `/W4 /WX`.
- Complete native test suite and isolated synchronization, anti-pop, and latency gates.
- Physical iPhone playback through Apple Music and QQ Music.
- Physical iPhone volume control reflected by the Windows per-application mixer.
