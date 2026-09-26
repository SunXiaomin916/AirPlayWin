# Timing boundary

Phase 7 introduced the timing interface. Phase 10 adds a remote-clock discipline path, and
phase 11 adds an absolute RTP/PTP phase, without coupling the core to Winsock, IOCP, or WASAPI.

- `IMonotonicClock` abstracts the local counter; the Windows adapter uses QPC.
- `ITimingEngine` maps a remote time value to a local QPC target and exposes a snapshot estimate.
- `BufferedRtpTimingEngine` is the S7 experimental implementation. It anchors one 32-bit RTP
  audio timestamp to `local QPC now + target buffer`, handles RTP timestamp wrap with modular
  subtraction, and maps later sample timestamps at the negotiated audio clock rate.
- RECORD, anchored resume, FLUSH, and stream stop reset the timing generation. Resume without an
  absolute sender clock re-locks on the first resumed frame.
- `PtpPacket` validates the PTPv2 common header and timestamp fields used by Sync, Follow_Up,
  Delay_Req, Delay_Resp, and Announce messages.
- `PtpClockDomain` completes one-step and two-step clock samples and owns `ClockServo`.
- `ClockServo` maps remote nanoseconds to local QPC, filters drift/offset, rejects outliers,
  bounds correction slew, and implements lock, holdover, relock, and unlock. On a master
  identity change, the old model remains the presentation model while the new source converges;
  a continuous handoff is aligned to the old target, while a large phase difference requests
  the protected hard-resync path.
- `DisciplinedRtpTimingEngine` applies the shared clock-domain rate correction while retaining a
  stream-local RTP anchor. A hard clock discontinuity is consumed exactly once by the audio
  epoch/transition boundary.
- `RtpPtpPhaseTimeline` publishes a session-epoch/master-qualified mapping between RTP sample
  position and remote PTP nanoseconds. Group mode requires this mapping and applies the
  endpoint-specific latency correction after remote PTP is mapped to local QPC.

The Windows service currently supplies passive Sync/Follow_Up receive samples. It does not yet
send Delay_Req, estimate asymmetric path delay, or perform grandmaster election. The S11 group
coordinator owns phase publication, membership, join/leave policy, and endpoint offsets outside
the timing engine.
