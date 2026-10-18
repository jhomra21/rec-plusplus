# Windows live tests: 6 Mb/s to Twitch, a real network drop, a 30-minute soak, and window and region capture, 2026-10-18

Same Windows 11 (26200) PC as earlier runs (RTX 5070 Ti, Realtek 2.5 GbE wired network, one 3840x2160 display at 125% scaling). The streaming runs used Twitch's bandwidth-test mode (`?bandwidthtest=true` after the key), so Twitch received the stream but nothing went live. The key was read from Credential Manager at run time and replaced with `****` in these logs. The movies were not kept; each folder holds the recorder's log and the probe of the local movie.

## Twitch at 6 Mb/s (`twitch-6m/`)

Display 1 at 1080p30 with system audio, 60 s, hardware H.264 at 6000 kb/s over RTMPS, while a full-screen window drew new random noise continuously so the encoder could not save bits. rec sent 1800/1800 frames with 0 skipped: 48.1 MB, 6.41 Mb/s average including audio and RTMP framing. The local movie has 1800 frames and 0 dropped. The earlier still-desktop run averaged 1.5 Mb/s; this one shows the stream holds the target rate when the content needs it.

## Real network drop (`network-drop/`)

A synthetic 720p30 stream (software H.264, flash/beep pattern) to Twitch over RTMPS for 60 s. About 10 s in, an elevated helper disabled the only network adapter for 5.7 s (`Disable-NetAdapter`, then `Enable-NetAdapter`). `adapter*.log` has the times.

| Run | Retry backoff | Stream lost | Stream back | Skipped frames | Local movie |
| --- | --- | --- | --- | --- | --- |
| `*-8s-cap` | 0.5 s, 1 s, 2 s, 4 s, then every 8 s | 00:14.0 | 00:30.1 | 539 of 1800 | 1800 frames, 60 pairs, worst 10.8 ms |
| main | 0.5 s, 1 s, then every 2 s | 00:13.6 | 00:29.7 | 490 of 1800 | 1800 frames, 60 pairs, worst 10.8 ms |

The stream was down about 16 s for a 5.7 s outage in both runs. The adapter, not the retry schedule, set most of that: after `Enable-NetAdapter` the first TCP connection to `live.twitch.tv:443` succeeded 6.3 s later (link negotiation and addressing), and rec was live again about 4 s after that (the connection attempt in flight when the network returned has to time out first, then TLS and the RTMP publish). The cap on the retry interval went from 8 s to 2 s, so a longer outage costs at most about 2 s more than the network itself. The recording was never interrupted.

## 30-minute soak (`soak/`)

Display 1 at 1080p60, system audio and the default microphone, streaming to Twitch at 6000 kb/s over RTMPS with hardware H.264, for 1800 s, while the PC was in normal use. `samples.csv` has the process's memory, CPU time, handles and threads every 10 s.

- Recording: 108,000/108,000 frames written, 0 dropped; system audio 179,970 buffers and microphone 179,972 buffers, 0 dropped; the movie probes as 108,000 frames, 1800 s of video and 1800 s of audio on 3 tracks.
- Stream: 108,000 frames sent, 0 skipped, 594.6 MB (2.6 Mb/s average for a mostly still desktop), no reconnects.
- Memory: working set 135.7 MB at 10 s and 139.4 MB at the end. Private memory rose from 212.7 MB to 225.6 MB over the first 15 minutes, then stayed within 0.8 MB (226.4 MB at the end). Handles stayed between 917 and 938, threads between 50 and 60.
- CPU: 197 s of CPU time in 1792 s, about 11% of one core for capture, two encodes, two audio sources, the mixer and the TLS stream.

The soak had no flash/beep stimulus, so it does not measure A/V drift over 30 minutes; the video and audio durations match to the millisecond.

## Window and region capture

At the commit that adds them. `scripts/e2e-live-audio.ps1` with `REC_E2E_SOURCE`:

- `window-flash/` (`REC_E2E_SOURCE=window`): records only the stimulus window, found by its title. 720/720 frames at 1920x1080, 11 flash/beep pairs, mean −10.4 ms, worst 29.0 ms. Windows delivers a window's frames only when it changes, so 23 captures filled the 60 fps grid.
- `region-flash/` (`REC_E2E_SOURCE=region`, `100,100,1280,720`): 720/720 frames at 1280x720, 7 pairs, mean −14.7 ms, worst 18.0 ms.
- `window-resize/`: a 30 fps recording of a test window that changes color every 100 ms. At 2 s it was resized from about 1000x750 to 1750x625 pixels, at 4 s minimized, at 6 s restored. The recording kept going: 240/240 frames, 0 dropped, at the 954x720 size chosen at the start. The frames were not inspected visually.

In the app (`rec-app.exe`, driven through its window messages): the Source list showed the display, Select a region… and the four open windows. Choosing Select a region… dimmed the screen. A mouse drag from (400,300) to (1680,1020) showed "1280 × 720" next to the selection and saved `source=region`, `region=400,300,1280,720`, and a recording from Start Recording was 1280x720. Choosing the LocalSend window saved `source=window`, `window_app=localsend_app`, `window_title=LocalSend`, and the recording was 1108x742, the window's size.

The Windows smoke run (`REC_E2E_CAPTURE=1`) also passed: a 640x360 region of the display, 90/90 frames, and the expected errors for malformed regions, a region that does not fit, a region with the test pattern, and an unknown window.

## Not covered

- macOS window and region capture (implemented, compiled on CI, not yet run on a Mac).
- Window capture with the window on another display, occluded, or closed during the recording.
- A/V drift over 30 minutes, and a longer network outage.
