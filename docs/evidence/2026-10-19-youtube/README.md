# YouTube ingest over RTMPS from Windows, 2026-10-19

The first test against YouTube, at commit `0de182b`, on the same Windows 11 (26200) PC as earlier runs (RTX 5070 Ti, 3840x2160 display). Every stream went to `rtmps://a.rtmps.youtube.com:443/live2`, the address of the apps' YouTube preset. The key was read from Credential Manager at run time and replaced with `****` in these logs. YouTube has no bandwidth-test mode like Twitch's, so it received each stream as a real broadcast. Each folder holds the recorder's log and the probe of the local movie; the movies were not kept.

## Streams

| Run | Content | Sent | Local movie |
| --- | --- | --- | --- |
| `synthetic/` (10:25) | test pattern + tone, 720p30, 60 s, software H.264 | 1800/1800 frames, 0 skipped, 2.0 MB (0.27 Mb/s: the pattern compresses well) | 1800 frames, 60 pairs, mean +0.3 ms, worst 10.8 ms |
| `screen/` (10:26) | display 1 at 1080p30 + system audio, full-screen noise window, 60 s, hardware H.264, 6000 kb/s target | 1800/1800 frames, 0 skipped, 48.2 MB (6.43 Mb/s average) | 1800 frames, 0 dropped |
| `app/` (10:28) | Rec app, YouTube preset, test pattern + tone, 30 s | LIVE badge the whole run, about 0.2 Mb/s | 904 frames, 31 pairs, mean +0.5 ms, worst 10.8 ms |

All three ended with status 0: the TLS connection verified against the system roots, YouTube accepted the publish, and the connection held until rec ended the stream. The app read the key from Credential Manager by itself (nothing was typed into it), showed the preset's server URL, and did not write the key to its settings file.

## What YouTube received

The Live Control Room's Stream health tab (checked by the account owner) showed "Stream is healthy – Stream health is excellent" at 10:28, during the app run, and "No data" at 10:29 after it ended. It keeps only the latest messages, so the earlier two runs' health was not seen.

## Not covered

- YouTube's view of the 6 Mb/s run (health, received bitrate) and sync of what YouTube received.
- macOS to YouTube.
- RTMP (unencrypted) to YouTube; the presets use RTMPS.
