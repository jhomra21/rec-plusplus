# Live A/V sync acceptance on a Windows PC with audible output. A full-screen window
# flashes white with a simultaneous beep once a second while rec records the primary
# display and system audio; rec probe then measures the offset in the file.
# REC_E2E_MIC=1 also records the default microphone (mix + stems).
# REC_E2E_PAUSE=1 pauses for 2 s in the middle of the stimulus: the movie must keep its
# full length and stay in sync across the cut.
# REC_E2E_STREAM=1 also streams to a local MediaMTX server; the stream it records must be
# in sync too.
# REC_E2E_PREPLAY=1 starts the stimulus 2 s before the recording, so its audio is already
# playing when capture starts.
# REC_E2E_SOURCE=window records only the stimulus window (found by its title) until it
# closes: the recording must end then and be saved, with audio as long as the video.
# REC_E2E_SOURCE=region records a 1280x720 region of the display.
# REC_E2E_REPLAY=1 keeps a 5 s replay buffer and saves it when the stimulus ends; the clip
# must be in sync too.
# REC_E2E_LOSE_AUDIO=system|mic:AFTER_MS[:HOLD_MS] makes rec drop that device AFTER_MS into
# the recording, as an unplug would, and reopen it HOLD_MS later: the movie must keep its
# length, report the outage, and stay in sync after it.
# Nothing else may play: the beep detector cannot tell other sound from beeps. The script
# stops if the output device is not silent for 2 s first; REC_E2E_ALLOW_SOUND=1 skips that.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
$artifacts = if ($env:REC_E2E_ARTIFACT_DIR) { $env:REC_E2E_ARTIFACT_DIR } else {
    Join-Path ([IO.Path]::GetTempPath()) ('rec-live-sync-' + [guid]::NewGuid().ToString('N'))
}
New-Item -ItemType Directory -Force $artifacts | Out-Null
$rec = Build-Rec $root (Join-Path $root 'build')
$movie = Join-Path $artifacts 'recording.mp4'
$pause = $env:REC_E2E_PAUSE -eq '1'

$source = $env:REC_E2E_SOURCE
if ($source -and $source -notin @('window', 'region')) { throw "REC_E2E_SOURCE must be window or region" }
$target = switch ($source) {
    'window' { @('--window', 'rec flash-beep') }
    'region' { @('--display', '1', '--region', '100,100,1280,720') }
    default { @('--display', '1') }
}
# A window recording ends when the window closes; 30 s only bounds a missed close.
$length = if ($source -eq 'window') { '30' } else { '12' }
$arguments = @('record') + $target + @('--system-audio', '-r', '1080p', '-f', '60', '-t', $length, '--codec', 'h264', '-o', $movie)
if ($env:REC_E2E_MIC -eq '1') { $arguments += @('--mic', 'default') }
$replay = $env:REC_E2E_REPLAY -eq '1'
if ($replay) { $arguments += @('--replay', '5') }
$stream = $env:REC_E2E_STREAM -eq '1'
$server = $null
if ($stream) {
    $received = Join-Path $artifacts 'stream-server'
    $server = Start-MediaMTX (Get-MediaMTX (Join-Path $root 'build\tools')) $received (Join-Path $artifacts 'mediamtx')
    $arguments += @('--stream', 'rtmp://127.0.0.1:1935/live/e2e')
}

Write-Host "Saving recordings and evidence to: $artifacts"
(Invoke-Rec $rec @('devices')).Text | Tee-Object -FilePath (Join-Path $artifacts 'devices.txt')
if ($env:REC_E2E_ALLOW_SOUND -ne '1') {
    $peak = Get-OutputPeak 2
    Set-Content -Path (Join-Path $artifacts 'background-peak.txt') -Value $peak
    # -40 dBFS; a steady tone from another app at a tenth of full scale broke Mac runs.
    if ($peak -gt 0.01) {
        if ($server) { Stop-MediaMTX $server }
        throw "other audio is playing (peak $([math]::Round($peak, 3)) of full scale on the default output). Silence it and retry, or set REC_E2E_ALLOW_SOUND=1."
    }
}
$seconds = if ($pause) { 8 } else { 6 }
function Start-Stimulus([int]$length) {
    $stimulusArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'flash-beep.ps1'), '-Seconds', $length)
    if ($source -eq 'window') { $stimulusArguments += @('-Title', '"rec flash-beep"') }
    return Start-Process -FilePath powershell -PassThru -WindowStyle Hidden -ArgumentList $stimulusArguments
}
$stimulus = $null
# Window capture needs the window open before the recording starts.
if ($env:REC_E2E_PREPLAY -eq '1' -or $source -eq 'window') {
    # Covers the 2 s lead and the recorder's 3 s start-up as well.
    $stimulus = Start-Stimulus ($seconds + 5)
    Start-Sleep -Seconds 2
}
$run = Start-RecProcess $rec $arguments
try {
    # Allow the recorder to establish its capture streams and first video frame.
    Start-Sleep -Seconds 3
    if ($run.Process.HasExited) { throw "Recording stopped before the stimulus: $((Stop-RecProcess $run).Text)" }
    if (-not $stimulus) { $stimulus = Start-Stimulus $seconds }
    if ($pause) {
        Start-Sleep -Milliseconds 3500
        Send-RecCommand $run 'pause'
        Start-Sleep -Seconds 2
        Send-RecCommand $run 'resume'
    }
    $stimulus.WaitForExit()
    if ($stimulus.ExitCode -ne 0) { throw 'stimulus failed' }
    if ($replay) { Send-RecCommand $run 'save' }
} finally {
    if ($stimulus -and -not $stimulus.HasExited) { $stimulus.Kill() }
}
try { $result = Stop-RecProcess $run 90 } finally { if ($server) { Start-Sleep -Seconds 1; Stop-MediaMTX $server } }
Set-Content -Path (Join-Path $artifacts 'record.log') -Value $result.Text
Write-Host $result.Text
if ($result.Code -ne 0) { throw "Recorder failed; see $artifacts\record.log" }

$probe = Get-Probe $rec $movie (Join-Path $artifacts 'recording.probe')
Expect-Field $probe 'codec' 'h264'
Expect-Field $probe 'audio_codec' 'aac'
if ($source -eq 'window') {
    Expect-Text $result.Text 'The window closed'
    $duration = [double]::Parse($probe['duration'], [Globalization.CultureInfo]::InvariantCulture)
    if ($duration -lt 5 -or $duration -gt 11.5) { throw "the window recording lasts $duration s" }
    Expect-Within $probe 'audio_duration' ($duration - 0.15) ($duration + 0.15)
} else {
    Expect-Field $probe 'frames' '720'
    Expect-Within $probe 'audio_duration' 11.95 12.05
}
if ($env:REC_E2E_MIC -eq '1') { Expect-Field $probe 'audio_tracks' '3' }
if ($source -eq 'region') { Expect-Field $probe 'width' '1280'; Expect-Field $probe 'height' '720' }
if ($pause) { Expect-Text $result.Text 'not in the movie' }
# Beeps played while the device was gone are missing, so fewer pairs remain.
$least = 4
if ($env:REC_E2E_LOSE_AUDIO) {
    Expect-Text $result.Text ' lost at '
    Expect-Text $result.Text ' back at '
    Expect-Text $result.Text 'Audio devices reopened 1'
    $least = 3
}
$events = [int]$probe['av_sync_events']
if ($events -lt $least) { throw "only $events flash/beep pairs found" }
# ITU-R BT.1359 detectability: audio more than 45 ms early or 125 ms late.
Expect-Within $probe 'av_offset_ms' -45 125

Write-Host "Live A/V sync: $events flash/beep pairs, mean offset $($probe['av_offset_ms']) ms (positive = audio late), worst $($probe['av_offset_max_ms']) ms."

if ($replay) {
    Expect-Text $result.Text 'Saved replay'
    $clip = Get-Probe $rec (Join-Path $artifacts 'recording-replay-1.mp4') (Join-Path $artifacts 'replay.probe')
    Expect-Field $clip 'codec' 'h264'
    Expect-Within $clip 'duration' 4.5 7.5
    $pairs = [int]$clip['av_sync_events']
    if ($pairs -lt 4) { throw "only $pairs flash/beep pairs in the replay" }
    Expect-Within $clip 'av_offset_ms' -45 125
    Write-Host "Replay: $($clip['frames']) frames, $pairs pairs, mean offset $($clip['av_offset_ms']) ms, worst $($clip['av_offset_max_ms']) ms."
}

if ($stream) {
    Expect-Text $result.Text 'skipped 0'
    $file = Get-ChildItem -Recurse -Filter *.mp4 $received | Select-Object -First 1
    if (-not $file) { throw 'MediaMTX recorded nothing' }
    $got = Get-Probe $rec $file.FullName (Join-Path $artifacts 'stream-received.probe')
    Expect-Field $got 'codec' 'h264'
    Expect-Within $got 'frames' 719 720
    $pairs = [int]$got['av_sync_events']
    if ($pairs -lt 4) { throw "only $pairs flash/beep pairs in the received stream" }
    Expect-Within $got 'av_offset_ms' -45 125
    Write-Host "Received stream: $($got['frames']) frames, $pairs pairs, mean offset $($got['av_offset_ms']) ms, worst $($got['av_offset_max_ms']) ms."
}
Write-Host "Evidence: $artifacts"
