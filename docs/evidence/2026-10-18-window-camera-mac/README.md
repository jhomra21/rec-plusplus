# macOS window close and real camera, 2026-10-18

Commit `32a2ccb`, Juan's MacBook Pro (built with `cmake --build build --parallel`, deprecation warnings only). The raw files (movies, probes, stills, per-frame brightness logs and the app's control dumps) stay on that Mac in `~/Documents/rec-plusplus-evidence/2026-10-18-window-camera-mac/`.

| Step | Result | Numbers |
| --- | --- | --- |
| Live window (`REC_E2E_SOURCE=window`), the window closing about 10 s in | Pass | `The window closed, so the recording stopped.`, exit 0. 620 frames written, 0 dropped, 10.33 s; audio 10.30 s. 10 flash/beep pairs, mean +22.2 ms (audio late), worst 34.5 ms |
| Real camera: `rec record --display 1 --camera "FaceTime HD Camera" -r 1080p -f 30 -t 8`, `hide 1` at 4 s | Pass | 240 frames, 8 s, 1670x1080 (the display's 4112x2658 aspect). 242 camera frames. `Hiding camera 1 at 00:04.1`; the camera is gone from the frame at 4.167 s |
| App: Source "Window: TextEdit", record, close the TextEdit window with ⌘W after 4 s | Pass | Stopped within 0.5 s: "Ready to record", "The window closed. Saved rec-2026-10-18-164920.mov". 297 frames, 4.95 s, audio 4.96 s; the Source list then shows the window as "(not open)" |

Camera first frames: the screen shows through the camera's corner for the first 0.30 s, then one nearly black frame at 0.30 s, then normal exposure from 0.333 s (mean brightness about 159, 151 by 1 s). At 2 s the corner shows the room; at 6 s it shows the desktop.

The first app attempt closed TextEdit's window through AppleScript, which hung (probably on a hidden automation prompt); that recording ran 156.8 s until the window was closed with ⌘W, and was then also saved correctly. A DAW audio host held the speaker output open during the run; the beep detector still found all pairs.
