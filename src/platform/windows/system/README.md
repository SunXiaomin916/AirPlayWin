# Windows lifecycle and deployment services

This directory contains platform-only adapters for operational recovery and deployment. Core
protocol, transport, and audio modules do not include Windows power or firewall APIs.

- `WindowsPowerEventMonitor` registers a `DEVICE_NOTIFY_CALLBACK` power notification. The
  callback records coalesced suspend/resume flags atomically; application-thread polling owns
  service teardown and recovery.
- `SingleInstanceGuard` uses a named local mutex so only one receiver owns the advertised TCP
  ports and active sender at a time.
- `WindowsFirewallManager` manages only two exact, program-scoped inbound rules. They are
  restricted to the Private profile and are changed only by explicit CLI or installer actions.

The platform-neutral `core/lifecycle/RecoveryCoordinator` tracks suspend/resume generations,
retry results, and errors. It deliberately does not call Windows APIs or audio/network objects.
