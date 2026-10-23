# Live test, 2026-10-16

Live run of the recorder on a real Mac, at commit `2e18660` on `feat/native-cpp-recorder-v0`.
This folder holds the logs, probe reports and console output. The `.mov` and `.mp4`
recordings (about 41 MB) were not committed because the repo ignores them.

## Machine

- MacBook Pro `Mac15,7`, Apple M3 Pro, 18 GB memory
- macOS 27.0.1 (26A434), Xcode toolchain, CMake 4.4.0, Swift 6.3
- One display, 4112x2658. Output: MacBook Pro Speakers at 48 kHz, volume 50%, not muted
- Commands ran from T3 Code (Beta), so macOS checks permissions against that app

## Commands

```sh
E=~/Documents/rec-plusplus-evidence
REC_E2E_ARTIFACT_DIR=$E/smoke ./scripts/smoke.sh
REC_E2E_ARTIFACT_DIR=$E/capture REC_E2E_CAPTURE=1 ./scripts/smoke.sh
REC_E2E_ARTIFACT_DIR=$E/live bash scripts/e2e-live-audio.sh
REC_E2E_ARTIFACT_DIR=$E/live-mic REC_E2E_MIC=1 bash scripts/e2e-live-audio.sh
/usr/bin/time -l ./build/rec record --display 1 -r 1080p -f 60 -t 30 --codec h264 -o $E/footprint-1080p.mov
/usr/bin/time -l ./build/rec record --display 1 -r 4k -f 60 -t 30 --codec hevc -o $E/footprint-4k.mov
```

## Outcome

All runs passed. The capture smoke test failed twice first because screen capture
permission was missing (see `capture-attempt1-denied/` and `capture-attempt2-denied/`).
The system log showed that the saved Screen & System Audio Recording permission for
T3 Code (Beta) was tied to an older build (code hash `39b8d7ed…`; installed build
`27d85318…`). Removing the app from that list, adding it again and restarting fixed it.

## Results

Video counts are captured / written / dropped. System audio is written / dropped (offset).
Mic is written / dropped. The capture run's synthetic checks matched smoke exactly, so
only its screen step is listed.

| Run | Size, codec | frames | audio_tracks | audio_duration | av_sync_events | av_offset_ms | av_offset_max_ms | Video | System audio | Mic | Mix written |
|---|---|---|---|---|---|---|---|---|---|---|---|
| smoke/h264 | 1920x1080 h264 | 60 | 0 | 0 | 0 | 0 | 0 | 1/60/0 | – | – | – |
| smoke/av | 1920x1080 h264 | 60 | 1 | 2 | 2 | 0.167 | 0.167 | 1/60/0 | 120/0 (0.0) | – | – |
| smoke/offset | 1920x1080 h264 | 60 | 1 | 2.100 | 2 | 100.146 | 100.146 | 1/60/0 | 120/0 (100.0) | – | – |
| smoke/mix | 1920x1080 h264 | 60 | 3 | 2 | 2 | 0.146 | 0.146 | 1/60/0 | 120/0 (0.0) | 120/0 | 60 |
| smoke/stems | 1920x1080 h264 | 30 | 2 | 1 | 1 | 0.167 | 0.167 | 1/30/0 | 60/0 (0.0) | 60/0 | – |
| smoke/mixonly | 1920x1080 h264 | 30 | 1 | 1 | 1 | 0.146 | 0.146 | 1/30/0 | 60/0 (0.0) | 60/0 | 30 |
| smoke/hevc | 1920x1080 hevc | 30 | 0 | 0 | 0 | 0 | 0 | 1/30/0 | – | – | – |
| capture/screen | 1670x1080 h264 | 90 | 0 | 0 | 0 | 0 | 0 | 81/90/0 | – | – | – |
| live | 1670x1080 h264 | 720 | 1 | 12.050 | 6 | 25.62 | 31.83 | 234/720/0 | 602/0 (28.0) | – | – |
| live-mic | 1670x1080 h264 | 720 | 3 | 12.038 | 5 | 21.49 | 30.42 | 239/720/0 | 616/2 (28.0) | 1141/3 | 715 |
| footprint-1080p | 1670x1080 h264 | 1800 | 0 | 0 | 0 | 0 | 0 | 946/1800/0 | – | – | – |
| footprint-4k | 3340x2160 hevc | 1800 | 0 | 0 | 0 | 0 | 0 | 999/1800/0 | – | – | – |

Positive offsets mean audio is late. The live script allows −45 to +125 ms.

## Footprint

Numbers come from `/usr/bin/time -l` and cover the `rec` process only. Encoding work in
system services is not counted.

| Run | Max resident memory | Peak memory footprint | User CPU | System CPU | Wall time |
|---|---|---|---|---|---|
| 1080p, H.264, 60 fps, 30 s | 35176448 B (35.2 MB) | 9552664 B | 0.60 s | 0.33 s | 30.25 s |
| 4K, HEVC, 60 fps, 30 s | 34832384 B (34.8 MB) | 10339120 B | 0.58 s | 0.32 s | 30.52 s |

## Follow-ups

- **live-mic dropped audio.** It dropped 2 system audio and 3 mic buffers and found 5
  flash/beep pairs, where the run without the mic found 6. The 16-buffer queues may have
  briefly overflowed with two sources running. The missing pair could be a dropped buffer
  landing on a beep, or the mic picking up the speaker beep in the mix track. Not yet
  checked which track the probe measures. Repeat the run to see if the drops come back.
- **Output size.** `-r 1080p` gives 1670x1080 and `-r 4k` gives 3340x2160 because the
  display's aspect ratio is kept.
- **Captured is lower than written.** ScreenCaptureKit only delivers a frame when the
  screen changes, and the recorder repeats the last frame to fill the 60 fps grid.
- **Build warnings.** `tracksWithMediaType:` is deprecated since macOS 15
  (`src/platform/macos/recorder.mm` lines 1124 and 1163).

## Files

- `*.console.log`: full script output for each run, including the build
- `<run>/*.record.log` and `<run>/*.probe`: recorder log and `rec probe` report for each recording
- `footprint-*.log`: recorder log plus `/usr/bin/time -l` output
- `devices.txt`: output of `./build/rec devices`
