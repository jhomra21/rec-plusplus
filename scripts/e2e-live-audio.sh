#!/usr/bin/env bash
# Live A/V sync acceptance on a Mac with Screen Recording permission and audible output.
# A full-screen window flashes white with a simultaneous beep once a second while rec
# records the display and system audio; rec probe then measures the offset in the file.
# REC_E2E_MIC=1 also records the default microphone (mix + stems).
# REC_E2E_PAUSE=1 pauses for 2 s in the middle of the stimulus: the movie must keep its
# full length and stay in sync across the cut.
# REC_E2E_STREAM=1 also streams to a local MediaMTX server; the stream it records must be
# in sync too.
# REC_E2E_PREPLAY=1 starts the stimulus 2 s before the recording, so its audio is already
# playing when capture starts.
# REC_E2E_SOURCE=window records only the stimulus window (found by its title), and
# REC_E2E_SOURCE=region records a 1280x720 region of the display.
set -euo pipefail

[[ "$(uname -s)" == Darwin ]] || { echo 'live A/V sync E2E requires macOS' >&2; exit 1; }
command -v swiftc >/dev/null || { echo 'swiftc (Xcode command line tools) is required' >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/scripts/mediamtx.sh"
artifacts="${REC_E2E_ARTIFACT_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/rec-live-sync.XXXXXX")}"
mkdir -p "$artifacts"
capture_pid=""
cleanup() {
  if [[ -n "$capture_pid" ]] && kill -0 "$capture_pid" 2>/dev/null; then
    kill -TERM "$capture_pid" || true
    wait "$capture_pid" || true
  fi
  stop_mediamtx
}
trap cleanup EXIT

cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" --parallel
rec="$root/build/rec"
swiftc -O "$root/scripts/flash-beep.swift" -o "$artifacts/flash-beep"

source="${REC_E2E_SOURCE:-}"
case "$source" in
  window) target=(--window "rec flash-beep"); title="rec flash-beep" ;;
  region) target=(--display 1 --region 100,100,1280,720); title="" ;;
  "") target=(--display 1); title="" ;;
  *) echo "REC_E2E_SOURCE must be window or region" >&2; exit 1 ;;
esac
args=(record "${target[@]}" --system-audio -r 1080p -f 60 -t 12 --codec h264 -o "$artifacts/recording.mov")
if [[ "${REC_E2E_MIC:-0}" == 1 ]]; then args+=(--mic default); fi
stream="${REC_E2E_STREAM:-0}"
if [[ "$stream" == 1 ]]; then
  start_mediamtx "$(get_mediamtx "$root/build/tools")" "$artifacts/stream-server" "$artifacts/mediamtx"
  args+=(--stream rtmp://127.0.0.1:1935/live/e2e)
fi

echo "Saving recordings and evidence to: $artifacts"
"$rec" devices | tee "$artifacts/devices.txt"
pause="${REC_E2E_PAUSE:-0}"
stimulus_seconds=6
if [[ "$pause" == 1 ]]; then stimulus_seconds=8; fi
stimulus_pid=""
# Window capture needs the window open before the recording starts.
if [[ "${REC_E2E_PREPLAY:-0}" == 1 || "$source" == window ]]; then
  # Covers the 2 s lead and the recorder's 3 s start-up as well.
  "$artifacts/flash-beep" "$((stimulus_seconds + 5))" "$title" >"$artifacts/stimulus.log" 2>&1 &
  stimulus_pid="$!"
  sleep 2
fi
# rec reads pause/resume commands from stdin. The FIFO stays open on fd 3 until the end so
# rec never sees end of input early.
control="$artifacts/control"
rm -f "$control"
mkfifo "$control"
"$rec" "${args[@]}" <"$control" >"$artifacts/record.log" 2>&1 &
capture_pid="$!"
exec 3>"$control"

# Allow the recorder to establish its screen/audio stream and first video frame.
sleep 3
if ! kill -0 "$capture_pid" 2>/dev/null; then
  wait "$capture_pid" || true
  echo "Recording stopped before the stimulus; see $artifacts/record.log" >&2
  exit 1
fi

if [[ -z "$stimulus_pid" ]]; then
  "$artifacts/flash-beep" "$stimulus_seconds" "$title" >"$artifacts/stimulus.log" 2>&1 &
  stimulus_pid="$!"
fi
if [[ "$pause" == 1 ]]; then
  sleep 3.5
  echo pause >&3
  sleep 2
  echo resume >&3
fi
wait "$stimulus_pid"
wait "$capture_pid" || { echo "Recorder failed; see $artifacts/record.log" >&2; exit 1; }
capture_pid=""
exec 3>&-
rm -f "$control"
if [[ "$stream" == 1 ]]; then sleep 1; stop_mediamtx; fi
cat "$artifacts/record.log"

"$rec" probe "$artifacts/recording.mov" | tee "$artifacts/recording.probe"
value() { awk -F= -v key="$1" '$1 == key { print $2 }' "$artifacts/recording.probe"; }
grep -qx 'codec=h264' "$artifacts/recording.probe"
grep -qx 'audio_codec=aac' "$artifacts/recording.probe"
grep -qx 'frames=720' "$artifacts/recording.probe"
awk -v d="$(value audio_duration)" 'BEGIN { exit !(d >= 11.9 && d <= 12.1) }' || { echo "audio lasts $(value audio_duration) s, not 12 s" >&2; exit 1; }
if [[ "${REC_E2E_MIC:-0}" == 1 ]]; then grep -qx 'audio_tracks=3' "$artifacts/recording.probe"; fi
if [[ "$source" == region ]]; then
  grep -qx 'width=1280' "$artifacts/recording.probe"
  grep -qx 'height=720' "$artifacts/recording.probe"
fi
if [[ "$pause" == 1 ]]; then grep -q 'not in the movie' "$artifacts/record.log"; fi
events="$(value av_sync_events)"
offset="$(value av_offset_ms)"
worst="$(value av_offset_max_ms)"
awk -v n="$events" 'BEGIN { exit !(n >= 4) }' || { echo "only $events flash/beep pairs found" >&2; exit 1; }
# ITU-R BT.1359 detectability: audio more than 45 ms early or 125 ms late.
awk -v m="$offset" 'BEGIN { exit !(m >= -45 && m <= 125) }' || { echo "A/V offset $offset ms is noticeable" >&2; exit 1; }

echo "Live A/V sync: $events flash/beep pairs, mean offset $offset ms (positive = audio late), worst $worst ms."

if [[ "$stream" == 1 ]]; then
  grep -q 'skipped 0' "$artifacts/record.log" || { echo 'the stream skipped frames' >&2; exit 1; }
  file="$(find "$artifacts/stream-server" -name '*.mp4' -print -quit)"
  [[ -n "$file" ]] || { echo 'MediaMTX recorded nothing' >&2; exit 1; }
  got="$artifacts/stream-received.probe"
  "$rec" probe "$file" | tee "$got"
  field() { awk -F= -v key="$1" '$1 == key { print $2 }' "$got"; }
  grep -qx 'codec=h264' "$got"
  awk -v n="$(field frames)" 'BEGIN { exit !(n >= 719 && n <= 720) }' || { echo "received $(field frames) frames, not 720" >&2; exit 1; }
  pairs="$(field av_sync_events)"
  awk -v n="$pairs" 'BEGIN { exit !(n >= 4) }' || { echo "only $pairs flash/beep pairs in the received stream" >&2; exit 1; }
  awk -v m="$(field av_offset_ms)" 'BEGIN { exit !(m >= -45 && m <= 125) }' || { echo "received A/V offset $(field av_offset_ms) ms is noticeable" >&2; exit 1; }
  echo "Received stream: $(field frames) frames, $pairs pairs, mean offset $(field av_offset_ms) ms, worst $(field av_offset_max_ms) ms."
fi
echo "Evidence: $artifacts"
