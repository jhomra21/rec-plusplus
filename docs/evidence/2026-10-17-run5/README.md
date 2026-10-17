# Live test, run 5, 2026-10-17

Mac check of RTMP streaming (`882fe13`) on `feat/native-cpp-recorder-v0`, against a local
MediaMTX v1.21.2 started by the scripts. The Mac session ran the checks and reported the
results below. It did not report the machine details again; runs 1–4 used a MacBook Pro
`Mac15,7` (Apple M3 Pro). The evidence stays on that Mac under
`~/Documents/rec-plusplus-evidence/2026-10-17-run5/`.

## Outcome

All four steps passed.

| Step | Folder | Local file | Received stream |
|---|---|---|---|
| `bash scripts/smoke.sh` | `smoke/` | `smoke passed` | 120 and 90 frames streamed, 0 skipped; lost server: `Stream stopped early: the stream server closed the connection` after 58 frames |
| `REC_E2E_STREAM=1` | `stream-retry/` | 7 pairs, mean +21.8 ms, worst 31.0 ms | 719 frames, 7 pairs, mean −15.0 ms, worst 25.4 ms |
| `REC_E2E_STREAM=1 REC_E2E_MIC=1` | `stream-mic-quiet/` | 6 pairs, mean +31.9 ms, worst 36.5 ms | 719 frames, 6 pairs, mean −11.0 ms, worst 16.2 ms |
| `REC_E2E_STREAM=1 REC_E2E_PAUSE=1` | `stream-pause-quiet/` | 6 pairs, mean +22.6 ms, worst 40.8 ms | 719 frames, 6 pairs, mean −15.7 ms, worst 24.2 ms |

Positive offsets mean audio is late. The live script allows −45 to +125 ms.

- Every run used the hardware H.264 encoder at 6000 kb/s and streamed 720 of 720 frames
  (8.7–8.8 MB) with 0 skipped. No run dropped video, system audio or mic buffers.
- The pause run logged `Paused 2.0 s (not in the movie)`.
- MediaMTX's MP4 writer drops the final frame, so received files have 719 frames.
- macOS never asked whether MediaMTX could accept incoming connections. It listens only on
  127.0.0.1.

## Received vs. local offset

The received stream reads earlier than the local file by 36.8, 42.9 and 38.3 ms. That is
close to the 44 ms (2112 frames of AAC priming) that AVFoundation trims from MediaMTX's
file, which has no edit list. The 1–7 ms remainder is within one 60 fps frame (16.7 ms)
of flash timing. On Windows, which keeps the priming, a macOS CI stream probed at
+2.6 ms mean. Live Mac streams have not been probed on Windows or with an FFmpeg-based
player.

## Failed attempts (not recorder faults)

The first runs of the three live steps (`stream/`, `stream-mic/`, `stream-pause/`) and
the retries of the mic and pause steps (`stream-mic-retry/`, `stream-pause-retry/`)
stopped with `only 1 flash/beep pairs found` in the local file, before the stream was
checked. Brightness and audio level sampled every 100 ms showed every flash in the video,
but the audio was a steady tone (RMS 0.127) with no separate beeps. Another app was
playing it: the `@daw-browser-desktop` app from `daw-browser-convex` and its
`daw-audio-host-macos` helper, which relaunched during the runs. The passing runs started
after it quit and the system was confirmed silent.

The test cannot tell another app's sound from its own beeps, so nothing else may play
audio during a live run.
