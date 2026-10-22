# End-to-end check of the Windows app, driven through Microsoft Active Accessibility, the
# interface screen readers use: every control is found by its accessible name and role and
# operated with its default action. (The managed UI Automation client sees bare panes for
# Win32 controls on some hosts; MSAA reads the same proxies directly.) The app records the
# synthetic flash/beep source (REC_APP_TEST_PATTERN=1) in a temporary home (REC_APP_HOME),
# so it needs no screen, speakers or saved settings:
#   1. Record with system audio and microphone, pause, resume, stop: the movie keeps sync.
#   2. Start and stop with the global hotkey Ctrl+Alt+R.
#   3. Go live to two paths of a local MediaMTX server at once while recording; both copies decode.
#      The second gets the vertical video (1080x1920), which is also saved beside the movie.
#      Get Key on a TikTok row fills its server and key from the clipboard; a new server takes
#      the key with it and the old one is forgotten.
#   4. Closing the window while recording keeps recording; the tray menu stops and saves it.
#   5. With the replay buffer on, Save Replay writes the last seconds while recording goes on.
#   6. A camera and an image are drawn; the scene moves the camera while recording. The
#      preview shows the same picture, before Start too, and stops when Show preview is off.
#      Dragging the camera in the preview moves and resizes it, before Start and while recording.
#      The camera's shape, border and shadow show before Start and change while recording.
#      A new scene with a display layer, placed by dragging, is switched away from while recording.
#      A phone camera (a headless browser with a synthetic camera) opens the address in the
#      phone code window (the relay's page), shows in the preview, and stays in a recording
#      started from there.
#   7. Volume and mute change the mix while recording, from the window and the menu.
#   8. Settings persist without the stream key, which Credential Manager holds.
#   9. Quit Rec from the notification-area menu while recording saves the movie, then exits.
#      With Keep Recording After Quit, the background engine records on; the next launch
#      takes the recording over, preview included, and stops it.
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
  [StructLayout(LayoutKind.Sequential)] public struct SIZE { public int cx, cy; }
  [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
    public int cbSize, flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret;
  }
  [DllImport("oleacc.dll")] static extern int AccessibleObjectFromWindow(IntPtr h, uint id, ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object o);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", EntryPoint="GetWindowLongW")] public static extern int GetWindowLong(IntPtr h, int index);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetFocus();
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO info);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr update, IntPtr erase, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint param, ref HIGHCONTRAST data, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct HIGHCONTRAST { public int cbSize, dwFlags; public IntPtr lpszDefaultScheme; }
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
  [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] static extern bool GetTextExtentPoint32(IntPtr dc, string text, int length, out SIZE size);
  [DllImport("user32.dll", EntryPoint="GetClassLongPtrW")] public static extern UIntPtr GetClassLongPtr(IntPtr h, int index);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder name, int count);
  [DllImport("user32.dll")] public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref RECT r, uint count);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern uint GetDoubleClickTime();
  public static Accessibility.IAccessible Of(IntPtr h) {
    Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71"); object o;
    Marshal.ThrowExceptionForHR(AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref iid, out o));
    return (Accessibility.IAccessible)o;
  }
  public static void SetValue(IntPtr h, string value) { Of(h).set_accValue(0, value); }
  public static int TextWidth(IntPtr h, string text) {
    IntPtr dc = GetDC(h); if (dc == IntPtr.Zero) throw new Exception("GetDC failed for status detail");
    IntPtr font = SendMessage(h, 0x0031, IntPtr.Zero, IntPtr.Zero); // WM_GETFONT
    IntPtr previous = font == IntPtr.Zero ? IntPtr.Zero : SelectObject(dc, font);
    try { SIZE size; if (!GetTextExtentPoint32(dc, text, text.Length, out size)) throw new Exception("GetTextExtentPoint32 failed"); return size.cx; }
    finally { if (previous != IntPtr.Zero) SelectObject(dc, previous); ReleaseDC(h, dc); }
  }
  public static bool TooltipMatches(IntPtr window, IntPtr detail) {
    return SendMessage(window, 0x8003, detail, IntPtr.Zero) != IntPtr.Zero;
  }
}
'@

# Window sizes in pixels, as the window draws itself.
[AppWindow]::SetProcessDPIAware() | Out-Null
# MSAA roles.
$PushButton = 43; $CheckButton = 44; $Text = 42; $ComboBox = 46; $Slider = 51; $Client = 10
$appHome = Join-Path $Artifacts 'app-home'
$folder = Join-Path $appHome 'recordings'
$server = 'rtmp://127.0.0.1:1935/live'
$server2 = 'rtmp://127.0.0.1:1935/live2'
foreach ($url in $server, $server2) { cmdkey "/delete:rec/stream/$url" 2>&1 | Out-Null }
if (Get-Process rec-app -ErrorAction SilentlyContinue) { throw 'quit Rec before running the app E2E' }

# A step that fails with the app still running may be a hang: every thread's stack goes to
# app-stacks.txt when the Windows debugger is installed.
function Save-Stacks($process) {
    if (-not $process -or $process.HasExited) { return }
    $cdb = @("${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe", "$env:ProgramFiles\Windows Kits\10\Debuggers\x64\cdb.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $cdb) { Write-Host 'no stacks: cdb.exe is not installed'; return }
    $out = Join-Path $Artifacts 'app-stacks.txt'
    $symbols = "$(Split-Path $App);srv*$(Join-Path $env:TEMP 'rec-symbols')*https://msdl.microsoft.com/download/symbols"
    $run = Start-Process -FilePath $cdb -NoNewWindow -PassThru -RedirectStandardOutput $out `
        -ArgumentList @('-pv', '-p', $process.Id, '-y', "`"$symbols`"", '-lines', '-c', '"~*kn 40; qd"')
    if (-not $run.WaitForExit(240000)) { $run.Kill(); Write-Host 'cdb did not finish in 4 minutes' }
    Get-Content $out | Write-Host
}

function Start-App {
    $env:REC_APP_HOME = $appHome
    $env:REC_APP_TEST_PATTERN = '1'
    $env:REC_ENGINE_SOCKET = $engineSocket
    $process = Start-Process -FilePath $App -PassThru
    Remove-Item Env:REC_APP_HOME, Env:REC_APP_TEST_PATTERN, Env:REC_ENGINE_SOCKET
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

# The control whose accessible name and role match; control ids run from 100, and the
# destination rows' from 400.
function Find($ui, [string]$name, [int]$role) {
    foreach ($id in @(100..399) + @(400..439)) {
        $handle = [AppWindow]::GetDlgItem($ui.Window, $id)
        if ($handle -eq [IntPtr]::Zero) { continue }
        $item = [AppWindow]::Of($handle)
        if ($item.accName(0) -eq $name -and $item.accRole(0) -eq $role) { return @{ Handle = $handle; Item = $item } }
    }
    throw "no control named '$name' with role $role"
}
function Press($ui, [string]$name) { (Find $ui $name $PushButton).Item.accDoDefaultAction(0) }
# Adds a destination row and waits until the app has named it.
function Add-Destination($ui, [int]$number) {
    Press $ui 'Add Destination'
    $deadline = (Get-Date).AddSeconds(5)
    while ($true) {
        try { Find $ui "Destination $number service" $ComboBox | Out-Null; return } catch {}
        if ((Get-Date) -gt $deadline) { throw "destination $number was not added" }
        Start-Sleep -Milliseconds 50
    }
}
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
function Choose-Item($ui, [string]$name, [int]$index) {
    $combo = (Find $ui $name $ComboBox).Handle
    [AppWindow]::SendMessage($combo, 0x014E, [IntPtr]$index, [IntPtr]::Zero) | Out-Null  # CB_SETCURSEL
    $id = [AppWindow]::GetDlgCtrlID($combo)
    [AppWindow]::SendMessage($ui.Window, 0x0111, [IntPtr]((1 -shl 16) -bor $id), $combo) | Out-Null  # WM_COMMAND, CBN_SELCHANGE
}
# cmdkey echoes the name it was asked for even when nothing is stored; only a Target line counts.
function Has-Key([string]$url) {
    return (cmdkey "/list:rec/stream/$url" | Out-String) -match "(?m)Target: (LegacyGeneric:target=)?$([regex]::Escape("rec/stream/$url"))\s*$"
}
function Wait-Until([scriptblock]$done, [string]$failure, [int]$seconds = 5) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while (-not (& $done)) {
        if ((Get-Date) -gt $deadline) { throw $failure }
        Start-Sleep -Milliseconds 100
    }
}
# The press arrives as a posted message; text copied before the app handles it is not seen.
function Get-Key($ui, [string]$name) {
    Press $ui "Get the $name stream key"
    Wait-Until { (Text-Of $ui 101) -match '^Copy the ' } "Get Key did not ask for the server and key: $(Text-Of $ui 101)"
}
# Copies text the way a browser does; the app sees the clipboard change.
# The app, a clipboard manager or the CI runner's session may hold the clipboard open for a while.
function Copy-To-Rec([string]$text) {
    for ($try = 1; ; $try++) {
        try { Set-Clipboard -Value $text; break }
        catch { if ($try -ge 50) { throw } Start-Sleep -Milliseconds 100 }
    }
    Start-Sleep -Milliseconds 300
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
    $start = Get-Date
    $deadline = $start.AddSeconds($timeout)
    $report = $start.AddSeconds(5)
    while ((Get-Date) -lt $deadline) {
        if ((Text-Of $ui 101) -match '(\d+):(\d\d) recorded' -and [int]$Matches[1] * 60 + [int]$Matches[2] -ge $seconds) { return }
        # A slow wait shows whether the engine still writes, drops, or stopped.
        if ((Get-Date) -gt $report) {
            Write-Host "  waiting for $seconds s, $([int]((Get-Date) - $start).TotalSeconds) s in: $(Text-Of $ui 100); $(Text-Of $ui 101)"
            $report = $report.AddSeconds(5)
        }
        Start-Sleep -Milliseconds 100
    }
    throw "the recording did not reach $seconds s: '$(Text-Of $ui 100)' ($(Text-Of $ui 101))"
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
# Captures only the test app's own window with PrintWindow.
function Snapshot-Window($ui, [string]$path) {
    $client = New-Object AppWindow+RECT
    [AppWindow]::GetClientRect($ui.Window, [ref]$client) | Out-Null
    if ($client.Right -lt 1 -or $client.Bottom -lt 1) {
        throw "test app client area is invalid for a screenshot: $($client.Right)x$($client.Bottom)"
    }
    $bitmap = New-Object System.Drawing.Bitmap $client.Right, $client.Bottom
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    if (-not [AppWindow]::PrintWindow($ui.Window, $dc, 3)) { throw 'PrintWindow failed for the test app window' }
    $graphics.ReleaseHdc($dc)
    $graphics.Dispose()
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    return $bitmap
}
function High-Contrast {
    $hc = New-Object AppWindow+HIGHCONTRAST
    $hc.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($hc)
    return [AppWindow]::SystemParametersInfo(0x0042, [uint32]$hc.cbSize, [ref]$hc, 0) -and (($hc.dwFlags -band 1) -ne 0)
}
function Expect-Strip($ui, $bitmap, [int]$base, [int]$surface) {
    $panel = Control-Rect $ui ($base + 11)
    $allControls = @(100..399 | ForEach-Object {
        $handle = [AppWindow]::GetDlgItem($ui.Window, $_)
        if ($handle -ne [IntPtr]::Zero -and $_ -ne ($base + 11)) { Control-Rect $ui $_ }
    })
    # On a small screen (CI runners) the third column can extend past the client area, which is
    # all the snapshot holds; probe only the part of each rect inside the bitmap.
    $left = [Math]::Max(0, $panel.Left); $right = [Math]::Min($bitmap.Width, $panel.Right)
    $top = [Math]::Max(0, $panel.Top); $bottom = [Math]::Min($bitmap.Height, $panel.Bottom)
    if ($right - $left -lt 16 -or $bottom - $top -lt 16) {
        Write-Host "strip panel $base is outside the $($bitmap.Width)x$($bitmap.Height) client snapshot; color probe skipped"
        return
    }
    $sample = $null
    for ($y = $top + 4; $y -lt $bottom - 4 -and -not $sample; $y++) {
        for ($x = $left + 4; $x -lt $right - 4; $x++) {
            $covered = $false
            foreach ($rect in $allControls) {
                if ($x -ge $rect.Left -and $x -lt $rect.Right -and $y -ge $rect.Top -and $y -lt $rect.Bottom) {
                    $covered = $true; break
                }
            }
            if (-not $covered) { $sample = $bitmap.GetPixel($x, $y); break }
        }
    }
    if (-not $sample) { throw "could not find an uncovered point inside strip panel $base" }
    $r = [int]$sample.R; $g = [int]$sample.G; $b = [int]$sample.B
    $brightness = [int](($r + $g + $b) / 3)
    if ($brightness -le $surface -or $brightness -ge 70) {
        throw "strip panel $base RGB $r,$g,$b is not lighter than the surface and still dark"
    }
    $probed = 0
    foreach ($id in @(($base + 4), ($base + 5), ($base + 6), ($base + 10))) {
        $handle = [AppWindow]::GetDlgItem($ui.Window, $id)
        if ($handle -eq [IntPtr]::Zero -or -not [AppWindow]::IsWindowVisible($handle)) {
            throw "strip control $id is missing or not visible"
        }
        $rect = Control-Rect $ui $id
        $controlLeft = [Math]::Max($left, $rect.Left); $controlRight = [Math]::Min($right, $rect.Right)
        $controlTop = [Math]::Max($top, $rect.Top); $controlBottom = [Math]::Min($bottom, $rect.Bottom)
        if (($controlRight - $controlLeft) * ($controlBottom - $controlTop) -lt 40) { continue }
        $probed++
        $coverage = 0
        for ($y = $controlTop; $y -lt $controlBottom; $y++) {
            for ($x = $controlLeft; $x -lt $controlRight; $x++) {
                $pixel = $bitmap.GetPixel($x, $y)
                $difference = [Math]::Abs([int]$pixel.R - $r) +
                              [Math]::Abs([int]$pixel.G - $g) +
                              [Math]::Abs([int]$pixel.B - $b)
                if ($difference -gt 60) { $coverage++ }
            }
        }
        if ($coverage -lt 20) { throw "strip control $id is covered by panel $base (only $coverage differing pixels)" }
    }
    if ($probed -eq 0) { throw "no strip $base control is inside the client snapshot" }
    $label = Control-Rect $ui ($base + 1)
    $labelText = $false
    for ($y = [Math]::Max(0, $label.Top); $y -lt [Math]::Min($bitmap.Height, $label.Bottom) -and -not $labelText; $y++) {
        for ($x = [Math]::Max(0, $label.Left); $x -lt [Math]::Min($bitmap.Width, $label.Right); $x++) {
            $pixel = $bitmap.GetPixel($x, $y)
            if ([Math]::Max([int]$pixel.R, [Math]::Max([int]$pixel.G, [int]$pixel.B)) -gt 150) {
                $labelText = $true; break
            }
        }
    }
    if (-not $labelText) { throw "strip label $($base + 1) has no light text pixels" }
}
function Expect-CustomTrackbar($ui, [int]$id, [string]$path) {
    $bitmap = Snapshot-Window $ui $path
    try {
        $rect = Control-Rect $ui $id
        $red = 0; $channel = 0; $thumb = 0
        for ($y = [Math]::Max(0, $rect.Top); $y -lt [Math]::Min($bitmap.Height, $rect.Bottom); $y++) {
            for ($x = [Math]::Max(0, $rect.Left); $x -lt [Math]::Min($bitmap.Width, $rect.Right); $x++) {
                $pixel = $bitmap.GetPixel($x, $y)
                if ($pixel.R -gt 150 -and $pixel.R -gt ($pixel.G * 1.7) -and $pixel.R -gt ($pixel.B * 1.7)) { $red++ }
                if ($pixel.R -ge 55 -and $pixel.R -le 90 -and
                    $pixel.G -ge 55 -and $pixel.G -le 90 -and
                    $pixel.B -ge 60 -and $pixel.B -le 96) { $channel++ }
                if ($pixel.R -ge 195 -and $pixel.G -ge 195 -and $pixel.B -ge 195 -and
                    ($pixel.B - $pixel.R) -le 12) { $thumb++ }
            }
        }
        if ($red -ne 0) { throw "custom trackbar $id has $red red pixels; red is reserved for recording/live" }
        if ($channel -lt 6) { throw "custom trackbar $id has only $channel channel pixels in its snapshot" }
        if ($thumb -lt 6) { throw "custom trackbar $id has only $thumb light-thumb pixels in its snapshot" }
        Write-Host "D3. custom trackbar ${id}: 0 red, $thumb thumb and $channel channel pixels"
    } finally { $bitmap.Dispose() }
}
function Expect-Dark($ui) {
    $path = Join-Path $Artifacts 'app-surface.png'
    $bitmap = Snapshot-Window $ui $path
    try {
        if (High-Contrast) {
            Write-Host 'D1. dark surface assertions skipped: system high-contrast mode is on; screenshot saved'
            return
        }
        $r = 0; $g = 0; $b = 0
        for ($y = 0; $y -lt 10; $y++) {
            for ($x = 0; $x -lt 10; $x++) {
                $pixel = $bitmap.GetPixel($x, $y)
                $r += $pixel.R; $g += $pixel.G; $b += $pixel.B
            }
        }
        $r = [int]($r / 100); $g = [int]($g / 100); $b = [int]($b / 100)
        if ($r -ge 45 -or $g -ge 45 -or $b -ge 45) {
            throw "app surface margin is not dark: mean RGB $r,$g,$b"
        }
        $script:appSurfaceMean = [int](($r + $g + $b) / 3)
        Expect-Strip $ui $bitmap 300 $script:appSurfaceMean
        $microphone = (Find $ui 'Microphone' $CheckButton).Handle
        $micRect = Control-Rect $ui ([AppWindow]::GetDlgCtrlID($microphone))
        $micText = $false
        for ($y = [Math]::Max(0, $micRect.Top); $y -lt [Math]::Min($bitmap.Height, $micRect.Bottom) -and -not $micText; $y++) {
            for ($x = [Math]::Max(0, $micRect.Left); $x -lt [Math]::Min($bitmap.Width, $micRect.Right); $x++) {
                $pixel = $bitmap.GetPixel($x, $y)
                if ([Math]::Max([int]$pixel.R, [Math]::Max([int]$pixel.G, [int]$pixel.B)) -gt 150) {
                    $micText = $true; break
                }
            }
        }
        if (-not $micText) { throw 'Microphone check-box text has no light pixels in its control rect' }
        Write-Host "D1. dark surface: margin mean $r,$g,$b; strip probes passed; saved $path"
    } finally { $bitmap.Dispose() }
}
function Set-TestDpi($ui, [int]$dpi, $baseRect, [int]$baseDpi) {
    $rect = New-Object AppWindow+RECT
    $scale = $dpi / [double]$baseDpi
    $rect.Left = $baseRect.Left
    $rect.Top = $baseRect.Top
    $rect.Right = $rect.Left + [int](($baseRect.Right - $baseRect.Left) * $scale)
    $rect.Bottom = $rect.Top + [int](($baseRect.Bottom - $baseRect.Top) * $scale)
    $memory = [Runtime.InteropServices.Marshal]::AllocHGlobal([Runtime.InteropServices.Marshal]::SizeOf($rect))
    try {
        [Runtime.InteropServices.Marshal]::StructureToPtr($rect, $memory, $false)
        $packed = [IntPtr]([long](($dpi -shl 16) -bor $dpi))
        [AppWindow]::SendMessage($ui.Window, 0x02E0, $packed, $memory) | Out-Null  # WM_DPICHANGED, test-owned app only
        [AppWindow]::RedrawWindow($ui.Window, [IntPtr]::Zero, [IntPtr]::Zero, 0x181) | Out-Null  # all children, now
    } finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($memory) }
}
function Snapshot-StripDpi($ui, [string]$directory, [string]$prefix) {
    $originalDpi = [int][AppWindow]::GetDpiForWindow($ui.Window)
    $baseRect = New-Object AppWindow+RECT
    if (-not [AppWindow]::GetWindowRect($ui.Window, [ref]$baseRect)) { throw 'GetWindowRect failed before strip DPI screenshots' }
    $camera = $prefix -eq 'app-strips-camera'
    # These ids are explicit in main.cpp's reserved strip blocks, independent of enum ordering.
    $stripIds = if ($camera) { @(320..331) } else { @(300..311) }
    $frameId = if ($camera) { 331 } else { 311 }
    $visible = @{}
    try {
        foreach ($id in 100..399) {
            $handle = [AppWindow]::GetDlgItem($ui.Window, $id)
            if ($handle -eq [IntPtr]::Zero) { continue }
            $visible[$id] = [AppWindow]::IsWindowVisible($handle)
            $keep = $id -in $stripIds
            if (-not $keep) { [AppWindow]::ShowWindow($handle, 0) | Out-Null }
        }
        foreach ($dpi in @(96, 144, 192)) {
            if ($dpi -ne $originalDpi) { Set-TestDpi $ui $dpi $baseRect $originalDpi }
            Assert-StripGeometry $ui ($prefix -eq 'app-strips-camera')
            $path = Join-Path $directory "$prefix-dpi-$([int]($dpi * 100 / 96)).png"
            $bitmap = Snapshot-Window $ui $path
            try {
                $frameHandle = [AppWindow]::GetDlgItem($ui.Window, $frameId)
                if ($frameHandle -eq [IntPtr]::Zero) { throw "strip frame control $frameId was not found" }
                $frame = New-Object AppWindow+RECT
                if (-not [AppWindow]::GetClientRect($frameHandle, [ref]$frame)) { throw "GetClientRect failed for strip frame $frameId" }
                [AppWindow]::MapWindowPoints($frameHandle, $ui.Window, [ref]$frame, 2) | Out-Null
                $left = [Math]::Max(0, $frame.Left - 8)
                $top = [Math]::Max(0, $frame.Top - 8)
                $right = [Math]::Min($bitmap.Width, $frame.Right + 8)
                $bottom = [Math]::Min($bitmap.Height, $frame.Bottom + 8)
                $frameInside = $frame.Left -ge 0 -and $frame.Top -ge 0 -and
                    $frame.Right -le $bitmap.Width -and $frame.Bottom -le $bitmap.Height
                if (-not $frameInside -or $right -le $left -or $bottom -le $top) {
                    Write-Host "skipping DPI $dpi strip crop: frame outside the captured window (bitmap $($bitmap.Width)x$($bitmap.Height), frame $($frame.Left),$($frame.Top),$($frame.Right),$($frame.Bottom))"
                    continue
                }
                $cropRect = New-Object System.Drawing.Rectangle $left, $top, ($right - $left), ($bottom - $top)
                $crop = $bitmap.Clone($cropRect, $bitmap.PixelFormat)
                try {
                    $crop.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
                } finally {
                    $crop.Dispose()
                }
            } finally {
                $bitmap.Dispose()
            }
        }
    } finally {
        Set-TestDpi $ui $originalDpi $baseRect $originalDpi
        foreach ($id in $visible.Keys) {
            $handle = [AppWindow]::GetDlgItem($ui.Window, [int]$id)
            if ($handle -ne [IntPtr]::Zero) {
                [AppWindow]::ShowWindow($handle, $(if ($visible[$id]) { 5 } else { 0 })) | Out-Null
            }
        }
        [AppWindow]::RedrawWindow($ui.Window, [IntPtr]::Zero, [IntPtr]::Zero, 0x181) | Out-Null
    }
}
function Show-TrackbarClassStyle($ui) {
    $trackbarHandle = (Find $ui 'Screen brightness' $Slider).Handle
    $classStyle = [uint64][AppWindow]::GetClassLongPtr($trackbarHandle, -26).ToUInt64()
    $className = New-Object Text.StringBuilder 128
    [AppWindow]::GetClassName($trackbarHandle, $className, $className.Capacity) | Out-Null
    if ($className.ToString() -ne 'msctls_trackbar32') { throw "expected msctls_trackbar32, got '$($className.ToString())'" }
    Write-Host "msctls_trackbar32 CS_DBLCLKS=$([bool]($classStyle -band 0x8)) (GCL_STYLE=0x$('{0:X}' -f $classStyle))"
}
function Assert-StripGeometry($ui, [bool]$camera) {
    $label = if ($camera) { 'Camera' } else { 'Screen' }
    $base = if ($camera) { 320 } else { 300 }
    $panelRect = New-Object AppWindow+RECT
    [AppWindow]::GetWindowRect([AppWindow]::GetDlgItem($ui.Window, $base + 11), [ref]$panelRect) | Out-Null
    $sliderRects = @()
    foreach ($name in @('brightness', 'contrast', 'saturation')) {
        $handle = (Find $ui "$label $name" $Slider).Handle
        $rect = New-Object AppWindow+RECT
        [AppWindow]::GetWindowRect($handle, [ref]$rect) | Out-Null
        $sliderRects += $rect
    }
    foreach ($rect in $sliderRects) {
        if ($rect.Left -le ($panelRect.Left + 2) -or $rect.Right -ge ($panelRect.Right - 2) -or
            $rect.Right -ne $sliderRects[0].Right) {
            throw "$label color sliders do not share the padded start and width"
        }
    }
    $valueId = $base + 7
    $valueRect = New-Object AppWindow+RECT
    [AppWindow]::GetWindowRect([AppWindow]::GetDlgItem($ui.Window, $valueId), [ref]$valueRect) | Out-Null
    if ($valueRect.Left -le $sliderRects[0].Right -or $valueRect.Right -ge ($panelRect.Right - 2)) {
        throw "$label value readouts are not beside the slider at the padded panel edge"
    }
    if ($camera) {
        $otherValue = New-Object AppWindow+RECT
        [AppWindow]::GetWindowRect([AppWindow]::GetDlgItem($ui.Window, 328), [ref]$otherValue) | Out-Null
        if (($valueRect.Right - $valueRect.Left) -ne ($otherValue.Right - $otherValue.Left) -or
            $valueRect.Left -ne $otherValue.Left) {
            throw 'Camera value labels do not share a fixed-width value column'
        }
    }
    $audioValueRect = New-Object AppWindow+RECT
    [AppWindow]::GetWindowRect([AppWindow]::GetDlgItem($ui.Window, 106), [ref]$audioValueRect) | Out-Null
    $micValueRect = Rect-OfId $ui 111 # kMicVolumeText
    if ($audioValueRect.Left -ne $micValueRect.Left -or
        ($audioValueRect.Right - $audioValueRect.Left) -ne ($micValueRect.Right - $micValueRect.Left)) {
        throw 'System and microphone dB readouts do not share a right-aligned column'
    }
    $reset = Rect-Of $ui "Reset $($label.ToLower()) color" $PushButton
    $header = Rect-Of $ui $label 41
    if ([Math]::Abs($header.Top - $reset.Top) -gt 4) { throw "$label Reset is not on the header row" }
}
function Rect-OfId($ui, [int]$id) {
    $r = New-Object AppWindow+RECT
    $handle = [AppWindow]::GetDlgItem($ui.Window, $id)
    if ($handle -eq [IntPtr]::Zero -or -not [AppWindow]::GetWindowRect($handle, [ref]$r)) { throw "GetWindowRect failed for control id $id" }
    return $r
}
function Assert-StatusRow($ui, [int]$dpi) {
    $status = Rect-OfId $ui 100
    $detailRect = Rect-OfId $ui 101
    $live = Rect-OfId $ui 102
    $centers = @(
        [int](($status.Top + $status.Bottom) / 2),
        [int](($detailRect.Top + $detailRect.Bottom) / 2),
        [int](($live.Top + $live.Bottom) / 2)
    )
    if ((($centers | Measure-Object -Maximum).Maximum - ($centers | Measure-Object -Minimum).Minimum) -gt 3) {
        throw "status controls do not share one row: centers=$($centers -join ',')"
    }
    if ($status.Right -gt $detailRect.Left -or $detailRect.Right -gt $live.Left) {
        throw "status controls overlap or are out of order: status=$($status.Left)-$($status.Right), detail=$($detailRect.Left)-$($detailRect.Right), live=$($live.Left)-$($live.Right)"
    }
    if ($detailRect.Right -gt $live.Left) { throw 'status detail overlaps the LIVE slot' }
    $minimumWidth = [int](290 * $dpi / 96)
    if (($detailRect.Right - $detailRect.Left) -lt $minimumWidth) {
        throw "idle status detail is narrower than $minimumWidth pixels at $dpi DPI: status=$($status.Right - $status.Left), detail=$($detailRect.Right - $detailRect.Left), live=$($live.Right - $live.Left)"
    }
}
function Assert-StatusTooltip($ui) {
    $handle = [AppWindow]::GetDlgItem($ui.Window, 101)
    $fullDetail = Text-Of $ui 101
    $detailRect = Rect-OfId $ui 101
    $detailWidth = $detailRect.Right - $detailRect.Left
    $textWidth = [AppWindow]::TextWidth($handle, $fullDetail)
    if ($textWidth -le $detailWidth) { throw "the recording detail did not truncate: text=$textWidth, slot=$detailWidth" }
    if (-not [AppWindow]::TooltipMatches($ui.Window, $handle)) { throw "the tooltip did not contain the full status detail '$fullDetail'" }
    if ($detailRect.Right -gt (Rect-OfId $ui 102).Left) { throw 'truncated status detail overlaps the LIVE slot' }
}
function Assert-StatusDpi($ui) {
    $originalDpi = [int][AppWindow]::GetDpiForWindow($ui.Window)
    $baseRect = New-Object AppWindow+RECT
    if (-not [AppWindow]::GetWindowRect($ui.Window, [ref]$baseRect)) { throw 'GetWindowRect failed before status DPI checks' }
    try {
        foreach ($dpi in @(96, 144, 192)) {
            if ($dpi -ne $originalDpi) { Set-TestDpi $ui $dpi $baseRect $originalDpi }
            Assert-StatusRow $ui $dpi
        }
    } finally {
        Set-TestDpi $ui $originalDpi $baseRect $originalDpi
    }
}
function Assert-StripVisibility($ui, [int[]]$ids, [bool]$visible, [string]$name) {
    foreach ($id in $ids) {
        $handle = [AppWindow]::GetDlgItem($ui.Window, $id)
        if ($handle -eq [IntPtr]::Zero) { throw "$name strip control $id was not created" }
        if ([AppWindow]::IsWindowVisible($handle) -ne $visible) {
            throw "$name strip control $id visibility did not match expected visible=$visible"
        }
    }
}
function Rect-Of($ui, [string]$name, [int]$role) {
    $r = New-Object AppWindow+RECT
    if (-not [AppWindow]::GetWindowRect((Find $ui $name $role).Handle, [ref]$r)) { throw "GetWindowRect failed for $name" }
    return $r
}
function Focused-Control($ui) {
    [uint32]$ownerProcessId = 0
    $thread = [AppWindow]::GetWindowThreadProcessId($ui.Window, [ref]$ownerProcessId)
    $info = New-Object AppWindow+GUITHREADINFO
    $info.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($info)
    if (-not [AppWindow]::GetGUIThreadInfo($thread, [ref]$info)) { throw 'GetGUIThreadInfo failed' }
    return $info.hwndFocus
}
function Control-Rect($ui, [int]$id) {
    $handle = [AppWindow]::GetDlgItem($ui.Window, $id)
    $r = New-Object AppWindow+RECT
    [AppWindow]::GetClientRect($handle, [ref]$r) | Out-Null
    [AppWindow]::MapWindowPoints($handle, $ui.Window, [ref]$r, 2) | Out-Null
    return $r
}
function Meter-LitPixels($ui, $bitmap, [int]$id) {
    $rect = Control-Rect $ui $id
    $left = [Math]::Max(0, $rect.Left); $top = [Math]::Max(0, $rect.Top)
    $right = [Math]::Min($bitmap.Width, $rect.Right); $bottom = [Math]::Min($bitmap.Height, $rect.Bottom)
    if ($right -le $left -or $bottom -le $top) { throw "meter control $id has an empty rectangle after clamping to the $($bitmap.Width)x$($bitmap.Height) bitmap" }
    $channel = $bitmap.GetPixel($right - 1, [int](($top + $bottom - 1) / 2))
    $lit = 0
    for ($y = $top; $y -lt $bottom; $y++) {
        for ($x = $left; $x -lt $right; $x++) {
            $pixel = $bitmap.GetPixel($x, $y)
            if ($pixel.ToArgb() -ne $channel.ToArgb()) { $lit++ }
        }
    }
    return $lit
}
function Meter-ColorPixels($ui, $bitmap, [int]$id, [int]$r, [int]$g, [int]$b) {
    $rect = Control-Rect $ui $id
    $left = [Math]::Max(0, $rect.Left); $top = [Math]::Max(0, $rect.Top)
    $right = [Math]::Min($bitmap.Width, $rect.Right); $bottom = [Math]::Min($bitmap.Height, $rect.Bottom)
    if ($right -le $left -or $bottom -le $top) { throw "meter control $id has an empty rectangle after clamping to the $($bitmap.Width)x$($bitmap.Height) bitmap" }
    $count = 0
    for ($y = $top; $y -lt $bottom; $y++) {
        for ($x = $left; $x -lt $right; $x++) {
            $pixel = $bitmap.GetPixel($x, $y)
            if ([Math]::Abs($pixel.R - $r) + [Math]::Abs($pixel.G - $g) + [Math]::Abs($pixel.B - $b) -le 45) { $count++ }
        }
    }
    return $count
}
function Closest-Status-Color($bitmap, $ui, [int]$r, [int]$g, [int]$b) {
    $rect = Control-Rect $ui 100
    $closest = 765
    for ($y = [Math]::Max(0, $rect.Top); $y -lt [Math]::Min($bitmap.Height, $rect.Bottom); $y++) {
        for ($x = [Math]::Max(0, $rect.Left); $x -lt [Math]::Min($bitmap.Width, $rect.Right); $x++) {
            $p = $bitmap.GetPixel($x, $y)
            $distance = [Math]::Abs($p.R - $r) + [Math]::Abs($p.G - $g) + [Math]::Abs($p.B - $b)
            if ($distance -lt $closest) { $closest = $distance }
        }
    }
    return $closest
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
function Wait-PreviewColor($ui, [string]$path, [double[]]$area, [string]$color, [switch]$Not, [int]$seconds = 15, [int]$pollMilliseconds = 200) {
    $deadline = (Get-Date).AddSeconds($seconds)
    for (;;) {
        $picture = Snapshot-Preview $ui $path
        $seen = Picture-Color $picture $area[0] $area[1] $area[2] $area[3]
        $picture.Dispose()
        $same = $true
        try { Expect-Color $seen 'color' $color } catch { $same = $false }
        if ($same -ne [bool]$Not) { return }
        if ((Get-Date) -gt $deadline) { throw "the preview at $($area -join ',') is $($seen['color']); expected $(if ($Not) { 'not ' })$color" }
        Start-Sleep -Milliseconds $pollMilliseconds
    }
}
function Wait-PreviewColors($ui, [string]$path, [double[]]$greenArea, [double[]]$redArea, [int]$seconds = 15) {
    $deadline = (Get-Date).AddSeconds($seconds)
    for (;;) {
        $picture = Snapshot-Preview $ui $path
        $green = Picture-Color $picture $greenArea[0] $greenArea[1] $greenArea[2] $greenArea[3]
        $red = Picture-Color $picture $redArea[0] $redArea[1] $redArea[2] $redArea[3]
        $picture.Dispose()
        $both = $true
        try {
            Expect-Color $green 'color' 'green'
            Expect-Color $red 'color' 'red'
        } catch { $both = $false }
        if ($both) { return }
        if ((Get-Date) -gt $deadline) {
            throw "one preview snapshot did not contain green and red; green=$($green['color']), red=$($red['color'])"
        }
        Start-Sleep -Milliseconds 100
    }
}
function Post-TrackbarDoubleClick([IntPtr]$slider) {
    $rect = New-Object AppWindow+RECT
    if (-not [AppWindow]::GetClientRect($slider, [ref]$rect)) { throw 'could not read trackbar bounds for double-click' }
    $point = [IntPtr](([int]($rect.Bottom / 2) -shl 16) -bor [int]($rect.Right / 2))
    foreach ($message in @(0x0201, 0x0202, 0x0201, 0x0202)) {
        $button = if ($message -eq 0x0201) { [IntPtr]1 } else { [IntPtr]::Zero }
        if (-not [AppWindow]::PostMessage($slider, $message, $button, $point)) {
            throw "could not post trackbar mouse message 0x$('{0:X4}' -f $message)"
        }
    }
}
function Drag-Readout([IntPtr]$readout, [int]$dips, [switch]$NoRelease) {
    $rect = New-Object AppWindow+RECT
    if (-not [AppWindow]::GetClientRect($readout, [ref]$rect)) { throw 'could not read value readout bounds' }
    $x = [int]($rect.Right / 2); $y = [int]($rect.Bottom / 2)
    $at = { param([int]$pointY) [IntPtr](($pointY -shl 16) -bor $x) }
    [AppWindow]::SendMessage($readout, 0x0201, [IntPtr]1, (& $at $y)) | Out-Null
    $pixels = [Math]::Max(1, [int][Math]::Round([AppWindow]::GetDpiForWindow($readout) * [Math]::Abs($dips) / 96))
    $direction = if ($dips -gt 0) { -1 } else { 1 }
    $count = [Math]::Abs($dips)
    for ($i = 1; $i -le $count; ++$i) {
        $nextY = $y + $direction * [int][Math]::Round($pixels * $i / $count)
        [AppWindow]::SendMessage($readout, 0x0200, [IntPtr]1, (& $at $nextY)) | Out-Null
    }
    $script:readoutDragEnd = & $at ($y + $direction * $pixels)
    if (-not $NoRelease) { Release-Readout $readout }
}
function Release-Readout([IntPtr]$readout) {
    [AppWindow]::SendMessage($readout, 0x0202, [IntPtr]::Zero, $script:readoutDragEnd) | Out-Null
}
function Click-Readout([IntPtr]$readout) {
    $rect = New-Object AppWindow+RECT
    [AppWindow]::GetClientRect($readout, [ref]$rect) | Out-Null
    $point = [IntPtr](([int]($rect.Bottom / 2) -shl 16) -bor [int]($rect.Right / 2))
    [AppWindow]::SendMessage($readout, 0x0201, [IntPtr]1, $point) | Out-Null
    [AppWindow]::SendMessage($readout, 0x0202, [IntPtr]::Zero, $point) | Out-Null
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
    $file = Get-ChildItem $folder -Filter '*.mp4' | Where-Object { $_.Name -notlike '*-vertical.mp4' } | Sort-Object LastWriteTime | Select-Object -Last 1
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
[IO.File]::WriteAllText((Join-Path $appHome 'settings.conf'), "fps=30`ncamera_device=test`nimage_path=$overlayImage`nkeep_recording=no`n")
# The app's recordings run in a background engine; this home has its own.
$engineSocket = Join-Path $appHome 'engine.sock'
# A rec command on this home's engine; its output, errors included.
function Engine-Command([string]$command) {
    $ErrorActionPreference = 'Continue'
    $env:REC_ENGINE_SOCKET = $engineSocket
    try { return (& $Rec $command 2>&1 | Out-String) } finally { Remove-Item Env:REC_ENGINE_SOCKET }
}
function Stop-Engine { Engine-Command 'shutdown' | Out-Null }
Stop-Engine
# The phone camera's page comes from the relay: smoke.ps1 runs one, or this runs its own.
$ownRelay = $null
if (-not $env:REC_PHONE_RELAY) {
    $ownRelay = Start-Relay (Join-Path (Split-Path -Parent $PSScriptRoot) 'support\phone-relay') (Join-Path $Artifacts 'app-relay.log')
    $env:REC_PHONE_RELAY = $ownRelay.Url
}
$relayed = $env:REC_PHONE_RELAY -ne 'off'
$mediamtx = Start-MediaMTX (Get-MediaMTX (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\tools')) $received (Join-Path $Artifacts 'app-mediamtx')
$ui = $null
try {
    $ui = Start-App
    Wait-Status $ui 'Ready to record' | Out-Null
    Expect-Dark $ui
    Assert-StatusDpi $ui
    Assert-StatusRow $ui ([int][AppWindow]::GetDpiForWindow($ui.Window))
    $statusRowShot = Snapshot-Window $ui (Join-Path $Artifacts 'app-status-row.png')
    $statusRowShot.Dispose()
    Write-Host "Status row snapshot: $(Join-Path $Artifacts 'app-status-row.png')"
    if ([AppWindow]::IsWindowVisible((Find $ui 'Camera brightness' $Slider).Handle)) { throw 'Camera strip is visible while Camera is off' }
    Assert-StripVisibility $ui @(320..331) $false 'Camera'
    Find $ui 'Screen' 41 | Out-Null
    $screenHeader = Rect-Of $ui 'Screen' 41
    $screenReset = Rect-Of $ui 'Reset screen color' $PushButton
    if ([Math]::Abs($screenHeader.Top - $screenReset.Top) -gt 4) { throw 'Screen Reset is not on the header row' }
    Show-TrackbarClassStyle $ui
    $screenSlider = (Find $ui 'Screen brightness' $Slider).Handle
    $volumeSlider = (Find $ui 'System audio volume' $Slider).Handle
    $screenSliderRect = Rect-Of $ui 'Screen brightness' $Slider
    $volumeSliderRect = Rect-Of $ui 'System audio volume' $Slider
    $screenPanelRect = New-Object AppWindow+RECT
    [AppWindow]::GetWindowRect([AppWindow]::GetDlgItem($ui.Window, 311), [ref]$screenPanelRect) | Out-Null
    $stripInset = [int][Math]::Round(8 * [AppWindow]::GetDpiForWindow($ui.Window) / 96)
    if ($screenSliderRect.Left -ne ($screenPanelRect.Left + $stripInset)) {
        throw 'Screen color slider does not use the strip left inset'
    }
    $screenValueRect = New-Object AppWindow+RECT
    $volumeValueRect = New-Object AppWindow+RECT
    [AppWindow]::GetWindowRect([AppWindow]::GetDlgItem($ui.Window, 307), [ref]$screenValueRect) | Out-Null
    [AppWindow]::GetWindowRect([AppWindow]::GetDlgItem($ui.Window, 106), [ref]$volumeValueRect) | Out-Null  # kSystemVolumeText
    if ($screenValueRect.Left -le $screenSliderRect.Right -or
        [Math]::Abs($screenValueRect.Right - ($screenPanelRect.Right - 2 * $stripInset)) -gt 2) {
        throw 'Screen value labels are not beside the slider at the inset panel edge'
    }
    Snapshot-StripDpi $ui $Artifacts 'app-strips'
    Set-Check $ui 'Camera' $true
    Assert-StripVisibility $ui @(320..331) $true 'Camera'
    $cameraStripBitmap = Snapshot-Window $ui (Join-Path $Artifacts 'app-strips-camera-contrast.png')
    try {
        if (-not (High-Contrast)) { Expect-Strip $ui $cameraStripBitmap 320 $script:appSurfaceMean }
    } finally { $cameraStripBitmap.Dispose() }
    Snapshot-StripDpi $ui $Artifacts 'app-strips-camera'
    Set-Check $ui 'Camera' $false
    Assert-StripVisibility $ui @(320..331) $false 'Camera'
    $cameraOff = Join-Path $Artifacts 'app-strips-camera-off.png'
    Snapshot-Window $ui $cameraOff | Out-Null
    Write-Host "Camera-off strip snapshot: $cameraOff"
    # DPI messages are synthetic here; relaunch the test-owned window at the real desktop DPI
    # before the keyboard, recording and permission checks.
    [AppWindow]::PostMessage($ui.Window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE, idle
    if (-not $ui.Process.WaitForExit(10000)) { throw 'the app did not close after DPI snapshots' }
    $ui = Start-App
    Wait-Status $ui 'Ready to record' | Out-Null

    # 0. Microphone access off for desktop apps: the app names it, offers its settings page,
    # and does not start. The machine's own switch comes back right after.
    $consent = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone\NonPackaged'
    $hadKey = Test-Path $consent
    $consentBefore = (Get-ItemProperty -Path $consent -Name Value -ErrorAction SilentlyContinue).Value
    $offText = 'Microphone access is off for desktop apps.'
    try {
        if (-not $hadKey) { New-Item -Path $consent -Force | Out-Null }
        Set-ItemProperty -Path $consent -Name Value -Value 'Deny'
        Set-Check $ui 'Microphone' $true
        $deadline = (Get-Date).AddSeconds(5)
        while ($true) {
            try { $row = Find $ui $offText 41; break } catch {}
            if ((Get-Date) -gt $deadline) { throw "the app did not say the microphone is off" }
            Start-Sleep -Milliseconds 100
        }
        if (-not [AppWindow]::IsWindowVisible($row.Handle)) { throw 'the missing permission is not shown' }
        $open = Find $ui 'Open privacy settings' $PushButton
        if (-not [AppWindow]::IsWindowVisible($open.Handle)) { throw 'Open privacy settings is not shown' }
        Press $ui 'Start Recording'
        Start-Sleep -Milliseconds 1500
        if (-not (Text-Of $ui 100).StartsWith('Ready to record')) { throw "it started without microphone access: $(Text-Of $ui 100)" }
        Expect-Text (Text-Of $ui 101) "$offText Turn it on to record."
    } finally {
        if ($null -ne $consentBefore) { Set-ItemProperty -Path $consent -Name Value -Value $consentBefore }
        elseif ($hadKey) { Remove-ItemProperty -Path $consent -Name Value -ErrorAction SilentlyContinue }
        else { Remove-Item -Path $consent -ErrorAction SilentlyContinue }
    }
    # Turned back on, the row goes once the app looks again (here: the box changes).
    Set-Check $ui 'Microphone' $false
    Set-Check $ui 'Microphone' $true
    $deadline = (Get-Date).AddSeconds(5)
    while ([AppWindow]::IsWindowVisible($row.Handle)) {
        if ((Get-Date) -gt $deadline) {
            $store = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone'
            $values = foreach ($key in @($store, "$store\NonPackaged", ($store -replace '^HKCU:', 'HKLM:'))) {
                "$key=$((Get-ItemProperty -Path $key -Name Value -ErrorAction SilentlyContinue).Value)"
            }
            throw "the permission row stayed after access came back: '$($row.Item.accName(0))'; $(Text-Of $ui 101); $($values -join '; ')"
        }
        Start-Sleep -Milliseconds 100
    }
    Write-Host '0. permissions: microphone access off was shown and kept the recording from starting'
    # Permission status may be cached by the current process; restart the test-owned app after
    # restoring the user's setting before exercising normal recording.
    [AppWindow]::PostMessage($ui.Window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE, idle
    if (-not $ui.Process.WaitForExit(10000)) { throw 'the app did not close after permission checks' }
    $ui = Start-App
    Wait-Status $ui 'Ready to record' | Out-Null
    try {
        $permissionWarning = Find $ui $offText 41
        if ([AppWindow]::IsWindowVisible($permissionWarning.Handle)) { throw 'permission warning remained after relaunch' }
    } catch {
        if ($_.Exception.Message -eq 'permission warning remained after relaunch') { throw }
    }
    # 1. Record, pause, resume, stop: 2 + 2 s recorded around a 1.5 s pause.
    Set-Check $ui 'System audio' $true
    Set-Check $ui 'Microphone' $true
    $micFader = (Find $ui 'Microphone volume' $Slider).Handle
    [AppWindow]::SetForegroundWindow($ui.Window) | Out-Null
    Wait-Until { [AppWindow]::GetForegroundWindow() -eq $ui.Window } 'refusing to send Tab because the test app window is not foreground' 5
    [uint32]$appProcessId = 0
    [uint32]$mainThread = [AppWindow]::GetWindowThreadProcessId($ui.Window, [ref]$appProcessId)
    [uint32]$testThread = [AppWindow]::GetCurrentThreadId()
    if (-not [AppWindow]::AttachThreadInput($testThread, $mainThread, $true)) { throw 'could not attach the app input thread for the tab-order check' }
    try {
        [AppWindow]::SetFocus($micFader) | Out-Null
        if (([AppWindow]::Of((Focused-Control $ui))).accName(0) -ne 'Microphone volume') { throw 'could not focus the microphone volume fader' }
        [Windows.Forms.SendKeys]::SendWait('{TAB}')
        $tabName = ([AppWindow]::Of((Focused-Control $ui))).accName(0)
        if ($tabName -eq 'Mute mic') {
            [Windows.Forms.SendKeys]::SendWait('{TAB}')
            $tabName = ([AppWindow]::Of((Focused-Control $ui))).accName(0)
        }
        if ($tabName -ne 'Screen brightness') { throw "Tab after microphone volume did not reach Screen brightness before later controls; focused '$tabName'" }
    } finally {
        [AppWindow]::AttachThreadInput($testThread, $mainThread, $false) | Out-Null
    }
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    $recordShot = Snapshot-Window $ui (Join-Path $Artifacts 'app-status-recording.png')
    if ((Closest-Status-Color $recordShot $ui 225 40 40) -gt 100) { throw 'recording status text did not use palette accent #E12828' }
    $recordShot.Dispose()
    Wait-Recorded $ui 2
    Assert-StatusTooltip $ui
    Press $ui 'Pause'
    Wait-Status $ui 'Paused' | Out-Null
    $pauseShot = Snapshot-Window $ui (Join-Path $Artifacts 'app-status-paused.png')
    if ((Closest-Status-Color $pauseShot $ui 126 34 36) -gt 100) { throw 'paused status text did not use the dimmed palette accent' }
    $pauseShot.Dispose()
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
    Choose-Last $ui 'Destination 1 service'
    Set-Field $ui 'Custom server URL' $server
    Set-Field $ui 'Custom stream key' 'appkey'
    Set-Field $ui 'Bitrate (kb/s)' '3000'
    # The same stream to a second server, with its own key.
    Add-Destination $ui 2
    Choose-Last $ui 'Destination 2 service'
    Set-Field $ui 'Custom 2 server URL' $server2
    Set-Field $ui 'Custom 2 stream key' 'appkey2'
    # A third destination, turned off, needs no key and gets nothing.
    Add-Destination $ui 3
    Set-Check $ui 'Stream to Twitch' $false
    # The second server gets the vertical video, which is also saved beside the movie.
    Set-Check $ui 'Send the vertical video to Custom 2' $true
    Set-Check $ui 'Also save a vertical video (9:16, MP4)' $true
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    $deadline = (Get-Date).AddSeconds(10)
    while (-not (Text-Of $ui 102).StartsWith("$([char]0x25CF) LIVE")) {
        if ((Get-Date) -gt $deadline) { throw "no LIVE badge: $(Text-Of $ui 101)" }
        Start-Sleep -Milliseconds 100
    }
    if ((Text-Of $ui 102) -notmatch 'LIVE') { throw 'live badge does not contain LIVE' }
    Start-Sleep -Seconds 3
    Assert-StatusTooltip $ui
    # The badge shows the measured send rate, here over a second of a 3000 kb/s stream.
    $badge = Text-Of $ui 102
    if (-not ($badge -match 'LIVE 2/2\s+(\d+\.\d) Mb/s') -or [double]$Matches[1] -le 0) { throw "no stream rate for both servers in the LIVE badge: '$badge'" }
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
    # GitHub's Windows runners have no GPU: WARP draws the 1080x1920 canvas on the CPU and skips
    # up to 30% of its frames there (28.5% seen). A real GPU keeps it within two frames. With both
    # canvases on the CPU, the main stream can show a frame one 33 ms slot late too (32.8 ms seen).
    $behind = if ($env:GITHUB_ACTIONS -eq 'true') { 0.3 } else { 0 }
    Expect-Within $got 'av_offset_max_ms' 0 $(if ($behind -gt 0) { 35 } else { 20 })
    $file = Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received 'live2\appkey2') | Select-Object -First 1
    if (-not $file) { throw 'MediaMTX recorded nothing from the second destination' }
    $second = Get-Probe $Rec $file.FullName (Join-Path $Artifacts 'app-stream-received-2.probe')
    Expect-Field $second 'width' '1080'
    Expect-Field $second 'height' '1920'
    Expect-Within $second 'frames' ([int]([int]$got['frames'] * (1 - $behind)) - 2) ([int]$got['frames'] + 2)
    Expect-Within $second 'av_sync_events' 3 6
    # A frame WARP skips moves its flash to the next 33 ms slot.
    Expect-Within $second 'av_offset_max_ms' 0 $(if ($behind -gt 0) { 35 } else { 20 })
    if ((Text-Of $ui 101) -notmatch 'and its vertical video') { throw "the vertical video was not reported: $(Text-Of $ui 101)" }
    $main = Newest-Movie
    $verticalMovie = $main -replace '\.mp4$', '-vertical.mp4'
    if (-not (Test-Path $verticalMovie)) { throw "no vertical video beside $main" }
    Copy-Item $verticalMovie (Join-Path $Artifacts 'app-vertical.mp4')
    $mainProbe = Get-Probe $Rec $main (Join-Path $Artifacts 'app-stream-main.probe')
    $verticalProbe = Get-Probe $Rec $verticalMovie (Join-Path $Artifacts 'app-vertical.probe')
    Expect-Field $verticalProbe 'width' '1080'
    Expect-Field $verticalProbe 'height' '1920'
    Expect-Within $verticalProbe 'frames' ([int]([int]$mainProbe['frames'] * (1 - $behind)) - 2) ([int]$mainProbe['frames'] + 2)
    Expect-Within $verticalProbe 'av_sync_events' 3 6
    # A vertical frame skipped behind a slow CI encoder moves its flash to the next 33 ms slot.
    Expect-Within $verticalProbe 'av_offset_max_ms' 0 $(if ($behind -gt 0) { 35 } else { 20 })
    Write-Host "3. stream: received $($got['frames']) and $($second['frames']) frames (the second 1080x1920), $($got['av_sync_events']) pairs, worst $($got['av_offset_max_ms']) ms; vertical movie $($verticalProbe['frames']) of $($mainProbe['frames']) frames"
    Set-Check $ui 'Send the vertical video to Custom 2' $false
    Press $ui 'Remove Twitch'
    Set-Check $ui 'Also save a vertical video (9:16, MP4)' $false
    foreach ($url in $server, $server2) {
        if (-not (Has-Key $url)) { throw "the stream key for $url was not stored in Credential Manager" }
    }
    # Get Key, then copying TikTok's server and key, fills the row. (A test home opens no page.)
    Add-Destination $ui 3
    Choose-Item $ui 'Destination 3 service' 3
    $tiktok = 'rtmp://127.0.0.1:1935/tiktok-a', 'rtmp://127.0.0.1:1935/tiktok-b'
    foreach ($url in $tiktok) { cmdkey "/delete:rec/stream/$url" 2>&1 | Out-Null }
    Get-Key $ui 'TikTok'
    Copy-To-Rec $tiktok[0]
    Wait-Until { (Find $ui 'TikTok server URL' $Text).Item.accValue(0) -eq $tiktok[0] } 'the copied TikTok server was not filled in'
    Copy-To-Rec 'tiktokkey-a'
    Wait-Until { Has-Key $tiktok[0] } 'the copied TikTok key was not kept'
    # The next LIVE: a new server and key. The old pair is forgotten.
    Get-Key $ui 'TikTok'
    Copy-To-Rec $tiktok[1]
    Wait-Until { (Has-Key $tiktok[1]) -and -not (Has-Key $tiktok[0]) } "the TikTok key did not move to the new server (server field '$((Find $ui 'TikTok server URL' $Text).Item.accValue(0))', old key kept $(Has-Key $tiktok[0]), new key kept $(Has-Key $tiktok[1]); $(Text-Of $ui 101))"
    Copy-To-Rec 'tiktokkey-b'
    Wait-Until { (Text-Of $ui 101) -match 'server and stream key filled in' } "no note that both were filled in: $(Text-Of $ui 101)"
    Press $ui 'Remove TikTok'
    cmdkey "/delete:rec/stream/$($tiktok[1])" 2>&1 | Out-Null
    Write-Host '3. Get Key: the TikTok server and key came from the clipboard and moved together'

    # 4. Close the window mid-recording: it hides and keeps recording.
    Set-Check $ui 'Go live while recording' $false
    $handle = $ui.Window
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    [AppWindow]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
    Start-Sleep -Seconds 2
    if ([AppWindow]::IsWindowVisible($handle)) { throw 'the window stayed open' }
    if ($ui.Process.HasExited) { throw 'closing the window ended the app' }
    # The tray menu stops it. (Typed keys would go to whichever window took the focus, which
    # drops them when it runs elevated; step 2 covers the hotkey.)
    [AppWindow]::PostMessage($handle, 0x0111, [IntPtr]1000, [IntPtr]::Zero) | Out-Null  # WM_COMMAND, tray "Stop Recording"
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
    if (((Find $ui 'Camera device' $ComboBox).Item.accState(0) -band 0x1) -ne 0) { throw 'the camera menu was disabled while Camera was off' }
    Set-Check $ui 'Camera' $true
    if (-not [AppWindow]::IsWindowVisible((Find $ui 'Camera brightness' $Slider).Handle)) { throw 'Camera strip stayed hidden after Camera was enabled' }
    $cameraHeader = Rect-Of $ui 'Camera' 41
    $cameraReset = Rect-Of $ui 'Reset camera color' $PushButton
    if ([Math]::Abs($cameraHeader.Top - $cameraReset.Top) -gt 4) { throw 'Camera Reset is not on the header row' }
    Assert-StripGeometry $ui $true
    Set-Check $ui 'Image overlay' $true
    # Before Start the preview already shows the scene; with Show preview off it stops.
    $idle = Join-Path $Artifacts 'app-preview-idle.png'
    Set-Check $ui 'Image overlay' $false
    Set-Check $ui 'Image overlay' $true
    Wait-PreviewColors $ui $idle @(0.76, 0.75, 0.18, 0.18) @(0.02, 0.03, 0.15, 0.15) 15
    # Color readouts track the slider, double-click resets the live preview, and Ctrl+0
    # exercises the keyboard reset on this test-owned app window.
    $screenBrightness = (Find $ui 'Screen brightness' $Slider).Handle
    [AppWindow]::SendMessage($screenBrightness, 0x0405, [IntPtr]1, [IntPtr](-50)) | Out-Null
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]8, $screenBrightness) | Out-Null
    if ((Find $ui 'Screen brightness' $Slider).Item.accValue(0) -ne '-50') { throw 'screen brightness slider value did not show -50' }
    # This probe checks trackbar colors, not the (unsaved) value.
    if (-not (High-Contrast)) {
        Expect-CustomTrackbar $ui 304 (Join-Path $Artifacts 'app-screen-brightness-d3.png')
        $screenBitmap = Snapshot-Window $ui (Join-Path $Artifacts 'app-screen-brightness-d3-fill.png')
        try {
            $screenRect = Control-Rect $ui 304
            $fillPixels = 0
            for ($y = [Math]::Max(0, $screenRect.Top); $y -lt [Math]::Min($screenBitmap.Height, $screenRect.Bottom); $y++) {
                for ($x = [Math]::Max(0, $screenRect.Left); $x -lt [Math]::Min($screenBitmap.Width, $screenRect.Right); $x++) {
                    $pixel = $screenBitmap.GetPixel($x, $y)
                    if ($pixel.R -ge 100 -and $pixel.R -le 125 -and
                        $pixel.G -ge 100 -and $pixel.G -le 125 -and
                        $pixel.B -ge 100 -and $pixel.B -le 125) { $fillPixels++ }
                }
            }
            if ($fillPixels -lt 6) { throw "screen brightness trackbar has only $fillPixels neutral fill pixels" }
        } finally { $screenBitmap.Dispose() }
    }
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-screen-gray.png') @(0.4, 0.4, 0.2, 0.2) 'gray' -pollMilliseconds 50
    Post-TrackbarDoubleClick $screenBrightness
    Wait-Until { (Find $ui 'Screen brightness' $Slider).Item.accValue(0) -eq '0' } 'double-click did not reset screen brightness'
    Wait-Until { (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("screen_color=0,0,0`n") } 'double-click reset was not saved'
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-screen-reset.png') @(0.4, 0.4, 0.2, 0.2) 'white' -pollMilliseconds 50
    [AppWindow]::SendMessage($screenBrightness, 0x0405, [IntPtr]1, [IntPtr](20)) | Out-Null
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]8, $screenBrightness) | Out-Null
    if ((Find $ui 'Screen brightness' $Slider).Item.accValue(0) -ne '+20') { throw 'screen brightness slider value did not show +20' }
    [AppWindow]::SetForegroundWindow($ui.Window) | Out-Null
    $sliderRect = New-Object AppWindow+RECT
    [AppWindow]::GetClientRect($screenBrightness, [ref]$sliderRect) | Out-Null
    $sliderPoint = [IntPtr](([int]($sliderRect.Bottom / 2) -shl 16) -bor [int]($sliderRect.Right / 2))
    [AppWindow]::SendMessage($screenBrightness, 0x0201, [IntPtr]1, $sliderPoint) | Out-Null # Focus the test app's trackbar.
    [AppWindow]::SendMessage($screenBrightness, 0x0202, [IntPtr]::Zero, $sliderPoint) | Out-Null
    [AppWindow]::SetFocus($screenBrightness) | Out-Null
    if ([AppWindow]::GetForegroundWindow() -ne $ui.Window) {
        throw 'refusing to send Ctrl+0 because the test app window is not the foreground window'
    }
    [Windows.Forms.SendKeys]::SendWait('^0')
    Wait-Until { (Find $ui 'Screen brightness' $Slider).Item.accValue(0) -eq '0' } 'Ctrl+0 did not reset screen brightness'
    Wait-Until { (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("screen_color=0,0,0`n") } 'Ctrl+0 reset was not saved'
    $volumeSlider = (Find $ui 'System audio volume' $Slider).Handle
    [AppWindow]::SendMessage($volumeSlider, 0x0405, [IntPtr]1, [IntPtr](-6)) | Out-Null
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]8, $volumeSlider) | Out-Null
    Wait-Until { (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("system_volume=-6`n") } 'volume was not saved after trackbar release'
    $volumeReadout = [AppWindow]::GetDlgItem($ui.Window, 106)
    Click-Readout $volumeReadout
    $afterClick = [int][AppWindow]::SendMessage($volumeSlider, 0x0400, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($afterClick -ne -6) { throw "click without movement changed the slider value from -6 to $afterClick" }
    if (-not (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("system_volume=-6`n")) {
        throw 'click without movement changed the saved volume'
    }
    Start-Sleep -Milliseconds ([AppWindow]::GetDoubleClickTime() + 100)
    Post-TrackbarDoubleClick $volumeSlider
    Wait-Until { (Find $ui 'System audio volume' $Slider).Item.accValue(0) -eq '0 dB' } 'double-click did not reset system volume to 0 dB'
    Wait-Until { (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("system_volume=0`n") } 'volume double-click reset was not saved'
    $beforeDrag = [int][AppWindow]::SendMessage($volumeSlider, 0x0400, [IntPtr]::Zero, [IntPtr]::Zero)
    Drag-Readout $volumeReadout 8 -NoRelease
    $duringDrag = [int][AppWindow]::SendMessage($volumeSlider, 0x0400, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($duringDrag -ne $beforeDrag + 4) { throw "readout drag up changed volume from $beforeDrag to $duringDrag, expected +4" }
    if (-not (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("system_volume=0`n")) {
        throw 'readout drag persisted before the release notification'
    }
    Release-Readout $volumeReadout
    Wait-Until { (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("system_volume=4`n") } 'readout drag up was not persisted on release'
    Drag-Readout $volumeReadout -8
    $afterDown = [int][AppWindow]::SendMessage($volumeSlider, 0x0400, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($afterDown -ne $beforeDrag) { throw "readout drag down changed volume to $afterDown, expected $beforeDrag" }
    Wait-Until { (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("system_volume=0`n") } 'readout drag down was not persisted on release'
    Write-Host "6a. slider resets and readout drag: click unchanged at -6 dB; +4 and -4 dB per 8 DIPs, saved on release"
    Set-Check $ui 'Show preview' $false
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-off.png') @(0.76, 0.75, 0.18, 0.18) 'green' -Not
    Set-Check $ui 'Show preview' $true
    Wait-PreviewColor $ui $idle @(0.76, 0.75, 0.18, 0.18) 'green'
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Wait-Recorded $ui 1
    # The preview: the camera in the bottom-right corner and the image's red block top left.
    # On a busy runner recording/scene changes can precede the next rendered preview frame.
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-corner.png') @(0.76, 0.75, 0.18, 0.18) 'green'
    $picture = Snapshot-Preview $ui (Join-Path $Artifacts 'app-preview-corner.png')
    Expect-Color (Picture-Color $picture 0.02 0.03 0.15 0.15) 'color' 'red'
    $picture.Dispose()
    Wait-Recorded $ui 2
    Choose-Last $ui 'Scene'  # Camera only
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-camera.png') @(0.35, 0.4, 0.3, 0.2) 'green'
    $picture = Snapshot-Preview $ui (Join-Path $Artifacts 'app-preview-camera.png')
    $picture.Dispose()
    Wait-Recorded $ui 5
    [AppWindow]::PostMessage($ui.Window, 0x0111, [IntPtr]2001, [IntPtr]::Zero) | Out-Null  # WM_COMMAND, tray "Screen only"
    Wait-Until { (Find $ui 'Scene' $ComboBox).Item.accValue(0) -eq 'Screen only' } 'the posted Screen only scene was not applied by the app'
    # The recording test pattern has dark bands and a white flash, unlike the white preview.
    # Zero contrast maps both to mid-gray, so the strict samples test scene composition.
    $screenBrightness = (Find $ui 'Screen brightness' $Slider).Handle
    [AppWindow]::SendMessage($screenBrightness, 0x0405, [IntPtr]1, [IntPtr]0) | Out-Null
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]5, $screenBrightness) | Out-Null
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]8, $screenBrightness) | Out-Null
    Wait-Until { (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("screen_color=0,0,0") } 'screen brightness was not reset before the contrast sample'
    $screenContrast = (Find $ui 'Screen contrast' $Slider).Handle
    [AppWindow]::SendMessage($screenContrast, 0x0405, [IntPtr]1, [IntPtr](-100)) | Out-Null
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]5, $screenContrast) | Out-Null
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]8, $screenContrast) | Out-Null
    Wait-Until { (Get-Content (Join-Path $appHome 'settings.conf') -Raw).Contains("screen_color=0,-100,0") } 'screen color was not saved after slider end-track'
    $settingsAfterColor = [IO.File]::ReadAllText((Join-Path $appHome 'settings.conf'))
    Expect-Text $settingsAfterColor 'screen_color=0,-100,0'
    Expect-Text $settingsAfterColor 'camera_color=0,0,0'
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-screen-recording.png') @(0.4, 0.4, 0.2, 0.2) 'gray'
    Wait-Recorded $ui 7
    $counted = Text-Of $ui 101
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    $movie = Newest-Movie
    Copy-Item $movie (Join-Path $Artifacts 'app-camera.mp4')
    $corner = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-camera-corner.probe') @('--sample', '1450,800,380,200')
    $center = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-camera-center.probe') @('--sample', '800,400,300,200')
    $logo = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-camera-image.probe') @('--sample', '40,40,300,150')
    $screenColorMovie = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-camera-screen-color.probe') @('--sample', '800,400,300,200')
    Expect-Color $corner 'sample_1' 'green'
    Expect-Color $center 'sample_1' 'white'
    Expect-Color $center 'sample_3' 'green'
    Expect-Color $center 'sample_4' 'green'
    Expect-Color $screenColorMovie 'sample_1' 'white'
    Expect-Color $screenColorMovie 'sample_6' 'gray'
    # A slow runner can drop slots and shorten the movie, so the hidden camera is checked in
    # the last sampled second rather than a fixed one.
    $last = 0
    while ($corner.ContainsKey("sample_$($last + 1)")) { $last++ }
    if ($last -lt 5) { throw "the camera recording is too short: $($corner['duration']) s; the app showed '$counted' at Stop" }
    Expect-Color $corner "sample_$last" 'gray'
    if ($last -ge 7) { foreach ($second in 7..$last) { Expect-Color $screenColorMovie "sample_$second" 'gray' } }
    foreach ($second in 0..$last) { Expect-Color $logo "sample_$second" 'red' }
    Press $ui 'Reset screen color'
    Write-Host "6. camera and image: $($corner['frames']) frames ($counted at Stop), corner, full frame, hidden; the preview matched, before Start too"

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

    # 6c. Camera look: before Start a circle with a thick black border and a drop shadow; while
    # recording, a rectangle again. The circle is the largest square in the camera's place from
    # 6b, against the frame's right edge.
    Choose-Item $ui 'Shape' 2  # Circle
    Choose-Item $ui 'Border' 4  # Thick black
    Set-Check $ui 'Drop shadow' $true
    if (((Find $ui 'Camera roundness' 51).Item.accState(0) -band 0x1) -eq 0) { throw 'Roundness stayed enabled for a circle' }
    Start-Sleep -Milliseconds 300
    $conf = [IO.File]::ReadAllText((Join-Path $appHome 'settings.conf'))
    Expect-Text $conf "camera_style=circle,border=10:000000,shadow`n"
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $place = ($conf -split "`n" | Where-Object { $_ -like 'scene.0.camera=*' } | Select-Object -First 1).Split('=')[1].Split(',') |
        ForEach-Object { [double]::Parse($_, $culture) }
    # Where a square of side pixels sits along one axis of the camera's place.
    $align = {
        param([double]$start, [double]$length, [double]$canvas, [double]$side)
        $before = $start; $after = 1 - $start - $length
        if ([Math]::Abs($before - $after) -lt 1e-3) { return $start * $canvas + ($length * $canvas - $side) / 2 }
        if ($before -lt $after) { return $start * $canvas }
        return ($start + $length) * $canvas - $side
    }
    $side = [Math]::Min($place[3] * 1920, $place[4] * 1080)
    $sx = & $align $place[1] $place[3] 1920 $side
    $sy = & $align $place[2] $place[4] 1080 $side
    $cx = [int]($sx + $side / 2); $cy = [int]($sy + $side / 2)
    # Outside the circle, and below the rectangle's border once the shape changes.
    $squareCorner = @((($sx + 6) / 1920), (($sy + 18) / 1080), (12 / 1920), (12 / 1080))
    $look = Join-Path $Artifacts 'app-preview-look.png'
    Wait-PreviewColor $ui $look @((($cx - 20) / 1920), (($cy - 20) / 1080), (40 / 1920), (40 / 1080)) 'green'
    Wait-PreviewColor $ui $look $squareCorner 'green' -Not
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Wait-Recorded $ui 2
    Choose-Item $ui 'Shape' 0  # Rectangle
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-preview-look-rectangle.png') $squareCorner 'green'
    Wait-Recorded $ui 5
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    $movie = Newest-Movie
    Copy-Item $movie (Join-Path $Artifacts 'app-look.mp4')
    $sample = { param([double]$x, [double]$y, [int]$w, [int]$h) "$([int]$x),$([int]$y),$w,$h" }
    $center = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-look-center.probe') @('--sample', (& $sample ($cx - 20) ($cy - 20) 40 40))
    $outside = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-look-corner.probe') @('--sample', (& $sample ($sx + 4) ($sy + 16) 10 10))
    $border = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-look-border.probe') @('--sample', (& $sample ($cx - 5) ($sy + 3) 10 4))
    $shadow = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-look-shadow.probe') @('--sample', (& $sample ($cx - 10) ($sy + $side + 2) 20 6))
    Expect-Color $center 'sample_1' 'green'
    Expect-Color $outside 'sample_1' 'white'
    Expect-Color $border 'sample_1' 'black'
    $r, $g, $b = $shadow['sample_1'].Split(',') | ForEach-Object { [int]$_ }
    # The screen there is white; the shadow, fading away from the circle, darkens it.
    if ($r -gt 230 -or $g -gt 230 -or $b -gt 230) { throw "no drop shadow below the circle: $($shadow['sample_1'])" }
    Expect-Color $center 'sample_4' 'green'
    Expect-Color $outside 'sample_4' 'green'
    Choose-Item $ui 'Border' 0  # None
    Set-Check $ui 'Drop shadow' $false
    Start-Sleep -Milliseconds 300
    Expect-Text ([IO.File]::ReadAllText((Join-Path $appHome 'settings.conf'))) "camera_style=rectangle`n"
    Write-Host "6c. camera look: $($center['frames']) frames; circle, border and shadow before Start, a rectangle while recording"

    # 6p. Phone camera: choosing Phone over Wi-Fi opens its code window with the address. A
    # headless browser with a synthetic camera (green) opens it; the window reports the phone
    # connected and the preview shows its picture in the camera's place (top right since 6b).
    # A recording takes the phone over from the preview. Then back to the test camera.
    $cameraList = $null
    foreach ($id in 100..199) {
        $control = [AppWindow]::GetDlgItem($ui.Window, $id)
        if ($control -eq [IntPtr]::Zero) { continue }
        $item = [AppWindow]::Of($control)
        if ($item.accRole(0) -eq $ComboBox -and $item.accValue(0) -eq 'Test camera') { $cameraList = $control; break }
    }
    if (-not $cameraList) { throw 'no camera list showing the test camera' }
    $cameraCount = [int][AppWindow]::SendMessage($cameraList, 0x0146, [IntPtr]::Zero, [IntPtr]::Zero)  # CB_GETCOUNT
    $chooseCamera = {
        param([int]$index)
        [AppWindow]::SendMessage($cameraList, 0x014E, [IntPtr]$index, [IntPtr]::Zero) | Out-Null  # CB_SETCURSEL
        [AppWindow]::SendMessage($ui.Window, 0x0111, [IntPtr]((1 -shl 16) -bor [AppWindow]::GetDlgCtrlID($cameraList)), $cameraList) | Out-Null
    }
    & $chooseCamera ($cameraCount - 1)
    if ([AppWindow]::Of($cameraList).accValue(0) -ne 'Phone over Wi-Fi') { throw "the last camera is $([AppWindow]::Of($cameraList).accValue(0))" }
    $code = [IntPtr]::Zero
    Wait-Until { $script:code = [AppWindow]::FindWindow('RecPhoneCode', 'Phone camera'); $script:code -ne [IntPtr]::Zero -and [AppWindow]::IsWindowVisible($script:code) } 'the phone code window did not open'
    $address = [AppWindow]::Of([AppWindow]::GetDlgItem($code, 102)).accValue(0)
    $expected = if ($relayed) { '^' + [regex]::Escape($env:REC_PHONE_RELAY) + '/#[A-Za-z0-9]{22}$' } else { '^https://[0-9.]+:47110/#[A-Za-z0-9]{22}$' }
    if ($address -notmatch $expected) { throw "the phone address is '$address'" }
    $backup = [AppWindow]::Of([AppWindow]::GetDlgItem($code, 104)).accName(0)
    if ($relayed -and $backup -notmatch '^Without internet: open https://[0-9.]+:47110/#[A-Za-z0-9]{22} ') { throw "the phone window's local address reads '$backup'" }
    $phoneStatus = { [AppWindow]::Of([AppWindow]::GetDlgItem($code, 103)).accName(0) }
    Wait-Until { (& $phoneStatus) -eq 'Waiting for the phone...' } "the phone window says '$(& $phoneStatus)'" 15
    $phoneArea = @(0.75, 0.12, 0.16, 0.14)
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-phone-waiting.png') $phoneArea 'green' -Not
    $phone = Start-Phone $address
    try {
        $page = Wait-PhoneStatus $phone 'Live in Rec' 20
        if ($page -ne 'Live in Rec') { throw "the phone page says '$page'" }
        Wait-Until { (& $phoneStatus) -like 'Connected*' } "the phone window says '$(& $phoneStatus)'" 15
        $codeRect = New-Object AppWindow+RECT
        [AppWindow]::GetClientRect($code, [ref]$codeRect) | Out-Null
        $codePicture = New-Object System.Drawing.Bitmap $codeRect.Right, $codeRect.Bottom
        $codeGraphics = [System.Drawing.Graphics]::FromImage($codePicture)
        $codeDc = $codeGraphics.GetHdc()
        [AppWindow]::PrintWindow($code, $codeDc, 3) | Out-Null  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
        $codeGraphics.ReleaseHdc($codeDc)
        $codeGraphics.Dispose()
        $codePicture.Save((Join-Path $Artifacts 'app-phone-code.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $codePicture.Dispose()
        Wait-PreviewColor $ui (Join-Path $Artifacts 'app-phone-preview.png') $phoneArea 'green'
        Press $ui 'Start Recording'
        Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
        Wait-Recorded $ui 8
        Press $ui 'Stop Recording'
        Wait-Status $ui 'Ready to record' | Out-Null
    } finally { Stop-Phone $phone }
    $movie = Newest-Movie
    Copy-Item $movie (Join-Path $Artifacts 'app-phone.mp4')
    $phoneMovie = Get-Probe $Rec $movie (Join-Path $Artifacts 'app-phone.probe') @('--sample', '1460,140,300,150')
    $first = 0
    while ($phoneMovie.ContainsKey("sample_$first")) {
        try { Expect-Color $phoneMovie "sample_$first" 'green'; break } catch { $first++ }
    }
    if ($first -gt 4) { throw "the phone's picture reached the recording only at $first s" }
    $last = $first
    while ($phoneMovie.ContainsKey("sample_$($last + 1)")) { $last++; Expect-Color $phoneMovie "sample_$last" 'green' }
    [AppWindow]::SendMessage($code, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
    if ([AppWindow]::IsWindowVisible($code)) { throw 'the phone code window did not close' }
    & $chooseCamera ($cameraCount - 2)
    if ([AppWindow]::Of($cameraList).accValue(0) -ne 'Test camera') { throw 'the test camera was not chosen again' }
    Write-Host "6p. phone camera: connected in the preview; in the recording from $first s to $last s"

    # 6d. Scenes: a new scene gets display 1 (covered by a blue window) as a layer over the
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
        $counted = Text-Of $ui 101
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
    if ($last -lt 4) { throw "the scenes recording is too short: $($quarter['duration']) s; the app showed '$counted' at Stop" }
    Expect-Color $quarter 'sample_1' 'blue'
    Expect-Color $quarter "sample_$last" 'white'
    # Remove the layer, so the later steps capture only the test pattern.
    $sources = (Find $ui 'Sources' 33).Handle  # A list
    [AppWindow]::SendMessage($sources, 0x0186, [IntPtr]1, [IntPtr]::Zero) | Out-Null  # LB_SETCURSEL: Display 1
    [AppWindow]::SendMessage($ui.Window, 0x0111, [IntPtr]((1 -shl 16) -bor [AppWindow]::GetDlgCtrlID($sources)), $sources) | Out-Null
    Press $ui 'Remove'
    Start-Sleep -Milliseconds 300
    if ([int][AppWindow]::SendMessage($sources, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero) -ne 2) { throw 'the display layer was not removed' }  # LB_GETCOUNT
    Write-Host "6d. scenes: $($quarter['frames']) frames ($counted at Stop); a display layer placed by dragging in a new scene, switched away while recording"
    # 7. Volume and mute: system audio at -6 dB from the slider, muted with its box 2.5 s in,
    # unmuted from the notification-area menu 4.5 s in. The mix loses the beeps at 3 and 4 s.
    Set-Check $ui 'Microphone' $true
    $slider = (Find $ui 'System audio volume' $Slider).Handle
    [AppWindow]::SendMessage($slider, 0x0405, [IntPtr]1, [IntPtr](-6)) | Out-Null  # TBM_SETPOS
    if (-not (High-Contrast)) {
        Expect-CustomTrackbar $ui 105 (Join-Path $Artifacts 'app-volume-slider-d3.png')
    }
    [AppWindow]::SendMessage($ui.Window, 0x0114, [IntPtr]8, $slider) | Out-Null  # WM_HSCROLL, TB_ENDTRACK
    $idleShot = Snapshot-Window $ui (Join-Path $Artifacts 'app-meter-idle.png')
    try {
        foreach ($meterId in 104, 109) {
            if ((Meter-LitPixels $ui $idleShot $meterId) -ne 0) { throw "idle meter $meterId showed lit pixels" }
        }
    } finally { $idleShot.Dispose() }
    Write-Host '7. meter idle probe: system=0 mic=0 lit pixels'
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    $meterRecordingAt = Get-Date
    Wait-Until {
        $probeShot = Snapshot-Window $ui (Join-Path $Artifacts 'app-meter-probe.png')
        try { (Meter-LitPixels $ui $probeShot 104) -gt 0 } finally { $probeShot.Dispose() }
    } 'system meter did not show lit pixels within 2 s' 2
    $meter = Find $ui 'System audio level' 48  # ROLE_SYSTEM_PROGRESSBAR
    $meterValue = [string]$meter.Item.accValue(0)
    if ($meterValue -notmatch '^-?\d+ dB$|^silent$') { throw "system meter has no accessible integer dB value: '$meterValue'" }
    Write-Host "7. meter accessibility probe: role=ProgressBar name=System audio level value=$meterValue"
    $meterShot = Snapshot-Window $ui (Join-Path $Artifacts 'app-meter.png')
    $meterLitBeforeMute = Meter-LitPixels $ui $meterShot 104
    if (-not (High-Contrast)) {
        $meterGreen = Meter-ColorPixels $ui $meterShot 104 70 190 105
        $meterRed = Meter-ColorPixels $ui $meterShot 104 225 40 40
        if ($meterGreen -le 0 -or $meterRed -ne 0) { throw "meter colors at -6 dB were unexpected: green=$meterGreen red=$meterRed" }
        Write-Host "7. meter color probe: green=$meterGreen red=$meterRed pixels"
    }
    $meterShot.Dispose()
    Write-Host "7. meter recording probe: system=$meterLitBeforeMute lit pixels"
    Start-Sleep -Milliseconds 2500
    Set-Check $ui 'Mute system' $true
    $meterShot = Snapshot-Window $ui (Join-Path $Artifacts 'app-meter-muted-probe.png')
    $meterLitAfterMuteStart = Meter-LitPixels $ui $meterShot 104
    $meterShot.Dispose()
    $meterLitAfterMute = $meterLitAfterMuteStart
    Wait-Until {
        $probeShot = Snapshot-Window $ui (Join-Path $Artifacts 'app-meter-muted-probe.png')
        try {
            $meterLitAfterMute = Meter-LitPixels $ui $probeShot 104
            $meterLitAfterMute -gt $meterLitAfterMuteStart
        } finally { $probeShot.Dispose() }
    } 'system meter did not rise on a later beep after post-meter mute' 2
    Write-Host "7. meter post-mute probe: before=$meterLitAfterMuteStart after=$meterLitAfterMute lit pixels"
    $untilUnmute = 4500 - [int]((Get-Date) - $meterRecordingAt).TotalMilliseconds
    if ($untilUnmute -gt 0) { Start-Sleep -Milliseconds $untilUnmute }
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
    Expect-Text $settings "destination.0.server=$server`n"
    Expect-Text $settings 'destination.0.service=custom'
    Expect-Text $settings "destination.1.server=$server2"
    Expect-Text $settings 'destination.1.service=custom'
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
    Expect-Text $settings 'keep_recording=no'
    if ($settings.Contains('appkey')) { throw 'the stream key was written to the settings file' }
    if ($settings.Contains('destination.2.')) { throw 'the removed destination is still in the settings' }

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

    # 9b. With Keep Recording After Quit, quitting leaves the recording to the background
    # engine. The next launch takes it over: status, timer and the preview (shared memory from
    # the engine; the camera sits top right since 6b), and Stop saves one movie spanning both.
    $conf = Join-Path $appHome 'settings.conf'
    [IO.File]::WriteAllText($conf, ((Get-Content -Raw $conf) -replace 'keep_recording=no', 'keep_recording=yes'))
    $keptBefore = @(Get-ChildItem $folder -Filter '*.mp4').Count
    $ui = Start-App
    Wait-Status $ui 'Ready to record' | Out-Null
    Press $ui 'Start Recording'
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Wait-Recorded $ui 2
    [AppWindow]::PostMessage($ui.Window, 0x0111, [IntPtr]1004, [IntPtr]::Zero) | Out-Null  # WM_COMMAND, Quit Rec
    if (-not $ui.Process.WaitForExit(15000)) { throw 'Quit Rec did not end the app with Keep Recording After Quit' }
    $engineStatus = Engine-Command 'status'
    $engineStatus | Set-Content (Join-Path $Artifacts 'app-keep-status.txt')
    Expect-Text $engineStatus 'state=recording'
    Start-Sleep -Seconds 2
    $ui = Start-App
    Wait-Status $ui "$([char]0x25CF) Recording" | Out-Null
    Wait-PreviewColor $ui (Join-Path $Artifacts 'app-keep-preview.png') @(0.72, 0.08, 0.22, 0.22) 'green'
    Wait-Recorded $ui 6
    Press $ui 'Stop Recording'
    Wait-Status $ui 'Ready to record' | Out-Null
    Expect-Text (Text-Of $ui 101) 'Saved rec-'
    if (@(Get-ChildItem $folder -Filter '*.mp4').Count -ne $keptBefore + 1) { throw 'the kept recording did not save one movie' }
    $kept = Get-Probe $Rec (Newest-Movie) (Join-Path $Artifacts 'app-keep.probe')
    Copy-Item (Newest-Movie) (Join-Path $Artifacts 'app-keep.mp4')
    Expect-Within $kept 'duration' 5.5 9
    Expect-Within $kept 'av_sync_events' 4 9
    Expect-Within $kept 'av_offset_max_ms' 0 20
    Write-Host "9b. kept after quit: $($kept['frames']) frames ($($kept['duration']) s) across the app quitting and reopening; the preview came from the engine"
    [IO.File]::WriteAllText($conf, ((Get-Content -Raw $conf) -replace 'keep_recording=yes', 'keep_recording=no'))
    [AppWindow]::PostMessage($ui.Window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE, idle: quits
    if (-not $ui.Process.WaitForExit(15000)) { throw 'the app did not quit' }
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
    if (@(Get-ChildItem $folder -Filter '*.mp4').Count -ne $before + 3) { throw 'log-off did not save the recording' }
    $ended = Get-Probe $Rec (Newest-Movie) (Join-Path $Artifacts 'app-logoff.probe')
    Expect-Within $ended 'duration' 1.5 3.5
    Expect-Within $ended 'av_offset_max_ms' 0 20
    Write-Host "10. log off while recording: $($ended['frames']) frames"
    Write-Host 'app e2e passed'
} catch {
    Save-Stacks $ui.Process
    throw
} finally {
    if ($ui -and -not $ui.Process.HasExited) { $ui.Process.Kill() }
    Stop-MediaMTX $mediamtx
    Stop-Engine
    if ($ownRelay) { Stop-Relay $ownRelay; Remove-Item Env:REC_PHONE_RELAY }
    if (Test-Path (Join-Path $appHome 'engine.log')) { Copy-Item (Join-Path $appHome 'engine.log') (Join-Path $Artifacts 'app-engine.log') }
    foreach ($url in $server, $server2) { cmdkey "/delete:rec/stream/$url" 2>&1 | Out-Null }
}
