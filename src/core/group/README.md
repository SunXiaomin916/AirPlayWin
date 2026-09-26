# Group synchronization boundary

Phase 11 keeps multi-speaker policy in the platform-neutral core. `GroupCoordinator` owns the
group identifier, 2–4 member capability checks, target format, active master identity, session
epoch, per-member state, endpoint latency offset, and future common join boundary. It does not
own sockets, WASAPI devices, or UI state.

The member state path is:

```text
WaitingForSession -> ClockAcquiring -> Prerolling -> MutedReady -> Active
                                              |                    |
                                              |                    +-> Holdover -> Active
                                              +-------------------------> Leaving -> Dropped
```

`RtpPtpPhaseTimeline` associates an RTP audio timestamp with remote PTP nanoseconds and a
session epoch. A join is planned only after the member reports the active master clock, has the
required pre-roll frames, and the shared phase exists. The target is rounded forward to the
configured presentation boundary. A late member never changes the other members' timeline.

A master change puts audible members in `Holdover`. The old phase remains available for
continuous rendering, but relock is refused until `UpdateMasterPhase` publishes an anchor whose
master identity and session epoch match the new master. This prevents a new servo from using an
old master's absolute phase.

`GroupMemberAudioGate` wraps `IAudioFrameSink`. It submits preallocated zero frames while a
member is acquiring, pre-rolling, or waiting for its boundary. At the activation RTP timestamp
it forwards real PCM and restores the desired gain through the wrapped sink; the existing
bottom `AudioTransitionGuard` performs the sample ramp. `Holdover` remains audible. Leaving,
dropped, and faulted members ramp to zero and receive silence. No member path calls WASAPI.

`IEndpointCalibrationStore` separates endpoint-offset policy from persistence. The Windows
adapter stores signed offsets under the current user's registry hive and verifies the complete
endpoint ID in every hashed record. The source is retained as manual or measured so a later
automatic measurement workflow can use the same interface.

The coordinator deliberately does not implement an inter-host group-control protocol or a
skew analyzer. Those are integration and S12 responsibilities; the S11 core exposes snapshots
and counters needed by them.
