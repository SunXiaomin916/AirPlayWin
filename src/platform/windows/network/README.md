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
are bounded. RTP packet transport remains deferred.
