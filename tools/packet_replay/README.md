# RTP/L16 packet replay

`rtp_l16_replay.ps1` produces deterministic 440 Hz, signed 16-bit big-endian RTP/L16
packets. It is intended for the phase 4 development transport, not for emulating an Apple
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

The receiver diagnostics should reflect the injected loss, duplicates, and reordering while
the decoder worker continues to emit a contiguous float32 frame sequence. `-NoPacing` is
useful for queue stress; normal runs pace packets at `FramesPerPacket / SampleRate`.
