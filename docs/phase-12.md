# Phase 12: synchronization analysis and regression hardening

Phase 12 implements sprint S12 from the development document: Group Sync Analyzer, repeatable
network fault injection, accelerated long-horizon clock regression, CI gates for synchronization,
anti-pop, and latency, plus a wall-clock soak runner. The production render path remains unchanged:
no analyzer, file I/O, random generator, or test fault policy runs on an audio thread.

## Group Sync Analyzer

`GroupSyncAnalyzer` is platform-neutral and accepts two input forms:

- an interleaved 2–4 member WAVE capture, with one capture channel per endpoint; or
- presentation timestamp observations identified by pulse and member.

For waveform input, threshold crossings locate pulse candidates. Each non-reference candidate is
matched within the configured skew window and refined against the reference by normalized
cross-correlation. Missing member pulses mark a pulse incomplete instead of silently treating it
as aligned. The result reports:

- reference, complete, and incomplete pulse counts;
- group skew p50, p95, p99, maximum, and first-pulse alignment;
- correlation refinements and onset-only fallbacks;
- per-member mean/minimum/maximum signed offset, absolute-offset percentiles, and mean correlation.

The coordinator diagnostics already expose group ID, member states, and active master identity.
S12 extends them with recent/maximum join and relock durations; callers provide the member timing
observation's remote PTP timestamp so the duration stays in the shared clock domain.

The default target is p95 at or below 1,000 microseconds, p99 at or below 2,000 microseconds, and
first alignment at or below 5,000 microseconds. Analyze a synchronized multichannel capture with:

```powershell
.\AirPlayWin.exe --analyze-group-sync group-capture.wav --group-channels 0,1,2,3
```

`--onset-threshold`, `--pulse-gap-ms`, and `--max-skew-ms` tune capture analysis. Exit code `31`
means the capture could not produce a valid result; `32` means analysis succeeded but the targets
were missed. Capture channels default to the first two through four channels when no explicit list
is supplied.

## Deterministic fault injection

`NetworkFaultInjector` produces a decision for each packet from a fixed seed. Its configuration
covers base latency, up to 100 ms jitter, random loss, periodic burst loss, duplication, reordering,
and delayed reordered delivery. Diagnostics count every decision and retain the observed delay
range. Resetting restores both counters and the exact pseudo-random sequence.

The component is test-only policy and is not inserted into production receive code. The PowerShell
RTP/L16 replay tool exposes the same practical scenarios at the process boundary:

```powershell
.\tools\packet_replay\rtp_l16_replay.ps1 -Port 54321 -PacketCount 10000 `
    -BaseLatencyMilliseconds 50 -JitterMilliseconds 100 `
    -RandomLossPercent 2 -BurstDropEvery 500 -BurstDropLength 5 -Seed 20260926
```

## Long-horizon and CI regression

`GroupSoakRegression` advances four independent clock servos through a deterministic simulated
24-hour period. It includes -100/-33/+50/+100 ppm clock rates, seeded packet loss and jitter,
0.5/2/5-second sample outages, a master change, holdover/relock, and distinct endpoint-latency
corrections. Pulse observations sampled from the recovered presentation times must meet the S12
p95/p99 skew targets without a hard resynchronization.

CTest exposes three separately labelled CI gates:

```powershell
ctest --preset ci-regression --output-on-failure
ctest --test-dir build\vs2022-x64 -C Release -L sync --output-on-failure
ctest --test-dir build\vs2022-x64 -C Release -L anti-pop --output-on-failure
ctest --test-dir build\vs2022-x64 -C Release -L latency --output-on-failure
```

For an actual elapsed-time software soak after a Release build:

```powershell
.\tools\Run-S12Regression.ps1 -Configuration Release -DurationHours 24
```

The runner repeats the native suite until the wall-clock deadline and writes a JSON artifact with
start/end time, run counts, failures, and captured failure output. `-Iterations 50` provides a
bounded repeat, and `-SyncOnly` selects only the accelerated clock simulation.

## Realtime and safety boundary

Network faults continue through the existing parser, jitter buffer, decoder, timing engine, member
gate, audio epoch, and `AudioTransitionGuard` path. Lost or stale timeline data can never inject an
old epoch into WASAPI. Hard resynchronization still means fade-out, epoch/timeline replacement, and
fade-in. The analyzer consumes completed capture data after rendering and therefore may allocate;
the render and decoder realtime constraints are unchanged.

## Completed automated validation

MSVC Debug and Release x64 builds pass with `/W4 /WX`. The complete 41-module suite passes in
both configurations, and the three Release CI-labelled gates pass independently. The Release
suite then completed 50/50 full repetitions in 52.728 seconds with zero failed runs. The packaged
copy of `Run-S12Regression.ps1` also completed a standalone synchronization-regression smoke test.

CPack produced `AirPlayWin-0.12.0-windows-x64.zip`. The archive contains the Release receiver,
native regression executable, S12 runner, RTP fault replay tool, S12 guide, project README, and
installer scripts. The final checksum is reported alongside the generated archive so the package
does not contain a self-referential digest.

## Acceptance boundary

Automated tests validate calculations and state transitions, but they do not substitute for the
development document's physical acceptance matrix. In particular, an accelerated 24-hour clock
simulation is not a 24-hour process soak, and a process soak is not a controlled acoustic or wired
multi-endpoint skew measurement.

Before declaring physical S12 acceptance, run calibrated 2/3/4-endpoint captures for at least two
continuous hours, dynamic join/leave/reconnect cycles, the 1,000-event hot-plug/transition matrix,
and real 2/8/24-hour soaks across USB DAC, Realtek, HDMI, and relevant Bluetooth hardware. Retain
the WAVE captures and JSON outputs with endpoint, driver, network, and calibration metadata.

At the S12 boundary, pairing, encryption, Apple codec decoding, full bidirectional PTP delay
measurement, inter-host group-control transport, and WinUI were outside the sprint. Subsequent
v1.0 productization added the narrow Classic RAOP RSA-AES and Apple Lossless path; modern
pairing/FairPlay, full PTP, inter-host group control, and WinUI remain outside v1.0.
