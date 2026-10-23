# Windows: 30-minute synthetic drift (2026-10-19)

Machine: the Windows development PC used for the other Windows evidence. Build: the WHIP branch
state after `e177125` (the recording path is unchanged since then).

```powershell
.\build\Release\rec.exe record --test-pattern --test-tone --test-mic -r 1080p -f 30 -t 1800 -o drift.mp4
.\build\Release\rec.exe probe drift.mp4
```

The test pattern flashes white and the test tone beeps once a second, both stamped from the host
clock (QPC). The microphone is the synthetic one, so the mix has two sources. The probe finds every
flash and every beep in the file and pairs them.

Result:

```
Captured 1, written 54000, dropped 0
System audio buffers written 108000, dropped 0 (offset 0.0 ms)
Microphone buffers written 108000, dropped 0
Mix buffers written 54000

codec=h264  width=1920  height=1080  frames=54000  duration=1800
audio_tracks=3  audio_codec=aac  audio_duration=1800
av_sync_events=1800  av_offset_ms=0.151742  av_offset_max_ms=10.8333
```

Beep minus flash, per minute of the run: 0.10 ms in minute 1, 0.10 ms at 15 minutes, 0.10 ms in
the last minute (min and max equal in each). The 10.8 ms worst is the first pair, where the encoder
starts. So the file shows no drift over 30 minutes: 1800 flashes at 0, 1, ..., 1799 s and 1800
beeps at 0.0001 s after each.

What this does not show: drift between real devices (a USB microphone's own clock against the
display and loopback). That needs a live stimulus run over 30 minutes; the 30-minute Windows soak
in [2026-10-18-windows-live](../2026-10-18-windows-live/README.md) checked frame counts and track
lengths with real devices but not flash/beep pairs.
