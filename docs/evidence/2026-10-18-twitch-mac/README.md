# macOS: RTMPS, reconnect and Twitch, 2026-10-18

The first run of the RTMPS and reconnect code on a real Mac, at commit `f6742b0`, reported by the Mac test session. Twitch streams used bandwidth-test mode (`?bandwidthtest=true` after the key), so nothing went live. The key was read from the Keychain at run time and did not appear in any log checked. The logs, probes and screenshots stayed on the Mac (`~/Documents/rec-plusplus-evidence/2026-10-18-twitch-mac/`) and are not copied here; the numbers below are from that report.

## Local checks

| Check | Result |
| --- | --- |
| `REC_E2E_CAPTURE=1 bash scripts/smoke.sh` | Passed. An untrusted self-signed MediaMTX certificate was refused. Trusted RTMPS: 120 frames streamed, 119 received, 4 pairs, mean −31.9 ms, worst 42.5 ms (AVFoundation trims 44 ms of AAC priming from MediaMTX files). Server restart: `Streamed 268 frames (6.4 MB), skipped 32, reconnected 1`; the session after the restart had 179 frames, 6 pairs, mean −44.3 ms, worst 53.2 ms. The app check inside it passed |
| `REC_E2E_STREAM=1 bash scripts/e2e-live-audio.sh` | Passed. Local file: 720 frames, 6 pairs, mean +28.7 ms, worst 35.5 ms. Received: 719 frames, 6 pairs, mean −14.0 ms, worst 25.5 ms. `Streamed 720 frames (8.7 MB), skipped 0` |
| `bash scripts/app-e2e.sh` | Passed |
| `rec record --stream rtmps://expired.badssl.com/...` | Refused: "the certificate ... is not trusted" |

The restart's 53.2 ms worst was 1.8 ms under the smoke limit of 55 ms. The limit is now 80 ms: the 44 ms priming trim plus the beep at the resume keyframe, which can decode faded in and read up to about 30 ms late (measured on Windows).

## Twitch

| Run | Result | Inspector |
| --- | --- | --- |
| Rec.app, Twitch preset, 30 s, display at 1080p60, system audio and microphone | The server field showed `rtmps://live.twitch.tv:443/app`; ● LIVE the whole time. Movie: 1,782 frames, 29.7 s, 3 audio tracks. The key was stored in the Keychain (service "rec stream key", account = the server URL) and not in `settings.conf` | 10:00, 30 s, `rec` |
| CLI, test pattern and tone, 720p30, 30 s, RTMPS | `Streamed 900 frames (22.2 MB), skipped 0`. Local movie: 900 frames, 30 s, 30 pairs, offset 0.17 ms | 9:58, 30 s, `rec` |
| CLI, same for 60 s, Wi-Fi off for 5 s starting 15 s in | `Streamed 1551 frames (36.3 MB), skipped 249, reconnected 1`. Local movie complete: 1,800 frames, 60 s, 60 pairs | Two sessions: 9:58, 44 s, then 9:59, 34 s |

The Wi-Fi drop cost about 8.3 s of stream (249 frames) for a 5 s outage: Wi-Fi rejoining, a new TLS connection, and waiting for the next keyframe. The first Twitch session reads 44 s although the connection dropped at 15 s, probably because Twitch holds a silent session open for about 30 s. That build printed no time for the outage. The CLI now prints `Stream lost at …, reconnecting` and `Stream reconnected at …` lines, which the smoke tests check.

## Not covered

- Sync of what Twitch received, sustained 6 Mb/s, and YouTube.
- A network drop on Windows, and outages longer than a few seconds.
