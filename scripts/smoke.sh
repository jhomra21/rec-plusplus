#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/scripts/mediamtx.sh"
tmp="$(mktemp -d)"
trap 'stop_mediamtx; rm -rf "$tmp"' EXIT
artifacts="${REC_E2E_ARTIFACT_DIR:-$tmp}"
mkdir -p "$artifacts"

cmake -S "$root" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$tmp/build" --parallel
rec="$tmp/build/rec"

# expect_failure PATTERN ARGS...: the command must fail and print PATTERN.
expect_failure() {
  local pattern="$1"; shift
  if "$rec" "$@" >"$tmp/failure.stdout" 2>"$tmp/failure.stderr"; then
    echo "accepted: rec $*" >&2; exit 1
  fi
  grep -q -- "$pattern" "$tmp/failure.stderr" || { cat "$tmp/failure.stderr" >&2; exit 1; }
}
# value KEY FILE: a key=value field from a probe report.
value() { awk -F= -v key="$1" '$1 == key { print $2 }' "$2"; }
# within VALUE LOW HIGH
within() {
  awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v != "" && v + 0 >= lo && v + 0 <= hi) }' \
    || { echo "$1 is outside [$2, $3]" >&2; exit 1; }
}

"$rec" --help | grep -q 'native screen recorder'
expect_failure 'fps must be' record --fps invalid
expect_failure 'resolution must be' record --resolution gigantic
expect_failure 'requires --test-pattern' record --test-tone
expect_failure 'requires --test-pattern' record --test-mic
expect_failure 'needs a real screen' record --test-pattern --system-audio
expect_failure 'needs a real screen' record --test-pattern --mic default
expect_failure 'audio tracks must be' record --audio-tracks surround
expect_failure 'gain must be' record --mic-gain 99
expect_failure 'offset must be' record --system-audio-offset-ms 5000
expect_failure 'stream bitrate must be' record --stream-bitrate 100

if [[ "$(uname -s)" != Darwin ]]; then
  expect_failure 'requires macOS' devices
else
  # The live A/V sync stimulus must at least compile.
  swiftc -typecheck "$root/scripts/flash-beep.swift"

  # Hardware encode and AVFoundation decode without screen permissions.
  "$rec" record --test-pattern -r 1080p -f 30 -t 2 --codec h264 -o "$artifacts/h264.mov" | tee "$artifacts/h264.record.log"
  "$rec" probe "$artifacts/h264.mov" | tee "$artifacts/h264.probe"
  grep -qx 'codec=h264' "$artifacts/h264.probe"
  grep -qx 'width=1920' "$artifacts/h264.probe"
  grep -qx 'height=1080' "$artifacts/h264.probe"
  grep -qx 'frames=60' "$artifacts/h264.probe"
  grep -qx 'audio_tracks=0' "$artifacts/h264.probe"
  grep -q 'written 60, dropped 0' "$artifacts/h264.record.log"

  # Flash + beep through the AAC writer: measured A/V offset in the decoded file.
  "$rec" record --test-pattern --test-tone -r 1080p -f 30 -t 2 --codec h264 -o "$artifacts/av.mov" | tee "$artifacts/av.record.log"
  "$rec" probe "$artifacts/av.mov" | tee "$artifacts/av.probe"
  grep -qx 'codec=h264' "$artifacts/av.probe"
  grep -qx 'frames=60' "$artifacts/av.probe"
  grep -qx 'audio_tracks=1' "$artifacts/av.probe"
  grep -qx 'audio_codec=aac' "$artifacts/av.probe"
  grep -q 'written 60, dropped 0' "$artifacts/av.record.log"
  within "$(value audio_duration "$artifacts/av.probe")" 1.95 2.05
  within "$(value av_sync_events "$artifacts/av.probe")" 2 2
  within "$(value av_offset_max_ms "$artifacts/av.probe")" 0 20

  # A system-audio offset moves audio later by exactly that much.
  "$rec" record --test-pattern --test-tone --system-audio-offset-ms 100 -f 30 -t 2 -o "$artifacts/offset.mov" | tee "$artifacts/offset.record.log"
  "$rec" probe "$artifacts/offset.mov" | tee "$artifacts/offset.probe"
  within "$(value av_sync_events "$artifacts/offset.probe")" 2 2
  within "$(value av_offset_ms "$artifacts/offset.probe")" 80 120

  # Mixer: default track is the system + mic mix, followed by both stems.
  "$rec" record --test-pattern --test-tone --test-mic -f 30 -t 2 -o "$artifacts/mix.mov" | tee "$artifacts/mix.record.log"
  "$rec" probe "$artifacts/mix.mov" | tee "$artifacts/mix.probe"
  grep -qx 'audio_tracks=3' "$artifacts/mix.probe"
  grep -qx 'audio_codec=aac' "$artifacts/mix.probe"
  grep -q 'Mix buffers written' "$artifacts/mix.record.log"
  grep -q 'written 60, dropped 0' "$artifacts/mix.record.log"
  within "$(value audio_duration "$artifacts/mix.probe")" 1.95 2.05
  within "$(value av_sync_events "$artifacts/mix.probe")" 2 2
  within "$(value av_offset_max_ms "$artifacts/mix.probe")" 0 20

  "$rec" record --test-pattern --test-tone --test-mic --audio-tracks stems -f 30 -t 1 -o "$artifacts/stems.mp4" | tee "$artifacts/stems.record.log"
  "$rec" probe "$artifacts/stems.mp4" | tee "$artifacts/stems.probe"
  grep -qx 'audio_tracks=2' "$artifacts/stems.probe"
  "$rec" record --test-pattern --test-tone --test-mic --audio-tracks mix -f 30 -t 1 -o "$artifacts/mixonly.mov" | tee "$artifacts/mixonly.record.log"
  "$rec" probe "$artifacts/mixonly.mov" | tee "$artifacts/mixonly.probe"
  grep -qx 'audio_tracks=1' "$artifacts/mixonly.probe"

  # Pause/resume through piped stdin: paused time is cut from the movie, A/V stays in sync.
  { sleep 1; echo pause; sleep 1.5; echo resume; sleep 6; } |
    "$rec" record --test-pattern --test-tone --test-mic -f 30 -t 3 -o "$artifacts/pause.mov" | tee "$artifacts/pause.record.log"
  "$rec" probe "$artifacts/pause.mov" | tee "$artifacts/pause.probe"
  grep -q 'Paused at' "$artifacts/pause.record.log"
  grep -q 'Resumed at' "$artifacts/pause.record.log"
  grep -q 'written 90, dropped 0' "$artifacts/pause.record.log"
  within "$(awk '/^Paused [0-9.]+ s/ { print $2 }' "$artifacts/pause.record.log")" 1.3 1.7
  grep -qx 'frames=90' "$artifacts/pause.probe"
  grep -qx 'audio_tracks=3' "$artifacts/pause.probe"
  within "$(value audio_duration "$artifacts/pause.probe")" 2.95 3.05
  within "$(value av_sync_events "$artifacts/pause.probe")" 3 3
  within "$(value av_offset_max_ms "$artifacts/pause.probe")" 0 20

  # Crash safety: a recording killed mid-write still decodes up to its last movie fragment.
  "$rec" record --test-pattern --test-tone -f 30 -t 0 --codec h264 -o "$artifacts/crash.mov" \
    </dev/null >"$artifacts/crash.record.log" 2>&1 &
  crash_pid="$!"
  sleep 4
  kill -9 "$crash_pid"
  wait "$crash_pid" || true
  "$rec" probe "$artifacts/crash.mov" | tee "$artifacts/crash.probe"
  within "$(value frames "$artifacts/crash.probe")" 60 120
  within "$(value av_sync_events "$artifacts/crash.probe")" 2 4
  within "$(value av_offset_max_ms "$artifacts/crash.probe")" 0 20
  # Live streaming: a local MediaMTX server receives the RTMP stream and records it. The
  # received file must decode with the frames and A/V sync of the local recording.
  expect_failure 'must start with rtmp://' record --test-pattern -t 1 --stream http://127.0.0.1/live/key -o "$tmp/bad-url.mov"
  expect_failure 'must look like rtmp://' record --test-pattern -t 1 --stream rtmp://127.0.0.1/key -o "$tmp/bad-path.mov"
  expect_failure 'could not connect to 127.0.0.1:1' record --test-pattern -t 1 --stream rtmp://127.0.0.1:1/live/key -o "$tmp/refused.mov"
  received="$artifacts/stream-server"
  start_mediamtx "$(get_mediamtx "$root/build/tools")" "$received" "$tmp/mediamtx"
  # Stem-only file: the stream still carries the mix.
  "$rec" record --test-pattern --test-tone --test-mic --audio-tracks stems -r 720p -f 30 -t 4 \
    --stream rtmp://127.0.0.1:1935/live/smokekey1 -o "$artifacts/stream-local.mov" | tee "$artifacts/stream.record.log"
  grep -q 'Streaming to rtmp://127.0.0.1:1935/live/\*\*\*\*' "$artifacts/stream.record.log"
  grep -q 'Streamed 120 frames' "$artifacts/stream.record.log"
  grep -q 'skipped 0' "$artifacts/stream.record.log"
  if grep -q smokekey1 "$artifacts/stream.record.log"; then echo 'the stream key was printed' >&2; exit 1; fi
  "$rec" probe "$artifacts/stream-local.mov" | tee "$artifacts/stream-local.probe"
  grep -qx 'audio_tracks=2' "$artifacts/stream-local.probe"
  # MediaMTX analyzes 2 s of media before it records, so streams here run longer than that.
  "$rec" record --test-pattern -r 720p -f 30 -t 3 --stream rtmp://127.0.0.1:1935/live/smokekey2 \
    -o "$artifacts/stream-silent-local.mov" | tee "$artifacts/stream-silent.record.log"
  grep -q 'Streamed 90 frames' "$artifacts/stream-silent.record.log"

  # Losing the server ends the stream, not the recording.
  "$rec" record --test-pattern --test-tone -r 720p -f 30 -t 4 --stream rtmp://127.0.0.1:1935/live/smokekey3 \
    -o "$artifacts/stream-lost.mov" </dev/null >"$artifacts/stream-lost.record.log" 2>&1 &
  lost_pid="$!"
  sleep 2
  stop_mediamtx
  if wait "$lost_pid"; then cat "$artifacts/stream-lost.record.log" >&2; echo 'a lost stream was not reported' >&2; exit 1; fi
  cat "$artifacts/stream-lost.record.log"
  grep -q 'Stream stopped early' "$artifacts/stream-lost.record.log"
  grep -q 'written 120, dropped 0' "$artifacts/stream-lost.record.log"
  if grep -q 'Streamed 120 frames' "$artifacts/stream-lost.record.log"; then
    echo 'frames after the loss were counted as streamed' >&2; exit 1
  fi
  "$rec" probe "$artifacts/stream-lost.mov" | grep -qx 'frames=120'

  for key in smokekey1 smokekey2; do
    file="$(find "$received/live/$key" -name '*.mp4' -print -quit)"
    [[ -n "$file" ]] || { echo "MediaMTX recorded nothing for $key" >&2; exit 1; }
    "$rec" probe "$file" | tee "$artifacts/stream-received-$key.probe"
  done
  got="$artifacts/stream-received-smokekey1.probe"
  grep -qx 'codec=h264' "$got"
  grep -qx 'width=1280' "$got"
  grep -qx 'height=720' "$got"
  # MediaMTX's MP4 writer drops the final frame, whose duration it cannot know.
  within "$(value frames "$got")" 119 120
  grep -qx 'audio_tracks=1' "$got"
  grep -qx 'audio_codec=aac' "$got"
  # MediaMTX writes no edit list, and AVFoundation then reads the audio 44 ms (2112 frames,
  # AAC's usual encoder delay) shorter and earlier than decoders that keep the priming, such
  # as Media Foundation and FFmpeg. The stream is timed for those: a CI file from this check
  # probed on Windows at +2.6 ms mean, 10 ms worst. So here the bounds allow for the 44 ms.
  within "$(value audio_duration "$got")" 3.8 4.05
  within "$(value av_sync_events "$got")" 4 4
  within "$(value av_offset_ms "$got")" -55 10
  within "$(value av_offset_max_ms "$got")" 0 55
  within "$(value frames "$artifacts/stream-received-smokekey2.probe")" 89 90
  grep -qx 'audio_codec=aac' "$artifacts/stream-received-smokekey2.probe"
  "$rec" record --test-pattern -r 1080p -f 30 -t 1 --codec hevc -o "$artifacts/hevc.mp4" | tee "$artifacts/hevc.record.log"
  "$rec" probe "$artifacts/hevc.mp4" | tee "$artifacts/hevc.probe"
  grep -qx 'codec=hevc' "$artifacts/hevc.probe"
  grep -qx 'frames=30' "$artifacts/hevc.probe"
  grep -q 'written 30, dropped 0' "$artifacts/hevc.record.log"

  expect_failure 'already exists' record --test-pattern -t 1 -o "$artifacts/h264.mov"
  "$rec" probe "$artifacts/h264.mov" | grep -qx 'frames=60'

  # The desktop app drives its own controls through a recording session.
  "$root/scripts/app-e2e.sh" "$rec" "$tmp/build/Rec.app" "$artifacts/app"

  if [[ "${REC_E2E_CAPTURE:-0}" == 1 ]]; then
    "$rec" record --display 1 -r 1080p -f 30 -t 3 --codec h264 -o "$artifacts/screen.mov" | tee "$artifacts/screen.record.log"
    "$rec" probe "$artifacts/screen.mov" | tee "$artifacts/screen.probe"
    grep -qx 'codec=h264' "$artifacts/screen.probe"
    grep -qx 'frames=90' "$artifacts/screen.probe"
  fi
fi
echo 'smoke passed'
