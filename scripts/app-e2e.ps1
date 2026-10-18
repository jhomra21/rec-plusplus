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
#   6. Settings persist without the stream key, which Credential Manager holds.
#   7. Quit Rec from the notification-area menu while recording saves the movie, then exits.
#   8. Logging off while recording saves the movie before WM_ENDSESSION returns.
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
  public static Accessibility.IAccessible Of(IntPtr h) {
    Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71"); object o;
    Marshal.ThrowExceptionForHR(AccessibleObjectFromWindow(h, 0xFFFFFFFC, ref iid, out o));
    return (Accessibility.IAccessible)o;
  }
  public static void SetValue(IntPtr h, string value) { Of(h).set_accValue(0, value); }
}
'@

# MSAA roles.
$PushButton = 43; $CheckButton = 44; $Text = 42; $ComboBox = 46
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
    for ($id = 100; $id -lt 160; $id++) {
        $handle = [AppWindow]::GetDlgItem($ui.Window, $id)
        if ($handle -eq [IntPtr]::Zero) { continue }
        $item = [AppWindow]::Of($handle)
        if ($item.accName(0) -eq $name -and $item.accRole(0) -eq $role) { return @{ Handle = $handle; Item = $item } }
    }
    throw "no control named '$name' with role $role"
}
function Press($ui, [string]$name) { (Find $ui $name $PushButton).Item.accDoDefaultAction(0) }
function Set-Check($ui, [string]$name, [bool]$on) {
    $box = (Find $ui $name $CheckButton).Item
    if ((($box.accState(0) -band 0x10) -ne 0) -ne $on) { $box.accDoDefaultAction(0) }
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
function Newest-Movie {
    $file = Get-ChildItem $folder -Filter '*.mp4' | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $file) { throw "no recording in $folder" }
    return $file.FullName
}

$received = Join-Path $Artifacts 'app-stream-server'
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
    while ((Text-Of $ui 102) -ne "$([char]0x25CF) LIVE") {
        if ((Get-Date) -gt $deadline) { throw "no LIVE badge: $(Text-Of $ui 101)" }
        Start-Sleep -Milliseconds 100
    }
    Start-Sleep -Seconds 4
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

    # 6. Closing when idle quits and saves settings, without the key.
    [AppWindow]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $ui.Process.WaitForExit(15000)) { throw 'the app did not quit' }
    $settings = Get-Content -Raw (Join-Path $appHome 'settings.conf')
    Copy-Item (Join-Path $appHome 'settings.conf') (Join-Path $Artifacts 'app-settings.conf')
    Expect-Text $settings "server=$server"
    Expect-Text $settings 'destination=custom'
    Expect-Text $settings 'stream_kbps=3000'
    Expect-Text $settings 'last_recording='
    Expect-Text $settings 'replay=1'
    if ($settings.Contains('appkey')) { throw 'the stream key was written to the settings file' }

    # 7. Quit from the notification-area menu while recording: the app saves the movie, then exits.
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
    Write-Host "7. quit while recording: $($quit['frames']) frames"

    # 8. Log off while recording: the movie is complete when WM_ENDSESSION returns, after which
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
    Write-Host "8. log off while recording: $($ended['frames']) frames"
    Write-Host 'app e2e passed'
} finally {
    if ($ui -and -not $ui.Process.HasExited) { $ui.Process.Kill() }
    Stop-MediaMTX $mediamtx
    cmdkey "/delete:rec/stream/$server" 2>&1 | Out-Null
}
