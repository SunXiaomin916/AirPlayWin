# Core audio

`AudioEngine` is the only PCM submission facade exposed to transport/session composition. It
owns the monotonic audio epoch and delegates platform rendering only through `IAudioOutput`.
Protocol code must not include or call WASAPI APIs.

`AudioTransitionGuard` is the final mutable waveform gate. It provides silent prewarm,
fade-in/out, sample-ramped volume, underrun-to-zero, zero fill, numeric sanitization, amplitude
clamping, and baseline DC monitoring. Safety commands are priority-merged, and a start/resume
arriving during fade-out is deferred until the old tail reaches zero.

Stop and pause fades follow the still-queued PCM waveform so phase remains continuous down to
silence. Timeline-breaking transitions (flush, seek, hard resync, and device switch) stay
isolated from replacement PCM and use the captured safe tail instead.

`ClickPopDetector` observes the guarded output immediately before platform buffer release. It
uses fixed per-channel state and atomics only. The detector reports adjacent-sample threshold
crossings and peak step sizes; it does not modify PCM and is not a replacement for physical
loopback capture.

`LoopbackLatencyAnalyzer` locates an impulse onset in a direct reference channel and a returned
output channel, or compares one returned channel with a known stimulus frame. It is a pure,
allocation-free analyzer over caller-owned samples. File parsing lives in the app layer so the
core has no filesystem dependency.

The render path must not allocate, format logs, perform file/network I/O, or acquire a mutex.
All diagnostics are recorded as counters/atomic snapshots and formatted outside the render
thread.
