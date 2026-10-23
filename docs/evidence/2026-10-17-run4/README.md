# Live test, run 4, 2026-10-17

Mac check of pause/resume and crash-safe recording (`861f938`, docs `22c478f`) on
`feat/native-cpp-recorder-v0`. Same machine and setup as
[run 3](../2026-10-16-run3/README.md): MacBook Pro `Mac15,7`, Apple M3 Pro, macOS 27.0.1,
MacBook Pro Speakers at volume 50% (not muted), default microphone. No debug patch.

## Outcome

All checks passed on the Mac.

- **Live mic runs (3 of 3):** 720 of 720 frames, 6 of 6 pairs, 0 dropped audio buffers.
- **Live pause:** a 2.0 s pause during the flash/beep stimulus was cut from the movie.
  The file has 720 frames, 12.04 s of audio on 3 tracks, 6 pairs, mean +24.4 ms,
  worst 36.6 ms, and 0 drops. Its sync matches the unpaused runs.
- **kill -9 after 5 s (2 runs):** both files decode. Each holds 240 frames (4.0 s) and
  about 4 s of audio on 3 tracks, which is everything up to the last 1 s fragment.
  The second run found 3 pairs, mean +25.5 ms, worst 28.1 ms.

Positive offsets mean audio is late. The live script allows −45 to +125 ms.

## Results

Video counts are captured / written / dropped. System audio and mic are written / dropped.
System audio offset was 28.0 ms in every run. All recordings were 1670x1080 H.264 at 60 fps.

| Run | frames | audio_tracks | audio_duration | av_sync_events | av_offset_ms | av_offset_max_ms | Video | System audio | Mic | Mix written |
|---|---|---|---|---|---|---|---|---|---|---|
| live-mic-1 | 720 | 3 | 12.026 | 6 | 25.77 | 33.56 | 137/720/0 | 618/0 | 1144/0 | 629 |
| live-mic-2 | 720 | 3 | 12.032 | 6 | 24.76 | 35.31 | 99/720/0 | 607/0 | 1136/0 | 652 |
| live-mic-3 | 720 | 3 | 12.045 | 6 | 19.24 | 23.90 | 106/720/0 | 608/0 | 1138/0 | 638 |
| pause | 720 | 3 | 12.041 | 6 | 24.38 | 36.60 | 112/720/0 | 607/0 | 1137/0 | 628 |
| crash (kill -9) | 240 | 3 | 3.967 | 1 | 27.27 | 27.27 | not printed | not printed | not printed | not printed |
| crash-2 (kill -9) | 240 | 3 | 3.988 | 3 | 25.48 | 28.15 | not printed | not printed | not printed | not printed |

Killed recordings print no final counts; their record logs hold only the start line.

## Pause check

```sh
{ sleep 5; echo pause; sleep 2; echo resume; sleep 30; } |
  ./build/rec record --display 1 --system-audio --mic default -f 60 -t 12 -o $E/pause.mov
# flash-beep 8 started 3 s after launch, in parallel
```

`pause.timeline` (seconds after launch): stimulus started at 3.03, pause sent at 5.02,
resume sent at 7.05, stimulus ended at 12.54. The record log shows
`Paused at 00:04.7`, `Resumed at 00:04.7` and `Paused 2.0 s (not in the movie)`.
Both stamps are recorded time, so they match.

flash-beep shows its first flash about 1.5 s after it starts, so the flashes fell at
roughly 4.7, 5.7, 6.7 … 11.7 s after launch. The pause removed the flashes at about 5.7
and 6.7 s, which leaves 6 of 8 in the movie, and the probe found all 6.

**Not covered:** no beep straddled a pause boundary; the nearest flash was about 0.3 s
before the pause. The macOS per-buffer cut (up to 10–20 ms of sound left at the cut)
was not exercised. Testing it would need a stimulus whose beep is playing at the
moment `pause` is sent.

## Crash check

```sh
./build/rec record --display 1 --system-audio --mic default -f 60 -t 0 -o $E/crash.mov </dev/null &
sleep 2; flash-beep 3 &; sleep 3; kill -9 <rec pid>          # crash
sleep 0.5; flash-beep 5 &; sleep 4.5; kill -9 <rec pid>      # crash-2
```

Both kills happened 5 s after launch and returned status 137 (SIGKILL). In `crash`,
flash-beep's late first flash left only one flash/beep pair before the last fragment.
`crash-2` started the stimulus at 0.5 s, so 3 pairs landed in the saved part. The files
were checked only with `rec probe` (AVFoundation); no other player was tried.

## Notes

- `scripts/e2e-live-audio.sh` has no pause option. The Windows `.ps1` has
  `REC_E2E_PAUSE=1`, so the Mac pause check was run by hand as above.
- Mac sync stays positive (audio late) at about +19 to +36 ms across runs 2–4. Windows
  reports negative offsets (−22 to −25 ms), so the platforms are not comparable directly.

## Files

- `live-mic-N.console.log`, `live-mic-N/`: e2e script output, record log, probe, devices, stimulus log.
- `pause.record.log`, `pause.probe`, `pause.timeline`, `pause.stimulus.log`.
- `crash.record.log`, `crash.probe`, `crash.stimulus.log`, and the same for `crash-2`.
