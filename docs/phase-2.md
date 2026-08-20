# Phase 2: Windows-native AirPlay discovery

## Delivered scope

Phase 2 publishes the receiver through Windows-native mDNS/DNS-SD without Bonjour or
another background service. It deliberately does not open RTSP/RTP listeners or implement
pairing, encryption, PTP, media decoding, multi-room coordination, or UI.

The development probe advertises both:

| Service | Instance label | Default port | Purpose |
|---|---|---:|---|
| `_raop._tcp.local` | `<12-hex-device-id>@<speaker-name>` | 5000 | Legacy AirPlay audio discovery |
| `_airplay._tcp.local` | `<speaker-name>` | 7000 | Unified AirPlay audio discovery |

The human-visible speaker name, DNS-SD instance name, service type, host label, and stable
device identifier are separate fields. Dots, backslashes, control characters, empty names,
UTF-8 byte limits, and numeric conflict suffixes are normalized in the core layer.

## Architecture and lifecycle

```text
app / future session coordinator
              |
              v
 core::discovery::IDiscoveryService
      | records + interface policy
      v
 windows::network::WindowsDiscoveryService
      |                         |
      | DnsServiceRegister      | NotifyIpInterfaceChange
      v                         v
 Windows DNS-SD/mDNS       signal-only callback
      ^                         |
      +---- withdraw/rebuild ---+  (500 ms coalescing)
```

Registration and deregistration are asynchronous Windows operations, but their request,
cancel handle, service instance, callback context, and completion event remain alive until
the callback completes. Callback-returned `DNS_SERVICE_INSTANCE` copies are released with
`DnsServiceFreeInstance`. Shutdown cancels interface notification before closing events and
waits for every published service to be withdrawn.

Each eligible interface gets its own RAOP and AirPlay registration. A network change first
withdraws the old set and then enumerates and publishes the new set. Name-conflict DNS
statuses or a callback-returned name that differs from the requested name cause all partial
registrations to be withdrawn and retried with ` (2)`, ` (3)`, and so on, keeping both
service names consistent even when Windows performs automatic conflict renaming.

## Interface and identity policy

- MVP publication is IPv4-first; the change monitor listens to `AF_UNSPEC`, so an IPv6
  topology change still causes a safe refresh. IPv6 records are reserved for a later pass.
- Interfaces must be up, have IPv4 unicast, support multicast, and not be loopback/tunnel.
- Obvious VM, VPN, WSL, container, TAP/TUN, packet-capture, and Bluetooth adapters are
  filtered by default. `--include-virtual-interfaces` overrides only the virtual filter.
- The development identity is a deterministic, locally administered unicast 48-bit value
  derived from Windows `MachineGuid`, with hostname fallback. A configured device ID wins.
  Pairing keys and cryptographic identity remain a later module.

## Advertised capability policy

The phase-2 records contain the metadata required to describe an audio receiver, but avoid
claiming pairing, encryption, buffered audio, PTP, ALAC, or AAC before those modules exist.
RAOP currently declares PCM (`cn=0`) and no encryption (`et=0`). The AirPlay feature mask
declares audio, PCM receive, unencrypted audio, and unified advertisement only.

This means a sender can discover the development receiver, but an attempted connection is
expected to fail until the RTSP/session phase supplies listeners on the advertised ports.

## Diagnostics

The snapshot reports running state, active eligible interfaces, live registrations,
publication generation, observed network changes, name conflicts, registration failures,
resolved advertised name, and the last Win32/DNS error.

## Development commands

```powershell
.\build\vs2022-x64\Debug\AirPlayWin.exe --list-network-interfaces
.\build\vs2022-x64\Debug\AirPlayWin.exe --discover --name "Living Room PC" --duration 300
.\build\vs2022-x64\Debug\AirPlayWin.exe --discover --device-id 02:11:22:33:44:55 --duration 30
```

## Automated coverage

- device-ID parse, format, deterministic generation, and local/unicast bits;
- device/host name normalization and conflict suffixing;
- RAOP and AirPlay instance names, ports, FQDNs, and critical TXT fields;
- physical/virtual/loopback interface policy;
- Windows system identity, hostname, eligible-interface enumeration, de-duplication;
- invalid discovery-service lifecycle behavior;
- all phase-1 audio regression suites.

## Recorded phase-2 verification

- Windows 11, MSVC 19.44, Windows SDK 10.0.26100, x64 Debug and Release: clean
  `/W4 /WX` builds.
- Debug and Release CTest: all suites passed.
- The development machine selected physical Realtek Ethernet interface index 4 at
  `192.168.3.43`; virtual adapters remained excluded.
- A five-second native DNS-SD smoke registered two services, reported one interface,
  generation 1, zero conflicts/failures, `last_error=0`, and exited normally after clean
  deregistration.
- A Release smoke with the Unicode service name `Windows原生音箱` also registered both
  services with zero errors and exited normally.
- Two local processes with different development device IDs both registered successfully;
  the Windows DNS-SD service coalesces/permits this local case, so an external-device name
  collision remains a manual LAN acceptance test.

## Remaining manual acceptance

- Confirm sustained visibility from at least one current iPhone/iPad and one Mac.
- Disable/enable Ethernet or Wi-Fi and verify generation/network-change counters increase
  and visibility returns.
- Switch between Ethernet and Wi-Fi and confirm old interface records disappear.
- Introduce the same service name from another LAN host and verify the receiver advertises
  a consistent numbered suffix.
- Verify behavior on VPN-heavy and IPv6-only networks; IPv6 publication is not yet claimed.
