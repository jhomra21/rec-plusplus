#!/usr/bin/env bash
# End-to-end check of the macOS app. Rec.app presses its own window's controls and menu bar
# menu (REC_APP_SELF_TEST, see src/app/macos/main.mm) while it records the synthetic
# flash/beep source (REC_APP_TEST_PATTERN=1) in a temporary home (REC_APP_HOME), so it needs
# no screen or microphone permission. In order:
#   1. every control has an accessible name;
#   2. record with system audio and microphone, pause, resume, stop: the movie keeps sync;
#   3. start and stop through the Shift-Command-R hotkey handler;
#   4. Go Live to two paths of a local MediaMTX server: both copies decode, the keys are in the
#      keychain; the second gets the vertical video (1080x1920), which is also saved; Get Key on
#      a TikTok row fills its server and key from the pasteboard, and a new server takes the key;
#   5. closing the window keeps recording, and the menu bar item's menu stops it;
#   6. with the replay buffer on, Save Replay writes the last seconds while recording goes on;
#   7. a camera and an image are drawn, and the scene moves the camera while recording; the
#      preview shows the same picture, before Start too, and Show preview turns it off;
#      dragging the camera in the preview moves and resizes it, before Start and while recording;
#      the camera's shape, border and shadow show before Start and change while recording;
#      a new scene places the screen by dragging and hides the camera, and is switched away
#      from while recording; a phone camera (a headless browser with a synthetic camera, run
#      by scripts/phone.py) opens the address in the phone code window (the relay's page),
#      shows in the preview, and stays in a recording started from there;
#   8. volume and mute change the mix while recording, from the window and the menu bar item;
#   9. quitting during a recording saves the movie; settings persist without the stream key;
#  10. with Keep Recording After Quit, the background engine records on after quitting; the
#      next launch takes the recording over, preview included, and stops it.
# Usage: app-e2e.sh REC REC_APP ARTIFACTS
set -euo pipefail

rec="$1"
app="$2"
artifacts="$3"
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/scripts/mediamtx.sh"
source "$root/scripts/relay.sh"
home="$artifacts/app-home"
report="$artifacts/self-test.txt"
keychain_service="rec stream key (test home) $home"
rm -rf "$home" "$report"
mkdir -p "$artifacts"

value() { awk -F= -v key="$1" '$1 == key { print $2 }' "$2"; }
within() {
  awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v != "" && v + 0 >= lo && v + 0 <= hi) }' \
    || { echo "$1 is outside [$2, $3]" >&2; exit 1; }
}
# color KEY FILE green|red|white|black: a sample_N=R,G,B probe field, with room for encoding error.
color() {
  awk -F= -v key="$1" -v want="$3" '$1 == key {
      split($2, c, ",")
      found = 1
      if (want == "green") ok = c[2] > 120 && c[2] > c[1] + 60 && c[2] > c[3] + 60
      if (want == "red") ok = c[1] > 170 && c[1] > c[2] + 100 && c[1] > c[3] + 100
      if (want == "white") ok = c[1] > 190 && c[2] > 190 && c[3] > 190
      if (want == "black") ok = c[1] < 40 && c[2] < 40 && c[3] < 40
      hi = c[1]; if (c[2] > hi) hi = c[2]; if (c[3] > hi) hi = c[3]
      lo = c[1]; if (c[2] < lo) lo = c[2]; if (c[3] < lo) lo = c[3]
      if (want == "gray") ok = c[1] >= 100 && c[1] <= 160 && c[2] >= 100 && c[2] <= 160 && c[3] >= 100 && c[3] <= 160 && hi - lo < 25
    }
    END { exit !(found && ok) }' "$2" || { echo "$1 in $2 is not $3" >&2; exit 1; }
}
# step NAME: the movie the self-test saved for that step.
step() { awk -v key="$1" 'index($0, key "=") == 1 { print substr($0, length(key) + 2) }' "$report"; }
probe() {
  [[ -f "$1" ]] || { echo "missing movie: $1" >&2; exit 1; }
  "$rec" probe "$1" | tee "$artifacts/$2.probe"
}

received="$artifacts/stream-server"
start_mediamtx "$(get_mediamtx "$root/build/tools")" "$received" "$artifacts/mediamtx"

# The app's recordings run in a background engine. This run has its own, on a short path:
# Unix socket paths are limited to 104 bytes.
engine_dir="$(mktemp -d /tmp/rec-e2e.XXXXXX)"
export REC_ENGINE_SOCKET="$engine_dir/engine.sock"
watchdog_pid=""
stop_engine() {
  local engine_pid
  engine_pid="$(lsof -t "$REC_ENGINE_SOCKET" 2>/dev/null | head -1)"
  "$rec" shutdown >/dev/null 2>&1 || true
  if [[ -n "$engine_pid" ]]; then
    for _ in $(seq 60); do
      kill -0 "$engine_pid" 2>/dev/null || return 0
      sleep 1
    done
    # This PID was obtained from this test's private socket, never by matching process names.
    kill -TERM "$engine_pid" 2>/dev/null || true
  fi
}
finish() {
  if [[ -n "$watchdog_pid" ]]; then
    kill "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
  fi
  stop_engine
  cp "$engine_dir/engine.log" "$artifacts/app-engine.log" 2>/dev/null || true
  if [[ ! -S "$REC_ENGINE_SOCKET" ]]; then rm -rf "$engine_dir"; fi
  stop_mediamtx
  stop_relay
  while /usr/bin/security delete-generic-password -s "$keychain_service" >/dev/null 2>&1; do :; done
}
trap finish EXIT
# If the runner is interrupted, leave through EXIT so the background engine is stopped too.
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
# The engine deliberately outlives Rec.app. Also stop it if this harness is killed before its
# EXIT trap can run; the deadline keeps a wedged test from recording indefinitely.
nohup bash -c '
  rec="$1"
  socket="$2"
  owner="$3"
  stop_engine() {
    local engine_pid
    engine_pid="$(lsof -t "$socket" 2>/dev/null | head -1)"
    "$rec" shutdown >/dev/null 2>&1 || true
    if [[ -n "$engine_pid" ]]; then
      for _ in $(seq 60); do
        kill -0 "$engine_pid" 2>/dev/null || return 0
        sleep 1
      done
      kill -TERM "$engine_pid" 2>/dev/null || true
    fi
  }
  for _ in $(seq 360); do
    kill -0 "$owner" 2>/dev/null || break
    sleep 5
  done
  stop_engine
' app-e2e-watchdog "$rec" "$REC_ENGINE_SOCKET" "$$" </dev/null >/dev/null 2>&1 &
watchdog_pid="$!"
# The phone camera step needs a Chromium-family browser.
if browser="$(python3 "$root/scripts/phone.py" --find 2>&1)"; then
  export REC_APP_SELF_TEST_PHONE="$root/scripts/phone.py"
  # The phone camera's page comes from the relay: smoke.sh runs one, or this runs its own.
  if [[ -z "${REC_PHONE_RELAY:-}" ]]; then
    if command -v npx >/dev/null; then
      start_relay "$root/support/phone-relay" "$artifacts/app-relay.log"
      export REC_PHONE_RELAY="$relay_url"
    else
      echo '6p. phone camera through the relay: not checked, it needs npx (Node.js); using rec'"'"'s own page'
      export REC_PHONE_RELAY=off
    fi
  fi
else
  echo "6p. phone camera: not checked, $browser"
fi
# Quitting during a recording stops it until step 10 turns Keep Recording After Quit on.
mkdir -p "$home"
printf 'keep_recording=no\n' >"$home/settings.conf"

# run_app REPORT LOG [PHASE]: runs Rec's self-test (or one phase of it) to the end.
run_app() {
  REC_APP_HOME="$home" REC_APP_TEST_PATTERN=1 REC_APP_SELF_TEST="$1" REC_APP_SELF_TEST_PHASE="${3:-}" \
    REC_APP_SELF_TEST_STREAM=rtmp://127.0.0.1:1935/live \
    "$app/Contents/MacOS/Rec" >"$2" 2>&1 &
  local pid="$!" code=0
  for _ in $(seq 360); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid"
    cat "$2" >&2
    echo 'Rec did not finish its self-test in 3 minutes' >&2
    exit 1
  fi
  wait "$pid" || code="$?"
  cat "$1"
  if [[ "$code" != 0 ]]; then
    cat "$2" >&2
    ps -axo pid=,command= | awk '/[R]ec --engine/ { print $1 }' | while read -r engine; do
      sample "$engine" 2 -file "$1.engine-sample.txt" >/dev/null 2>&1 || true
    done
    echo "Rec exited with $code" >&2
    exit 1
  fi
}
run_app "$report" "$artifacts/app.log"
grep -qx 'appearance=app:NSAppearanceNameDarkAqua window:NSAppearanceNameDarkAqua' "$report"
grep -qx 'accessibility=ok' "$report"
grep '^margins=' "$report"
grep -qx 'status-line=ok' "$report"
grep -qx 'screen-strip=ok' "$report"
grep -qx 'camera-strip-hidden=ok' "$report"
grep -Eqx 'tab-strip-order=ok focusable=[0-9]+' "$report"
grep -qx 'status-recording-color=ok' "$report"
grep -Eqx 'paused-status=ok text=[0-9]+\.[0-9]{2} dot=[0-9]+\.[0-9]{2},[0-9]+\.[0-9]{2},[0-9]+\.[0-9]{2} recording-dot=[0-9]+\.[0-9]{2},[0-9]+\.[0-9]{2},[0-9]+\.[0-9]{2}' "$report"
grep -Eq '^readout-drag=ok .*live-apply-during-drag=ok save-on-release-once=ok disabled-readout-inert=ok window-coordinates=ok ' "$report"
grep -qx 'attention-text=ok' "$report"
grep -qx 'camera-strip-visible=ok' "$report"
check_window_png() {
  local file="$1"
  [[ -s "$file" ]] || { echo "the app window snapshot is missing: $file" >&2; exit 1; }
  local expected actual
  expected="$(awk -F= -v key="snapshot.$(basename "$file")" '$1 == key { print $2 }' "$report")"
  actual="$(sips -g pixelWidth -g pixelHeight "$file" | awk '
    /pixelWidth:/ { w=$2 } /pixelHeight:/ { h=$2 } END { if (w && h) print w "x" h }')"
  [[ -n "$expected" && "$actual" == "$expected" ]] \
    || { echo "the app window snapshot is not a real image of its expected size ($expected, got $actual): $file" >&2; exit 1; }
  grep -Eq "^snapshot-transparency.$(basename "$file")=0/[0-9]+x[0-9]+$" "$report" \
    || { echo "the app window snapshot contains transparent pixels: $file" >&2; exit 1; }
  grep -Eq "^snapshot-luminance.$(basename "$file")=[0-9]+\.[0-9]+$" "$report" \
    || { echo "the app window snapshot has no opaque-pixel luminance measurement: $file" >&2; exit 1; }
}
check_window_png "$artifacts/app-strips.png"
check_window_png "$artifacts/app-status-row.png"
check_window_png "$artifacts/app-recording-status.png"
check_window_png "$artifacts/app-paused-status.png"
check_window_png "$artifacts/app-camera-strip.png"
echo "Snapshots verified (opaque alpha and measured luminance):"
grep '^snapshot-luminance\.' "$report"
grep '^snapshot-transparency\.' "$report"
grep '^paused-status=' "$report"
grep -qx 'keychain=ok' "$report"
grep -qx 'getkey=ok' "$report"
grep -qx 'quit=started' "$report"
grep -qx 'idle-preview=ok' "$report"
grep -qx 'screen-color=ok' "$report"
grep -qx 'slider-values=ok' "$report"
grep -qx 'slider-double-click=ok' "$report"
grep -qx 'slider-command-zero=ok' "$report"
grep -qx 'slider-release-save=ok' "$report"
grep -qx 'slider-mouse-release=not-synthesized' "$report"
grep -q '^readout-drag=ok ' "$report"
grep -qx 'menu-mute-save=ok' "$report"
[[ -s "$artifacts/app-preview-screen-gray.png" && -s "$artifacts/app-preview-screen-reset.png" ]] \
  || { echo 'the screen color slider preview artifacts are missing' >&2; exit 1; }
grep -qx 'preview=ok' "$report"
grep -qx 'camera-layout=ok' "$report"
grep -qx 'camera-look=ok' "$report"
grep -qx 'scene-editor=ok' "$report"
grep -q '^live=● LIVE 2/2' "$report"

# 6p: the phone camera's picture reaches the recording within 4 s of its start and stays.
if [[ -n "${REC_APP_SELF_TEST_PHONE:-}" ]]; then
  grep -qx 'phone-camera=ok' "$report"
  phone="$(step phone)"
  [[ -f "$phone" ]] || { echo "missing movie: $phone" >&2; exit 1; }
  "$rec" probe "$phone" --sample 1460,140,300,150 | tee "$artifacts/app-phone.probe"
  first=0
  while grep -q "^sample_$first=" "$artifacts/app-phone.probe" && ! (color "sample_$first" "$artifacts/app-phone.probe" green 2>/dev/null); do
    first=$((first + 1))
  done
  ((first <= 4)) || { echo "the phone's picture reached the recording only at $first s" >&2; exit 1; }
  last=$first
  while grep -q "^sample_$((last + 1))=" "$artifacts/app-phone.probe"; do
    last=$((last + 1))
    color "sample_$last" "$artifacts/app-phone.probe" green
  done
  echo "6p. phone camera: connected in the preview; in the recording from $first s to $last s"
fi
probe "$(step pause)" app-pause
grep -qx 'audio_tracks=3' "$artifacts/app-pause.probe"
# About 2.6 s recorded (0.6 s, then 2 s after resuming) plus start and snapshot latency; a pause
# that cut nothing would add its 1.5 s and land above the range.
within "$(value duration "$artifacts/app-pause.probe")" 2.5 4.2
within "$(value av_sync_events "$artifacts/app-pause.probe")" 3 5
# A hosted VM's first frame can land one 33 ms slot late; a real Mac stays within a few ms.
within "$(value av_offset_max_ms "$artifacts/app-pause.probe")" 0 "$([[ "${GITHUB_ACTIONS:-}" == true ]] && echo 35 || echo 20)"

probe "$(step hotkey)" app-hotkey
within "$(value duration "$artifacts/app-hotkey.probe")" 1.5 3.5

probe "$(step stream)" app-stream-local
file="$(find "$received/live/appkey" -name '*.mp4' -print -quit)"
[[ -n "$file" ]] || { echo 'MediaMTX recorded nothing' >&2; exit 1; }
probe "$file" app-stream-received
grep -qx 'codec=h264' "$artifacts/app-stream-received.probe"
grep -qx 'audio_codec=aac' "$artifacts/app-stream-received.probe"
within "$(value av_sync_events "$artifacts/app-stream-received.probe")" 3 6
# AVFoundation trims the AAC priming from MediaMTX's files; see smoke.sh.
within "$(value av_offset_max_ms "$artifacts/app-stream-received.probe")" 0 55
file="$(find "$received/live2/appkey2" -name '*.mp4' -print -quit)"
[[ -n "$file" ]] || { echo 'MediaMTX recorded nothing from the second destination' >&2; exit 1; }
probe "$file" app-stream-received-2
frames="$(value frames "$artifacts/app-stream-received.probe")"
# GitHub's macOS runners draw the 1080x1920 canvas on a paravirtual GPU and skip up to a
# fifth of its frames there (112 of 123 seen). A real GPU keeps it within two frames.
least() { if [[ "${GITHUB_ACTIONS:-}" == true ]]; then echo "$(($1 * 3 / 4 - 2))"; else echo "$(($1 - 2))"; fi; }
grep -qx 'width=1080' "$artifacts/app-stream-received-2.probe"
grep -qx 'height=1920' "$artifacts/app-stream-received-2.probe"
within "$(value frames "$artifacts/app-stream-received-2.probe")" "$(least "$frames")" "$((frames + 4))"
within "$(value av_sync_events "$artifacts/app-stream-received-2.probe")" 3 6
# Each frame the runner skips can move a flash one 33 ms slot later (1.067 s seen).
within "$(value av_offset_max_ms "$artifacts/app-stream-received-2.probe")" 0 "$([[ "${GITHUB_ACTIONS:-}" == true ]] && echo 125 || echo 55)"
probe "$(step stream-vertical)" app-vertical
grep -qx 'width=1080' "$artifacts/app-vertical.probe"
grep -qx 'height=1920' "$artifacts/app-vertical.probe"
frames="$(value frames "$artifacts/app-stream-local.probe")"
within "$(value frames "$artifacts/app-vertical.probe")" "$(least "$frames")" "$((frames + 4))"
within "$(value av_sync_events "$artifacts/app-vertical.probe")" 3 6
# A vertical frame the runner skips moves its flash to the next 33 ms slot (31.5 ms seen).
within "$(value av_offset_max_ms "$artifacts/app-vertical.probe")" 0 "$([[ "${GITHUB_ACTIONS:-}" == true ]] && echo 35 || echo 20)"

probe "$(step hidden)" app-hidden
within "$(value duration "$artifacts/app-hidden.probe")" 1.5 3.5

# The camera step: the test camera in the corner, then full frame, then hidden; the image's
# red block in every second. The pattern flashes white at each second.
camera="$(step camera)"
[[ -f "$camera" ]] || { echo "missing movie: $camera" >&2; exit 1; }
"$rec" probe "$camera" --sample 1450,800,380,200 | tee "$artifacts/app-camera-corner.probe"
"$rec" probe "$camera" --sample 800,400,300,200 | tee "$artifacts/app-camera-center.probe"
"$rec" probe "$camera" --sample 40,40,300,150 | tee "$artifacts/app-camera-image.probe"
"$rec" probe "$camera" --sample 600,100,300,100 | tee "$artifacts/app-camera-screen-color.probe"
color sample_1 "$artifacts/app-camera-corner.probe" green
color sample_1 "$artifacts/app-camera-center.probe" white
color sample_3 "$artifacts/app-camera-center.probe" green
color sample_4 "$artifacts/app-camera-center.probe" green
color sample_6 "$artifacts/app-camera-corner.probe" gray
color sample_6 "$artifacts/app-camera-screen-color.probe" gray
for second in 0 1 2 3 4 5 6; do color "sample_$second" "$artifacts/app-camera-image.probe" red; done

# The layout step: the camera dragged to the middle before Start, then to the top right at 2 s.
layout="$(step layout)"
[[ -f "$layout" ]] || { echo "missing movie: $layout" >&2; exit 1; }
"$rec" probe "$layout" --sample 770,430,290,160 | tee "$artifacts/app-layout-middle.probe"
"$rec" probe "$layout" --sample 1380,90,440,250 | tee "$artifacts/app-layout-moved.probe"
color sample_1 "$artifacts/app-layout-middle.probe" green
color sample_1 "$artifacts/app-layout-moved.probe" white
color sample_4 "$artifacts/app-layout-middle.probe" white
color sample_4 "$artifacts/app-layout-moved.probe" green

# The camera look step: a circle with a thick black border and a drop shadow, then a
# rectangle from 2 s. The self-test notes where to look.
look="$(step look)"
[[ -f "$look" ]] || { echo "missing movie: $look" >&2; exit 1; }
for part in center corner top border shadow; do
  "$rec" probe "$look" --sample "$(value "look-$part" "$report")" | tee "$artifacts/app-look-$part.probe"
done
color sample_1 "$artifacts/app-look-center.probe" green
color sample_1 "$artifacts/app-look-corner.probe" white
color sample_1 "$artifacts/app-look-border.probe" black
# The screen there is white; the shadow, fading away from the circle, darkens it.
awk -F= '$1 == "sample_1" { split($2, c, ","); found = 1; ok = c[1] <= 230 && c[2] <= 230 && c[3] <= 230 }
  END { exit !(found && ok) }' "$artifacts/app-look-shadow.probe" || { echo "no drop shadow below the circle" >&2; exit 1; }
color sample_4 "$artifacts/app-look-center.probe" green
color sample_4 "$artifacts/app-look-top.probe" green

# The scenes step: the screen in the bottom-right quarter without the camera, then the first
# scene (the whole screen, the camera top right) from 2 s.
scenes="$(step scenes)"
[[ -f "$scenes" ]] || { echo "missing movie: $scenes" >&2; exit 1; }
"$rec" probe "$scenes" --sample 100,100,500,300 | tee "$artifacts/app-scenes-corner.probe"
"$rec" probe "$scenes" --sample 1380,90,440,250 | tee "$artifacts/app-scenes-camera.probe"
color sample_1 "$artifacts/app-scenes-corner.probe" black
color sample_4 "$artifacts/app-scenes-corner.probe" white
color sample_1 "$artifacts/app-scenes-camera.probe" black
color sample_4 "$artifacts/app-scenes-camera.probe" green

# System audio at -6 dB, muted 2.5 s in, unmuted 4.5 s in: the mix has no beeps at 3 and 4 s.
probe "$(step mute)" app-mute
value beeps "$artifacts/app-mute.probe" | tr , '\n' | awk '
  $1 < 2.5 { before++ } $1 > 2.7 && $1 < 4.5 { muted++ } $1 > 4.7 { after++ }
  END { exit !(before >= 2 && muted == 0 && after >= 1) }' \
  || { echo "mute: unexpected beeps $(value beeps "$artifacts/app-mute.probe")" >&2; exit 1; }

probe "$(step replay)" app-replay
grep -qx 'codec=h264' "$artifacts/app-replay.probe"
within "$(value duration "$artifacts/app-replay.probe")" 3 6
within "$(value av_sync_events "$artifacts/app-replay.probe")" 2 6
within "$(value av_offset_max_ms "$artifacts/app-replay.probe")" 0 25
probe "$(step replay-recording)" app-replay-recording
within "$(value duration "$artifacts/app-replay-recording.probe")" 5 7.5

settings="$home/settings.conf"
cp "$settings" "$artifacts/app-settings.conf"
quit="$(value last_recording "$settings")"
[[ "$quit" != "$(step hidden)" ]] || { echo 'quitting did not save a new recording' >&2; exit 1; }
probe "$quit" app-quit
within "$(value duration "$artifacts/app-quit.probe")" 1.5 3.5
grep -qx 'destination.0.server=rtmp://127.0.0.1:1935/live' "$settings"
grep -qx 'destination.0.service=custom' "$settings"
grep -qx 'destination.1.server=rtmp://127.0.0.1:1935/live2' "$settings"
grep -qx 'destination.1.service=custom' "$settings"
if grep -q '^destination\.2\.' "$settings"; then echo 'the removed destination is still in the settings' >&2; exit 1; fi
grep -qx 'stream_kbps=3000' "$settings"
grep -qx 'fps=30' "$settings"
grep -qx 'replay=0' "$settings"
grep -qx 'camera=1' "$settings"
grep -qx 'camera_device=test' "$settings"
grep -qx 'screen_color=0,0,0' "$settings"
grep -qx 'camera_color=0,0,0' "$settings"
grep -qx 'scene=0' "$settings"
grep -qx 'scene.1.name=Corner screen' "$settings"
grep -qx 'scene.1.screen=1,0.5,0.5,0.5,0.5' "$settings"
grep -q '^scene.1.camera=0,' "$settings"
grep -qx 'image=0' "$settings"
grep -qx 'preview=1' "$settings"
grep -qx 'system_volume=-6' "$settings"
grep -qx 'system_muted=0' "$settings"
grep -qx 'keep_recording=no' "$settings"
if grep -q appkey "$settings"; then echo 'the stream key was written to the settings file' >&2; exit 1; fi

# 10. With Keep Recording After Quit, quitting leaves the recording to the background engine;
# the next launch takes it over, preview included, and Stop saves one movie spanning both.
sed -i '' 's/^keep_recording=no$/keep_recording=yes/' "$settings"
run_app "$artifacts/self-test-keep.txt" "$artifacts/app-keep.log" keep
grep -qx 'keep=started' "$artifacts/self-test-keep.txt"
"$rec" status | tee "$artifacts/app-keep-status.txt"
grep -qx 'state=recording' "$artifacts/app-keep-status.txt"
sleep 2
run_app "$artifacts/self-test-attach.txt" "$artifacts/app-attach.log" attach
grep -qx 'keep-preview=ok' "$artifacts/self-test-attach.txt"
kept="$(awk 'index($0, "keep=") == 1 { print substr($0, 6) }' "$artifacts/self-test-attach.txt")"
probe "$kept" app-keep
within "$(value duration "$artifacts/app-keep.probe")" 5.5 9
within "$(value av_sync_events "$artifacts/app-keep.probe")" 4 9
within "$(value av_offset_max_ms "$artifacts/app-keep.probe")" 0 20
echo "10. kept after quit: $(value frames "$artifacts/app-keep.probe") frames across the app quitting and reopening"
if "$rec" status | tee "$artifacts/app-final-status.txt" | grep -q '^state=recording$'; then
  "$rec" stop >/dev/null || true
  echo 'the app E2E left a recording running; stopped it through the isolated engine socket' >&2
  exit 1
fi
grep -qx 'state=idle' "$artifacts/app-final-status.txt"
echo 'app e2e passed'
