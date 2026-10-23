# Live test, run 3, 2026-10-17

Mac check of `8cfe365` ("drain pre-roll audio before counting drops") on
`feat/native-cpp-recorder-v0`. Same machine and setup as
[run 2](../2026-10-16-run2/README.md): MacBook Pro `Mac15,7`, Apple M3 Pro, macOS 27.0.1,
MacBook Pro Speakers at volume 50% (not muted), default microphone.

## Commands

```sh
E=~/Documents/rec-plusplus-evidence/2026-10-16-run3
# recdbg.patch from run 2, applied temporarily; reverted afterwards
REC_E2E_ARTIFACT_DIR=$E/mic-N REC_E2E_MIC=1 bash scripts/e2e-live-audio.sh   # N = 1..10
```

The run 2 patch did not apply cleanly: the fix sits exactly where the patch adds its
`recording_started` line. All other hunks applied, and that line was added by hand
after `recording_.store(true)`. `recdbg.patch` in this folder is the version that was used,
rebased on `8cfe365`. It was reverted after the runs; the source is unchanged.

## Outcome

**The fix works.** No run showed a `counted=1` overflow line, and every run reported 0
dropped system and mic buffers. All 10 runs passed and found 6 of 6 flash/beep pairs.

Two runs still overflowed a ring before the start, and both were correctly not counted:

- `mic-1`: 17 system audio overflows, all with `recording=0`. This is the system-side
  pre-roll that run 2 could only infer; it now shows up and is not counted.
- `mic-2`: 3 mic overflows, all with `recording=0`.

No overflow happened after the start in any run. No stem input was blocked for more than
20 ms. Only one video append waited over 20 ms (`mic-2`, first slot, 21.9 ms). The only
drain gaps over 40 ms were 64.8 ms at `mic-2` start-up (that same first slot) and
63.4 ms during stop in `mic-3`.

## Results

Video counts are captured / written / dropped. System audio and mic are written / dropped.
System audio offset was 28.0 ms in every run. "counted=1" is the number of overflow lines
that incremented a drop counter; "pre-start" is overflows logged with `recording=0`.

| Run | frames | audio_tracks | audio_duration | av_sync_events | av_offset_ms | av_offset_max_ms | Video | System audio | Mic | Mix written | counted=1 | pre-start |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mic-1 | 720 | 3 | 12.029 | 6 | 30.53 | 36.98 | 144/720/0 | 617/0 | 1144/0 | 620 | 0 | 17 (system) |
| mic-2 | 720 | 3 | 12.029 | 6 | 36.18 | 39.85 | 218/720/0 | 613/0 | 1144/0 | 617 | 0 | 3 (mic) |
| mic-3 | 720 | 3 | 12.042 | 6 | 20.82 | 22.92 | 126/720/0 | 612/0 | 1145/0 | 615 | 0 | 0 |
| mic-4 | 720 | 3 | 12.021 | 6 | 28.96 | 38.06 | 186/720/0 | 608/0 | 1138/0 | 617 | 0 | 0 |
| mic-5 | 720 | 3 | 12.071 | 6 | 26.17 | 33.63 | 134/720/0 | 609/0 | 1140/0 | 621 | 0 | 0 |
| mic-6 | 720 | 3 | 12.028 | 6 | 29.81 | 43.69 | 129/720/0 | 607/0 | 1136/0 | 630 | 0 | 0 |
| mic-7 | 720 | 3 | 12.037 | 6 | 32.12 | 37.77 | 127/720/0 | 609/0 | 1138/0 | 647 | 0 | 0 |
| mic-8 | 720 | 3 | 12.058 | 6 | 32.87 | 35.21 | 127/720/0 | 609/0 | 1140/0 | 624 | 0 | 0 |
| mic-9 | 720 | 3 | 12.027 | 6 | 24.14 | 33.38 | 130/720/0 | 607/0 | 1136/0 | 632 | 0 | 0 |
| mic-10 | 720 | 3 | 12.049 | 6 | 24.90 | 37.00 | 131/720/0 | 611/0 | 1144/0 | 628 | 0 | 0 |

All recordings were 1670x1080 H.264 at 60 fps. Mean A/V offset ranged from +20.8 to
+36.2 ms (average of the 10 means: +28.7 ms; positive = audio late), and the worst single
event was 43.7 ms. The script allows −45 to +125 ms. Both extremes are a little wider
than run 2 (+21.5 to +31.3 ms, worst 39.2 ms), with no trend across the runs.

## Files

- `mic-N.console.log`: full script output, including the build.
- `mic-N/record.log`: recorder log with the `RECDBG` lines mixed in.
- `mic-N/recording.probe`, `devices.txt`, `stimulus.log`: probe report and live-run files.
- `recdbg.patch`: the debug patch as applied on `8cfe365`
  (`git apply docs/evidence/2026-10-16-run3/recdbg.patch`). Not in the source.
