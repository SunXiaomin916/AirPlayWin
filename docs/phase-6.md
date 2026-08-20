# Phase 6: operational recovery and beta deployment

Phase 6 implements sprint S6 from the development document: harden disconnect/reconnect,
device and network transitions, sleep/resume, diagnostics, and the first repeatable x64 beta
package. It preserves the phase 5 open RTP/L16 interoperability boundary and does not add
pairing, encryption, Apple codecs, PTP, multi-room, or UI work.

## Runtime recovery path

```text
Windows power callback                 network / endpoint notifications
        | atomic flags                           | short native callbacks
        v                                        v
WindowsPowerEventMonitor       WindowsDiscoveryService / WasapiAudioOutput
        | application-thread poll                | worker-owned rebuild
        v                                        |
core::lifecycle::RecoveryCoordinator             |
        |                                        |
        +-------------- recovery diagnostics ----+
        |
        +-- suspend: withdraw DNS-SD -> stop TCP -> session teardown -> audio fade/stop
        +-- resume:  bind TCP -> publish DNS-SD -> accept a fresh sender
                     retry once per second on failure
```

Power callbacks never stop services directly. They only set atomic event flags. The receiver
loop owns the ordered transition, which avoids reentrant COM, Winsock, discovery, or audio work
inside a Windows callback. Suspending both TCP servers triggers ordinary connection shutdown,
so the active session, RTP worker, UDP ports, decoded PCM queue, and audio output unwind through
their existing RAII boundaries. A resume always starts a fresh control/media timeline.

Default endpoint changes and selected-device removal remain inside `WasapiAudioOutput`: the
render worker fades/mutes, clears queued PCM, retries endpoint activation, primes the new device
with silence, and fades in only after recovery. Phase 6 adds observable switch, attempt,
success, failure, HRESULT, and recovering-state metrics to that path.

## Disconnect and network behavior

- `AirPlayControlService` categorizes peer, requested, idle, transport, shutdown, and protocol
  disconnects. Every path releases active-sender ownership and tears down the session transport.
- `IocpTcpServer` can be stopped and started again on the same object. The restart path is
  exercised over real loopback sockets.
- Discovery continues to rebuild on interface notifications. Resume additionally performs a
  full withdraw/rebind/republish cycle, covering sleep, Wi-Fi loss, DHCP changes, and adapters
  that return with a different address.
- Only physical, up, multicast-capable LAN/Wi-Fi interfaces are used by default. The existing
  development override can include virtual adapters explicitly.

## Single instance and firewall boundary

`--serve` acquires `Local\\AirPlayWin.Core`. A second receiver exits before binding ports,
preventing split session ownership and inconsistent advertisements.

Firewall changes are never an implicit side effect of normal receiver startup. The executable
provides explicit status/install/remove commands. Rules are inbound allow rules scoped to the
exact executable and Private profile:

- TCP: configured RAOP and AirPlay control ports;
- UDP: negotiated media/control/timing ports. These are dynamic, so the rule covers UDP for the
  exact executable rather than opening a fixed machine-wide port range.

Install is idempotent and repairs only rules with the exact AirPlayWin rule names. Uninstall
removes only those names. Public and Domain profiles are not enabled.

## Beta package and upgrade behavior

CMake install rules and CPack produce `AirPlayWin-0.6.0-windows-x64.zip`. The PowerShell
installer uses the exact per-user `%LOCALAPPDATA%\\Programs\\AirPlayWin` target, creates a Start
Menu shortcut, optionally creates an HKCU startup entry, and delegates firewall work to the
installed executable. It refuses an upgrade while that installed executable is running.

The ZIP is intentionally unsigned. Production identity, signing, MSIX/App Installer update
feeds, rollback, and publisher certificate operations remain a later productization task.

## Diagnostics

The receiver reports:

- power notification registration and suspend/resume event counts;
- recovery generation, attempts, successes, failures, and last error;
- disconnect reasons and active sender/session ownership;
- TCP/discovery restart state and native errors;
- audio endpoint switches, recovery attempts/results, current HRESULT, and recovering state;
- all earlier RTP, jitter, decoder, epoch, transition, latency, and underrun metrics.

## Automated acceptance

Phase 6 tests cover:

- recovery state-machine idempotence, retry, success, and failure accounting;
- 100 peer-disconnect/reconnect sessions without leaked sender ownership;
- IOCP TCP stop/start followed by a successful request on the rebound port;
- retransmitted audio arriving after the next-sequence packet on the separate control socket,
  without premature loss classification;
- actual Windows power notification registration plus deterministic suspend/resume coalescing;
- single-instance mutex contention and release;
- firewall rule specification and read-only status queries without mutating the host.

The `--lifecycle-smoke` development flag injects one suspend/resume cycle while the real TCP and
DNS-SD composition is running. `--firewall-status` is the read-only deployment probe. Physical
sleep/resume, Wi-Fi off/on, DHCP changes, default-device switching, and USB DAC removal/reinsert
remain manual hardware acceptance checks because automated tests must not change the developer
machine's power, network, audio, or firewall state.

## Next stage

Per the development document, phase 7 is the AirPlay 2 buffered/timing experiment. It should
populate the existing optional target-QPC field through a timing abstraction while keeping the
recovery coordinator independent of protocol clocks. Adaptive low-latency work belongs to S9;
the complete Windows PTP/ClockServo and holdover/relock path belongs to S10.
