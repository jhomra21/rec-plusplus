#!/usr/bin/env bash
set -Eeuo pipefail
# A failed check (often a quiet grep -q) names its line instead of ending the run silently.
trap 'echo "smoke.sh: line $LINENO failed: $BASH_COMMAND" >&2' ERR

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
# color KEY FILE green|red|white|black|yellow: a sample_N=R,G,B probe field, with room for encoding error.
color() {
  awk -F= -v key="$1" -v want="$3" '$1 == key {
      split($2, c, ",")
      found = 1
      if (want == "green") ok = c[2] > 120 && c[2] > c[1] + 60 && c[2] > c[3] + 60
      if (want == "red") ok = c[1] > 170 && c[1] > c[2] + 100 && c[1] > c[3] + 100
      if (want == "white") ok = c[1] > 190 && c[2] > 190 && c[3] > 190
      if (want == "black") ok = c[1] < 40 && c[2] < 40 && c[3] < 40
      if (want == "yellow") ok = c[1] > 190 && c[2] > 150 && c[3] < c[1] - 70
      print key "=" $2
    }
    END { exit !(found && ok) }' "$2" || { echo "$1 is not $3" >&2; exit 1; }
}

"$rec" --help | grep -q 'native screen recorder'
expect_failure 'fps must be' record --fps invalid
expect_failure 'resolution must be' record --resolution gigantic
expect_failure 'scaler must be' record --scaler bicubic
expect_failure 'requires --test-pattern' record --test-tone
expect_failure 'requires --test-pattern' record --test-mic
expect_failure 'needs a real screen' record --test-pattern --system-audio
expect_failure 'needs a real screen' record --test-pattern --mic default
expect_failure 'audio tracks must be' record --audio-tracks surround
expect_failure 'gain must be' record --mic-gain 99
expect_failure 'offset must be' record --system-audio-offset-ms 5000
expect_failure 'stream bitrate must be' record --stream-bitrate 100
expect_failure 'region must be' record --region 1,2,3
expect_failure 'region must be' record --region 0,0,1,100
expect_failure 'needs a real screen' record --test-pattern --region 0,0,640,360

if [[ "$(uname -s)" != Darwin ]]; then
  expect_failure 'requires macOS' devices
else
  # The live A/V sync stimulus must at least compile.
  swiftc -typecheck "$root/scripts/flash-beep.swift"
  swiftc -typecheck "$root/scripts/stripes.swift"

  # Hardware encode and AVFoundation decode without screen permissions. A fresh CI VM's first
  # VideoToolbox session can stall long enough to drop frames, so an unmeasured recording warms it.
  "$rec" record --test-pattern -r 1080p -f 30 -t 1 --codec h264 -o "$tmp/warmup.mov" >"$artifacts/warmup.record.log"
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

  # Overlays: a camera and an image composited into every frame on the GPU; "hide 1" removes
  # the camera mid-recording. overlay-test.png is 200x100 of (220, 30, 30).
  png="$root/scripts/overlay-test.png"
  expect_failure 'could not open the image' record --test-pattern -t 1 --image "$artifacts/missing.png" -o "$artifacts/missing.mov"
  expect_failure 'the area after @' record --test-pattern --camera test@0.9,0.9,0.2,0.2
  { sleep 2.5; echo 'hide 1'; sleep 5; } |
    "$rec" record --test-pattern -r 1080p -f 30 -t 4 --camera test --image "$png@0.05,0.05,0.2,0.2" -o "$artifacts/overlay.mov" \
    | tee "$artifacts/overlay.record.log"
  grep -q 'Hiding camera 1' "$artifacts/overlay.record.log"
  grep -q 'written 120, dropped 0' "$artifacts/overlay.record.log"
  within "$(awk '/^Camera frames/ { print $3 }' "$artifacts/overlay.record.log")" 30 1000
  # The camera's default area at 1080p is 1402,772 480x270; the image fits 96,54 384x216 as 384x192.
  "$rec" probe "$artifacts/overlay.mov" --sample 1450,800,380,200 | tee "$artifacts/overlay-camera.probe"
  color sample_0 "$artifacts/overlay-camera.probe" green
  color sample_1 "$artifacts/overlay-camera.probe" green
  color sample_3 "$artifacts/overlay-camera.probe" white
  "$rec" probe "$artifacts/overlay.mov" --sample 150,100,250,100 | tee "$artifacts/overlay-image.probe"
  for second in 0 1 2 3; do color "sample_$second" "$artifacts/overlay-image.probe" red; done
  # Camera shapes: over the red image filling the frame, the camera at 1152,324 576x324 is a
  # circle (the 324-pixel square against the right edge, from 1404) with a 12-pixel white
  # border and a shadow; mid-recording "style 2 rectangle,border=12,shadow" fills the box.
  expect_failure 'follows the --camera' record --test-pattern --camera-style circle
  expect_failure "unknown shape 'hexagon'" record --test-pattern --camera test --camera-style hexagon
  expect_failure 'the border must be' record --test-pattern --camera test --camera-style circle,border=99
  { sleep 2.5; echo 'style 2 rectangle,border=12,shadow'; sleep 5; } |
    "$rec" record --test-pattern -r 1080p -f 30 -t 4 --image "$png@0,0,1,1" --camera test@0.6,0.3,0.3,0.3 \
      --camera-style circle,border=12,shadow -o "$artifacts/shape.mov" | tee "$artifacts/shape.record.log"
  grep -q 'Styled camera 2 rectangle,border=12:ffffff,shadow' "$artifacts/shape.record.log"
  grep -q 'written 120, dropped 0' "$artifacts/shape.record.log"
  for part in center:1556,476,20,20 box:1200,476,20,20 corner:1408,340,8,8 border:1560,326,10,6 shadow:1560,652,10,6; do
    "$rec" probe "$artifacts/shape.mov" --sample "${part#*:}" | tee "$artifacts/shape-${part%%:*}.probe"
  done
  color sample_1 "$artifacts/shape-center.probe" green
  color sample_1 "$artifacts/shape-box.probe" red
  color sample_1 "$artifacts/shape-corner.probe" red
  color sample_1 "$artifacts/shape-border.probe" white
  awk -F= '$1 == "sample_1" { split($2, c, ","); found = 1; ok = c[1] >= 120 && c[1] <= 200 }
    END { exit !(found && ok) }' "$artifacts/shape-shadow.probe" || { echo 'no drop shadow below the camera' >&2; exit 1; }
  color sample_3 "$artifacts/shape-center.probe" green
  color sample_3 "$artifacts/shape-box.probe" green
  color sample_3 "$artifacts/shape-corner.probe" green
  # A second canvas: a vertical 720x1280 movie of the same recording. The middle third of the
  # pattern fills it and the camera sits near the bottom; mid-recording "place canvas2 1 0 ..."
  # hides the camera there while the main movie keeps showing it.
  expect_failure 'need --canvas2' record --test-pattern --canvas2-output "$artifacts/no-canvas.mp4"
  expect_failure 'must be even' record --test-pattern -t 1 --canvas2 721x1280 -o "$artifacts/odd-canvas.mov"
  expect_failure 'add the overlay first' record --test-pattern --canvas2 vertical --canvas2-place 1:0,0,1,1
  { sleep 2.5; echo 'place canvas2 1 0 0.6,0.8,0.35,0.15'; sleep 5; } |
    "$rec" record --test-pattern --test-tone -r 1080p -f 30 -t 5 --camera test --canvas2 720x1280 \
      --canvas2-place screen:0,0,1,1:0.34,0,0.32,1 --canvas2-place 1:0.6,0.8,0.35,0.15 \
      --canvas2-output "$artifacts/canvas2-vertical.mp4" -o "$artifacts/canvas2-main.mov" | tee "$artifacts/canvas2.record.log"
  # On a slow CI GPU the second canvas may skip a few frames; the main canvas may not.
  grep -Eq 'Second canvas 720x1280: (14[5-9]|150) frames, skipped [0-5]([^0-9]|$)' "$artifacts/canvas2.record.log"
  grep -q 'written 150, dropped 0' "$artifacts/canvas2.record.log"
  "$rec" probe "$artifacts/canvas2-vertical.mp4" | tee "$artifacts/canvas2-vertical.probe"
  grep -qx 'codec=h264' "$artifacts/canvas2-vertical.probe"
  grep -qx 'width=720' "$artifacts/canvas2-vertical.probe"
  grep -qx 'height=1280' "$artifacts/canvas2-vertical.probe"
  grep -Eqx 'frames=(14[5-9]|150)' "$artifacts/canvas2-vertical.probe"
  grep -qx 'audio_codec=aac' "$artifacts/canvas2-vertical.probe"
  within "$(value av_sync_events "$artifacts/canvas2-vertical.probe")" 5 5
  within "$(value av_offset_max_ms "$artifacts/canvas2-vertical.probe")" 0 20
  # The camera's area is 432,1024 252x192 in the vertical movie and 1402,772 480x270 in the main one.
  "$rec" probe "$artifacts/canvas2-vertical.mp4" --sample 460,1050,190,140 | tee "$artifacts/canvas2-vertical-camera.probe"
  color sample_1 "$artifacts/canvas2-vertical-camera.probe" green
  color sample_4 "$artifacts/canvas2-vertical-camera.probe" white
  "$rec" probe "$artifacts/canvas2-main.mov" --sample 1450,800,380,200 | tee "$artifacts/canvas2-main-camera.probe"
  color sample_1 "$artifacts/canvas2-main-camera.probe" green
  color sample_4 "$artifacts/canvas2-main-camera.probe" green
  # Scene layers: the pattern in the bottom middle on black and the test camera top left.
  # Mid-recording "hide 1" hides the camera and "move screen" moves the pattern to the bottom
  # left. Display and window layers need Screen Recording permission, which CI runners lack.
  expect_failure 'the screen area must be' record --test-pattern --screen-area 0.5,0.5,0.6,0.5
  expect_failure 'needs a display number' record --test-pattern --add-display main
  { sleep 2.5; echo 'hide 1'; echo 'move screen 0,0.5,0.5,0.5'; sleep 5; } |
    "$rec" record --test-pattern -r 1080p -f 30 -t 5 --screen-area 0.25,0.5,0.5,0.5 --camera test@0,0,0.5,0.5 \
      -o "$artifacts/scene.mov" | tee "$artifacts/scene.record.log"
  grep -q 'Hiding camera 1' "$artifacts/scene.record.log"
  grep -q 'Moved the screen to 0,0.5,0.5,0.5' "$artifacts/scene.record.log"
  grep -q 'written 150, dropped 0' "$artifacts/scene.record.log"
  "$rec" probe "$artifacts/scene.mov" --sample 280,170,400,200 | tee "$artifacts/scene-camera.probe"
  "$rec" probe "$artifacts/scene.mov" --sample 1000,700,400,250 | tee "$artifacts/scene-screen-before.probe"
  "$rec" probe "$artifacts/scene.mov" --sample 100,700,300,250 | tee "$artifacts/scene-screen-after.probe"
  color sample_0 "$artifacts/scene-camera.probe" green
  color sample_1 "$artifacts/scene-camera.probe" green
  color sample_4 "$artifacts/scene-camera.probe" black
  color sample_0 "$artifacts/scene-screen-before.probe" white
  color sample_4 "$artifacts/scene-screen-before.probe" black
  color sample_0 "$artifacts/scene-screen-after.probe" black
  color sample_4 "$artifacts/scene-screen-after.probe" white
  # Click marks: the left button held at a quarter of the main display's width and height,
  # 1 to 3.5 s after the recording starts. The mark lands where that point falls in the test
  # pattern's frame. Posting mouse events needs Accessibility permission; without it the
  # check is skipped.
  swiftc -O -o "$tmp/mouse" "$root/scripts/mouse.swift"
  "$tmp/mouse" 0.25 0.25 1.0 2.5 >"$artifacts/mouse.log" &
  clicker=$!
  "$rec" record --test-pattern --clicks -r 1080p -f 30 -t 6 -o "$artifacts/clicks.mov" | tee "$artifacts/clicks.record.log"
  wait "$clicker"
  # The hosted runner's few cores also run the helper posting mouse events; it can lose a
  # frame there. The other runs hold every frame.
  if [ "${GITHUB_ACTIONS:-}" = true ]; then
    grep -Eq 'written (180, dropped 0|179, dropped 1)' "$artifacts/clicks.record.log"
  else
    grep -q 'written 180, dropped 0' "$artifacts/clicks.record.log"
  fi
  if grep -qx untrusted "$artifacts/mouse.log"; then
    echo "click marks: not checked, this runner may not post mouse events"
  else
    size="$(value size "$artifacts/mouse.log")"
    sample="$(awk -v w="${size%x*}" -v h="${size#*x}" 'BEGIN {
      s = 1920 / w; if (1080 / h < s) s = 1080 / h
      printf "%d,%d,12,12", (1920 - w * s) / 2 + 0.25 * w * s - 6, (1080 - h * s) / 2 + 0.25 * h * s - 6 }')"
    "$rec" probe "$artifacts/clicks.mov" --sample "$sample" | tee "$artifacts/clicks.probe"
    # Over the white flash: half yellow, then the pattern once the mark has faded.
    color sample_2 "$artifacts/clicks.probe" yellow
    color sample_5 "$artifacts/clicks.probe" white
  fi  # Volume and mute act on the program audio. A lone tone, its only track, starts muted and is
  # unmuted 3.5 s in: no beep with the first flash. Mixed with the mic, the tone is turned down
  # 6 dB, then muted 2.5 s in: the mix has no beeps after that.
  { sleep 3.5; echo 'unmute system'; sleep 5; } |
    "$rec" record --test-pattern --test-tone --mute-system -f 30 -t 6 -o "$artifacts/unmute.mov" \
    | tee "$artifacts/unmute.record.log"
  grep -q 'Unmuted system audio' "$artifacts/unmute.record.log"
  "$rec" probe "$artifacts/unmute.mov" | tee "$artifacts/unmute.probe"
  grep -qx 'audio_tracks=1' "$artifacts/unmute.probe"
  within "$(value av_sync_events "$artifacts/unmute.probe")" 1 5
  within "$(value beeps "$artifacts/unmute.probe" | cut -d, -f1)" 0.9 6
  { sleep 1; echo 'volume system -6'; sleep 1.5; echo 'mute system'; sleep 5; } |
    "$rec" record --test-pattern --test-tone --test-mic -f 30 -t 5 -o "$artifacts/mute.mov" \
    | tee "$artifacts/mute.record.log"
  grep -q 'Set system audio to -6.0 dB' "$artifacts/mute.record.log"
  grep -q 'Muted system audio' "$artifacts/mute.record.log"
  "$rec" probe "$artifacts/mute.mov" | tee "$artifacts/mute.probe"
  grep -qx 'audio_tracks=3' "$artifacts/mute.probe"
  within "$(value av_sync_events "$artifacts/mute.probe")" 1 3
  within "$(value beeps "$artifacts/mute.probe" | awk -F, '{ print $NF }')" 0 3.5
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
  mediamtx="$(get_mediamtx "$root/build/tools")"
  new_test_certificate "$tmp/tls"
  start_mediamtx "$mediamtx" "$received" "$tmp/mediamtx" "$tmp/tls"
  # Stem-only file: the stream still carries the mix.
  "$rec" record --test-pattern --test-tone --test-mic --audio-tracks stems -r 720p -f 30 -t 4 \
    --stream rtmp://127.0.0.1:1935/live/smokekey1 -o "$artifacts/stream-local.mov" | tee "$artifacts/stream.record.log"
  grep -q 'Streaming to rtmp://127.0.0.1:1935/live/\*\*\*\*' "$artifacts/stream.record.log"
  grep -q 'Streamed 120 frames' "$artifacts/stream.record.log"
  grep -q 'kb/s), skipped 0' "$artifacts/stream.record.log"
  if grep -q smokekey1 "$artifacts/stream.record.log"; then echo 'the stream key was printed' >&2; exit 1; fi
  "$rec" probe "$artifacts/stream-local.mov" | tee "$artifacts/stream-local.probe"
  grep -qx 'audio_tracks=2' "$artifacts/stream-local.probe"
  # MediaMTX analyzes 2 s of media before it records, so streams here run longer than that.
  "$rec" record --test-pattern -r 720p -f 30 -t 3 --stream rtmp://127.0.0.1:1935/live/smokekey2 \
    -o "$artifacts/stream-silent-local.mov" | tee "$artifacts/stream-silent.record.log"
  grep -q 'Streamed 90 frames' "$artifacts/stream-silent.record.log"

  # RTMPS checks the server's certificate: this server's self-signed one fails until the
  # test trusts it explicitly.
  expect_failure 'is not trusted' record --test-pattern -t 1 --stream rtmps://127.0.0.1:1936/live/untrusted -o "$tmp/untrusted.mov"
  REC_TLS_TRUST_CERT="$tmp/tls/cert.pem" "$rec" record --test-pattern --test-tone -r 720p -f 30 -t 4 \
    --stream rtmps://127.0.0.1:1936/live/smokekey4 -o "$artifacts/stream-rtmps-local.mov" | tee "$artifacts/stream-rtmps.record.log"
  grep -q 'Streaming to rtmps://127.0.0.1:1936/live/\*\*\*\*' "$artifacts/stream-rtmps.record.log"
  grep -q 'Streamed 120 frames' "$artifacts/stream-rtmps.record.log"

  # Several servers: one encode, sent to each over its own connection. Losing one server is
  # checked in smoke.ps1; the code that keeps the others going is shared.
  expect_failure 'at most 4 --stream' record --test-pattern \
    --stream rtmp://127.0.0.1:1935/live/m1 --stream rtmp://127.0.0.1:1935/live/m2 --stream rtmp://127.0.0.1:1935/live/m3 \
    --stream rtmp://127.0.0.1:1935/live/m4 --stream rtmp://127.0.0.1:1935/live/m5
  expect_failure 'rtmp://127.0.0.1:1/live/\*\*\*\*: could not connect' record --test-pattern -t 1 \
    --stream rtmp://127.0.0.1:1935/live/refusedpair --stream rtmp://127.0.0.1:1/live/key -o "$tmp/refused-pair.mov"
  REC_TLS_TRUST_CERT="$tmp/tls/cert.pem" "$rec" record --test-pattern --test-tone -r 720p -f 30 -t 4 \
    --stream rtmp://127.0.0.1:1935/live/smokekey6 --stream rtmps://127.0.0.1:1936/live/smokekey7 \
    -o "$artifacts/stream-multi-local.mov" | tee "$artifacts/stream-multi.record.log"
  grep -q 'Streaming to rtmp://127.0.0.1:1935/live/\*\*\*\*, rtmps://127.0.0.1:1936/live/\*\*\*\*' "$artifacts/stream-multi.record.log"
  grep -q 'rtmp://127.0.0.1:1935/live/\*\*\*\*: 120 frames' "$artifacts/stream-multi.record.log"
  grep -q 'rtmps://127.0.0.1:1936/live/\*\*\*\*: 120 frames' "$artifacts/stream-multi.record.log"
  if grep -q smokekey "$artifacts/stream-multi.record.log"; then echo 'a stream key was printed' >&2; exit 1; fi

  # Both canvases live at once, each to its own server: landscape and vertical.
  expect_failure 'at most 4 stream servers across both canvases' record --test-pattern --canvas2 vertical \
    --stream rtmp://127.0.0.1:1935/live/a1 --stream rtmp://127.0.0.1:1935/live/a2 --stream rtmp://127.0.0.1:1935/live/a3 \
    --canvas2-stream rtmp://127.0.0.1:1935/live/b1 --canvas2-stream rtmp://127.0.0.1:1935/live/b2
  "$rec" record --test-pattern --test-tone -r 720p -f 30 -t 4 --stream rtmp://127.0.0.1:1935/live/smokekey10 \
    --canvas2 720x1280 --canvas2-place screen:0,0,1,1:0.34,0,0.32,1 --canvas2-stream rtmp://127.0.0.1:1935/live/smokekey11 \
    -o "$artifacts/stream-canvas2-local.mov" | tee "$artifacts/stream-canvas2.record.log"
  grep -q 'Streaming to rtmp://127.0.0.1:1935/live/\*\*\*\*; the second canvas: rtmp://127.0.0.1:1935/live/\*\*\*\*' \
    "$artifacts/stream-canvas2.record.log"
  grep -Eq 'Second canvas 720x1280: (11[5-9]|120) frames, skipped [0-5]([^0-9]|$)' "$artifacts/stream-canvas2.record.log"
  if grep -q smokekey "$artifacts/stream-canvas2.record.log"; then echo 'a stream key was printed' >&2; exit 1; fi

  # A server restart mid-stream: the stream reconnects and resumes at the next keyframe.
  "$rec" record --test-pattern --test-tone -r 720p -f 30 -t 10 --stream rtmp://127.0.0.1:1935/live/smokekey5 \
    -o "$artifacts/stream-reconnect.mov" </dev/null >"$artifacts/stream-reconnect.record.log" 2>&1 &
  again_pid="$!"
  sleep 3
  stop_mediamtx
  sleep 0.5
  start_mediamtx "$mediamtx" "$received" "$tmp/mediamtx-2" "$tmp/tls"
  if ! wait "$again_pid"; then
    cat "$artifacts/stream-reconnect.record.log" >&2; echo 'the stream did not survive a server restart' >&2; exit 1
  fi
  cat "$artifacts/stream-reconnect.record.log"
  grep -q 'reconnected 1' "$artifacts/stream-reconnect.record.log"
  grep -q 'Stream lost at 00:0[0-9.]*, reconnecting' "$artifacts/stream-reconnect.record.log"
  grep -q 'Stream reconnected at 00:0' "$artifacts/stream-reconnect.record.log"
  grep -q 'written 300, dropped 0' "$artifacts/stream-reconnect.record.log"

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
  # Same AVFoundation priming allowance as below.
  # The second canvas (smokekey11) may skip up to 5 frames on a slow CI GPU, as checked above.
  for key in smokekey10 smokekey11; do
    file="$(find "$received/live/$key" -name '*.mp4' -print -quit)"
    [[ -n "$file" ]] || { echo "MediaMTX recorded nothing for $key" >&2; exit 1; }
    "$rec" probe "$file" | tee "$artifacts/stream-received-$key.probe"
    least=119; [[ "$key" == smokekey11 ]] && least=114
    within "$(value frames "$artifacts/stream-received-$key.probe")" "$least" 120
    within "$(value av_sync_events "$artifacts/stream-received-$key.probe")" 4 4
    within "$(value av_offset_max_ms "$artifacts/stream-received-$key.probe")" 0 55
  done
  grep -qx 'width=1280' "$artifacts/stream-received-smokekey10.probe"
  grep -qx 'height=1280' "$artifacts/stream-received-smokekey11.probe"
  grep -qx 'width=720' "$artifacts/stream-received-smokekey11.probe"
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
  # Same AVFoundation priming allowance as above.
  file="$(find "$received/live/smokekey4" -name '*.mp4' -print -quit)"
  "$rec" probe "$file" | tee "$artifacts/stream-received-smokekey4.probe"
  within "$(value frames "$artifacts/stream-received-smokekey4.probe")" 119 120
  within "$(value av_sync_events "$artifacts/stream-received-smokekey4.probe")" 4 4
  within "$(value av_offset_max_ms "$artifacts/stream-received-smokekey4.probe")" 0 55
  for key in smokekey6 smokekey7; do
    file="$(find "$received/live/$key" -name '*.mp4' -print -quit)"
    [[ -n "$file" ]] || { echo "MediaMTX recorded nothing for $key" >&2; exit 1; }
    "$rec" probe "$file" | tee "$artifacts/stream-received-$key.probe"
    within "$(value frames "$artifacts/stream-received-$key.probe")" 119 120
    within "$(value av_sync_events "$artifacts/stream-received-$key.probe")" 4 4
    within "$(value av_offset_max_ms "$artifacts/stream-received-$key.probe")" 0 55
  done
  # The newest server recording is the session after the restart. MediaMTX may not have
  # written the 3 s before the kill, and that session cannot reach 100 frames anyway.
  file="$(find "$received/live/smokekey5" -name '*.mp4' | sort | tail -1)"
  [[ -n "$file" ]] || { echo "MediaMTX recorded nothing for the restarted stream" >&2; exit 1; }
  "$rec" probe "$file" | tee "$artifacts/stream-received-smokekey5.probe"
  within "$(value frames "$artifacts/stream-received-smokekey5.probe")" 100 210
  within "$(value av_sync_events "$artifacts/stream-received-smokekey5.probe")" 3 7
  # The resumed session adds the beep at the resume keyframe, which can decode faded in and
  # read up to about 30 ms late (see smoke.ps1), to the 44 ms above. A Mac measured 53.2 ms.
  within "$(value av_offset_max_ms "$artifacts/stream-received-smokekey5.probe")" 0 80
  "$rec" record --test-pattern -r 1080p -f 30 -t 1 --codec hevc -o "$artifacts/hevc.mp4" | tee "$artifacts/hevc.record.log"
  "$rec" probe "$artifacts/hevc.mp4" | tee "$artifacts/hevc.probe"
  grep -qx 'codec=hevc' "$artifacts/hevc.probe"
  grep -qx 'frames=30' "$artifacts/hevc.probe"
  grep -q 'written 30, dropped 0' "$artifacts/hevc.record.log"
  # ProRes 422 is MOV only and every frame is a keyframe.
  expect_failure 'ProRes needs a .mov' record --test-pattern -t 1 --codec prores -o "$tmp/prores.mp4"
  "$rec" record --test-pattern -r 1080p -f 30 -t 2 --codec prores -o "$artifacts/prores.mov" | tee "$artifacts/prores.record.log"
  "$rec" probe "$artifacts/prores.mov" | tee "$artifacts/prores.probe"
  grep -qx 'codec=prores' "$artifacts/prores.probe"
  grep -qx 'frames=60' "$artifacts/prores.probe"
  grep -q 'written 60, dropped 0' "$artifacts/prores.record.log"
  ls -l "$artifacts/prores.mov" "$artifacts/h264.mov" | tee "$artifacts/prores.size"

  expect_failure 'already exists' record --test-pattern -t 1 -o "$artifacts/h264.mov"
  "$rec" probe "$artifacts/h264.mov" | grep -qx 'frames=60'

  # The desktop app drives its own controls through a recording session.
  "$root/scripts/app-e2e.sh" "$rec" "$tmp/build/Rec.app" "$artifacts/app"

  # Replay: "save" writes the last 5 s (from a keyframe) to an MP4 while the recording continues.
  expect_failure 'replay must be between' record --test-pattern --replay 2
  { sleep 8; echo save; sleep 6; } |
    "$rec" record --test-pattern --test-tone -f 30 -t 10 --replay 5 -o "$artifacts/replay.mov" | tee "$artifacts/replay.record.log"
  grep -q 'Saved replay' "$artifacts/replay.record.log"
  grep -q 'written 300, dropped 0' "$artifacts/replay.record.log"
  "$rec" probe "$artifacts/replay-replay-1.mp4" | tee "$artifacts/replay-clip.probe"
  grep -qx 'codec=h264' "$artifacts/replay-clip.probe"
  within "$(value frames "$artifacts/replay-clip.probe")" 150 215
  within "$(value audio_duration "$artifacts/replay-clip.probe")" 4.8 7.2
  within "$(value av_sync_events "$artifacts/replay-clip.probe")" 4 8
  within "$(value av_offset_max_ms "$artifacts/replay-clip.probe")" 0 25

  # Engine: `rec start` launches the background engine, which keeps recording after each
  # client exits; other clients pause, change and stop it. A short recording ends by itself.
  export REC_ENGINE_SOCKET="$tmp/engine.sock"
  trap '"$rec" shutdown >/dev/null 2>&1 || true; cp "$tmp/engine.log" "$artifacts/engine.log" 2>/dev/null || true; stop_mediamtx; rm -rf "$tmp"' EXIT
  engine() {
    echo "> rec $*" >>"$artifacts/engine.client.log"
    "$rec" "$@" 2>&1 | tee -a "$artifacts/engine.client.log"
    return "${PIPESTATUS[0]}"
  }
  expect_failure 'the engine is not running' status
  out="$(engine start --test-pattern --test-tone -f 30 --camera test -o "$artifacts/engine.mov")"
  grep -q 'Recording to' <<<"$out"
  expect_failure 'already recording' start --test-pattern -o "$tmp/second.mov"
  sleep 2
  out="$(engine pause)"; grep -q 'Paused at' <<<"$out"
  sleep 1
  out="$(engine resume)"; grep -q 'Resumed at' <<<"$out"
  out="$(engine send hide 1)"; grep -q 'Hiding camera 1' <<<"$out"
  expect_failure 'unknown command' send bogus
  out="$(engine status)"; grep -qx 'state=recording' <<<"$out"
  sleep 1
  out="$(engine stop)"
  # Paused for the 1 s sleep plus the time the commands take on a busy machine.
  grep -Eq 'Paused (0\.9|1\.[0-5]) s' <<<"$out"
  grep -qx "Saved $artifacts/engine.mov" <<<"$out"
  out="$(engine status)"; grep -qx "last_saved=$artifacts/engine.mov" <<<"$out"
  out="$(engine start --test-pattern -f 30 -t 2 -o "$artifacts/engine-short.mov")"
  sleep 4
  out="$(engine status)"
  grep -qx 'state=idle' <<<"$out"
  grep -qx "last_saved=$artifacts/engine-short.mov" <<<"$out"
  out="$(engine shutdown)"; grep -q 'the engine stopped' <<<"$out"
  sleep 0.5
  expect_failure 'the engine is not running' status
  cp "$tmp/engine.log" "$artifacts/engine.log"
  "$rec" probe "$artifacts/engine.mov" | tee "$artifacts/engine.probe"
  within "$(value frames "$artifacts/engine.probe")" 80 110
  within "$(value av_sync_events "$artifacts/engine.probe")" 2 4
  within "$(value av_offset_max_ms "$artifacts/engine.probe")" 0 25
  "$rec" probe "$artifacts/engine-short.mov" | tee "$artifacts/engine-short.probe"
  grep -qx 'frames=60' "$artifacts/engine-short.probe"

  if [[ "${REC_E2E_CAPTURE:-0}" == 1 ]]; then
    "$rec" record --display 1 -r 1080p -f 30 -t 3 --codec h264 -o "$artifacts/screen.mov" | tee "$artifacts/screen.record.log"
    "$rec" probe "$artifacts/screen.mov" | tee "$artifacts/screen.probe"
    grep -qx 'codec=h264' "$artifacts/screen.probe"
    grep -qx 'frames=90' "$artifacts/screen.probe"
    "$rec" windows | tee "$artifacts/windows.txt"
    "$rec" record --display 1 --region 0,0,640,360 -f 30 -t 3 --codec h264 -o "$artifacts/region.mov" | tee "$artifacts/region.record.log"
    "$rec" probe "$artifacts/region.mov" | tee "$artifacts/region.probe"
    grep -qx 'width=640' "$artifacts/region.probe"
    grep -qx 'height=360' "$artifacts/region.probe"
    grep -qx 'frames=90' "$artifacts/region.probe"
    expect_failure 'does not fit display 1' record --display 1 --region 0,0,99999,100 -t 1 -o "$tmp/too-big.mov"
    # Scaling quality: 1-pixel stripes beside 8-pixel stripes, a 1920x1080 region recorded at
    # 720p. Lanczos must turn the fine stripes into flat gray (no moire) and keep the coarse
    # ones sharp; the fast scaler's numbers are kept for comparison.
    swiftc -O -o "$tmp/stripes" "$root/scripts/stripes.swift"
    "$tmp/stripes" 0 200 1920 1080 >"$artifacts/stripes.log" &
    stripes=$!
    for _ in $(seq 50); do grep -qE '^(shown|small)$' "$artifacts/stripes.log" && break; sleep 0.1; done
    if grep -qx small "$artifacts/stripes.log"; then
      echo "scaling quality: not checked, display 1 is too small ($(value pixels "$artifacts/stripes.log"))"
    else
      for scaler in lanczos fast; do
        "$rec" record --display 1 --region 0,200,1920,1080 -r 720p -f 30 -t 2 --no-cursor --scaler "$scaler" \
          -o "$artifacts/scaler-$scaler.mov" | tee "$artifacts/scaler-$scaler.record.log"
        "$rec" probe "$artifacts/scaler-$scaler.mov" --sample 100,100,440,520 | tee "$artifacts/scaler-$scaler-fine.probe"
        "$rec" probe "$artifacts/scaler-$scaler.mov" --sample 740,100,440,520 | tee "$artifacts/scaler-$scaler-coarse.probe"
        grep -qx 'width=1280' "$artifacts/scaler-$scaler-fine.probe"
      done
      kill "$stripes" 2>/dev/null || true
      for scaler in lanczos fast; do
        echo "$scaler-fine=$(value sample_1_spread "$artifacts/scaler-$scaler-fine.probe")" \
          "$scaler-coarse=$(value sample_1_spread "$artifacts/scaler-$scaler-coarse.probe")"
      done | tee "$artifacts/scaler.txt"
      within "$(value sample_1_spread "$artifacts/scaler-lanczos-fine.probe")" 0 4
      within "$(value sample_1_spread "$artifacts/scaler-lanczos-coarse.probe")" 90 128
    fi
    kill "$stripes" 2>/dev/null || true
  fi
fi
echo 'smoke passed'
