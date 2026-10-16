# AirPlayWin 1.1.2

AirPlayWin 1.1.2 fixes the volume control synchronization:

- The GUI volume slider now follows the live value in both directions:
  adjusting the volume on the iPhone updates the slider (via the session
  volume), and dragging the AirPlayWin entry in the Windows Volume Mixer
  is read back from the per-app audio session every diagnostics tick
  instead of reporting the last internally requested value.
- The volume set from the GUI applies even when no sender session is
  active (previously the sink silently dropped it until a session
  configured the audio path); the requested volume is applied to the
  per-app session as soon as the endpoint opens.
- Fixed numeric/boolean payload parsing in the GUI bridge (the volume,
  port, offset, and toggle commands now read their values correctly).

Note on direction: the AirPlay protocol's volume control is one-way
(sender to receiver). The receiver cannot drive the volume indicator on
the iPhone, but changes made on the phone are reflected in the app and in
Windows immediately, and changes made in the app or Windows apply to
playback right away.

See 1.1.0 below for the graphical application feature set and 1.1.1 for
the DPI fix.

# AirPlayWin 1.1.1

AirPlayWin 1.1.1 fixes per-monitor DPI awareness: both executables now embed a PerMonitorV2 manifest (the GUI also declares it at startup), so text renders crisply on 125% / 150% / 200% scaled displays instead of being bitmap-stretched. See the 1.1.0 notes below for the graphical application feature set.

# AirPlayWin 1.1.0

AirPlayWin 1.1.0 adds the graphical application: a native Win32 window hosting a WebView2
UI with a tray icon, persisted settings, autostart, firewall management, and a test-tone
probe. The receiver core is unchanged from 1.0.2 — the console CLI and the GUI share the
same `ReceiverHost` composition.

## Added

- **AirPlayWinApp.exe**: full graphical application.
  - 首页: receiver start/stop, sender/session state, local volume, latency and quality
    counters.
  - 音频: output device selection or follow-default, low-latency (IAudioClient3),
    Exclusive, strict-exclusive, test tone (440 Hz / 1 kHz / sweep / silence).
  - 网络: device name with live DNS-SD republish, control ports, firewall rule status
    with one-click install/remove (UAC elevation), discovery statistics.
  - 高级: buffered-timing and receive-only PTP experiments, per-endpoint latency
    calibration, autostart, minimize-to-tray, diagnostics interval, restart banner for
    settings that need a receiver restart.
  - 诊断: live transport/audio/clock/lifecycle counters and log-folder access.
  - 关于: version, features, compatibility disclaimer, project link.
- Settings persist to `%LOCALAPPDATA%\AirPlayWin\settings.json` (versioned, hand-rolled
  JSON parser/writer, zero new dependencies).
- Tray icon with open / start / stop / exit; closing the window minimizes to the tray by
  default; second launch focuses the existing instance.
- Receiver start/stop runs on a worker thread so WASAPI/DNS-SD setup can never stall the
  UI message loop.
- `--test-bridge`: automated end-to-end self test that loads the real UI in WebView2,
  round-trips every command (start, getState, setConfig, setVolume, listDevices,
  listInterfaces, calibration, autostart, firewall status, stop) through the real
  receiver, and writes the result to `%LOCALAPPDATA%\AirPlayWin\test-bridge.json`.
- Installer installs both executables plus the `ui` resources; the start-menu entry
  launches the GUI, a diagnostics shortcut runs the console receiver; autostart registers
  the GUI directly (no PowerShell wrapper); the uninstaller covers both executables and
  the UI directory.
- WebView2 SDK 1.0.4258.31 headers and the static loader are vendored under
  `third_party/webview2` (WebView2 is an inbox Windows 11 component; the Evergreen
  runtime is already installed on supported systems).

## Changed

- The console CLI's receiver composition moved into `ReceiverHost`, shared verbatim by
  the console and the GUI; console behavior and exit codes are unchanged.

## Verified

- MSVC Debug and Release x64 builds with `/W4 /WX` (both executables).
- Full native test suite (42 modules) plus the sync, anti-pop, and latency CI gates.
- `--test-bridge` passes all 12 checks against the real receiver on a clean machine.
- Console `--serve`, `--play`, lifecycle smoke, and the GUI window render verified
  locally.
