# rec- engineering notes

## Scope

Build a compact recorder for macOS (primary) and Windows. Keep the recording engine usable from C++ without the CLI or a GUI. On macOS the current implementation captures one display, system audio and a microphone with ScreenCaptureKit and writes H.264 or HEVC video plus AAC audio (a mix and per-source stems) with AVAssetWriter. On Windows the same engine contract uses Windows.Graphics.Capture, Direct3D 11, WASAPI and the Media Foundation sink writer. The product goal is full feature parity with rec-rs, including audio, camera, scenes, replay, streaming, and a native UI. Track implemented, verified, and missing behavior separately in `docs/parity.md`. Do not imply parity from a compiled interface.

## Decisions

- Use C++20. Limit Objective-C++ to Apple framework adapters in `src/platform/macos/`. Windows code uses C++/WinRT only for capture interop in `src/platform/windows/`.
- Platform-neutral audio logic (mixer, limiter, synthetic sources, flash/beep sync analysis) lives in `src/core/` and is shared by both engines.
- The public object API is `rec::Recorder` with `start()`, `stop()`, `finished()`, `stats()`, and `failure()`. The CLI calls the same API.
- Keep capture callbacks free of encoding, blocking I/O, and allocations except retaining the delivered buffer.
- Stamp every source on the host clock (mach time on macOS, QPC on Windows). Video is written on an exact 1/fps grid, emitted 40 ms behind real time so each slot uses the frame that was on screen at that instant. The engine holds at most three pending surfaces plus the current one (SCK `queueDepth` 6; on Windows five NV12 slots and a two-buffer WGC pool). Use VideoToolbox via AVAssetWriter on macOS and the hardware Media Foundation encoder on the GPU that drives the captured monitor on Windows.
- Audio sources use fixed 16-buffer queues that drop the oldest on overflow. The recording thread drains them, including while it waits for video backpressure, because AVAssetWriter interleaves inputs. The mixer owns a 2-second timeline and is allocated only when two sources are mixed. Add other queues only for a demonstrated need, with explicit size and overload behavior.
- Expose errors at the API boundary. No exceptions across the Apple callback boundary or out of WinRT event handlers.
- Windows loopback delivers no packets while nothing plays, and the MP4 sink holds video until audio catches up. The engine plays silence on the output endpoint during system-audio capture to keep the audio clock moving. The MP4 sink also stores tracks in reverse `AddStream` order; streams are added microphone stem, system stem, mix, video.
- Prefer the platform media frameworks and the C++ standard library. Do not add FFmpeg, Qt, Electron, or a plugin framework by default.
- Verify end-to-end output files on each platform before claiming recording quality or performance there. Linux builds exercise the CLI and unsupported-platform contract only.
- Do not write unit tests. Use end-to-end capture and output verification for behavior; retain output recordings and probe results as reproducible CI artifacts.
- Tests on a person's own computer must never act on their session or on dialogs the test did not open. In AppleScript, never put `log` (or any other statement) inside a `tell application "System Events"` block unless it is meant for System Events: there `log out` ends the session. Return text from the top level instead. Read a window's owner and text through accessibility before acting on it, never from a screenshot alone; never click buttons in system prompts (keychain, login window, permission or logout alerts), which are for the person; cancel a logout, restart or shut-down confirmation if one appears. Prefer the app's own self-test (`REC_APP_SELF_TEST`) and accessibility labels over UI scripting.
- A test that goes live on a real service must end on its own: give the recording a duration (`-t`) or start a watchdog that runs `rec stop` at the deadline, independent of the agent. Never wait for a person while a live stream or recording runs; stop it first, then ask.
- Keep benchmarks reproducible: same machine, same screen content, codec, output frame rate, resolution, and recording duration. Include encoder backpressure and memory samples.
- Add an abstraction only when a concrete second implementation needs it.
- The native interface should feel joyful, clear and responsive: one-step recording, obvious status, accessible controls, and no hidden-preview background rendering. Keep the recording core independent from UI lifecycles.
- Follow the design languages of Apple Logic Pro and Ableton Live: dense but calm panels, a dark neutral surface with one clear accent for state (recording, live), compact controls grouped by source like mixer channel strips, direct manipulation (drag to change a value, double-click to reset it to its default), values shown next to their controls, and no decoration that does not carry information.

## Reference codebases

Use these projects to understand patterns and tradeoffs, not as templates to copy.

- `michaelshimeles/rec-rs`: ScreenCaptureKit frame handling, timing, AVAssetWriter output, A/V synchronization, replay and streaming requirements. Study its tests and `PERFORMANCE.md` before changing output semantics.
- `R44VC0RP/isocast` (Isocast): native macOS recorder in Rust. Study its OBS-style scenes and sources, per-source ISO recordings beside the program output, RTMP/RTMPS streaming, the camera extension, and the background engine daemon (`isocastd`) that the app, CLI, MCP and plugins all drive through one API. Use it for scene, ISO and headless-capture design. It is MIT/Apache-2.0 licensed; still record the upstream revision when copying behavior.
- `obsproject/obs-studio`: separation of capture sources and outputs, texture lifetime, device changes, bounded queues, and encoder backpressure. Follow the architecture, not the plugin surface or GPL source code.
- `jhomra21/daw-browser-convex`: native C++ ownership, realtime callback discipline, and isolation of runtime from UI.
- `jhomra21/bgcut`: object-oriented public APIs, dependency direction, no speculative package splitting, and observable E2E acceptance.

- `anomalyco/opencode` (OpenCode v2): package and domain ownership, Solid application structure, service boundaries, persistence, command/action design, and keeping UI state separate from lower-level runtime services.
- `earendil-works/pi` (Pi): narrow interfaces, small composable building blocks, direct code, package boundaries, and avoiding unnecessary abstraction.
- `diffusionstudio/editor` (Diffusion Studio), plus authorized `monorepo-new` when available: editor architecture, media/application boundaries, worker and background processing, and larger product organization.
- `joshpuckett/dialkit` (DialKit): fine-grained interactive controls, parameter editing, reactive UI APIs, and small composable building blocks.
- `solidjs-community/solid-primitives` (Solid Primitives): Solid API design, browser behavior, storage/persistence, lifecycle and cleanup, and package-local ownership.
- `anomalyco/opentui` (OpenTUI): explicit command/result contracts, keyboard and interaction ownership, renderer/core separation, package APIs, and cleanup behavior in interactive systems.

These are references, not dependencies. Record the upstream revision when copying a precise behavior or benchmark. Do not copy incompatible licensed source.

## Layout

- `include/rec/`: public C++ API.
- `src/`: command-line frontend.
- `src/cli/`: record options and control lines shared by `rec record` and the background engine (`rec serve`), which hosts one recording behind a per-user local socket.
- `src/core/`: platform-neutral mixer, sync analysis, RTMP client and its TCP transport, and the WHIP (WebRTC) publisher with its DTLS client and SRTP, shared by the engines.
- `src/platform/macos/`: Apple capture, timing and encoding adapter, TLS for RTMPS (Network.framework), and crypto and Opus for WHIP (Security.framework, AudioToolbox).
- `src/platform/windows/`: Windows capture (WGC, WASAPI), timing and Media Foundation encoding adapter, TLS for RTMPS (Schannel), and crypto and Opus for WHIP (CNG, libopus fetched by CMake).
- `src/platform/unsupported.cpp`: clear error on other hosts.
- `src/app/`: the desktop app. `session.{hpp,cpp}` is the shared layer (settings, stream destinations, one recording session hosted by the background engine and followed by a worker thread); `macos/main.mm` (AppKit) and `windows/main.cpp` (Win32 controls) only present it.
- `support/`: Info.plists, the Windows app manifest, and the phone camera's relay (`phone-relay/`, a Cloudflare Worker that serves `src/core/phone-page.html`).
- `scripts/`: build and end-to-end runs (`*.sh` for macOS, `*.ps1` for Windows), including the app checks `app-e2e.*`.
- `.github/workflows/`: native macOS and Windows builds and capture-independent smoke gates.
