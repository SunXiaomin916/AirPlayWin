# Core audio

`AudioEngine` is the only PCM submission facade exposed to transport/session composition. It
owns the monotonic audio epoch and delegates platform rendering only through `IAudioOutput`.
Protocol code must not include or call WASAPI APIs.

`AudioTransitionGuard` is the final mutable waveform gate. It provides silent prewarm,
fade-in/out, sample-ramped volume, underrun-to-zero, zero fill, numeric sanitization, amplitude
clamping, and baseline DC monitoring. Safety commands are priority-merged, and a start/resume
arriving during fade-out is deferred until the old tail reaches zero.

`ClickPopDetector` observes the guarded output immediately before platform buffer release. It
uses fixed per-channel state and atomics only. The detector reports adjacent-sample threshold
crossings and peak step sizes; it does not modify PCM and is not a replacement for physical
loopback capture.

The render path must not allocate, format logs, perform file/network I/O, or acquire a mutex.
All diagnostics are recorded as counters/atomic snapshots and formatted outside the render
thread.
