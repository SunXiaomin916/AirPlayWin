# AirPlayWin 1.0.2

AirPlayWin 1.0.2 is a hardening release of the Classic RAOP receiver for Windows 11 x64. It
contains the fixes from an independent code review and clean-machine verification pass; the
supported profile and the compatibility boundary are unchanged from 1.0.1.

## Changed

- Classic RAOP retransmission is functional end to end: resend requests are sent to the
  sender's control port for missing runs, and loss gaps hold briefly so retransmitted copies
  replay in place.
- The decode worker sleeps on the jitter buffer between packets instead of spinning a CPU
  core for the whole session.
- Loss declaration uses a fixed reorder window, so one dropped packet yields one concealed
  frame instead of a multi-packet stall followed by a burst.
- Live sessions veto the transport idle-timeout scan (paused senders stay connected);
  connections that never complete a request are hard-closed after the timeout.
- Queue-overflow backpressure ends in a guarded hard resync instead of splicing dropped
  frames into the waveform.
- The audio transition guard defers lower-priority commands (quick Pause -> Resume can no
  longer strand playback in silence); stop/pause fades follow live PCM through a partial
  underrun; fade endpoints reach exactly zero.
- The WASAPI output facade is serialized against Close with a lifecycle mutex; the sink
  prefill is clamped to the output high-water target.
- ALAC packets up to 32 KiB are accepted; the Media Foundation decoder drains transient
  MF_E_NOTACCEPTING state.
- The DNS-SD registration timeout path waits for the completion callback after cancelling.
- Recovery retries back off exponentially; the stop event is created before services start;
  autostart logs are pruned to 30 days.
- PTP experiment: conformant minimal two-step Sync packets parse, stale FollowUps expire,
  other-domain traffic is ignored while locked, and the resample ratio is a bounded
  closed-loop depth servo instead of a fixed feed-forward ratio.
- Group experiment: externally observed grandmaster changes trigger holdover, drained
  members re-preroll before relock, lifecycle actions are emitted, past-boundary join
  plans are refused, missed activations fault the member, and the skew analyzer requires
  at least two complete pulses and rejects cross-pulse alias matches.
- GitHub Actions CI runs Debug and Release builds and tests on push and pull requests; a
  deterministic parser fuzz smoke test is part of the native suite; the repository now has
  an MIT license.

## Verified

- MSVC Debug and Release x64 builds with `/W4 /WX`.
- Full native test suite (43 modules) plus the sync, anti-pop, and latency CI gates.
- Clean-machine `--serve`, `--play`, device, and interface enumeration smoke tests.
