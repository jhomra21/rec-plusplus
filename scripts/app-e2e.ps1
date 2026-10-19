# End-to-end check of the Windows app, driven through Microsoft Active Accessibility, the
# interface screen readers use: every control is found by its accessible name and role and
# operated with its default action. (The managed UI Automation client sees bare panes for
# Win32 controls on some hosts; MSAA reads the same proxies directly.) The app records the
# synthetic flash/beep source (REC_APP_TEST_PATTERN=1) in a temporary home (REC_APP_HOME),
# so it needs no screen, speakers or saved settings:
#   1. Record with system audio and microphone, pause, resume, stop: the movie keeps sync.
#   2. Start and stop with the global hotkey Ctrl+Alt+R.
#   3. Go live to a local MediaMTX server while recording; the server's copy decodes.
#   4. Closing the window while recording keeps recording; the next stop saves the movie.
#   5. With the replay buffer on, Save Replay writes the last seconds while recording goes on.
#   6. A camera and an image are drawn; the scene moves the camera while recording. The
#      preview shows the same picture, before Start too, and stops when Show preview is off.
#      Dragging the camera in the preview moves and resizes it, before Start and while recording.
#      A new scene with a display layer, placed by dragging, is switched away from while recording.
#   7. Volume and mute change the mix while recording, from the window and the menu.
#   8. Settings persist without the stream key, which Credential Manager holds.
#   9. Quit Rec from the notification-area menu while recording saves the movie, then exits.
#  10. Logging off while recording saves the movie before WM_ENDSESSION returns.
param([string]$Rec, [string]$App, [string]$Artifacts)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')
Add-Type -AssemblyName Accessibility, System.Windows.Forms
Add-Type -ReferencedAssemblies Accessibility @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AppWindow {
  [DllImport("oleacc.dll")] static extern int AccessibleObjectFromWindow(IntPtr h, uint id, ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object o);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref RECT r, uint count);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static Accessibility.IAccessible Of(IntPtr h) {
    Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71"); object o;
    Marshal.ThrowExceptionForHR(AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref iid, out o));
    return (Accessibility.IAccessible)o;
  }
  public static void SetValue(IntPtr h, string value) { Of(h).set_accValue(0, value); }
}
'@

# Window sizes in pixels, as the window draws itself.
[AppWindow]::SetProcessDPIAware() | Out-Null
# MSAA roles.
$PushButton = 43; $CheckButton = 44; $Text = 42; $ComboBox = 46; $Slider = 51; $Client = 10
$appHome = Join-Path $Artifacts 'app-home'
$folder = Join-Path $appHome 'recordings'
$server = 'rtmp://127.0.0.1:1935/live'
cmdkey "/delete:rec/stream/$server" 2>&1 | Out-Null
if (Get-Process rec-app -ErrorAction SilentlyContinue) { throw 'quit Rec before running the app E2E' }

function Start-App {
    $env:REC_APP_HOME = $appHome
    $env:REC_APP_TEST_PATTERN = '1'
    $process = Start-Process -FilePath $App -PassThru
    Remove-Item Env:REC_APP_HOME, Env:REC_APP_TEST_PATTERN
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $window = [AppWindow]::FindWindow('RecWindow', [NullString]::Value)
        if ($window -ne [IntPtr]::Zero -and [AppWindow]::GetDlgItem($window, 100) -ne [IntPtr]::Zero) {
            return @{ Process = $process; Window = $window }
        }
        Start-Sleep -Milliseconds 200
    }
    throw 'the Rec window did not appear'
}

# The control whose accessible name and role match; control ids run from 100.
function Find($ui, [string]$name, [int]$role) {
    for ($id = 100; $id -lt 200; $id++) {
        $handle = [AppWindow]::GetDlgItem($ui.Window, $id)
        if ($handle -eq [IntPtr]::Zero) { continue }
        $item = [AppWindow]::Of($handle)
        if ($item.accName(0) -eq $name -and $item.accRole(0) -eq $role) { return @{ Handle = $handle; Item = $item } }
    }
    throw "no control named '$name' with role $role"
}
function Press($ui, [string]$name) { (Find $ui $name $PushButton).Item.accDoDefaultAction(0) }
# The click arrives as a posted message, so a press sent right after it could be handled
# first; wait until the box shows the new state.
function Set-Check($ui, [string]$name, [bool]$on) {
    $box = (Find $ui $name $CheckButton).Item
    if ((($box.accState(0) -band 0x10) -ne 0) -ne $on) { $box.accDoDefaultAction(0) }
    $deadline = (Get-Date).AddSeconds(5)
    while ((($box.accState(0) -band 0x10) -ne 0) -ne $on) {
        if ((Get-Date) -gt $deadline) { throw "'$name' did not change" }
        Start-Sleep -Milliseconds 50
    }
    Start-Sleep -Milliseconds 100
}
function Set-Field($ui, [string]$name, [string]$value) {
    [AppWindow]::SetValue((Find $ui $name $Text).Handle, $value)
}
# Selects the last entry of a drop-down list the way the keyboard does, so the app gets
# its normal selection-change notification.
function Choose-Last($ui, [string]$name) {
    $combo = (Find $ui $name $ComboBox).Handle
    $count = [int][AppWindow]::SendMessage($combo, 0x0146, [IntPtr]::Zero, [IntPtr]::Zero)  # CB_GETCOUNT
    [AppWindow]::SendMessage($combo, 0x014E, [IntPtr]($count - 1), [IntPtr]::Zero) | Out-Null  # CB_SETCURSEL
    $id = [AppWindow]::GetDlgCtrlID($combo)
    [AppWindow]::SendMessage($ui.Window, 0x0111, [IntPtr]((1 -shl 16) -bor $id), $combo) | Out-Null  # WM_COMMAND, CBN_SELCHANGE
}
function Text-Of($ui, [int]$id) { return [AppWindow]::Of([AppWindow]::GetDlgItem($ui.Window, $id)).accName(0) }
function Wait-Status($ui, [string]$prefix, [int]$seconds = 20) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $status = Text-Of $ui 100
        if ($status.StartsWith($prefix)) { return $status }
        Start-Sleep -Milliseconds 100
    }
    throw "status is '$(Text-Of $ui 100)' ($(Text-Of $ui 101)), expected '$prefix...'"
}
# Waits until the status shows at least this many recorded seconds, so steps land at the movie
# times the checks expect however slow the machine is.
function Wait-Recorded($ui, [int]$seconds, [int]$timeout = 60) {
    $deadline = (Get-Date).AddSeconds($timeout)
    while ((Get-Date) -lt $deadline) {
        if ((Text-Of $ui 100) -match '(\d+):(\d\d)$' -and [int]$Matches[1] * 60 + [int]$Matches[2] -ge $seconds) { return }
        Start-Sleep -Milliseconds 50
    }
    throw "the recording did not reach $seconds s: '$(Text-Of $ui 100)'"
}
# The preview as the window shows it (the window's composed picture, cropped), saved as a PNG.
function Snapshot-Preview($ui, [string]$path) {
    $handle = (Find $ui 'Preview' $Client).Handle
    $client = New-Object AppWindow+RECT
    [AppWindow]::GetClientRect($ui.Window, [ref]$client) | Out-Null
    $window = New-Object System.Drawing.Bitmap $client.Right, $client.Bottom
    $graphics = [System.Drawing.Graphics]::FromImage($window)
    $dc = $graphics.GetHdc()
    [AppWindow]::PrintWindow($ui.Window, $dc, 3) | Out-Null  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $graphics.ReleaseHdc($dc)
    $graphics.Dispose()
    $rect = New-Object AppWindow+RECT
    [AppWindow]::GetClientRect($handle, [ref]$rect) | Out-Null
    [AppWindow]::MapWindowPoints($handle, $ui.Window, [ref]$rect, 2) | Out-Null
    $bitmap = $window.Clone((New-Object System.Drawing.Rectangle $rect.Left, $rect.Top, ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)), $window.PixelFormat)
    $window.Dispose()
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    return $bitmap
}
# The mean color of part of a picture, given in fractions of its size, as Expect-Color reads it.
function Picture-Color($bitmap, [double]$x, [double]$y, [double]$w, [double]$h) {
    $r = 0; $g = 0; $b = 0
    foreach ($i in 0..9) {
        foreach ($j in 0..9) {
            $p = $bitmap.GetPixel([int](($x + $w * ($i + 0.5) / 10) * $bitmap.Width), [int](($y + $h * ($j + 0.5) / 10) * $bitmap.Height))
            $r += $p.R; $g += $p.G; $b += $p.B
        }
    }
    return @{ color = "$([int]($r / 100)),$([int]($g / 100)),$([int]($b / 100))" }
}
# Waits until the preview's color over part of it is (or with -Not, is not) the given one.
function Wait-PreviewColor($ui, [string]$path, [double[]]$area, [string]$color, [switch]$Not, [int]$seconds = 15) {
    $deadline = (Get-Date).AddSeconds($seconds)
    for (;;) {
        $picture = Snapshot-Preview $ui $path
        $seen = Picture-Color $picture $area[0] $area[1] $area[2] $area[3]
        $picture.Dispose()
        $same = $true
        try { Expect-Color $seen 'color' $color } catch { $same = $false }
        if ($same -ne [bool]$Not) { return }
        if ((Get-Date) -gt $deadline) { throw "the preview at $($area -join ',') is $($seen['color']); expected $(if ($Not) { 'not ' })$color" }
        Start-Sleep -Milliseconds 200
    }
}
# Drags in the preview from one point to another, given as fractions of the 16:9 frame,
# with the button messages and moves the mouse sends.
function Drag-Preview($ui, [double[]]$from, [double[]]$to) {
    $handle = (Find $ui 'Preview' $Client).Handle
    $rect = New-Object AppWindow+RECT
    [AppWindow]::GetClientRect($handle, [ref]$rect) | Out-Null
    $w = $rect.Right; $h = [int][math]::Floor($w * 9 / 16)
    if ($h -gt $rect.Bottom) { $h = $rect.Bottom; $w = [int][math]::Floor($h * 16 / 9) }
    $x0 = [int][math]::Floor(($rect.Right - $w) / 2); $y0 = [int][math]::Floor(($rect.Bottom - $h) / 2)
    $at = { param([double]$x, [double]$y) [IntPtr](([int]($y0 + $y * $h) -shl 16) -bor [int]($x0 + $x * $w)) }
    [AppWindow]::SendMessage($handle, 0x0201, [IntPtr]1, (& $at $from[0] $from[1])) | Out-Null  # WM_LBUTTONDOWN
    foreach ($i in 1..10) {
        $x = $from[0] + ($to[0] - $from[0]) * $i / 10; $y = $from[1] + ($to[1] - $from[1]) * $i / 10
        [AppWindow]::SendMessage($handle, 0x0200, [IntPtr]1, (& $at $x $y)) | Out-Null  # WM_MOUSEMOVE, MK_LBUTTON
        Start-Sleep -Milliseconds 20
    }
    [AppWindow]::SendMessage($handle, 0x0202, [IntPtr]::Zero, (& $at $to[0] $to[1])) | Out-Null  # WM_LBUTTONUP
}
function Newest-Movie {
    $file = Get-ChildItem $folder -Filter '*.mp4' | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $file) { throw "no recording in $folder" }
    return $file.FullName
}

$received = Join-Path $Artifacts 'app-stream-server'
# The camera step's image, and the synthetic camera, chosen as a person would with Browse.
New-Item -ItemType Directory -Force $appHome | Out-Null
$overlayImage = Join-Path $Artifacts 'app-overlay.png'
Add-Type -AssemblyName System.Drawing
$bitmap = New-Object System.Drawing.Bitmap 1920, 1080, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::Transparent)
$graphics.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 220, 30, 30))), 0, 0, 384, 216)
$graphics.Dispose()
$bitmap.Save($overlayImage, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
# 30 fps keeps the check light on CI runners, which encode in software and draw with WARP;
# at 60 fps a slow runner fell over 500 ms behind in the camera step and dropped frames.
[IO.File]::WriteAllText((Join-Path $appHome 'settings.conf'), "fps=30`ncamera_device=test`nimage_path=$overlayImage`n")
$mediamtx = Start-MediaMTX (Get-MediaMTX (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\tools')) $received (Join-Path $Artifacts 'app-mediamtx')
$ui = $null
try {
    $ui = Start-App
    Wait-Status $ui 'Ready to record' | Out-Null

    # 1. Record, pause, resume, stop: 2 + 2 s recorded around a 1.5 s pause.
    Set-Check $ui 'System audio' $true
    Set-Check $ui 'Microphone' $true
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Start-Sleep -Seconds 2
    Press $ui 'Pause'
    Wait-Status $ui 'Paused' | Out-Null
    Start-Sleep -Milliseconds 1500
    Press $ui 'Resume'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Start-Sleep -Seconds 2
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    $detail = Text-Of $ui 101
    Expect-Text $detail 'Saved rec-'
    $movie = Newest-Movie
    Copy-Item $movie (Join-Path $Artifacts 'app-pause.mp4')
    $probe = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-pause.probe')
    Expect-Field $probe 'codec' 'h264'
    Expect-Field $probe 'audio_tracks' '3'
    Expect-Within $probe 'duration' 3.5 4.9
    Expect-Within $probe 'av_sync_events' 3 5
    Expect-Within $probe 'av_offset_max_ms' 0 20
    Write-Host "1. pause/resume: $($probe['frames']) frames, $($probe['av_sync_events']) pairs, worst $($probe['av_offset_max_ms']) ms"

    # 2. Global hotkey.
    [AppWindow]::SetForegroundWindow($ui.Window) | Out-Null
    [Windows.Forms.SendKeys]::SendWait('^%r')
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Start-Sleep -Seconds 2
    [Windows.Forms.SendKeys]::SendWait('^%r')
    Wait-Status $ui 'Ready to record' | Out-Null
    $hotkey = Get-Probe $Rec (Newest-Movie) (Join-Path $Artifacts 'app-hotkey.probe')
    Expect-Within $hotkey 'duration' 1.5 3.5
    Write-Host "2. hotkey: $($hotkey['frames']) frames"

    # 3. Go live to MediaMTX, long enough for its 2 s stream analysis.
    Set-Check $ui 'Microphone' $false
    Set-Check $ui 'Go live while recording' $true
    Choose-Last $ui 'Service'
    Set-Field $ui 'Server URL' $server
    Set-Field $ui 'Stream key' 'appkey'
    Set-Field $ui 'Bitrate (kb/s)' '3000'
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    $deadline = (Get-Date).AddSeconds(10)
    while (-not (Text-Of $ui 102).StartsWith("$([char]0x25CF) LIVE")) {
        if ((Get-Date) -gt $deadline) { throw "no LIVE badge: $(Text-Of $ui 101)" }
        Start-Sleep -Milliseconds 100
    }
    Start-Sleep -Seconds 3
    # The badge shows the measured send rate, here over a second of a 3000 kb/s stream.
    $badge = Text-Of $ui 102
    if (-not ($badge -match 'LIVE\s+(\d+\.\d) Mb/s') -or [double]$Matches[1] -le 0) { throw "no stream rate in the LIVE badge: '$badge'" }
    Start-Sleep -Seconds 1
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    if ((Text-Of $ui 101) -match 'Stream stopped early') { throw (Text-Of $ui 101) }
    Start-Sleep -Seconds 1
    $file = Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received 'live\appkey') | Select-Object -First 1
    if (-not $file) { throw 'MediaMTX recorded nothing' }
    $got = Get-Probe $Rec $file.FullName (Join-Path $Artifacts 'app-stream-received.probe')
    Expect-Field $got 'codec' 'h264'
    Expect-Field $got 'audio_codec' 'aac'
    Expect-Within $got 'av_sync_events' 3 6
    Expect-Within $got 'av_offset_max_ms' 0 20
    Write-Host "3. stream: received $($got['frames']) frames, $($got['av_sync_events']) pairs, worst $($got['av_offset_max_ms']) ms"
    $stored = cmdkey "/list:rec/stream/$server" | Out-String
    if ($stored -notmatch [regex]::Escape("rec/stream/$server")) { throw 'the stream key was not stored in Credential Manager' }

    # 4. Close the window mid-recording: it hides and keeps recording.
    Set-Check $ui 'Go live while recording' $false
    $handle = $ui.Window
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    [AppWindow]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
    Start-Sleep -Seconds 2
    if ([AppWindow]::IsWindowVisible($handle)) { throw 'the window stayed open' }
    if ($ui.Process.HasExited) { throw 'closing the window ended the app' }
    [Windows.Forms.SendKeys]::SendWait('^%r')
    $deadline = (Get-Date).AddSeconds(20)
    while ((Text-Of $ui 100) -ne 'Ready to record') {
        if ((Get-Date) -gt $deadline) { throw 'the hidden recording did not stop' }
        Start-Sleep -Milliseconds 100
    }
    $hidden = Get-Probe $Rec (Newest-Movie) (Join-Path $Artifacts 'app-hidden.probe')
    Expect-Within $hidden 'duration' 1.5 3.5

    # 5. Replay buffer: save the last seconds mid-recording, then keep recording.
    [AppWindow]::SetForegroundWindow($ui.Window) | Out-Null
    Set-Check $ui 'Keep the last' $true
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Start-Sleep -Seconds 5
    Press $ui 'Save Replay'
    $deadline = (Get-Date).AddSeconds(5)
    while (-not (Text-Of $ui 101).StartsWith('Saved replay')) {
        if ((Get-Date) -gt $deadline) { throw "Save Replay: $(Text-Of $ui 101)" }
        Start-Sleep -Milliseconds 100
    }
    Start-Sleep -Seconds 1
    # Show Last Recording must reveal the clip until the recording itself is saved.
    $last = (Get-Content (Join-Path $appHome 'settings.conf') | Where-Object { $_ -like 'last_recording=*' }) -replace '^last_recording=', ''
    if ((Split-Path -Leaf $last) -notlike 'replay-*.mp4') { throw "after Save Replay the last recording is $last" }
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    $clip = Get-ChildItem $folder -Filter 'replay-*.mp4' | Select-Object -First 1
    if (-not $clip) { throw 'Save Replay wrote no file' }
    Copy-Item $clip.FullName (Join-Path $Artifacts 'app-replay.mp4')
    $replay = Get-Probe $Rec $clip.FullName (Join-Path $Artifacts 'app-replay.probe')
    Expect-Field $replay 'codec' 'h264'
    Expect-Within $replay 'duration' 3 6
    Expect-Within $replay 'av_sync_events' 2 6
    Expect-Within $replay 'av_offset_max_ms' 0 25
    $full = Get-Probe $Rec (Newest-Movie) (Join-Path $Artifacts 'app-replay-recording.probe')
    Expect-Within $full 'duration' 5 7.5
    Write-Host "5. replay: $($replay['frames']) frames ($($replay['duration']) s), $($replay['av_sync_events']) pairs, worst $($replay['av_offset_max_ms']) ms"

    # 6. Camera and image: the test camera in the corner, then full frame from the Scene list,
    # then hidden from the notification-area menu, all during one recording. The image is a
    # transparent 1080p PNG with a red block in its top-left corner.
    Set-Check $ui 'Keep the last' $false
    # Step 4 hid the window; the preview draws only while it shows.
    [AppWindow]::PostMessage($ui.Window, 0x0111, [IntPtr]1002, [IntPtr]::Zero) | Out-Null  # WM_COMMAND, tray "Show Rec"
    Set-Check $ui 'Camera' $true
    Set-Check $ui 'Image overlay' $true
    # Before Start the preview already shows the scene; with Show preview off it stops.
    $idle = Join-Path $Artifacts 'app-preview-idle.png'
    Wait-PreviewColor $ui $idle @(0.76, 0.75, 0.18, 0.18) 'green'
    Wait-PreviewColor $ui $idle @(0.02, 0.03, 0.15, 0.15) 'red'
    Set-Check $ui 'Show preview' $false
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-off.png') @(0.76, 0.75, 0.18, 0.18) 'green' -Not
    Set-Check $ui 'Show preview' $true
    Wait-PreviewColor $ui $idle @(0.76, 0.75, 0.18, 0.18) 'green'
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Wait-Recorded $ui 1
    # The preview: the camera in the bottom-right corner and the image's red block top left.
    $picture = Snapshot-Preview $ui (Join-Path $Artifacts 'app-preview-corner.png')
    Expect-Color (Picture-Color $picture 0.76 0.75 0.18 0.18) 'color' 'green'
    Expect-Color (Picture-Color $picture 0.02 0.03 0.15 0.15) 'color' 'red'
    $picture.Dispose()
    Wait-Recorded $ui 2
    Choose-Last $ui 'Scene'  # Camera only
    Start-Sleep -Milliseconds 1500
    $picture = Snapshot-Preview $ui (Join-Path $Artifacts 'app-preview-camera.png')
    Expect-Color (Picture-Color $picture 0.35 0.4 0.3 0.2) 'color' 'green'
    $picture.Dispose()
    Wait-Recorded $ui 5
    [AppWindow]::PostMessage($ui.Window, 0x0111, [IntPtr]2001, [IntPtr]::Zero) | Out-Null  # WM_COMMAND, tray "Screen only"
    Wait-Recorded $ui 7
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    $movie = Newest-Movie
    Copy-Item $movie (Join-Path $Artifacts 'app-camera.mp4')
    $corner = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-camera-corner.probe') @('--sample', '1450,800,380,200')
    $center = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-camera-center.probe') @('--sample', '800,400,300,200')
    $logo = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-camera-image.probe') @('--sample', '40,40,300,150')
    Expect-Color $corner 'sample_1' 'green'
    Expect-Color $center 'sample_1' 'white'
    Expect-Color $center 'sample_3' 'green'
    Expect-Color $center 'sample_4' 'green'
    # A slow runner can drop slots and shorten the movie, so the hidden camera is checked in
    # the last sampled second rather than a fixed one.
    $last = 0
    while ($corner.ContainsKey("sample_$($last + 1)")) { $last++ }
    if ($last -lt 5) { throw "the camera recording is too short: $($corner['duration']) s" }
    Expect-Color $corner "sample_$last" 'white'
    foreach ($second in 0..$last) { Expect-Color $logo "sample_$second" 'red' }
    Write-Host "6. camera and image: $($corner['frames']) frames, corner, full frame, hidden; the preview matched, before Start too"

    # 6b. Camera layout: before Start, drag the camera from its corner to the middle and its
    # top-left corner outwards; while recording, drag it to the top right. The preview and
    # the movie follow each drag.
    $combo = (Find $ui 'Scene' $ComboBox).Handle
    [AppWindow]::SendMessage($combo, 0x014E, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # CB_SETCURSEL: Screen and camera
    [AppWindow]::SendMessage($ui.Window, 0x0111, [IntPtr]((1 -shl 16) -bor [AppWindow]::GetDlgCtrlID($combo)), $combo) | Out-Null
    $layout = Join-Path $Artifacts 'app-preview-layout.png'
    Wait-PreviewColor $ui $layout @(0.76, 0.75, 0.18, 0.18) 'green'
    Drag-Preview $ui @(0.855, 0.84) @(0.5, 0.5)  # The camera's middle: it moves to 0.375,0.375
    Wait-PreviewColor $ui $layout @(0.42, 0.42, 0.16, 0.16) 'green'
    Wait-PreviewColor $ui $layout @(0.8, 0.75, 0.15, 0.15) 'green' -Not
    Drag-Preview $ui @(0.376, 0.376) @(0.3, 0.3)  # Its top-left corner: 0.3,0.3 to 0.625,0.625
    Wait-PreviewColor $ui $layout @(0.31, 0.31, 0.07, 0.07) 'green'
    if (((Find $ui 'Size' $ComboBox).Item.accState(0) -band 0x1) -eq 0) { throw 'Size stayed enabled for a dragged camera' }
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Start-Sleep -Milliseconds 2000
    Drag-Preview $ui @(0.46, 0.46) @(0.96, 0.21)  # To the right edge; the top snaps to the margin
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-layout-moved.png') @(0.72, 0.08, 0.22, 0.22) 'green'
    Start-Sleep -Milliseconds 2500
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    $movie = Newest-Movie
    Copy-Item $movie (Join-Path $Artifacts 'app-layout.mp4')
    $middle = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-layout-middle.probe') @('--sample', '770,430,290,160')
    $moved = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-layout-moved.probe') @('--sample', '1380,90,440,250')
    Expect-Color $middle 'sample_1' 'green'
    Expect-Color $moved 'sample_1' 'white'
    Expect-Color $middle 'sample_4' 'white'
    Expect-Color $moved 'sample_4' 'green'
    Write-Host "6b. camera layout: $($middle['frames']) frames; dragged and resized before Start, moved while recording"

    # 6c. Scenes: a new scene gets display 1 (covered by a blue window) as a layer over the
    # whole frame; dragging the layer's top-left corner makes it the bottom-right quarter, and
    # the scene is renamed. Recording starts in it and switches to the first scene, where the
    # layer is hidden. Then the layer is removed again.
    Set-Check $ui 'Image overlay' $false  # It fills the frame, so it would take the corner.
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $blue = Show-ColorWindow 'rec-app-blue' $screen.X $screen.Y $screen.Width $screen.Height 'Blue' $Artifacts
    try {
        Press $ui 'New Scene'
        Start-Sleep -Milliseconds 300
        [AppWindow]::PostMessage($ui.Window, 0x0111, [IntPtr]1100, [IntPtr]::Zero) | Out-Null  # WM_COMMAND, Add Display > Display 1
        $scenes = Join-Path $Artifacts 'app-preview-scene.png'
        Wait-PreviewColor $ui $scenes @(0.1, 0.55, 0.3, 0.3) 'blue'
        Drag-Preview $ui @(0.004, 0.006) @(0.5, 0.5)
        Wait-PreviewColor $ui $scenes @(0.1, 0.55, 0.3, 0.3) 'blue' -Not
        Wait-PreviewColor $ui $scenes @(0.6, 0.6, 0.3, 0.3) 'blue'
        Set-Field $ui 'Name' 'Blue corner'
        $combo = (Find $ui 'Scene' $ComboBox).Item
        if ($combo.accValue(0) -ne 'Blue corner') { throw "the Scene list shows '$($combo.accValue(0))' after renaming" }
        Press $ui 'Start Recording'
        Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
        Wait-Recorded $ui 2
        $sceneList = (Find $ui 'Scene' $ComboBox).Handle
        [AppWindow]::SendMessage($sceneList, 0x014E, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # CB_SETCURSEL: the first scene
        [AppWindow]::SendMessage($ui.Window, 0x0111, [IntPtr]((1 -shl 16) -bor [AppWindow]::GetDlgCtrlID($sceneList)), $sceneList) | Out-Null
        Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-scene-switched.png') @(0.6, 0.6, 0.3, 0.3) 'blue' -Not
        Wait-Recorded $ui 5
        Press $ui 'Stop Recording'
        Wait-Status $ui 'Ready to record' | Out-Null
    } finally {
        Stop-Process -Id $blue.Id -Force -ErrorAction SilentlyContinue
    }
    $movie = Newest-Movie
    Copy-Item $movie (Join-Path $Artifacts 'app-scenes.mp4')
    $quarter = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-scenes-quarter.probe') @('--sample', '1100,650,600,300')
    $last = 0
    while ($quarter.ContainsKey("sample_$($last + 1)")) { $last++ }
    if ($last -lt 4) { throw "the scenes recording is too short: $($quarter['duration']) s" }
    Expect-Color $quarter 'sample_1' 'blue'
    Expect-Color $quarter "sample_$last" 'white'
    # Remove the layer, so the later steps capture only the test pattern.
    $sources = (Find $ui 'Sources' 33).Handle  # A list
    [AppWindow]::SendMessage($sources, 0x0186, [IntPtr]1, [IntPtr]::Zero) | Out-Null  # LB_SETCURSEL: Display 1
    [AppWindow]::SendMessage($ui.Window, 0x0111, [IntPtr]((1 -shl 16) -bor [AppWindow]::GetDlgCtrlID($sources)), $sources) | Out-Null
    Press $ui 'Remove'
    Start-Sleep -Milliseconds 300
    if ([int][AppWindow]::SendMessage($sources, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero) -ne 2) { throw 'the display layer was not removed' }  # LB_GETCOUNT
    Write-Host "6c. scenes: $($quarter['frames']) frames; a display layer placed by dragging in a new scene, switched away while recording"
    # 7. Volume and mute: system audio at -6 dB from the slider, muted with its box 2.5 s in,
    # unmuted from the notification-area menu 4.5 s in. The mix loses the beeps at 3 and 4 s.
    Set-Check $ui 'Microphone' $true
    $slider = (Find $ui 'System audio volume' $Slider).Handle
    [AppWindow]::SendMessage($slider, 0x0405, [IntPtr]1, [IntPtr](-6)) | Out-Null  # TBM_SETPOS
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]8, $slider) | Out-Null  # WM_HSCROLL, TB_ENDTRACK
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Start-Sleep -Milliseconds 2500
    Set-Check $ui 'Mute system' $true
    Start-Sleep -Milliseconds 2000
    [AppWindow]::PostMessage($ui.Window, 0x0111, [IntPtr]1006, [IntPtr]::Zero) | Out-Null  # WM_COMMAND, tray "Mute System Audio"
    Start-Sleep -Milliseconds 2000
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    $movie = Newest-Movie
    Copy-Item $movie (Join-Path $Artifacts 'app-mute.mp4')
    $muted = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-mute.probe')
    $beeps = @($muted['beeps'].Split(',') | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) })
    if (@($beeps | Where-Object { $_ -lt 2.5 }).Count -lt 2) { throw "beeps before the mute are missing: $($muted['beeps'])" }
    if (@($beeps | Where-Object { $_ -gt 2.7 -and $_ -lt 4.5 }).Count -ne 0) { throw "beeps while muted: $($muted['beeps'])" }
    if (@($beeps | Where-Object { $_ -gt 4.7 }).Count -lt 1) { throw "no beeps after unmuting: $($muted['beeps'])" }
    Write-Host "7. volume and mute: beeps at $($muted['beeps'])"
    # Click marks follow the box into the settings (the engine's marks are checked by the smoke run).
    Set-Check $ui 'Show clicks' $true
    # 8. Closing when idle quits and saves settings, without the key.
    [AppWindow]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $ui.Process.WaitForExit(15000)) { throw 'the app did not quit' }
    $settings = Get-Content -Raw (Join-Path $appHome 'settings.conf')
    Copy-Item (Join-Path $appHome 'settings.conf') (Join-Path $Artifacts 'app-settings.conf')
    Expect-Text $settings "server=$server"
    Expect-Text $settings 'destination=custom'
    Expect-Text $settings 'stream_kbps=3000'
    Expect-Text $settings 'fps=30'
    Expect-Text $settings 'last_recording='
    Expect-Text $settings 'replay=0'
    Expect-Text $settings 'camera=1'
    Expect-Text $settings 'camera_device=test'
    Expect-Text $settings 'scene=0'
    Expect-Text $settings 'scene.1.name=Blue corner'
    Expect-Text $settings 'scene.0.camera=1,'
    if ($settings.Contains('layer.0.kind')) { throw 'the removed display layer is still in the settings' }
    Expect-Text $settings 'image=0'
    Expect-Text $settings 'clicks=1'
    Expect-Text $settings 'preview=1'
    Expect-Text $settings 'system_volume=-6'
    Expect-Text $settings 'system_muted=0'
    if ($settings.Contains('appkey')) { throw 'the stream key was written to the settings file' }

    # 9. Quit from the notification-area menu while recording: the app saves the movie, then exits.
    $before = @(Get-ChildItem $folder -Filter '*.mp4').Count
    $ui = Start-App
    Wait-Status $ui 'Ready to record' | Out-Null
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Start-Sleep -Seconds 2
    [AppWindow]::PostMessage($ui.Window, 0x0111, [IntPtr]1004, [IntPtr]::Zero) | Out-Null  # WM_COMMAND, Quit Rec
    if (-not $ui.Process.WaitForExit(15000)) { throw 'Quit Rec did not end the app while recording' }
    if (@(Get-ChildItem $folder -Filter '*.mp4').Count -ne $before + 1) { throw 'Quit Rec did not save the recording' }
    $quit = Get-Probe $Rec (Newest-Movie) (Join-Path $Artifacts 'app-quit.probe')
    Expect-Within $quit 'duration' 1.5 3.5
    Expect-Within $quit 'av_offset_max_ms' 0 20
    Write-Host "9. quit while recording: $($quit['frames']) frames"

    # 10. Log off while recording: the movie is complete when WM_ENDSESSION returns, after which
    # Windows may end the process at any moment.
    $ui = Start-App
    Wait-Status $ui 'Ready to record' | Out-Null
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Start-Sleep -Seconds 2
    $logoff = [IntPtr]([int64]0x80000000)  # ENDSESSION_LOGOFF
    if ([AppWindow]::SendMessage($ui.Window, 0x0011, [IntPtr]::Zero, $logoff) -eq [IntPtr]::Zero) { throw 'the app refused to log off' }  # WM_QUERYENDSESSION
    [AppWindow]::SendMessage($ui.Window, 0x0016, [IntPtr]1, $logoff) | Out-Null  # WM_ENDSESSION
    $ui.Process.Kill()
    $ui.Process.WaitForExit()
    if (@(Get-ChildItem $folder -Filter '*.mp4').Count -ne $before + 2) { throw 'log-off did not save the recording' }
    $ended = Get-Probe $Rec (Newest-Movie) (Join-Path $Artifacts 'app-logoff.probe')
    Expect-Within $ended 'duration' 1.5 3.5
    Expect-Within $ended 'av_offset_max_ms' 0 20
    Write-Host "10. log off while recording: $($ended['frames']) frames"
    Write-Host 'app e2e passed'
} finally {
    if ($ui -and -not $ui.Process.HasExited) { $ui.Process.Kill() }
    Stop-MediaMTX $mediamtx
    cmdkey "/delete:rec/stream/$server" 2>&1 | Out-Null
}
