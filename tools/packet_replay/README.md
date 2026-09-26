# RTP/L16 packet replay

`rtp_l16_replay.ps1` produces deterministic 440 Hz, signed 16-bit big-endian RTP/L16
packets. It is intended for the phase 4/5 development transport, not for emulating an Apple
sender.

After an RTSP client completes `ANNOUNCE`, `SETUP`, and `RECORD`, copy the negotiated
`server_port` and run:

```powershell
.\tools\packet_replay\rtp_l16_replay.ps1 -Port 54321 -PacketCount 1000
```

Deterministic fault cases can be combined:

```powershell
.\tools\packet_replay\rtp_l16_replay.ps1 -Port 54321 -DropEvery 25 `
    -DuplicateEvery 40 -ReorderPairs
```

Phase 12 also supports a fixed-seed network profile with scheduled base latency, bounded jitter,
random loss, and burst loss:

```powershell
.\tools\packet_replay\rtp_l16_replay.ps1 -Port 54321 -PacketCount 10000 `
    -BaseLatencyMilliseconds 50 -JitterMilliseconds 100 `
    -RandomLossPercent 2.0 -BurstDropEvery 500 -BurstDropLength 5 -Seed 20260926
```

The reported counters distinguish periodic, random, and burst drops. Jitter affects scheduled
delivery only when pacing is enabled; `-NoPacing` intentionally bypasses all delay scheduling.

The receiver diagnostics should reflect the injected loss, duplicates, and reordering while
the decoder worker continues to emit a contiguous float32 frame sequence. `-NoPacing` is
useful for queue stress; normal runs pace packets at `FramesPerPacket / SampleRate`.

Phase 5 timeline and retransmission-wrapper checks can start at the exact RECORD/FLUSH anchor
and route every Nth wrapped packet to the negotiated control port:

```powershell
.\tools\packet_replay\rtp_l16_replay.ps1 -Port 54321 -ControlPort 54322 `
    -StartSequence 1000 -StartTimestamp 441000 -RetransmitEvery 20
```

`RTP-Info` must use the same `seq` and `rtptime` values. The wrapper option validates inbound
retransmitted audio only; the receiver does not yet originate resend requests.
