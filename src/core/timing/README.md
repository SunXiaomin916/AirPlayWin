# Timing boundary

Phase 7 introduced the timing interface. Phase 10 adds a remote-clock discipline path without
coupling the core to Winsock, IOCP, or WASAPI.

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
  bounds correction slew, and implements lock, holdover, relock, and unlock.
- `DisciplinedRtpTimingEngine` applies the shared clock-domain rate correction while retaining a
  stream-local RTP anchor. A hard clock discontinuity is consumed exactly once by the audio
  epoch/transition boundary.

The Windows service currently supplies passive Sync/Follow_Up receive samples. It does not yet
send Delay_Req, estimate asymmetric path delay, perform grandmaster election, or establish the
absolute RTP-to-PTP phase. Group membership, join/leave policy, endpoint calibration exchange,
and phase alignment remain isolated for S11.
