# The phone camera's relay (support/phone-relay) on this computer, in wrangler's local Workers
# runtime, so the E2E checks this checkout's relay rather than the deployed one. Needs Node.js;
# the first run downloads wrangler. Source this file; scripts/windows-common.ps1 has the Windows
# twin.
relay_version=4.151.3
relay_pid=""
relay_url=""

# start_relay DIR LOG [PORT]: runs the relay on http://127.0.0.1:PORT (8787) and sets relay_url.
start_relay() {
  local directory="$1" log="$2" port="${3:-8787}" attempt
  # In its own process group, so stop_relay ends npx, wrangler and workerd together.
  (cd "$directory" && WRANGLER_SEND_METRICS=false exec python3 -c 'import os, sys; os.setsid(); os.execvp(sys.argv[1], sys.argv[1:])' \
    npx --yes "wrangler@$relay_version" dev --ip 127.0.0.1 --port "$port" --show-interactive-dev-session=false) \
    </dev/null >"$log" 2>&1 &
  relay_pid="$!"
  relay_url="http://127.0.0.1:$port"
  for attempt in $(seq 360); do
    if [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$relay_url/")" == 200 ]]; then return 0; fi
    if ! kill -0 "$relay_pid" 2>/dev/null; then break; fi
    sleep 0.5
  done
  stop_relay
  echo "the relay did not start: $(cat "$log")" >&2
  return 1
}

stop_relay() {
  if [[ -n "$relay_pid" ]]; then
    kill -TERM -- "-$relay_pid" 2>/dev/null || true
    wait "$relay_pid" 2>/dev/null || true
    relay_pid=""
  fi
}

# forged_offer RELAY TOKEN: posts an offer with a wrong signature, as a relay or anyone who
# knows the room could, and prints the reply and its status. rec must refuse it.
forged_offer() {
  local room
  room="$(printf 'rec phone room\n%s' "$2" | shasum -a 256 | awk '{ print $1 }')"
  curl -s -w ' [%{http_code}]' -X POST -H 'Content-Type: application/sdp' \
    -H "Rec-Mac: $(printf '0%.0s' $(seq 64))" --data 'v=0' "$1/rooms/$room/offer"
}
