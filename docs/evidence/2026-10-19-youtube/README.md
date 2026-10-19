# YouTube ingest over RTMPS, 2026-10-19

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

## macOS

At commit `801fa17` on a Mac (Mac15,7, macOS 27.0.1), reported by the Mac test session. The key was read from the login keychain at run time and replaced with `****` in the log; a scan of every saved file found no key. The stream was set to Private.

| Run | Content | Sent | Local movie |
| --- | --- | --- | --- |
| CLI (10:39 CDT) | test pattern + tone, 720p30, 60 s, hardware H.264 (VideoToolbox), 6000 kb/s target | 1800/1800 frames, 0 skipped, 44.8 MB (5.98 Mb/s: VideoToolbox holds the constant rate even on the simple pattern) | 1800 frames, 60 pairs, mean and worst 0.2 ms |

It exited with status 0. Rec.app's YouTube preset was not run: the key had been added with the `security` tool, so Rec was not on the item's access list, and macOS asked for the login keychain password, which the test session declined.

## Not covered

- YouTube's view of the 6 Mb/s runs (health, received bitrate) and sync of what YouTube received.
- Rec.app's YouTube preset on macOS.
- RTMP (unencrypted) to YouTube; the presets use RTMPS.
