# Timing boundary

Phase 7 introduces an explicit timing boundary without claiming a complete AirPlay 2 clock
implementation.

- `IMonotonicClock` abstracts the local counter; the Windows adapter uses QPC.
- `ITimingEngine` maps a remote time value to a local QPC target and exposes a snapshot estimate.
- `BufferedRtpTimingEngine` is the S7 experimental implementation. It anchors one 32-bit RTP
  audio timestamp to `local QPC now + target buffer`, handles RTP timestamp wrap with modular
  subtraction, and maps later sample timestamps at the negotiated audio clock rate.
- RECORD, anchored resume, FLUSH, and stream stop reset the timing generation. Resume without an
  absolute sender clock re-locks on the first resumed frame.

The model reconstructs a single-stream buffered presentation timeline. It does not parse PTP,
estimate a remote wall-clock offset, servo drift, enter holdover, or synchronize multiple
receivers. Those responsibilities remain isolated for the documented S10/S11 stages.
