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
#   6. quitting during a recording saves the movie; settings persist without the stream key.
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
for _ in $(seq 240); do
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.5
done
if kill -0 "$pid" 2>/dev/null; then
  kill -9 "$pid"
  cat "$artifacts/app.log" >&2
  echo 'Rec did not finish its self-test in 2 minutes' >&2
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
if grep -q appkey "$settings"; then echo 'the stream key was written to the settings file' >&2; exit 1; fi
echo 'app e2e passed'
