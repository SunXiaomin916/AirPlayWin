# Tools

The `AirPlayWin` executable doubles as the audio and phase 4 receiver probe: it enumerates
endpoints, generates deterministic PCM, runs timed soaks, exercises guarded transitions, and
composes RTSP/RTP/L16 through WASAPI.

`packet_replay/rtp_l16_replay.ps1` is a deterministic RTP/L16 440 Hz sender and network fault
injector. Latency capture and click/pop analysis remain future tools.
