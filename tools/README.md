# Tools

The `AirPlayWin` executable doubles as the audio and receiver probe: it enumerates
endpoints, generates deterministic PCM, runs timed soaks, exercises guarded transitions, and
composes RTSP/RTP/L16 through WASAPI.

Phase 6 adds a read-only firewall status probe and the development-only `--lifecycle-smoke`
switch. The latter injects a coalesced suspend/resume pair into the real app composition so TCP
restart and DNS-SD republish can be validated without suspending the workstation.

Phase 7 adds the `--experimental-buffered-timing` receiver mode and
`--buffered-timing-ms <20..2000>`. Deterministic clock/wrap/drift simulation lives in the timing
unit suite; the CLI reports its lock, mapping, lateness, and sink scheduling metrics.

`packet_replay/rtp_l16_replay.ps1` is a deterministic RTP/L16 440 Hz sender and network fault
injector. Latency capture and click/pop analysis remain future tools.
