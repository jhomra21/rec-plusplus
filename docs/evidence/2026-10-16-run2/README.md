# Live test, run 2, 2026-10-16

Run on a Mac at commit `b03e40e` on `feat/native-cpp-recorder-v0`. This is the first
live run since the mixer and sync analysis moved into `src/core/`. The folder holds logs,
probe reports and console output. The `.mov` recordings were not committed.

Same machine as [run 1](../2026-10-16-live-test/README.md): MacBook Pro `Mac15,7`,
Apple M3 Pro, 18 GB, macOS 27.0.1, one 4112x2658 display, MacBook Pro Speakers at
volume 50% (not muted), default microphone. Commands ran from T3 Code (Beta).

## Commands

```sh
E=~/Documents/rec-plusplus-evidence/2026-10-16-run2
REC_E2E_ARTIFACT_DIR=$E/smoke ./scripts/smoke.sh
REC_E2E_ARTIFACT_DIR=$E/capture REC_E2E_CAPTURE=1 ./scripts/smoke.sh
REC_E2E_ARTIFACT_DIR=$E/live bash scripts/e2e-live-audio.sh
REC_E2E_ARTIFACT_DIR=$E/live-mic-N REC_E2E_MIC=1 bash scripts/e2e-live-audio.sh   # N = 1, 2, 3
# Diagnosis only, with debug/recdbg.patch applied (not committed to source):
REC_E2E_ARTIFACT_DIR=$E/debug/mic-N REC_E2E_MIC=1 bash scripts/e2e-live-audio.sh  # N = 1..5
```

## Outcome

All runs passed. Every live run found 6 of 6 flash/beep pairs. Mean A/V offset was
24 to 31 ms with audio late, and the worst single event was 39.2 ms. The script allows
−45 to +125 ms. One of 3 clean mic runs and one of 5 instrumented mic runs counted dropped
audio buffers. All of those were start-up bookkeeping, and no audio was lost (see
Diagnosis).

## Results

Video counts are captured / written / dropped. System audio is written / dropped (offset).
Mic is written / dropped. The capture run's synthetic checks matched smoke exactly, so
only its screen step is listed. `debug/` rows used the instrumented build.

| Run | Size, codec | frames | audio_tracks | audio_duration | av_sync_events | av_offset_ms | av_offset_max_ms | Video | System audio | Mic | Mix written |
|---|---|---|---|---|---|---|---|---|---|---|---|
| smoke/h264 | 1920x1080 h264 | 60 | 0 | 0 | 0 | 0 | 0 | 1/60/0 | – | – | – |
| smoke/av | 1920x1080 h264 | 60 | 1 | 2 | 2 | 0.17 | 0.17 | 1/60/0 | 120/0 (0.0) | – | – |
| smoke/offset | 1920x1080 h264 | 60 | 1 | 2.100 | 2 | 100.15 | 100.15 | 1/60/0 | 120/0 (100.0) | – | – |
| smoke/mix | 1920x1080 h264 | 60 | 3 | 2 | 2 | 0.15 | 0.15 | 1/60/0 | 120/0 (0.0) | 120/0 | 60 |
| smoke/stems | 1920x1080 h264 | 30 | 2 | 1 | 1 | 0.17 | 0.17 | 1/30/0 | 60/0 (0.0) | 60/0 | – |
| smoke/mixonly | 1920x1080 h264 | 30 | 1 | 1 | 1 | 0.15 | 0.15 | 1/30/0 | 60/0 (0.0) | 60/0 | 30 |
| smoke/hevc | 1920x1080 hevc | 30 | 0 | 0 | 0 | 0 | 0 | 1/30/0 | – | – | – |
| capture/screen | 1670x1080 h264 | 90 | 0 | 0 | 0 | 0 | 0 | 66/90/0 | – | – | – |
| live | 1670x1080 h264 | 720 | 1 | 12.024 | 6 | 24.29 | 38.85 | 136/720/0 | 601/0 (28.0) | – | – |
| live-mic-1 | 1670x1080 h264 | 720 | 3 | 12.031 | 6 | 24.69 | 36.13 | 193/720/0 | 615/**2** (28.0) | 1139/**6** | 612 |
| live-mic-2 | 1670x1080 h264 | 720 | 3 | 12.062 | 6 | 31.28 | 36.60 | 168/720/0 | 609/0 (28.0) | 1140/0 | 623 |
| live-mic-3 | 1670x1080 h264 | 720 | 3 | 12.043 | 6 | 27.67 | 36.23 | 136/720/0 | 609/0 (28.0) | 1140/0 | 630 |
| debug/mic-1 | 1670x1080 h264 | 720 | 3 | 12.058 | 6 | 26.76 | 37.44 | 136/720/0 | 615/0 (28.0) | 1142/**5** | 635 |
| debug/mic-2 | 1670x1080 h264 | 720 | 3 | 12.057 | 6 | 31.03 | 34.48 | 135/720/0 | 610/0 (28.0) | 1142/0 | 639 |
| debug/mic-3 | 1670x1080 h264 | 720 | 3 | 12.074 | 6 | 29.13 | 39.23 | 132/720/0 | 610/0 (28.0) | 1142/0 | 629 |
| debug/mic-4 | 1670x1080 h264 | 720 | 3 | 12.070 | 6 | 27.87 | 35.88 | 138/720/0 | 610/0 (28.0) | 1141/0 | 615 |
| debug/mic-5 | 1670x1080 h264 | 720 | 3 | 12.071 | 6 | 27.27 | 32.71 | 134/720/0 | 610/0 (28.0) | 1142/0 | 631 |

Positive offsets mean audio is late.

## Diagnosis: audio drops on mic runs

**Verdict: start-up burst.** The drops are not caused by video backpressure starving the
drain, and they don't happen in steady state. The counted buffers were stamped before the
recording start, so they would never have been written. No audio is lost; the drop counter
overstates.

### Method

`debug/recdbg.patch` adds temporary `RECDBG` stderr lines to
`src/platform/macos/recorder.mm`. It was applied for the `debug/` runs and then reverted;
it is not in the source. It logs:

- every `PacketRing` overflow, with time since recording start, source, depth, time since
  that ring was last drained, recording-thread state, and whether the counter counted it
- the four other audio drop paths (missing timestamp, failed stem append, failed PCM
  conversion, flush timeout)
- stem inputs blocked for more than 20 ms
- video appends that waited more than 20 ms
- gaps over 40 ms between audio drains

### Evidence

All overflow lines came from `debug/mic-1/record.log` (the large `t=214287` values are
before the start clock was set):

```
RECDBG t=214287.711 ring_overflow source=mic recording=0 depth=16 since_last_pop_ms=-1.0 pacer_state=0 blocked=0 counted=0
RECDBG t=214287.720 ring_overflow source=mic recording=0 depth=16 since_last_pop_ms=-1.0 pacer_state=0 blocked=0 counted=0
RECDBG t=0.000 recording_started
RECDBG t=0.005 ring_overflow source=mic recording=1 depth=16 since_last_pop_ms=-1.0 pacer_state=0 blocked=0 counted=1
RECDBG t=0.016 ring_overflow source=mic recording=1 depth=16 since_last_pop_ms=-1.0 pacer_state=0 blocked=0 counted=1
RECDBG t=0.026 ring_overflow source=mic recording=1 depth=16 since_last_pop_ms=-1.0 pacer_state=0 blocked=0 counted=1
RECDBG t=0.037 ring_overflow source=mic recording=1 depth=16 since_last_pop_ms=-1.0 pacer_state=0 blocked=0 counted=1
RECDBG t=0.048 ring_overflow source=mic recording=1 depth=16 since_last_pop_ms=-1.0 pacer_state=1 blocked=0 counted=1
```

- All 5 counted drops fall in the first 48 ms after recording started.
- `since_last_pop_ms=-1` means the ring had never been drained.
- `pacer_state=0` means the recording thread was sleeping until its first video slot,
  which is due 40 ms after the start (the emit delay).
- `blocked=0`: no writer input was refusing audio.
- In all 5 instrumented runs (60 s total): no overflow after 48 ms, no blocked stem, no
  video append waiting over 20 ms, and no other drop reason. The only drain gap over
  40 ms was 68 ms at t=12.09, during stop.

### Mechanism

1. `start_capture()` starts the stream with the microphone, then waits for the first
   screen frame. `create_writer()` runs next. During that time mic buffers arrive
   (about 95 a second, 10 ms each) and nothing drains the ring. Once 16 have arrived
   (about 170 ms), the ring holds the newest 16 and evicts older ones without counting.
2. `recording_` becomes true. The first `pump_audio()` call only happens inside the
   first video slot, about 40 to 50 ms later.
3. Each mic buffer that arrives in that window evicts the oldest pre-start buffer.
   Because `recording_` is now true, `on_audio` counts the eviction as a drop.
4. The evicted buffers were all delivered before the start, so their timestamps are
   before it too. The mic offset is 0. `process_audio` skips stem buffers with
   `start < 0`, and `core::Mixer::add` discards frames before sample 0. The evicted audio
   would never have been written.

The drops come and go because they need the mic to deliver 16 buffers before the start.
That depends on how long the first screen frame and writer setup take; mic start-up time
varies from run to run. The system audio drops (2 in `live-mic-1`, 2 in run 1) fit the
same pattern: at 50 buffers a second, that ring needs about 320 ms of pre-roll. They did
not recur in the instrumented runs, so the system side is inferred, not observed.

The run 1 mic test that found only 5 of 6 flash/beep pairs is not explained by this: the
dropped audio was all pre-start, and runs here with drops still found 6 of 6.

### Proposed smallest fix (not applied)

Drain the rings once at the start of `run()`, before the first `sleep_until`:

```cpp
void run() {
    ...
    pump_audio(false);  // Free the rings of pre-start audio; buffers before the origin are skipped.
    for (std::uint64_t slot = 0; ...
```

This runs on the recording thread right after `recording_` is set. Pre-start buffers are
discarded by the existing `start < 0` checks. Buffers the 28 ms system audio offset moves
after the start are still written. The rings are then nearly empty, and a mic ring takes
about 160 ms to fill against the 40 ms wait for the first slot.

The alternative is to clear both rings under `mutex_` just before `recording_.store(true)`.
That is also one line, but it throws away system audio that the output-latency offset
would place in the first 28 ms. A more precise counter would make `PacketRing::push`
return the evicted buffer, and count it only if its timestamp plus offset is at or after
the start.

Not yet verified. A check would be about 10 mic runs with the fix and `debug/recdbg.patch`,
expecting no `counted=1` overflow lines.

## Files

- `*.console.log`: full script output for each run, including the build.
  `debug-mic-N.console.log` belongs to `debug/mic-N`.
- `<run>/*.record.log` and `<run>/*.probe` (smoke, capture): recorder log and `rec probe` report.
- `<run>/record.log`, `recording.probe`, `devices.txt`, `stimulus.log` (live runs). In
  `debug/mic-N/record.log` the `RECDBG` lines are mixed in with the recorder log.
- `debug/recdbg.patch`: the temporary instrumentation, for repeating the diagnosis
  (`git apply docs/evidence/2026-10-16-run2/debug/recdbg.patch`). It is not applied to the source.
