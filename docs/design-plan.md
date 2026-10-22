# App design plan

Plan for the Logic Pro / Ableton Live design direction (see AGENTS.md). Written as a read-only review on 2026-10-21; file:line references point at that revision.

Decisions: one red accent for recording/live; paused state uses gray text with a dimmed red dot, and red text is reserved for recording (user decision 2026-10-22). The Windows app is always dark except in high-contrast mode; the Mac app uses Dark Aqua.

Small steps first: (1) slider readouts, double-click and Ctrl/Cmd+0 reset, center mark, save on release (done on Windows and macOS); (2) one red accent and Screen/Camera strips (done on Windows; macOS in progress); (3) single status line; (4) Mac dark appearance and drag on readouts. Then the large steps below.


## 0. What exists today (the facts the plan builds on)

**Audio levels already flow end to end; no engine API change is needed.**

- Capture path: `core::raise_peak()` at `src/core/mixer.hpp:16-20` is a lock-free CAS max into `std::atomic<float>`; no allocation, no blocking. Called from the audio delivery path at `src/platform/windows/recorder.cpp:1800` and `src/platform/macos/recorder.mm:1413`.
- `Recorder::stats()` takes the peak with `exchange(0)` (`recorder.cpp:1060-1061`, `recorder.mm:898-899`), so `Stats::system_peak/mic_peak` (`include/rec/rec.hpp:211-213`) is "peak since the previous stats() call".
- It crosses the engine socket in `src/cli/wire.cpp`, lands in `Status::system_peak/mic_peak`, and both apps draw it with identical dB-scale/decay code: Windows uses owner-drawn `RecMeter`; the macOS app uses `NSLevelIndicator`.
- Update rate: the poll timer in `App::handle(WM_TIMER)` runs at 50 ms only while a recording is active, and `Session::status()` zeroes peaks when idle. The idle preview is `preview_only` with no audio, so **meters are empty while idle**. That is correct under "no hidden-preview background rendering"; the mixer should show empty meters while idle and not try to meter idle audio.
- Gaps: peaks are mono (max over channels) and there is no RMS. Logic/Ableton show peak with a hold; a peak-hold and clip indicator can be computed in the app from the existing peak. Stereo L/R is not needed for this step; add it only if a strip design demands it (it would be a `float[2]` in `Stats`, same CAS path).

**UI shells.** Windows: one `App` class, manual pixel layout in `App::layout()` with a `level()` lambda for each audio row, controls created in `App::create()`, ids in `enum Control`. macOS: `NSStackView` columns, a `volumeRow` block, meters via `NSLevelIndicator`.

**E2E coupling (what must stay stable).**
- Windows `Find()` iterates control ids **100..399 and 400..439** (`app-e2e.ps1`, `Find`) and matches MSAA `accName` + role (Slider 51, CheckButton 44, PushButton 43, ComboBox 46, Client 10). The widened range includes the camera color sliders (ids 200+) added in the Windows design step 1. Status is read by **id 100**, detail by **101**, live badge by **102**.
- The visual control id block includes screen/camera color labels, sliders, values, reset buttons and strip frames; camera controls are above 200. `Find()` now covers all these ids through 399, plus destination rows 400..439.
- On this PC, `msctls_trackbar32` reports `GCL_STYLE=0x4000`, without `CS_DBLCLKS`; `slider_procedure` therefore detects the second mouse-down itself and preserves the `WM_LBUTTONDBLCLK` path for classes that send it.
- Slider operation is `TBM_SETPOS` then `WM_HSCROLL` with `TB_ENDTRACK` to `App::handle()`; `App::change_volume()` and `App::change_color()` persist values on release. A vertical trackbar sends `WM_VSCROLL`, so the script line changes with the fader orientation.
- Mute goes through `Set-Check 'Mute system'` (role CheckButton) and tray command `1006`.
- macOS: the self-test checks every listed control has an accessible name (`main.mm:2612-2632`), checks window margins ≥ 12 pt and walks the stack hierarchy by position (`:2669-2684`, `stack → body → columns`), and drives volume with `_systemVolume.integerValue = -6; sendAction` (`:3399-3400`).
- Preview snapshots use `PrintWindow` on the client rect (`:209-226`); colors are unaffected by chrome.

## 1. Architecture

### Shared layer (`session.hpp/.cpp`): data only, justified under the abstraction rule

Two consumers already exist (Win32 and AppKit), and `session.hpp` already does exactly this for scenes: `scene_sources()`/`source_name()` (`session.hpp:230-231`). Add the same shape for the mixer, nothing more:

```cpp
// One mixer column, in order: system audio, microphone, screen, camera, then layers.
struct Strip {
    enum class Kind { audio, visual };
    Kind kind;
    std::string name;                     // "System audio", "Microphone", "Screen", "Camera", layer_name().
    std::optional<AudioInput> input;      // audio: fader, meter, mute.
    std::optional<SourceRef> source;      // visual: color controls (screen, camera), shown box (layers).
    bool color = false;                   // Has brightness/contrast/saturation.
};
std::vector<Strip> mixer_strips(const Settings& settings);   // Camera only when settings.camera.

// Meter ballistics shared by both apps: dB over the last 60 dB, falling ~20 dB/s at 20 Hz.
struct Meter { float shown = 0; float feed(float peak); };
```

`Meter` is a straight dedupe of `main.cpp:2268-2271` and `main.mm:2310-2313`. No widget abstraction, no toolkit types, no callbacks. Accessible names come from `Strip::name` plus fixed suffixes so both platforms keep the exact strings the tests look up ("System audio volume", "Mute system", "Screen brightness").

Not shared: layout, drawing, theming, focus. These stay in each `main.*`.

### L1: dark surface on Windows (all in `src/app/windows/main.cpp`)

Decision to make first: **always dark, except in high-contrast mode**. Reason: step 4 forces `NSAppearanceNameDarkAqua` on macOS, and the design language is one dark neutral surface. Following `AppsUseLightTheme` would double the palette work and the test matrix. If the system setting must be honored later, it is one `WM_SETTINGCHANGE("ImmersiveColorSet")` handler and a palette swap, so nothing in this plan blocks it.

Techniques, in dependency order:

1. **Palette** struct in `main.cpp` (surface, panel, text, dim text, accent red, dim accent, meter green/amber) with a `high_contrast()` branch (`SystemParametersInfo(SPI_GETHIGHCONTRAST)`) that returns `GetSysColor` values. `color_for()` (`:1230-1238`) and the preview brush (`:835`) move onto it; the LIVE purple at `:1231` becomes accent red (your step 2).
2. **Title bar**: `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` (attribute 20; 19 on 1809, try 20 then 19). Also for the phone window (`:2444`).
3. **Backgrounds**: class brush at `:393` and `:2468`; `WM_CTLCOLORSTATIC` (`:671-677`, also covers check boxes and trackbars, which both send it), add `WM_CTLCOLOREDIT`, `WM_CTLCOLORLISTBOX`, `WM_CTLCOLORBTN`. The phone window handler at `:2541-2543` too.
4. **Themes**: `SetWindowTheme(h, L"DarkMode_Explorer", nullptr)` on buttons, check boxes, edits, the Sources list and trackbars; `L"DarkMode_CFD"` on combo boxes. The Sources list scrollbar follows the list's `DarkMode_Explorer` theme. Call in `add()` (`:788-793`) so every control gets it.
5. **Trackbars**: `NM_CUSTOMDRAW` through `WM_NOTIFY`: `CDDS_PREPAINT → CDRF_NOTIFYITEMDRAW`; at `CDDS_ITEMPREPAINT` draw `TBCD_CHANNEL` (dark track, accent fill from center or from min, your step 1 center mark) and `TBCD_THUMB` (flat thumb), return `CDRF_SKIPDEFAULT`. Because native tick clearing is a no-op for this custom-drawn trackbar, redraw the 0 tick in the custom draw path. Focus ring: draw `DrawFocusRect`-style outline on the thumb when `GetFocus()==hwnd` and `WM_QUERYUISTATE` does not report `UISF_HIDEFOCUS`. The `slider_procedure` subclass handles reset and release behavior.
6. **Meters**: the progress bar cannot be recolored under visual styles. Replace `PROGRESS_CLASSW` (`:815, 820`) with an owner window class `RecMeter`, same pattern as `RecPreview` (`:400`), same control ids. Store level and clip state in `GWLP_USERDATA`; repaint on change and honor `WM_PRINTCLIENT`. Paint green through -12 dB and amber from -12 dB to 0 dB. Paint the top segment red only after a peak reaches 0 dBFS, held for about one second. In high contrast use one `COLOR_HIGHLIGHT` fill.
7. **Combo drop-down lists and menus**: the popup list (`ComboLBox`) and context menus only go dark through undocumented uxtheme ordinals (135 `SetPreferredAppMode`, `FlushMenuThemes`). Keep this a separate, guarded, last commit (`GetProcAddress`; skip on failure). Acceptable interim: dark closed combo, light open list.

Risks (L1):
- `WM_CTLCOLOR*` does not theme standard controls. Until D2, themed check-box and button text can still render in the light-theme style; combo drop-down lists and menus also remain light. `DwmSetWindowAttribute` and the dark surface do not change that.
- `DWMWA_USE_IMMERSIVE_DARK_MODE` needs build 1809+; ignore the HRESULT.
- Custom-drawn trackbar must respect DPI (`px()`), tick marks off (`TBS_NOTICKS` is already set for volume), and `WM_DPICHANGED` (`:661-669`) recreates fonts but not brushes: palette brushes must be recreated or made DPI-free (solid brushes are). The channel uses RGB 72,72,76 so it stays at least 60 summed-RGB steps from the panel used by the strip coverage probe. The channel rect is swapped for vertical orientation so M2 can reuse the path.
- High contrast toggled live: handle `WM_SETTINGCHANGE` → rebuild palette, `InvalidateRect(window_, nullptr, TRUE)`.
- In high-contrast mode custom draw returns `CDRF_DODEFAULT`, leaving all trackbar parts to the system theme.
- Nothing here changes names, roles, ids, or messages the script sends.

### L2: mixer view

**Placement.** The strips replace the two `level()` rows in the status column (`main.cpp:1068-1069`; `main.mm:955`), under the status line and above the Record buttons, so the preview on the right (`:1083-1091`) and the three settings columns below are untouched. One horizontal row of fixed-width strips (Windows `px(76)` each; Mac 76 pt), height about `px(220)` for audio strips. Order from `mixer_strips()`: System audio, Microphone, Screen, Camera (hidden while `settings.camera` is off), later layers. If the row outgrows the column (more than ~5 strips), it wraps; the layer strips will force this and are deferred.

**Audio strip (top to bottom):** name label; vertical meter beside a vertical fader; dB readout (the existing `kSystemVolumeText`, double-click/Ctrl+0 reset and drag from steps 1 and 4); Mute as the existing `BS_AUTOCHECKBOX` (role stays CheckButton; visible text "Mute", accessible name kept at "Mute system"/"Mute mic" through `IAccPropServices::SetHwndPropStr(PROPID_ACC_NAME)`, already used at `:1318`).

**Visual strip:** name label; three short horizontal sliders Brightness/Contrast/Saturation with value readouts (the existing `kScreenBrightness`… controls and names, `:985-995`), then the existing Reset button. For the camera also the Shape/Border/Shadow stays where it is; a strip holds only per-source picture controls.

**Windows techniques.**
- Fader: `TRACKBAR_CLASSW` with `TBS_VERT | TBS_REVERSED | TBS_NOTICKS`, plus the volume subclass's inverse position mapping, puts `kVolumeMin` at the bottom while preserving logical dB values through `TBM_SETPOS`/`TBM_GETPOS`; route `WM_VSCROLL` into the same `change_volume()` as `WM_HSCROLL` (`:681-691`). Custom draw from L1 handles both orientations (channel rect is given by `TBM_GETCHANNELRECT`, which for vertical trackbars returns the rect as if horizontal: swap x/y).
- Meter: `RecMeter` from L1 gets a vertical mode (taller than wide).
- Layout: a `strip()` lambda in `layout()` beside `level()`; ids unchanged.
- Focus order follows creation order (`:810-823`): system fader, mute, mic fader, mute, then screen sliders, reset, camera sliders, reset. Since the visual controls are created later today (`:843-852` and the camera block), re-create them right after the mic strip in `create()` so Tab moves strip by strip. `WS_EX_CONTROLPARENT` on the main window is already set (`:402`).
- Camera strip hidden: `ShowWindow(SW_HIDE)` of its controls; MSAA `Find` still finds hidden windows by name, so the test does not need the camera on to find them (same as today).

**AppKit techniques.**
- `NSSlider.vertical = YES` (tall frame), `trackFillColor` accent (macOS 11+); `NSLevelIndicator` orients vertically when taller than wide; verify once, fallback is a 30-line custom `NSView` meter. Each strip is a vertical `NSStackView`, the row a horizontal one inserted where the `meters` grid is today (`:955`). The self-test's hierarchy walk (`:2669-2684`) reads `stack → body → columns` by position; the strip row sits inside `top`'s status column, so those indices stay valid.
- Accessibility: `accessibilityLabel` unchanged for every control; the strip stacks get no label (they are groups).

Risks (L2):
- `Find()` id range (see §0): widen in the first Windows mixer commit.
- `WM_HSCROLL` → `WM_VSCROLL` for the volume sliders in `app-e2e.ps1:826` only; color sliders stay horizontal.
- Vertical trackbar values are mapped through `TBS_REVERSED` so +20 dB is at the top and −40 dB at the bottom; the subclass keeps the public dB values and mouse/keyboard changes aligned.
- Window height grows by about 44 px at 96 DPI; keep the added mixer height inside the existing fitted window and verify channel alignment at 96/144/192 DPI.
- Keyboard: vertical trackbar maps Up/Down to +/−; `slider_procedure` already treats `VK_UP/VK_DOWN` as change keys (`:450-451`).
- Hidden camera strip must not take Tab focus: `WS_TABSTOP` on hidden windows is skipped by `IsDialogMessage`, fine.
- The preview area and its drag test (`Drag-Preview`, `:254-269`) are untouched.
- Layers: a layer strip would hold only a Shown box today, so defer until layers get per-source controls; reserve `kStrip = 300 + 10 * index` now and include `300..399` in `Find()`.

## 2. Commit plan

Each commit runs `scripts/app-e2e.ps1` (Windows) or `app-e2e.sh` (macOS) to completion; "assert" lists additions.

| # | Commit | Size | Verification |
|---|---|---|---|
| D1 | **Implemented and Windows app E2E verified locally**: always-dark Windows palette with a `SPI_GETHIGHCONTRAST`/system-color branch; palette-owned surface and strip-panel brushes; `color_for()` and preview brush use the palette; `DWMWA_USE_IMMERSIVE_DARK_MODE` on the main and phone-code windows; all four `WM_CTLCOLOR*` handlers on both windows; Screen and Camera group frames use lighter panel fills and `WS_CLIPSIBLINGS`, with parent erase/print painting that keeps the panels behind their children. Strip children use the panel brush. | ~120 lines | `Expect-Dark` captures the client with `PrintWindow`, checks the 10×10 px margin is dark and the Screen panel is lighter than the surface but below 70, then checks visible brightness/contrast/saturation sliders and Reset each have ≥20 pixels differing from panel color by >60 RGB total. Release build and full app E2E passed locally on 2026-10-22. High-contrast mode saves the screenshot and skips dark-color assertions. |
| D2 | **Implemented**: `add()` applies `DarkMode_Explorer` to buttons/check boxes, edits, Sources list and trackbars, and `DarkMode_CFD` to combo boxes, using case-insensitive class names. The Sources scrollbar follows the list theme. Phone-window controls use the same path. Themed controls restore system theming in high-contrast mode. `WM_SETTINGCHANGE` only rebuilds the palette and re-themes for high-contrast changes or `ImmersiveColorSet`. | ~30 lines | Text-contrast and Screen/Camera strip coverage probes; `Find`/`Set-Check`/`Choose-Last` still assert accessible roles and names. |
| D3 | **Implemented**: Trackbar `NM_CUSTOMDRAW` uses neutral channel (RGB 72,72,76), fill and thumb colors; red is reserved for recording/live state. Volume faders have no fill; bipolar color sliders fill from the zero detent and show its mark. Focused thumbs have a neutral focus ring. High contrast uses system colors and the system trackbar theme. The channel color stays above the strip probe's >60 summed-RGB contrast threshold. Vertical audio faders map +20 dB to the top; roundness has no fill and dims when disabled. | ~120 lines | E2E snapshots assert zero red, at least six light-thumb and channel pixels on volume and Screen brightness (including neutral fill on brightness). |
| D4 | **Implemented**: registered `RecMeter` owner-drawn controls replace the Windows progress bars (ids `kSystemMeter/kMicMeter` unchanged); level and clip state are held in `GWLP_USERDATA`, painted without background erase, and exposed to MSAA as ProgressBar with integer dB or `silent`. Accessibility properties are installed once; value-change events are limited to 2 Hz and emitted only when the rounded value changes. Shared `rec::app::Meter` ballistics remain compatible with macOS `feed()` and add a one-second clip hold. Windows draws green through -12 dB, amber to 0 dB, and red only in the top segment while clipped; high contrast uses one highlight fill. Renderer supports bottom-up vertical meters for M2. | ~100 lines | Windows app E2E step 7 checks bounded meter probes, idle pixels, green and no red at -6 dB, and a post-mute rising-level check on a later beep. |
| D5 | Guarded uxtheme ordinal 135 for drop-down lists and menus. Brush rebuilds also update `GCLP_HBRBACKGROUND` on `RecWindow` and `RecPhoneCode` and invalidate both windows; preview and surface share one brush because they use the same color. | ~80 lines | ps1 unchanged; screenshot artifact of an open combo (`CB_SHOWDROPDOWN`, PrintWindow) saved as `app-combo.png`; no assertion on color because the API is undocumented. |
| D6 | Follow-up: Windows blue remains on check-box checks, Sources selection and focused edit borders. Plan `NM_CUSTOMDRAW` for `BS_AUTOCHECKBOX` (keep role `CheckButton`) to draw the glyph in the palette, and `LBS_OWNERDRAWFIXED|LBS_HASSTRINGS` for Sources so MSAA names survive. | Follow-up | Verify palette-colored check glyph and selection while preserving accessible names and roles. |
| M1 | **Implemented**: both apps build audio rows from `mixer_strips()`; Windows retains the established audio control IDs and accessible names. | ~80 lines | Full Windows app E2E finds the model-ordered System audio and Microphone controls, and verifies saved volume/mute behavior. The existing `Find()` range `100..399` includes reserved strip IDs. |
| M2 | **Implemented**: Windows audio columns use vertical `TBS_VERT|TBS_REVERSED` faders (minimum at bottom, maximum at top) and bottom-up `RecMeter`s, with draggable dB readouts and Mute below. `WM_HSCROLL`/`WM_VSCROLL` share live-apply and save-on-release handling; the original dB range and sign are unchanged. The layout uses one left edge per strip, centered readouts, and aligns the meter to the trackbar channel at each DPI. The strip adds about 44 px at 96 DPI. | ~180 lines | Release build and full guarded Windows app E2E passed locally on 2026-10-22: fader direction, bottom-up meter, channel alignment and screenshot crops at 96/144/192 DPI, status colors, focus order, recording, mute, stream, replay and log-off. The same app E2E is included in `scripts/smoke.ps1`; CI result for this change is pending. |
| M3 | macOS vertical audio strips, with one-line source titles, aligned controls, accessible bottom-up meters, and a Dark Aqua panel shared with Screen/Camera strips. | ~120 lines | `scripts/app-e2e.sh`: accessible controls and margins; title fit and paired fader alignment; meter/fader/readout/mute geometry; key-view order; max/min thumb direction and Up/Down keyboard behavior; 1440x900 Record visibility; meter bottom-up on test tone. Full app E2E and smoke run after review fixes; snapshot `build/m3-artifacts/app-strips-m3.png`. |
| M4 | Screen and Camera visual strips on both apps: move color sliders + Reset under the strip; camera strip hidden/shown with `settings.camera` | ~150 lines (Win) + ~100 (Mac) | ps1: `:552-581` brightness path unchanged (names, ids, `WM_HSCROLL`); add: camera strip controls hidden before `Set-Check 'Camera'`, visible after (`IsWindowVisible` of `kCameraBrightness`). sh: `screen-color=ok` and the gray/reset PNG artifacts unchanged; add camera strip hidden/visible check. |
| M5 | Reserve layer strip ids and `Find` range; no UI | ~10 lines | Scripts unchanged; documented in `docs/parity.md` as "layer strips: not built". |

Order: D1 → D2 → D3 → D4 → M1 → M2 → M3 → M4 → D5 (D5 last because it is optional and undocumented). D4 and M1 can swap. D1-D3 are implemented on `wip/d3-windows`; D4 is implemented and Windows app E2E verified on `wip/d4-windows`.

## 3. Recommended changes to steps 2-4 so they lead into L1/L2

- **Step 2 (one red accent, Screen/Camera strips)**: Windows has the one red accent and Screen/Camera strips. Each strip spans the Capture column, uses an inset header and rows, a compact Reset button, and value readouts beside the sliders at the panel edge, all sized with `px()`. macOS has the same strips (NSBox groups with a lighter fill) and proves them with in-app window snapshots. The visual values are plain static labels whose accessible names are their displayed text; `PROPID_ACC_VALUE` belongs to the sliders, while their established accessible names stay unchanged. Keep the reset buttons' accessible names `Reset screen color` / `Reset camera color` while their visible captions are `Reset`.
- **Step 3 (single status line)**: keep three HWNDs (ids 100, 101, 102) and lay them out on one line rather than merging text. `Wait-Status`, `Get-Key`, the LIVE check (`:415`) and `:912` read those ids; a merge would touch nine script sites. Windows sizes the status slot to its title-font label, keeps the LIVE slot only while live, and gives the detail at least 290 DIPs at 96 DPI. The detail keeps full text for MSAA, truncates visually with tail ellipsis, and shows a hover tooltip only while truncated. The title font is 16 pt, reduced from 18 pt during implementation to keep the state label compact; elapsed time now leads the detail text. The app E2E checks row geometry, truncation and tooltip text, and saves `app-status-row.png`. macOS keeps `_status`, `_detail`, and `_live` as separate controls in one row (the self-test's `wait_for(@"Ready to record")` reads `_status` only); the status font is 15 pt semibold. `_detail` has a 120 pt minimum width, truncates visually with its full text in the accessibility value, and shows an expansion tooltip only while truncated. The self-test checks the row geometry and saves `app-status-row.png`.
- **Step 4 (DarkAqua, drag on readouts)**: Implemented and passed the full macOS app E2E on 2026-10-22. `NSApp.appearance` is set to Dark Aqua before windows are created. Paired volume and Screen/Camera readouts remain accessible static text. Vertical drag changes one step per 2 pt (Shift: 8 pt); continuous slider actions update readout and applied preview/recording during movement, while release runs only the finish/save action once. Click is inert, double-click resets, and disabled source readouts ignore drag/reset; self-test confirms window-coordinate orientation, mid-drag applied state, persistence only after release, and no save for a disabled mic readout. Window PNGs are SourceOver-flattened onto dark and alpha-gated (0 transparent of every pixel); opaque-pixel mean luminance: `app-status-row.png` 51.8, `app-strips.png` 54.7, `app-recording-status.png` 53.3, `app-paused-status.png` 31.9, `app-camera-strip.png` 55.6. Paused status uses neutral gray text with at least 4.5:1 contrast against the surface and a 55%-alpha red dot; red text is reserved for recording. The self-test verifies text neutrality/contrast and measures the paused and recording dots from their snapshots. User decision 2026-10-22. Windows is also implemented: one `readout_procedure` handles all eight labels through their existing readout-to-slider pairing; dragging up/down changes the paired slider at one step per 2 DIPs, Shift uses 8 DIPs per step, and `IDC_SIZENS` indicates the gesture. It best-effort captures the pointer but tracks direct posted messages too; click-only leaves the value alone, double-click still resets, and release sends the trackbar end notification so the existing save-on-release path runs once. `SS_NOTIFY` is enabled on readouts; `PROPID_ACC_VALUE` remains on sliders and readouts remain plain static MSAA labels. The Windows app E2E checks both drag directions, release persistence and a click without movement without moving the real pointer; full local E2E passed on 2026-10-22.

One thing to settle before D1: whether Windows should always be dark (my recommendation above) or follow the system setting. Everything else in the plan is independent of that choice.
