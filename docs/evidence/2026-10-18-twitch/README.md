# Twitch ingest over RTMPS and RTMP, 2026-10-18

The first test against a real streaming service, at commit `5350f07`, on the same Windows 11 (26200) PC as earlier runs (RTX 5070 Ti, 3840x2160 display). Every stream used Twitch's bandwidth-test mode (`?bandwidthtest=true` after the key), so Twitch received it but nothing went live. The key was read from Credential Manager at run time and replaced with `****` in these logs. Each folder holds the recorder's log and the probe of the local movie; the movies were not kept.

## Streams

| Run | Server | Content | Sent | Local movie |
| --- | --- | --- | --- | --- |
| `rtmps/` | `rtmps://live.twitch.tv:443/app` | test pattern + tone, 720p30, 30 s, software H.264 | 900/900 frames, 0 skipped, 1.0 MB | 900 frames, 30 pairs, mean +0.5 ms, worst 10.8 ms |
| `rtmp/` | `rtmp://live.twitch.tv/app` | same | 900/900 frames, 0 skipped, 1.0 MB | same |
| `screen-rtmps/` | `rtmps://live.twitch.tv:443/app` | display 1 at 1080p30 + system audio, 60 s, hardware H.264, 6000 kb/s target | 1800/1800 frames, 0 skipped, 11.3 MB (1.5 Mb/s average) | 1800 frames, 0 dropped |

All three exited with status 0: the TLS connection verified against the system roots, Twitch accepted the publish, and the connection held until rec ended the stream.

## What Twitch received

Twitch Inspector (inspector.twitch.tv, checked by the account owner) listed both synthetic sessions, 9:31:17–9:31:48 and 9:31:49–9:32:20, each 31 s and "stable". It showed the encoder as `rec` and rated the configuration check "Excellent" with no recommended changes. Its bitrate graph stayed near zero because the test pattern compresses to about 270 kb/s. The screen session was not checked in Inspector.

## Not covered

- Throughput at the target rate: the mostly still desktop averaged 1.5 Mb/s, and the Windows hardware encoder does not pad up to the target. A busy screen (full-screen video) is still needed to send 6 Mb/s.
- Sync of what Twitch received: Inspector does not expose the received media, and the real-screen run had no flash/beep stimulus.
- Reconnecting to Twitch after a real network drop, macOS to Twitch, and YouTube.
