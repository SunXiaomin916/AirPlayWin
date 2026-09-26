# Phase 11: group coordination and absolute presentation phase

Phase 11 implements sprint S11 from the development document: `GroupCoordinator`, dynamic
member join/leave, and endpoint-offset calibration for a 2–4 member synchronization group. It
builds on the S10 PTP/QPC clock domain without making the timing engine depend on UI or WASAPI.

This phase establishes and tests the in-process coordination contract. It does not claim that
several physical computers have met the document's p95/p99 skew goals; external multi-host
control, synchronized capture, the Group Sync Analyzer, 24-hour soak, and fault injection are
reserved for S12 and hardware validation.

## Architecture

```text
                         GroupCoordinator
       group/session/master/member state/capability/offset policy
                    |                         |
                    | shared phase            | member runtime
                    v                         v
             RtpPtpPhaseTimeline      GroupMemberAudioGate
            RTP <-> remote PTP ns      mute/pre-roll/boundary
                    |                         |
                    v                         v
         DisciplinedRtpTimingEngine -> IAudioFrameSink
                    |                         |
             PtpClockDomain/QPC        AudioTransitionGuard
                                              |
                                            WASAPI
```

Core code has no Windows or UI dependency. The Windows calibration adapter is the only S11
component that touches the registry. The protocol and transport layers still cannot call
WASAPI directly.

## Common presentation timeline

`RtpPtpPhaseAnchor` contains the RTP timestamp, remote PTP nanoseconds, audio sample rate,
session epoch, and master clock identity. `RtpPtpPhaseTimeline` performs wrap-aware conversion
in both directions and rejects invalid or unrepresentable mappings.

When a shared phase is supplied, `DisciplinedRtpTimingEngine` maps:

```text
RTP timestamp -> remote PTP nanoseconds -> local QPC -> endpoint offset correction
```

Group mode requires the absolute mapping. Missing, stale, or master-mismatched phase data
returns no target; `RtpAudioStream` drops and counts those frames instead of silently falling
back to a receiver-local timeline. This is the key rule preventing nominally healthy members
from rendering different moments.

## Join, leave, and master changes

A member must support the target format, scheduled render, and drift correction. After its
clock locks to the active master and its buffered frames meet the configured pre-roll, the
coordinator rounds a future target to a shared presentation boundary. Existing members keep
playing while a late member waits.

The member audio gate sends preallocated silence before the target and restores real PCM at
the target RTP timestamp. Volume restoration uses the existing sample-ramped sink path. It
does not allocate in `Submit` and never bypasses `AudioTransitionGuard`.

Removing or dropping one member does not increment the group session epoch and does not modify
the other members' activation or presentation timeline. A master change puts active members in
`Holdover`. Relock is refused until a new matching master phase is published; the update then
returns members to `Active` without creating a discontinuous local timeline jump.

The clock servo uses a two-model handoff during that interval. The old model continues mapping
presentation targets while samples from the replacement master converge in a candidate model.
At relock, a small phase difference is aligned to the previous target; a difference above the
hard-resync threshold emits the existing one-shot hard-resync request instead of stepping PCM.

## Endpoint calibration

`IEndpointCalibrationStore` supports manual and measured records with a signed offset limited
to ±1,000,000 microseconds. The Windows implementation stores per-endpoint values under:

```text
HKCU\Software\AirPlayWin\EndpointCalibration
```

The registry value name is a stable hash, while the full endpoint ID is retained and checked
inside the value to guard against collisions. Runtime calibration changes are rejected for
active, holdover, muted-ready, or leaving members.

Manage a fixed endpoint's persisted offset with:

```powershell
.\AirPlayWin.exe --save-endpoint-offset --device "{endpoint-id}" --endpoint-offset-us 1500
.\AirPlayWin.exe --show-endpoint-offset --device "{endpoint-id}"
.\AirPlayWin.exe --clear-endpoint-offset --device "{endpoint-id}"
```

`--play` and `--serve` automatically load a stored offset for an explicitly selected endpoint
unless `--endpoint-offset-us` overrides it. The default endpoint is intentionally not
auto-loaded because its identity can change during the process.

## Diagnostics and validation

The group snapshot reports session/master state, member counts, active/holdover counts, join
plans and activations, add/remove/drop activity, master changes, relocks, and calibration
updates. Member snapshots expose clock identity, buffer depth, uncertainty, activation phase,
state, and endpoint offset. The audio gate reports muted/audible/rejected frames and ramp
transitions. Media timing reports phase availability, session epoch, phase mappings/failures,
unmapped dropped frames, and the applied endpoint offset.

Automated coverage includes:

- 2–4 member capacity and capability enforcement;
- common-boundary activation for two members and non-disruptive late join;
- leave/drop without changing the surviving timeline or session epoch;
- master-change holdover, rejection before new phase, and smooth relock after it;
- endpoint calibration validation, persistence, collision-safe identity checking, and removal;
- silence pre-roll, boundary ramp-in, audible holdover, and leave ramp-out;
- RTP/PTP wrap-aware absolute mapping and endpoint-offset application;
- mandatory drop when required absolute phase is missing;
- 100 repeated add/leave cycles.

Build and test with:

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
```

The final Debug and Release builds passed under MSVC `/W4 /WX`. All 38 test modules passed in
both configurations, and the Release executable completed 50/50 full-suite repetitions in
46.97 seconds. The calibration CLI also completed a real save/show/clear HKCU registry
round-trip with exit codes `0/0/0`.

CPack produced `AirPlayWin-0.11.0-windows-x64.zip`. The archive contains the Release executable,
project README, installer guide, and install/uninstall scripts; it is 251,323 bytes with SHA-256
`099356564E8414235F8DAB24256BA20964C659A653D9E4EAFEA4437643987158`.

## Acceptance boundary and next stage

S11's deterministic core criterion is satisfied when 2–4 members can be dynamically managed,
join plans share a future presentation boundary, unsafe local fallback is impossible, a late
member does not pause active members, master replacement uses holdover plus a new phase, and
per-endpoint offsets persist and enter both scheduling and latency reporting.

S12 should build the Group Sync Analyzer around these snapshots, add controlled-network skew
measurement (p95/p99), automate 24-hour soak and fault injection, and validate real USB,
Realtek, HDMI, and Bluetooth endpoint offsets. Physical Apple interoperability still requires
the deferred pairing, encryption, codec, and complete AirPlay 2 session work.
