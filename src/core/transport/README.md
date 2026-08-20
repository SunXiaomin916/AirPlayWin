# Transport boundary

`IControlConnectionHandler` is the platform-neutral phase 3 TCP/control callback. Windows IOCP
delivers opaque byte spans through this interface; the protocol layer returns complete writes
and a close-after-write decision.

Future audio packet ordering, loss accounting, and jitter buffering remain separate from this
control transport. They will emit decoded/scheduled buffers to the core audio facade and cannot
include WASAPI headers.
