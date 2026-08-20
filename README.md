# AirPlayWin

AirPlayWin is a Windows-native AirPlay/AirPlay 2 audio receiver project. Phases 1 through 8
provide the standalone WASAPI audio engine, native AirPlay/RAOP discovery, defensive RTSP/HTTP
control sessions, a runnable unencrypted RTP/L16 transport, and sender-control-to-AudioEngine
timeline integration, operational recovery, a repeatable x64 beta package, and an opt-in
buffered RTP-to-QPC timing experiment. Phase 8 formalizes the bottom anti-pop path with an
automatic waveform-discontinuity detector and 1,000-cycle regression harness. Pairing, Apple
codec decoding, encryption, PTP, multi-room, and WinUI are not implemented yet.

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

## Phase 4 capabilities

- Defensive RTP v2 parsing, including CSRC, extension, padding, payload limit, sequence,
  timestamp, SSRC, marker, and payload type.
- Fixed-capacity jitter buffer with sequence wrap extension, startup prefill, reorder,
  duplicate, late, loss, overflow, and interarrival-jitter diagnostics.
- `IAudioDecoder` abstraction plus a signed big-endian RTP/L16 decoder with zero-frame loss
  concealment and float32 output.
- `RtpAudioStream` separates IOCP packet receipt from decode/audio submission using a dedicated
  worker and bounded, preallocated packet/decode storage.
- Windows overlapped UDP/IOCP receiver and a session-scoped controller for negotiated
  audio/control/timing UDP ports.
- RTSP media methods: SDP `ANNOUNCE`, UDP `SETUP`, `RECORD`, `PAUSE`, `FLUSH`, volume
  `SET_PARAMETER`, and `TEARDOWN`.
- `IAudioFrameSink` keeps decoded PCM behind `WindowsAudioStreamSink`, so protocol/transport
  code cannot bypass `AudioEngine`, audio epochs, or `AudioTransitionGuard`.
- Deterministic L16 packet replay/fault injection and end-to-end RTSP-to-UDP-to-decoder tests.

## Phase 5 capabilities

- Strict single-stream `RTP-Info` parsing for optional `seq` and `rtptime` anchors on RECORD,
  resumed RECORD, and FLUSH.
- Wrap-aware sequence/timestamp gates reject pre-boundary packets before jitter-buffer entry;
  FLUSH and an anchored resume reset decoder history and create a new AudioEngine epoch.
- Classic four-byte retransmitted-audio wrappers are accepted on the negotiated control port,
  unwrapped, and passed through the normal RTP/SSRC/payload/timeline checks.
- RECORD returns `Audio-Latency: 11025` for the classic L16 development profile.
- `text/parameters` volume supports multiline SET and query GET; changes remain sample-ramped
  at the bottom audio transition guard.
- Decoded frames carry an optional target QPC scheduling field. Phase 5 uses arrival QPC; phase
  7 can populate a local buffered RTP timeline while full sender-clock synchronization remains
  deferred.
- The platform composition boundary is dependency-injectable for tests. A full automated path
  now crosses RTP -> L16 decode -> WindowsAudioStreamSink -> WindowsAudioEngine -> AudioEngine
  -> IAudioOutput and verifies epoch, pause/resume, flush, volume, and stale-timeline rejection.

## Phase 6 capabilities

- Native suspend/resume notifications are atomically coalesced and handled on the application
  thread; suspend withdraws discovery and closes control/media/audio sessions, while resume
  rebinds TCP and republishes DNS-SD with bounded one-second retries.
- Default audio endpoint changes and fixed-device removal/reinsert recovery now expose endpoint
  switch, retry, success/failure, HRESULT, and recovering-state diagnostics.
- Peer, requested, idle, transport, shutdown, and protocol disconnect reasons are counted; 100
  reconnect cycles verify that sender ownership and session resources are released every time.
- `--serve` is single-instance through a local named mutex. The IOCP TCP server is restartable
  on the same object after lifecycle transitions.
- Explicit firewall status/install/remove commands manage exact executable-scoped inbound
  rules on the Private profile only. Normal receiver startup never mutates the firewall.
- CMake install rules, CPack ZIP output, and guarded PowerShell install/uninstall scripts provide
  the first per-user x64 beta deployment path.

## Phase 7 capabilities

- `ITimingEngine` and `IMonotonicClock` keep presentation-time math platform independent;
  production uses an injected Windows QPC source while tests use a deterministic manual clock.
- The opt-in `BufferedRtpTimingEngine` maps 32-bit RTP audio timestamps to local target QPC
  values with wrap-safe arithmetic and a configurable 20–2,000 ms local buffer.
- RECORD, anchored resume, FLUSH, pause/resume re-lock, and teardown maintain explicit timing
  generations aligned with the packet timeline and AudioEngine epoch boundary.
- Decoded and concealed frames carry mapped target QPC values through `IAudioFrameSink`.
- `WindowsAudioStreamSink` waits on the non-real-time decoder worker until the safe submission
  window; scheduling can be cancelled by a concurrent timeline transition.
- Timing lock, target buffer, mapped/late counts, maximum lateness, scheduled frames, wait time,
  and last target QPC are observable in runtime diagnostics.

## Phase 8 capabilities

- A fixed-storage `ClickPopDetector` runs after the final transition gate and measures
  inter-frame output steps without allocating or locking on the render thread.
- Runtime diagnostics expose analyzed frames, threshold-crossing events, maximum/recent output
  step, transition requests, fade-in/out, SAFE_MUTE, hard-resync, and underrun-transition counts.
- Safety commands are priority-merged so a pending flush, hard resync, or device switch cannot
  be overwritten by a racing start/resume command.
- Start/resume received during a fade-out is deferred until the old tail reaches zero; device
  switches remain muted until the rebuilt endpoint explicitly starts its new prewarm path.
- The automated anti-pop harness runs 1,000 transitions over a continuous 48 kHz stereo sine,
  including stop, pause, flush, seek, hard resync, device switch, underrun, volume steps, and
  deliberate phase jumps after timeline changes.
- Epoch regression now cycles every required timeline-change reason 1,000 times and verifies
  that the previous epoch immediately becomes stale.

## Architecture

```text
DNS-SD -> IocpTcpServer -> AirPlayControlService -> SessionManager
                              |            |
                 ISessionAuthenticator    | IAudioTransportController
                                           v
                      WindowsRtpTransportController -> IocpUdpReceiver
                                                        |
                              RtpAudioStream <- RtpJitterBuffer
                                    | <-> ITimingEngine
                                    |      RTP time -> target QPC
                                    IAudioDecoder (L16)
                                    |
                         float32 IAudioFrameSink
                                    v
                         WindowsAudioStreamSink
                                    |
                         core::audio::AudioEngine
             |
             | IAudioOutput only
             v
 windows::audio::WasapiAudioOutput
       | fixed SPSC ring
       | AudioTransitionGuard (unskippable render gate)
       | ClickPopDetector (post-gate, fixed storage)
       v
 WASAPI shared/event-driven endpoint
```

The control service only calls `IAudioTransportController`; RTP callbacks only parse and queue;
the decoder worker only calls `IAudioFrameSink`. Decoded PCM must enter through `AudioEngine`,
preserving epoch and transition-guard invariants. The WASAPI render buffer is never exposed to
protocol or transport code.

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

Operational lifecycle remains outside the audio/protocol layers:

```text
WindowsPowerEventMonitor -> RecoveryCoordinator -> app composition root
                                                | stop/start TCP + DNS-SD
                                                | fresh session/audio timeline
                                                v
                                         recovery diagnostics
```

Windows callback code only records atomic flags. The composition root owns ordered teardown,
retry, and recovery; protocol code still cannot call power, firewall, or WASAPI APIs.

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

Create the phase 8 x64 beta ZIP after the Release build:

```powershell
cpack --config .\build\vs2022-x64\CPackConfig.cmake -C Release
```

See [installer/README.md](installer/README.md) for the per-user install, startup, firewall, and
uninstall workflow. The beta package is unsigned.

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
.\build\vs2022-x64\Debug\AirPlayWin.exe --play --signal 440 --transition-cycles 1000
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
advertises without opening control ports. Use the phase 4 receiver probe below for control and
development-media connections.

## Control and RTP/L16 development receiver

Publish both services and listen on the matching RAOP/AirPlay TCP ports for five minutes:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --serve --name "Living Room PC" --duration 300
```

After an L16 SDP `ANNOUNCE` and UDP `SETUP`, the process accepts RTP on its negotiated
`server_port`, decodes on a separate stream worker, and submits float32 frames to WASAPI. Add
`--device "{endpoint-id}"` to select a fixed output endpoint; otherwise the sink follows the
default endpoint. The process prints control, RTP, jitter-buffer, decoder, and WASAPI metrics.
`--raop-port` and `--airplay-port` override the control ports.

Enable the S7 local buffered-timing experiment with a 120 ms presentation reserve:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --serve --experimental-buffered-timing `
    --buffered-timing-ms 120 --name "Living Room PC" --duration 300
```

This flag does not advertise complete AirPlay 2 timing. It anchors one RTP audio stream to local
QPC; the reserved timing UDP socket remains isolated until the later PTP/ClockServo stage.

The deterministic sender can inject normal, lost, duplicate, reordered, timeline-anchored,
and retransmission-wrapped L16 packets:

```powershell
.\tools\packet_replay\rtp_l16_replay.ps1 -Port <negotiated-server-port> `
    -DropEvery 25 -DuplicateEvery 40 -ReorderPairs
```

Inspect the two program-scoped Private-profile rules without changing the machine:

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --firewall-status
```

An Administrator can explicitly create or remove those rules with
`--install-firewall-rules` and `--remove-firewall-rules`. Normal `--serve` startup does not
change firewall configuration.

For an installed long-running receiver use `--serve --run-until-stopped`; press Ctrl+C for an
ordered service/audio shutdown. A second receiver instance exits before attempting to bind.

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

Phase 4 adds tests for RTP header/extension/padding parsing, jitter-buffer reorder/loss/late/
duplicate/sequence-wrap behavior, SDP and transport negotiation, L16 decoding and zero
concealment, stream-worker continuity, a real IOCP UDP loopback receiver, and an end-to-end
RTSP `ANNOUNCE -> SETUP -> RECORD -> RTP -> FLUSH/PAUSE/TEARDOWN` path.

Phase 5 adds strict RTP-Info parser cases, wrap-aware stale-timeline rejection, inbound
retransmission-wrapper delivery through the real control UDP port, multiline volume SET/GET,
RECORD latency response checks, and a concrete RTP-to-AudioEngine epoch integration test.

A local phase 4 application smoke received five 200 responses for `ANNOUNCE`, `SETUP`,
`RECORD`, `FLUSH`, and `TEARDOWN`; negotiated an ephemeral audio UDP port; and replayed 63
datagrams with three deterministic drops and two duplicates before clean server shutdown.

The phase 5 verification added a zero-warning Debug/Release build, passing Debug/Release
CTest, and 20 consecutive Debug suite runs. A local application smoke completed nine 200
responses across `ANNOUNCE`, `SETUP`, anchored `RECORD`, multiline volume SET/GET, `PAUSE`,
anchored resume, anchored `FLUSH`, and `TEARDOWN`. Three replay batches delivered 36 L16
packets/12,672 frames, including six retransmission-wrapped packets routed through the control
port; the server reported zero parse/transport errors and zero WASAPI underruns.

Phase 6 adds recovery coordinator, disconnect classification, restartable IOCP TCP, Windows
power notification, single-instance, and read-only firewall-query tests. The complete suite now
contains 22 test modules and passed 50 consecutive Debug runs. A local lifecycle smoke withdrew
both services on a simulated suspend, then rebound the same TCP ports and republished DNS-SD
after resume with one successful recovery attempt and zero native errors; a fresh RTSP request
then received 200 OK, and a competing receiver exited with the documented single-instance code.
CPack generated and verified the expected executable, README, and installer contents. Firewall
mutation, actual machine sleep, Wi-Fi/DHCP transitions, and physical USB/default-endpoint
changes are deliberately left as manual acceptance because the automated suite does not change
host configuration or hardware state.

Phase 7 adds pure timing-model tests, 32-bit timestamp-wrap cases, a deterministic +100 ppm
clock simulation, pause/FLUSH generation checks, target-QPC propagation through RTP decode, and
an inspectable Windows sink scheduling test. The complete suite now contains 23 test modules;
Debug and Release CTest passed, followed by 50 consecutive Release suite runs in 43.53 seconds.
After adding explicit in-flight scheduling-cancellation coverage, another 20 consecutive Release
runs passed. The existing no-timing path remains covered and is the default unless the
experimental CLI flag is supplied.

A real loopback `ANNOUNCE -> SETUP -> RECORD -> RTP/L16` application smoke used a 500 ms local
reserve and delivered 30 packets / 10,560 frames through IOCP, decode, QPC scheduling,
AudioEngine, and WASAPI. All frames were scheduled with zero late or rejected frames, the sink
recorded 697,824 microseconds of active scheduling wait, and protocol/output errors remained
zero. An intentionally low 80 ms run with a slow PowerShell packet producer reported late
mappings as designed; buffer selection and cold-sender behavior remain experimental calibration
work rather than a latency guarantee.

CPack generated `AirPlayWin-0.7.0-windows-x64.zip`, and the executable, installer scripts, and
documentation entries were inspected in the archive.

Phase 8 adds the post-gate click/pop detector, safety-command priority tests, deferred restart
coverage, all-reason epoch cycling, and a 1,000-cycle 48 kHz waveform regression. The synthetic
stress path analyzes 1,920,720 stereo frames while rotating stop, pause, flush, seek, hard
resync, device switch, underrun, volume steps, and discontinuous post-reset source phases. It
completed with zero threshold crossings at the stricter 0.20 full-scale step threshold.

Debug and Release x64 builds passed with `/W4 /WX`; all 25 test modules passed in both
configurations. A 50-run Release regression initially exposed a pause/worker race in which a
packet queued during pause could be popped after the sink was gated but before resume. The worker
now rechecks running/recording state under the transition serialization lock, and the complete
Release suite subsequently passed 50/50 runs in 47.68 seconds.

The real Release WASAPI probe completed 1,000 silent transition cycles with 10,737,153 rendered
frames, epoch 1 -> 1,001, 250 hard resyncs, zero click/pop threshold events, zero stale epochs,
and zero numeric/clipping/DC events. Thirty-one underruns were observed during
the deliberately aggressive loop, and all 31 entered the guarded underrun transition path. The
tested endpoint reported an approximately 23 ms latency estimate and the process exited normally.
CPack generated `AirPlayWin-0.8.0-windows-x64.zip`; the Release executable, installer scripts,
installer guide, and project README entries were inspected in the archive.

## Current boundary

Shared mode remains the only enabled WASAPI mode. The backend uses the application format
with the Windows audio engine's shared-mode format converter. Exclusive mode and
`IAudioClient3` minimum-period tuning remain later audio-backend work.

Discovery is IPv4-first and advertises only PCM/unencrypted capabilities that do not imply the
absent pairing, crypto, PTP, or Apple codec modules. Phase 7 adds only a local single-stream
buffered timing experiment and does not expand advertised media/security capabilities. Phase 8
adds output-safety validation only and likewise changes no advertised protocol capability.
Phase 5 accepts inbound retransmitted audio on the control port, but it does not originate resend
requests or implement timing replies/PTP. Current Apple senders normally need the deferred pairing,
encryption, codec, and timing work; physical iPhone/iPad/Mac interoperability is therefore not
claimed in this phase.

See [phase 1](docs/phase-1.md) for audio invariants, [phase 2](docs/phase-2.md) for discovery,
[phase 3](docs/phase-3.md) for parser/session/TCP boundaries, and
[phase 4](docs/phase-4.md) for RTP, jitter-buffer, decoder, and UDP/IOCP boundaries, and
[phase 5](docs/phase-5.md) for AudioEngine/timeline/control integration, and
[phase 6](docs/phase-6.md) for lifecycle recovery, diagnostics, and beta deployment, and
[phase 7](docs/phase-7.md) for the buffered RTP-to-QPC timing experiment, and
[phase 8](docs/phase-8.md) for anti-pop invariants and waveform regression.
