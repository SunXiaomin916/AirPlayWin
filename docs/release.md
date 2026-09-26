# AirPlayWin 1.0 release and acceptance

Version 1.0 has one supported product profile: a Windows 11 x64 Classic RAOP audio receiver.
The executable defaults to that profile and neither advertises nor listens on the experimental
AirPlay 2 control service unless `--experimental-airplay2` is supplied explicitly.

## Release gates

A release archive is acceptable only when all of these gates pass from a clean Git checkout:

1. Debug and Release compile with `/W4 /WX`.
2. Debug and Release CTest pass, including the isolated sync, anti-pop, and latency labels.
3. `AirPlayWin.exe --version` exactly matches the CMake/CPack version and Windows file metadata.
4. Default `--serve` publishes only Classic RAOP and binds TCP 5000, while `--stop` produces an
   orderly shutdown.
5. The ZIP contains the Release executable, install/start/uninstall scripts, README, release
   notes, security policy, and this document; it excludes test binaries and build artifacts.
6. The adjacent SHA-256 digest matches the final archive.
7. A clean extracted-package install/start/stop/upgrade/uninstall smoke succeeds.

`tools/New-ReleasePackage.ps1` automates the build, test, optional Authenticode signing, CPack,
package-content inspection, version smoke, and release-manifest generation.

## Physical acceptance

Before broad distribution, retain logs for:

- continuous iPhone playback on the default endpoint;
- sender volume changes from mute through full scale;
- default endpoint change during playback;
- USB DAC removal and reinsertion;
- Windows sleep/resume and Wi-Fi reconnect;
- repeated start/stop, pause/resume, and flush transitions.

These are hardware/driver checks and cannot be proven by the deterministic unit suite alone.

## Signing

The release script accepts a production certificate thumbprint and `signtool.exe` path. It does
not create or trust a development certificate. Local unsigned packages remain usable after hash
verification, but public distribution should use a timestamped production signature.
