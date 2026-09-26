# Windows audio backend

`WasapiAudioOutput` is the only class that owns WASAPI render resources. Shared event-driven
mode remains the default. Phase 9 adds two explicit low-latency paths:

- Shared `IAudioClient3`, initialized at the endpoint's minimum supported engine period;
- Exclusive event-driven output, trying float32 first and PCM16 second, including
  `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` retry handling.

Unsupported Shared low latency reactivates a fresh `IAudioClient` and falls back to legacy
Shared. Exclusive can either fall back the same way or fail strictly. Diagnostics always expose
requested mode, active mode, client path, endpoint sample format, and the HRESULT that caused a
fallback.

The render worker starts before an Exclusive stream, matching the event-driven requirement that
the first buffer event be serviced promptly. A bounded watchdog detects a stream that starts but
never renders. Exclusive events release the entire endpoint buffer; Shared events use current
padding to calculate available frames.

PCM is queued in float32. PCM16 conversion, when needed, happens after `AudioTransitionGuard`
inside a preallocated scratch buffer. Endpoint reopening may resize that scratch storage before
rendering resumes; the active render callback never allocates, logs, performs file/network I/O,
or holds the control mutex.

`EndpointLatencyModel` reports software queue + current endpoint padding + measured engine
latency (or period fallback) + signed calibration offset. This is operational telemetry, not a
physical latency measurement. Use the loopback WAVE analyzer before declaring endpoint-specific
acceptance.

Phase 11 adds `WindowsEndpointCalibrationStore`, an `IEndpointCalibrationStore` adapter backed
by the current-user registry. It stores manual or measured offsets per complete Windows endpoint
ID. Fixed-device `--play` and `--serve` runs load a stored value unless the command line supplies
an explicit override; default-device runs do not reuse a potentially stale endpoint record.
