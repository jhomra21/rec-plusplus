# Windows: sound already playing at the start, 2026-10-17

The Windows follow-up to the [Mac app run](../2026-10-17-app-mac/README.md), at commit `525d4da` plus the background-audio guard added with this evidence. It ran on the same Windows 11 (26200) PC as earlier runs, with an RTX 5070 Ti driving a 3840x2160 display and the default output plus a Behringer UMC204HD microphone (`devices.txt`). Nothing else was playing. Each folder holds the recorder's log and the probe of the movie; the movies were not kept.

## Live flash/beep runs (`scripts/e2e-live-audio.ps1`, 1080p60 H.264, 12 s)

| Run | Pairs | Mean (positive = audio late) | Worst | Frames, drops |
| --- | --- | --- | --- | --- |
| `preplay/`: `REC_E2E_PREPLAY=1` | 10 | −13.3 ms | 30.4 ms | 720/720, 0 dropped |
| `preplay-mic/`: `REC_E2E_PREPLAY=1 REC_E2E_MIC=1` (3 tracks) | 10 | −10.8 ms | 21.9 ms | 720/720, 0 audio drops |
| `normal/`: stimulus after the recording starts | 7 | −17.4 ms | 31.1 ms | 720/720, 0 dropped |
| `preplay-guard/`: `REC_E2E_PREPLAY=1` with the background check (peak 0) | 11 | −9.5 ms | 26.6 ms | 720/720, 0 dropped |

With the stimulus started first, its stream is already open when capture starts. The pre-play movies have the first beep at 0.9 s, where the 1 s beep cycle puts it, on the system track and the mix. The Windows stimulus plays silence between beeps, so these runs cannot show sound in the first 100 ms.

## Desktop app with a tone already playing

A looping 440 Hz tone at −12 dBFS (peak 0.244) started 2 s before rec-app.exe opened. The run was driven through MSAA with system audio and the microphone on, recorded 4 s, then stopped.

- The movie had 243 frames and 3 tracks. The system track and the mix carry the tone from the first 100 ms: RMS 0.154 in 0–0.1 s, then 0.173, peak 0.245. They were read with a temporary Media Foundation level reader that was not kept.
- The meters while recording, out of 1000, were system 795 (the tone at −12 dB on the 60 dB scale) and microphone 157–238. Both read 0 right after Stop and 2 s later.

## Background-audio guard

`e2e-live-audio.ps1` now reads the default output device's peak meter for 2 s before the stimulus and stops if the peak is above 0.01 (−40 dBFS). With the tone playing it stopped with "other audio is playing (peak 0.244 of full scale ...)" before recording. With a quiet system it read 0 and the run passed (`preplay-guard/`). `REC_E2E_ALLOW_SOUND=1` skips the check.

## Microphone permission (inconclusive)

Setting `HKCU\...\CapabilityAccessManager\ConsentStore\microphone\NonPackaged` to `Deny` from a script did not block capture. The app recorded and the microphone track kept its usual noise floor (mean RMS 0.003). The value was set back to `Allow`. The engine maps `E_ACCESSDENIED` from the microphone to "Turn on "Let desktop apps access your microphone" in Settings > Privacy & security > Microphone", but turning the setting off in the Settings app, which this did not do, is still needed to see that message.
