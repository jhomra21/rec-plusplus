# MediaMTX, a local RTMP server for the streaming E2E on macOS: pinned release,
# checksum-verified. Source this file; scripts/windows-common.ps1 has the Windows twin.
mediamtx_version=v1.21.2
mediamtx_pid=""

# get_mediamtx CACHE: downloads MediaMTX once into CACHE and prints the binary's path.
get_mediamtx() {
  local cache="$1" arch sha dir archive hash
  case "$(uname -m)" in
    arm64) arch=arm64; sha=7d0d0c5c11527cbd10bde1c3d728864eb0334a9b653566cd73c0259750cbf093 ;;
    x86_64) arch=amd64; sha=87a00062b6a588aa1158f875504a3b56c80d62e2611dc450b83d0534e1b49198 ;;
    *) echo "no MediaMTX build for $(uname -m)" >&2; return 1 ;;
  esac
  dir="$cache/mediamtx-$mediamtx_version-$arch"
  if [[ ! -x "$dir/mediamtx" ]]; then
    mkdir -p "$dir"
    archive="$dir/mediamtx.tar.gz"
    curl -fsSL -o "$archive" \
      "https://github.com/bluenviron/mediamtx/releases/download/$mediamtx_version/mediamtx_${mediamtx_version}_darwin_$arch.tar.gz"
    hash="$(shasum -a 256 "$archive" | awk '{ print $1 }')"
    if [[ "$hash" != "$sha" ]]; then
      rm -f "$archive"
      echo "MediaMTX download checksum mismatch: $hash" >&2
      return 1
    fi
    tar -xzf "$archive" -C "$dir" mediamtx
    rm -f "$archive"
  fi
  echo "$dir/mediamtx"
}

# new_test_certificate DIR: a throwaway self-signed certificate for localhost and 127.0.0.1,
# DIR/cert.pem and DIR/key.pem, for MediaMTX's RTMPS listener.
new_test_certificate() {
  local dir="$1"
  mkdir -p "$dir"
  cat >"$dir/openssl.cnf" <<'CONFIG'
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = localhost
[ext]
subjectAltName = DNS:localhost,IP:127.0.0.1
extendedKeyUsage = serverAuth
basicConstraints = CA:FALSE
CONFIG
  openssl req -x509 -newkey rsa:2048 -nodes -days 2 -config "$dir/openssl.cnf" \
    -keyout "$dir/key.pem" -out "$dir/cert.pem" 2>/dev/null
}

# start_mediamtx BINARY RECORDINGS WORK [TLS]: serves RTMP on 127.0.0.1:1935 and WHIP on
# http://127.0.0.1:8889/<path>/whip (media over UDP 127.0.0.1:8189), and records every
# published stream as fragmented MP4 under RECORDINGS. With TLS, a directory from
# new_test_certificate, it also serves RTMPS on 127.0.0.1:1936.
start_mediamtx() {
  local binary="$1" recordings="$2" work="$3" tls="${4:-}" attempt secure=""
  mkdir -p "$recordings" "$work"
  if [[ -n "$tls" ]]; then
    secure="rtmpEncryption: optional
rtmpsAddress: 127.0.0.1:1936
rtmpServerKey: $tls/key.pem
rtmpServerCert: $tls/cert.pem"
  fi
  cat >"$work/mediamtx.yml" <<CONFIG
logLevel: info
api: no
metrics: no
pprof: no
playback: no
rtsp: no
rtmp: yes
rtmpAddress: 127.0.0.1:1935
$secure
hls: no
webrtc: yes
webrtcAddress: 127.0.0.1:8889
webrtcLocalUDPAddress: 127.0.0.1:8189
webrtcLocalTCPAddress: ''
webrtcIPsFromInterfaces: no
webrtcAdditionalHosts: [127.0.0.1]
webrtcICEServers2: []
srt: no
moq: no
pathDefaults:
  record: yes
  recordPath: $recordings/%path/%Y-%m-%d_%H-%M-%S-%f
  recordFormat: fmp4
  recordPartDuration: 1s
  recordSegmentDuration: 1h
paths:
  all_others:
CONFIG
  "$binary" "$work/mediamtx.yml" >"$work/mediamtx.log" 2>&1 &
  mediamtx_pid="$!"
  for attempt in $(seq 75); do
    if ! kill -0 "$mediamtx_pid" 2>/dev/null; then
      echo "MediaMTX exited: $(cat "$work/mediamtx.log")" >&2
      mediamtx_pid=""
      return 1
    fi
    if (exec 9<>/dev/tcp/127.0.0.1/1935) 2>/dev/null; then return 0; fi
    sleep 0.2
  done
  stop_mediamtx
  echo 'MediaMTX did not open 127.0.0.1:1935' >&2
  return 1
}

stop_mediamtx() {
  if [[ -n "$mediamtx_pid" ]]; then
    kill "$mediamtx_pid" 2>/dev/null || true
    wait "$mediamtx_pid" 2>/dev/null || true
    mediamtx_pid=""
  fi
}
