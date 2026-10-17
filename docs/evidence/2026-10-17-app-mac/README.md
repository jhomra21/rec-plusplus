# Mac app run, 2026-10-17

First use of the macOS desktop app (`Rec.app`, commit `8303185`) with real capture: an M3 Pro on macOS 27.0.1 with a 4112x2658 display. Recordings were 1080p60 H.264 MOV (1670x1080, matching the display's aspect). The evidence stayed on the Mac in `~/Documents/rec-plusplus-evidence/2026-10-17-app-mac/`: GUI screenshots in `gui/`, the audio investigation in `cli-preexisting/` and the movies in `~/Movies/`.

## Checklist

| Step | Result |
| --- | --- |
| Start with system audio and microphone | Passed. The timer and frame count ran and both meters moved (system about 0.93, microphone about 0.43) |
| Pause, Resume, Stop | Passed. "Saved rec-2026-10-17-135137.mov" (6.13 s, 3 audio tracks) |
| Show Last Recording | Passed. Finder opened with the file selected |
| ⇧⌘R from another app | Passed with a real keystroke while Finder was in front; Finder stayed in front. Starting 0.1 s after a save also worked |
| Close the window while recording | Passed. Recording continued, and the menu bar item showed the timer |
| Stop from the menu bar | Passed. The menu offers Stop Recording, Pause, Show Rec, Open Recordings Folder and Quit Rec |
| ⌘Q while recording | Passed. `rec-2026-10-17-135353.mov` (3.48 s, 3 tracks, system audio present) was saved and Rec exited 0.145 s later |
| Self-test (`REC_APP_SELF_TEST`) with real capture | Every step and the keychain check passed. Three of its movies had silent system audio (below) |

The movies in the table had no flash/beep stimulus, so their probes report 0 or 1 sync pairs; they show frames, durations and track layout only.

## Found: audio that was already playing was lost

When another app was already playing sound as recording started, the system audio track was all zeros (peak 0.0000) for the whole recording, including sounds that started later from the same app. Sounds that started after the recording began were captured (an `afplay` clip started 1.5 s in peaked at 0.32). It reproduced with `afplay`, QuickTime Player and the flash/beep stimulus, each started 2 s before recording, with both the CLI and the app. A temporary log in the audio callback showed the buffers arrive from ScreenCaptureKit already silent.

With `excludesCurrentProcessAudio = NO` the same test recorded the clip (peak 0.345). A standalone ScreenCaptureKit program with rec's other settings and exclusion on did capture audio that was already playing, so the trigger inside rec is not fully understood. rec plays no sound of its own, so the engine now leaves its own audio in. The live scripts never caught this because they start the stimulus after the recording; `REC_E2E_PREPLAY=1` now starts it 2 s before.

## Smaller findings

- Without Screen Recording permission, Start showed the raw "The user declined TCCs ..." text. The engine now explains which setting to turn on, and the app checks permission before starting and offers to open System Settings.
- The meters kept their last level after Stop. They now drop to zero.
- macOS left its own Screen Recording prompt on screen after access was granted in System Settings. That window belongs to the system.
