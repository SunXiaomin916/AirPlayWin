# Windows network adapters

`WindowsDiscoveryService` uses the Windows DNS-SD API for native mDNS publication and
`NotifyIpInterfaceChange` for adapter changes. It withdraws and rebuilds per-interface
registrations on the control worker; the notification callback only signals an event.

`WindowsNetworkInterfaceEnumerator` uses `GetAdaptersAddresses`. IPv4 multicast-capable,
up, non-loopback, non-tunnel physical interfaces are enabled by default. Obvious virtual
interfaces can be included explicitly for development.

`IocpTcpServer` is the phase 3 RTSP/HTTP control transport. Accepted sockets use overlapped
`WSARecv`/`WSASend` on a completion port and communicate with core code only through
`IControlConnectionHandler`. Connection counts, receive buffers, pending writes, and idle time
are bounded.

`IocpUdpReceiver` is the phase 4 datagram adapter. It posts a bounded set of preallocated
overlapped `WSARecvFrom` operations and forwards completed spans through
`IUdpDatagramHandler`. `WindowsRtpTransportController` composes one audio receiver, reserved
control/timing receivers, `RtpAudioStream`, and the platform audio sink for the active session.
The IOCP callback never decodes audio.
