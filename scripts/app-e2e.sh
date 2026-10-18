#!/usr/bin/env bash
# End-to-end check of the macOS app. Rec.app presses its own window's controls and menu bar
# menu (REC_APP_SELF_TEST, see src/app/macos/main.mm) while it records the synthetic
# flash/beep source (REC_APP_TEST_PATTERN=1) in a temporary home (REC_APP_HOME), so it needs
# no screen or microphone permission. In order:
#   1. every control has an accessible name;
#   2. record with system audio and microphone, pause, resume, stop: the movie keeps sync;
#   3. start and stop through the Shift-Command-R hotkey handler;
#   4. Go Live to a local MediaMTX server: the server's copy decodes, the key is in the keychain;
#   5. closing the window keeps recording, and the menu bar item's menu stops it;
#   6. with the replay buffer on, Save Replay writes the last seconds while recording goes on;
#   7. a camera and an image are drawn, and the scene moves the camera while recording; the
#      preview shows the same picture;
#   8. volume and mute change the mix while recording, from the window and the menu bar item;
#   9. quitting during a recording saves the movie; settings persist without the stream key.
# Usage: app-e2e.sh REC REC_APP ARTIFACTS
set -euo pipefail

rec="$1"
app="$2"
artifacts="$3"
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/scripts/mediamtx.sh"
home="$artifacts/app-home"
report="$artifacts/self-test.txt"
rm -rf "$home" "$report"
mkdir -p "$artifacts"
trap stop_mediamtx EXIT

value() { awk -F= -v key="$1" '$1 == key { print $2 }' "$2"; }
within() {
  awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v != "" && v + 0 >= lo && v + 0 <= hi) }' \
    || { echo "$1 is outside [$2, $3]" >&2; exit 1; }
}
# color KEY FILE green|red|white: a sample_N=R,G,B probe field, with room for encoding error.
color() {
  awk -F= -v key="$1" -v want="$3" '$1 == key {
      split($2, c, ",")
      found = 1
      if (want == "green") ok = c[2] > 120 && c[2] > c[1] + 60 && c[2] > c[3] + 60
      if (want == "red") ok = c[1] > 170 && c[1] > c[2] + 100 && c[1] > c[3] + 100
      if (want == "white") ok = c[1] > 190 && c[2] > 190 && c[3] > 190
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

REC_APP_HOME="$home" REC_APP_TEST_PATTERN=1 REC_APP_SELF_TEST="$report" \
  REC_APP_SELF_TEST_STREAM=rtmp://127.0.0.1:1935/live \
  "$app/Contents/MacOS/Rec" >"$artifacts/app.log" 2>&1 &
pid="$!"
for _ in $(seq 360); do
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.5
done
if kill -0 "$pid" 2>/dev/null; then
  kill -9 "$pid"
  cat "$artifacts/app.log" >&2
  echo 'Rec did not finish its self-test in 3 minutes' >&2
  exit 1
fi
code=0
wait "$pid" || code="$?"
cat "$report"
if [[ "$code" != 0 ]]; then
  cat "$artifacts/app.log" >&2
  echo "Rec exited with $code" >&2
  exit 1
fi
grep -qx 'accessibility=ok' "$report"
grep -qx 'keychain=ok' "$report"
grep -qx 'quit=started' "$report"
grep -qx 'preview=ok' "$report"
grep -q '^live=● LIVE' "$report"

probe "$(step pause)" app-pause
grep -qx 'audio_tracks=3' "$artifacts/app-pause.probe"
within "$(value duration "$artifacts/app-pause.probe")" 3.5 4.9
within "$(value av_sync_events "$artifacts/app-pause.probe")" 3 5
within "$(value av_offset_max_ms "$artifacts/app-pause.probe")" 0 20

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

probe "$(step hidden)" app-hidden
within "$(value duration "$artifacts/app-hidden.probe")" 1.5 3.5

# The camera step: the test camera in the corner, then full frame, then hidden; the image's
# red block in every second. The pattern flashes white at each second.
camera="$(step camera)"
[[ -f "$camera" ]] || { echo "missing movie: $camera" >&2; exit 1; }
"$rec" probe "$camera" --sample 1450,800,380,200 | tee "$artifacts/app-camera-corner.probe"
"$rec" probe "$camera" --sample 800,400,300,200 | tee "$artifacts/app-camera-center.probe"
"$rec" probe "$camera" --sample 40,40,300,150 | tee "$artifacts/app-camera-image.probe"
color sample_1 "$artifacts/app-camera-corner.probe" green
color sample_1 "$artifacts/app-camera-center.probe" white
color sample_3 "$artifacts/app-camera-center.probe" green
color sample_4 "$artifacts/app-camera-center.probe" green
color sample_6 "$artifacts/app-camera-corner.probe" white
for second in 0 1 2 3 4 5 6; do color "sample_$second" "$artifacts/app-camera-image.probe" red; done

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
grep -qx 'server=rtmp://127.0.0.1:1935/live' "$settings"
grep -qx 'destination=custom' "$settings"
grep -qx 'stream_kbps=3000' "$settings"
grep -qx 'fps=30' "$settings"
grep -qx 'replay=0' "$settings"
grep -qx 'camera=1' "$settings"
grep -qx 'camera_device=test' "$settings"
grep -qx 'scene=screen' "$settings"
grep -qx 'image=1' "$settings"
grep -qx 'system_volume=-6' "$settings"
grep -qx 'system_muted=0' "$settings"
if grep -q appkey "$settings"; then echo 'the stream key was written to the settings file' >&2; exit 1; fi
echo 'app e2e passed'
