# AirPlayWin

AirPlayWin is a Windows-native AirPlay/AirPlay 2 audio receiver project. Phases 1 through 3
provide the standalone WASAPI audio engine, native AirPlay/RAOP discovery, and the defensive
RTSP/HTTP control-session foundation. Pairing, RTP, encryption, PTP, media decoding,
multi-room, and WinUI are not implemented yet.

## Phase 1 capabilities

- CMake C++23 project for Visual Studio 2022 and Windows 11.
- Protocol-independent `AudioEngine` and `IAudioOutput` boundary.
- WASAPI shared-mode, event-driven float32 renderer.
- Default endpoint or an explicitly selected endpoint.
- `IMMNotificationClient` recovery for default-device changes and endpoint hot-plug/removal.
- Fixed-capacity, preallocated SPSC PCM ring buffer.
- Monotonic audio epochs for flush, seek, format change, sender replacement, hard resync, and session reset.
- Bottom-of-stack `AudioTransitionGuard` for fade/gate handling, sample-ramped volume, underrun ramp-to-zero, zero fill, silent device prewarm, numeric validation, amplitude limiting, and baseline DC-offset detection.
- 440 Hz, 1 kHz, silence, impulse, and logarithmic sweep test signals.
- Snapshot diagnostics for queue depth, underruns, rendered frames, stale epochs, estimated latency, endpoint ID, sample rate, epoch, transition state, and numeric-safety counters.

## Phase 2 capabilities

- Windows-native mDNS/DNS-SD through `DnsServiceRegister`; no Bonjour dependency.
- Separate `_raop._tcp.local` and `_airplay._tcp.local` service records with bounded,
  normalized instance names and explicit TXT metadata.
- Stable local device ID derived from Windows machine identity, plus CLI override.
- IPv4-first publication on every eligible physical LAN interface.
- `NotifyIpInterfaceChange` monitoring, 500 ms event coalescing, safe withdraw, and rebuild.
- DNS name-conflict detection with consistent numbered renaming across both services.
- Default virtual/VPN/loopback/tunnel filtering with an explicit development override.
- Discovery diagnostics for interfaces, live registrations, generations, network changes,
  conflicts, failures, advertised name, and last Windows error.

## Phase 3 capabilities

- Incremental RTSP/HTTP request parsing across arbitrary TCP fragments and pipelined requests.
- Strict request boundaries: CRLF framing, one `Content-Length`, no chunked transfer coding,
  64 KiB request-head limit, 1 MiB body limit, 100-header limit, and terminal parser errors.
- RTSP response construction with mirrored `CSeq`, explicit `Content-Length`, and safe headers.
- Per-connection session state, sender identity, request/activity tracking, volume state, and
  generated session identifiers.
- One active sender by default; a second `ANNOUNCE` receives RTSP 453. A tested preemption
  policy is available to a future coordinator but is not enabled by the CLI.
- Control methods for `OPTIONS`, SDP `ANNOUNCE`, empty `GET_PARAMETER`, volume
  `SET_PARAMETER`, and `TEARDOWN`. Deferred media methods return explicit RTSP errors.
- Authentication is behind `ISessionAuthenticator`; the current unencrypted profile uses an
  open implementation. Pairing routes return 501 and no long-term/test keys exist.
- Winsock2 overlapped receive/send on an IO completion port, keep-alive TCP connections,
  bounded queues/connections, idle timeouts, deterministic shutdown, and transport metrics.

## Architecture

```text
DNS-SD -> windows::network::IocpTcpServer -> core::protocol parser/service
                                                |
                                  core::session::SessionManager
                                                |
                                   ISessionAuthenticator

future RTP/decoder
       |
       | float32 AudioBuffer + epoch + QPC timestamp
       v
 core::audio::AudioEngine
             |
             | IAudioOutput only
             v
 windows::audio::WasapiAudioOutput
       | fixed SPSC ring
       | AudioTransitionGuard (unskippable render gate)
       v
 WASAPI shared/event-driven endpoint
```

The control service currently stops at the session boundary. It does not call the audio engine;
future decoded PCM must still enter through `AudioEngine`, preserving the epoch and transition
guard invariants. The WASAPI render buffer is never exposed to protocol code.

Discovery is independently layered:

```text
app / future session coordinator
             |
             v
 core::discovery::IDiscoveryService
             |
             v
 windows::network::WindowsDiscoveryService
       | Windows DNS-SD registration
       | IP interface change notification
       v
 native mDNS on eligible LAN interfaces
```

## Prerequisites

- Windows 11
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.26 or newer (the Visual Studio-bundled CMake is supported)

## Configure, build, and test

From a Visual Studio 2022 Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

The checked-in preset is equivalent:

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset debug
```

Warnings are compiled as errors (`/W4 /WX`). No third-party packages are required in the
current phases.

## Audio probe

List endpoints:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --list-devices
```

Play 440 Hz on the current default endpoint for ten seconds:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --play --signal 440 --duration 10
```

Play on a selected endpoint (copy the endpoint ID from `--list-devices`):

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --play --device "{endpoint-id}" --signal sweep --duration 30
```

Run a 30-minute local soak:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --play --signal silence --duration 1800 --diagnostics-interval 60
```

Run 100 combined start/stop, pause/resume, flush, and volume-ramp cycles:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --play --signal 440 --transition-cycles 100
```

During a default-device run, changing the Windows default render endpoint causes a guarded fade/mute, endpoint rebuild, silent prewarm, and fade-in. A selected USB DAC is retried once per second after removal until the same endpoint ID becomes available again.

## Discovery probe

List eligible IPv4 multicast interfaces:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --list-network-interfaces
```

Advertise both AirPlay service types for five minutes:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --discover --name "Living Room PC" --duration 300
```

Use `--device-id AA:BB:CC:DD:EE:FF` to override the stable development identity, or
`--include-virtual-interfaces` when explicitly testing a VM/VPN adapter. The probe only
advertises without opening control ports. Use the phase 3 receiver probe below for control
connections.

## Control-session probe

Publish both services and listen on the matching RAOP/AirPlay TCP ports for five minutes:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --serve --name "Living Room PC" --duration 300
```

The process prints active sessions, parsed/rejected requests, bytes sent/received, idle
timeouts, and transport failures. `--raop-port` and `--airplay-port` override the defaults.
The CLI does not create Windows Firewall rules; allow the executable on the intended private
network profile when testing from another device.

## Verified on the development machine

The following checks were completed on Windows 11 with MSVC 19.44 and Windows SDK 10.0.26100:

- Debug and Release x64 builds passed with `/W4 /WX` (zero warnings).
- Debug and Release CTest passed all core and Windows platform suites.
- Device enumeration returned the default Realtek endpoint plus active/inactive HDMI, USB, Bluetooth, and built-in endpoints.
- Default Realtek 440 Hz probe: five seconds, zero underruns, zero stale epochs, normal exit.
- Explicit NVIDIA HDMI endpoint: three-second silent probe, zero underruns, normal exit.
- 100 combined start/stop, pause/resume, flush, and volume-ramp cycles: zero underruns, zero stale epochs, normal exit; epoch advanced from 1 to 101.
- 30-minute default-endpoint silent soak: 86,401,953 rendered frames, zero underruns, zero stale epochs, normal exit.
- Discovery interface selection returned physical Realtek Ethernet index 4 at
  `192.168.3.43`, with virtual interfaces excluded.
- Five-second native DNS-SD smoke: one active interface, two live registrations,
  generation 1, zero conflicts/failures, `last_error=0`, and clean shutdown.

The measured shared-mode latency estimate on the tested Realtek endpoint was normally 27–37 ms. Its device/engine baseline was about 23 ms; consistently meeting the design target of 30 ms or less requires the deferred `IAudioClient3` minimum-period path and hardware-specific validation.

Default-device switching and physical USB DAC removal/reinsertion are implemented but were not physically exercised in this automated run; they remain manual hardware acceptance checks.

Phase 3 adds fixture-driven parser/session tests plus a real loopback Winsock/IOCP integration
test covering fragmented input, persistent responses, `TEARDOWN`, connection closure, and idle
timeout. A local `--serve` application smoke bound both advertised ports, completed
`OPTIONS -> ANNOUNCE -> TEARDOWN` with three 200 responses, recorded one connection / three
requests / 275 received bytes / 326 sent bytes, and reported zero parser or transport errors.

## Current boundary

Shared mode remains the only enabled WASAPI mode. The backend uses the application format
with the Windows audio engine's shared-mode format converter. Exclusive mode and
`IAudioClient3` minimum-period tuning remain later audio-backend work.

Discovery is IPv4-first and advertises only PCM/unencrypted capabilities that do not imply
the absent pairing, crypto, PTP, or codec modules. The phase 3 server deliberately returns
461 for `SETUP`, 455 for media-state methods, and 501 for pairing routes. Consequently it can
establish and close fixture control sessions but cannot yet receive audio from a current Apple
sender. Physical iPhone/iPad/Mac interoperability, external-host name collision, and live
adapter switching remain later/manual checks.

See [phase 1](docs/phase-1.md) for audio invariants, [phase 2](docs/phase-2.md) for discovery,
and [phase 3](docs/phase-3.md) for parser/session/IOCP boundaries and verification.
