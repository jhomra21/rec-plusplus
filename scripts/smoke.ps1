# Windows smoke: build with MSVC, then encode and decode real Media Foundation output.
# REC_E2E_ARTIFACT_DIR keeps the recordings and probe reports; REC_E2E_CAPTURE=1 also
# records the real display. The streaming check downloads a pinned MediaMTX release. The
# desktop app check (app-e2e.ps1) needs an interactive desktop session.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-common.ps1')

$root = Split-Path -Parent $PSScriptRoot
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('rec-smoke-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
$artifacts = if ($env:REC_E2E_ARTIFACT_DIR) { $env:REC_E2E_ARTIFACT_DIR } else { $tmp }
New-Item -ItemType Directory -Force $artifacts | Out-Null

try {
    $rec = Build-Rec $root (Join-Path $tmp 'build')

    function Expect-Failure([string]$pattern, [string[]]$arguments) {
        $run = Invoke-Rec $rec $arguments
        if ($run.Code -eq 0) { throw "accepted: rec $arguments" }
        if (-not $run.Text.Contains($pattern)) { throw "rec $arguments printed '$($run.Text)', expected '$pattern'" }
    }
    function Record([string]$name, [string]$file, [string[]]$arguments) {
        $path = Join-Path $artifacts $file
        $run = Invoke-Rec $rec (@('record') + $arguments + @('-o', $path))
        Set-Content -Path (Join-Path $artifacts "$name.record.log") -Value $run.Text
        Write-Host $run.Text
        if ($run.Code -ne 0) { throw "rec record $arguments failed" }
        $probe = Get-Probe $rec $path (Join-Path $artifacts "$name.probe")
        return @{ Log = $run.Text; Probe = $probe }
    }

    if (-not (Invoke-Rec $rec @('--help')).Text.Contains('native screen recorder')) { throw 'help text' }
    Expect-Failure 'fps must be' @('record', '--fps', 'invalid')
    Expect-Failure 'resolution must be' @('record', '--resolution', 'gigantic')
    Expect-Failure 'requires --test-pattern' @('record', '--test-tone')
    Expect-Failure 'requires --test-pattern' @('record', '--test-mic')
    Expect-Failure 'needs a real screen' @('record', '--test-pattern', '--system-audio')
    Expect-Failure 'needs a real screen' @('record', '--test-pattern', '--mic', 'default')
    Expect-Failure 'audio tracks must be' @('record', '--audio-tracks', 'surround')
    Expect-Failure 'gain must be' @('record', '--mic-gain', '99')
    Expect-Failure 'offset must be' @('record', '--system-audio-offset-ms', '5000')
    Expect-Failure 'region must be' @('record', '--region', '1,2,3')
    Expect-Failure 'region must be' @('record', '--region', '0,0,1,100')
    Expect-Failure 'needs a real screen' @('record', '--test-pattern', '--region', '0,0,640,360')
    Expect-Failure 'window not found' @('record', '--window', 'no-such-window-title-xyz')
    $windows = Invoke-Rec $rec @('windows')
    if ($windows.Code -ne 0) { throw "rec windows failed: $($windows.Text)" }

    # The live A/V sync stimulus must at least compile.
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'flash-beep.ps1') -CheckOnly
    if ($LASTEXITCODE -ne 0) { throw 'flash-beep.ps1 does not compile' }

    # Media Foundation encode and decode without capture.
    $h264 = Record 'h264' 'h264.mp4' @('--test-pattern', '-r', '1080p', '-f', '30', '-t', '2', '--codec', 'h264')
    Expect-Field $h264.Probe 'codec' 'h264'
    Expect-Field $h264.Probe 'width' '1920'
    Expect-Field $h264.Probe 'height' '1080'
    Expect-Field $h264.Probe 'frames' '60'
    Expect-Field $h264.Probe 'audio_tracks' '0'
    Expect-Text $h264.Log 'written 60, dropped 0'

    # Flash + beep through the AAC encoder: measured A/V offset in the decoded file.
    $av = Record 'av' 'av.mp4' @('--test-pattern', '--test-tone', '-r', '1080p', '-f', '30', '-t', '2', '--codec', 'h264')
    Expect-Field $av.Probe 'frames' '60'
    Expect-Field $av.Probe 'audio_tracks' '1'
    Expect-Field $av.Probe 'audio_codec' 'aac'
    Expect-Text $av.Log 'written 60, dropped 0'
    Expect-Within $av.Probe 'audio_duration' 1.95 2.05
    Expect-Within $av.Probe 'av_sync_events' 2 2
    Expect-Within $av.Probe 'av_offset_max_ms' 0 20

    # A system-audio offset moves audio later by exactly that much.
    $offset = Record 'offset' 'offset.mp4' @('--test-pattern', '--test-tone', '--system-audio-offset-ms', '100', '-f', '30', '-t', '2')
    Expect-Within $offset.Probe 'av_sync_events' 2 2
    Expect-Within $offset.Probe 'av_offset_ms' 80 120

    # Mixer: default track is the system + mic mix, followed by both stems.
    $mix = Record 'mix' 'mix.mov' @('--test-pattern', '--test-tone', '--test-mic', '-f', '30', '-t', '2')
    Expect-Field $mix.Probe 'audio_tracks' '3'
    Expect-Field $mix.Probe 'audio_codec' 'aac'
    Expect-Text $mix.Log 'Mix buffers written'
    Expect-Text $mix.Log 'written 60, dropped 0'
    Expect-Within $mix.Probe 'audio_duration' 1.95 2.05
    Expect-Within $mix.Probe 'av_sync_events' 2 2
    Expect-Within $mix.Probe 'av_offset_max_ms' 0 20

    $stems = Record 'stems' 'stems.mp4' @('--test-pattern', '--test-tone', '--test-mic', '--audio-tracks', 'stems', '-f', '30', '-t', '1')
    Expect-Field $stems.Probe 'audio_tracks' '2'
    $mixonly = Record 'mixonly' 'mixonly.mp4' @('--test-pattern', '--test-tone', '--test-mic', '--audio-tracks', 'mix', '-f', '30', '-t', '1')
    Expect-Field $mixonly.Probe 'audio_tracks' '1'

    # Pause/resume through piped stdin: paused time is cut from the movie, A/V stays in sync.
    $pausePath = Join-Path $artifacts 'pause.mp4'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '--test-mic', '-f', '30', '-t', '3', '-o', $pausePath)
    Start-Sleep -Milliseconds 1000
    Send-RecCommand $run 'pause'
    Start-Sleep -Milliseconds 1500
    Send-RecCommand $run 'resume'
    $paused = Stop-RecProcess $run
    $watch.Stop()
    Set-Content -Path (Join-Path $artifacts 'pause.record.log') -Value $paused.Text
    Write-Host $paused.Text
    if ($paused.Code -ne 0) { throw "paused recording failed: $($paused.Text)" }
    Expect-Text $paused.Text 'Paused at'
    Expect-Text $paused.Text 'Resumed at'
    Expect-Text $paused.Text 'written 90, dropped 0'
    Expect-Text $paused.Text 'not in the movie'
    if ($watch.Elapsed.TotalSeconds -lt 4.2) { throw "pause did not hold the recording ($($watch.Elapsed.TotalSeconds) s)" }
    $pause = Get-Probe $rec $pausePath (Join-Path $artifacts 'pause.probe')
    Expect-Field $pause 'frames' '90'
    Expect-Field $pause 'audio_tracks' '3'
    Expect-Within $pause 'audio_duration' 2.95 3.05
    Expect-Within $pause 'av_sync_events' 3 3
    Expect-Within $pause 'av_offset_max_ms' 0 20

    # Replay: "save" writes the last 5 s (from a keyframe) to an MP4 while the recording continues.
    Expect-Failure 'replay must be between' @('record', '--test-pattern', '--replay', '2')
    $replayPath = Join-Path $artifacts 'replay.mp4'
    $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '-f', '30', '-t', '10', '--replay', '5', '-o', $replayPath)
    Start-Sleep -Milliseconds 8000
    Send-RecCommand $run 'save'
    $replayed = Stop-RecProcess $run
    Set-Content -Path (Join-Path $artifacts 'replay.record.log') -Value $replayed.Text
    Write-Host $replayed.Text
    if ($replayed.Code -ne 0) { throw "replay recording failed: $($replayed.Text)" }
    Expect-Text $replayed.Text 'Saved replay'
    Expect-Text $replayed.Text 'written 300, dropped 0'
    $replay = Get-Probe $rec (Join-Path $artifacts 'replay-replay-1.mp4') (Join-Path $artifacts 'replay-clip.probe')
    Expect-Field $replay 'codec' 'h264'
    Expect-Within $replay 'frames' 150 215
    Expect-Within $replay 'audio_duration' 4.8 7.2
    Expect-Within $replay 'av_sync_events' 4 8
    Expect-Within $replay 'av_offset_max_ms' 0 25

    # Overlays: a camera and an image drawn into every frame on the GPU; "hide 1" removes the
    # camera mid-recording. overlay-test.png is 200x100 of (220, 30, 30).
    $png = Join-Path $PSScriptRoot 'overlay-test.png'
    Expect-Failure 'could not open the image' @('record', '--test-pattern', '-t', '1', '--image', (Join-Path $artifacts 'missing.png'), '-o', (Join-Path $artifacts 'missing.mp4'))
    Expect-Failure 'the area after @' @('record', '--test-pattern', '--camera', 'test@0.9,0.9,0.2,0.2')
    $overlayPath = Join-Path $artifacts 'overlay.mp4'
    $run = Start-RecProcess $rec @('record', '--test-pattern', '-r', '1080p', '-f', '30', '-t', '4', '--camera', 'test', '--image', "$png@0.05,0.05,0.2,0.2", '-o', $overlayPath)
    Start-Sleep -Milliseconds 2500
    Send-RecCommand $run 'hide 1'
    $overlaid = Stop-RecProcess $run
    Set-Content -Path (Join-Path $artifacts 'overlay.record.log') -Value $overlaid.Text
    Write-Host $overlaid.Text
    if ($overlaid.Code -ne 0) { throw "overlay recording failed: $($overlaid.Text)" }
    Expect-Text $overlaid.Text 'Hiding camera 1'
    Expect-Text $overlaid.Text 'written 120, dropped 0'
    if (-not ($overlaid.Text -match 'Camera frames (\d+)') -or [int]$Matches[1] -lt 30) { throw 'the test camera delivered too few frames' }
    # The camera's default area at 1080p is 1402,772 480x270; the image fits 96,54 384x216 as 384x192.
    $camera = Get-Probe $rec $overlayPath (Join-Path $artifacts 'overlay-camera.probe') @('--sample', '1450,800,380,200')
    Expect-Color $camera 'sample_0' 'green'
    Expect-Color $camera 'sample_1' 'green'
    Expect-Color $camera 'sample_3' 'white'
    $image = Get-Probe $rec $overlayPath (Join-Path $artifacts 'overlay-image.probe') @('--sample', '150,100,250,100')
    foreach ($second in 0..3) { Expect-Color $image "sample_$second" 'red' }
    # Volume and mute act on the program audio. A lone tone, its only track, starts muted and
    # is unmuted 3.5 s in: no beep with the first flash. Mixed with the mic, the tone is turned
    # down 6 dB, then muted 2.5 s in: the mix has no beeps after that.
    $unmutePath = Join-Path $artifacts 'unmute.mp4'
    $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '--mute-system', '-f', '30', '-t', '6', '-o', $unmutePath)
    Start-Sleep -Milliseconds 3500
    Send-RecCommand $run 'unmute system'
    $unmuted = Stop-RecProcess $run
    Set-Content -Path (Join-Path $artifacts 'unmute.record.log') -Value $unmuted.Text
    Write-Host $unmuted.Text
    if ($unmuted.Code -ne 0) { throw "unmute recording failed: $($unmuted.Text)" }
    Expect-Text $unmuted.Text 'Unmuted system audio'
    $unmute = Get-Probe $rec $unmutePath (Join-Path $artifacts 'unmute.probe')
    Expect-Field $unmute 'audio_tracks' '1'
    Expect-Within $unmute 'av_sync_events' 1 5
    $unmute['first_beep'] = $unmute['beeps'].Split(',')[0]
    Expect-Within $unmute 'first_beep' 0.9 6
    $mutePath = Join-Path $artifacts 'mute.mp4'
    $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '--test-mic', '-f', '30', '-t', '5', '-o', $mutePath)
    Start-Sleep -Milliseconds 1000
    Send-RecCommand $run 'volume system -6'
    Start-Sleep -Milliseconds 1500
    Send-RecCommand $run 'mute system'
    $muted = Stop-RecProcess $run
    Set-Content -Path (Join-Path $artifacts 'mute.record.log') -Value $muted.Text
    Write-Host $muted.Text
    if ($muted.Code -ne 0) { throw "mute recording failed: $($muted.Text)" }
    Expect-Text $muted.Text 'Set system audio to -6.0 dB'
    Expect-Text $muted.Text 'Muted system audio'
    $mute = Get-Probe $rec $mutePath (Join-Path $artifacts 'mute.probe')
    Expect-Field $mute 'audio_tracks' '3'
    Expect-Within $mute 'av_sync_events' 1 3
    $mute['last_beep'] = $mute['beeps'].Split(',')[-1]
    Expect-Within $mute 'last_beep' 0 3.5
    # Crash safety: a recording killed mid-write still decodes up to its last fragment.
    $crashPath = Join-Path $artifacts 'crash.mp4'
    $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '-f', '30', '-t', '0', '--codec', 'h264', '-o', $crashPath)
    Start-Sleep -Seconds 4
    $run.Process.Kill()
    $run.Process.WaitForExit()
    $crash = Get-Probe $rec $crashPath (Join-Path $artifacts 'crash.probe')
    Expect-Within $crash 'frames' 60 120
    Expect-Within $crash 'av_sync_events' 2 4
    Expect-Within $crash 'av_offset_max_ms' 0 20
    # Live streaming: a local MediaMTX server receives the RTMP stream and records it. The
    # received file must decode with the frames and A/V sync of the local recording.
    Expect-Failure 'must start with rtmp://' @('record', '--test-pattern', '-t', '1', '--stream', 'http://127.0.0.1/live/key', '-o', (Join-Path $tmp 'bad-url.mp4'))
    Expect-Failure 'must look like rtmp://' @('record', '--test-pattern', '-t', '1', '--stream', 'rtmp://127.0.0.1/key', '-o', (Join-Path $tmp 'bad-path.mp4'))
    Expect-Failure 'could not connect to 127.0.0.1:1' @('record', '--test-pattern', '-t', '1', '--stream', 'rtmp://127.0.0.1:1/live/key', '-o', (Join-Path $tmp 'refused.mp4'))
    Expect-Failure 'stream bitrate must be' @('record', '--stream-bitrate', '100')
    $mediamtx = Get-MediaMTX (Join-Path $root 'build\tools')
    $received = Join-Path $artifacts 'stream-server'
    $tls = New-TestCertificate (Join-Path $tmp 'tls')
    $server = Start-MediaMTX $mediamtx $received (Join-Path $tmp 'mediamtx') $tls
    try {
        # Stem-only file: the stream still carries the mix.
        $live = Record 'stream' 'stream-local.mp4' @('--test-pattern', '--test-tone', '--test-mic', '--audio-tracks', 'stems',
            '-r', '720p', '-f', '30', '-t', '4', '--stream', 'rtmp://127.0.0.1:1935/live/smokekey1')
        Expect-Text $live.Log 'Streaming to rtmp://127.0.0.1:1935/live/****'
        Expect-Text $live.Log 'Streamed 120 frames'
        Expect-Text $live.Log 'kb/s), skipped 0'
        if ($live.Log.Contains('smokekey1')) { throw 'the stream key was printed' }
        Expect-Field $live.Probe 'audio_tracks' '2'
        # MediaMTX analyzes 2 s of media before it records, so streams here run longer than that.
        $silent = Record 'stream-silent' 'stream-silent-local.mp4' @('--test-pattern', '-r', '720p', '-f', '30', '-t', '3',
            '--stream', 'rtmp://127.0.0.1:1935/live/smokekey2')
        Expect-Text $silent.Log 'Streamed 90 frames'

        # RTMPS checks the server's certificate: this server's self-signed one fails until the
        # test trusts it explicitly.
        Expect-Failure 'is not trusted' @('record', '--test-pattern', '-t', '1', '--stream', 'rtmps://127.0.0.1:1936/live/untrusted',
            '-o', (Join-Path $tmp 'untrusted.mp4'))
        $env:REC_TLS_TRUST_CERT = $tls.Cert
        try {
            $secure = Record 'stream-rtmps' 'stream-rtmps-local.mp4' @('--test-pattern', '--test-tone', '-r', '720p', '-f', '30', '-t', '4',
                '--stream', 'rtmps://127.0.0.1:1936/live/smokekey4')
        } finally { Remove-Item Env:REC_TLS_TRUST_CERT }
        Expect-Text $secure.Log 'Streaming to rtmps://127.0.0.1:1936/live/****'
        Expect-Text $secure.Log 'Streamed 120 frames'

        # A server restart mid-stream: the stream reconnects and resumes at the next keyframe.
        $againPath = Join-Path $artifacts 'stream-reconnect.mp4'
        $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '-r', '720p', '-f', '30', '-t', '10',
            '--stream', 'rtmp://127.0.0.1:1935/live/smokekey5', '-o', $againPath)
        Start-Sleep -Seconds 3
        Stop-MediaMTX $server
        Start-Sleep -Milliseconds 500
        $server = Start-MediaMTX $mediamtx $received (Join-Path $tmp 'mediamtx-2') $tls
        $again = Stop-RecProcess $run
        Set-Content -Path (Join-Path $artifacts 'stream-reconnect.record.log') -Value $again.Text
        Write-Host $again.Text
        if ($again.Code -ne 0) { throw "the stream did not survive a server restart: $($again.Text)" }
        Expect-Text $again.Text 'reconnected 1'
        Expect-Text $again.Text 'Stream lost at 00:0'
        Expect-Text $again.Text 'Stream reconnected at 00:0'
        Expect-Text $again.Text 'written 300, dropped 0'

        # Losing the server ends the stream, not the recording.
        $lostPath = Join-Path $artifacts 'stream-lost.mp4'
        $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '-r', '720p', '-f', '30', '-t', '4',
            '--stream', 'rtmp://127.0.0.1:1935/live/smokekey3', '-o', $lostPath)
        Start-Sleep -Seconds 2
        Stop-MediaMTX $server
        $lost = Stop-RecProcess $run
        Set-Content -Path (Join-Path $artifacts 'stream-lost.record.log') -Value $lost.Text
        Write-Host $lost.Text
        if ($lost.Code -eq 0) { throw 'a lost stream was not reported' }
        Expect-Text $lost.Text 'Stream stopped early'
        Expect-Text $lost.Text 'written 120, dropped 0'
        if ($lost.Text -match 'Streamed 120 frames') { throw 'frames after the loss were counted as streamed' }
        Expect-Field (Get-Probe $rec $lostPath (Join-Path $artifacts 'stream-lost.probe')) 'frames' '120'
    } finally {
        Stop-MediaMTX $server
    }
    $files = @{}
    foreach ($key in 'smokekey1', 'smokekey2') {
        $file = Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received "live\$key") | Select-Object -First 1
        if (-not $file) { throw "MediaMTX recorded nothing for $key" }
        $files[$key] = Get-Probe $rec $file.FullName (Join-Path $artifacts "stream-received-$key.probe")
    }
    $got = $files['smokekey1']
    Expect-Field $got 'codec' 'h264'
    Expect-Field $got 'width' '1280'
    Expect-Field $got 'height' '720'
    # MediaMTX's MP4 writer drops the final frame, whose duration it cannot know.
    Expect-Within $got 'frames' 119 120
    Expect-Field $got 'audio_tracks' '1'
    Expect-Field $got 'audio_codec' 'aac'
    Expect-Within $got 'audio_duration' 3.9 4.05
    Expect-Within $got 'av_sync_events' 4 4
    Expect-Within $got 'av_offset_max_ms' 0 20
    Expect-Within $files['smokekey2'] 'frames' 89 90
    Expect-Field $files['smokekey2'] 'audio_codec' 'aac'
    $tlsGot = Get-Probe $rec (Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received 'live\smokekey4') | Select-Object -First 1).FullName `
        (Join-Path $artifacts 'stream-received-smokekey4.probe')
    Expect-Within $tlsGot 'frames' 119 120
    Expect-Within $tlsGot 'av_sync_events' 4 4
    Expect-Within $tlsGot 'av_offset_max_ms' 0 20
    # The newest server recording is the session after the restart. MediaMTX may not have
    # written the 3 s before the kill, and that session cannot reach 100 frames anyway.
    $resumedFile = Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received 'live\smokekey5') | Sort-Object Name | Select-Object -Last 1
    if (-not $resumedFile) { throw 'MediaMTX recorded nothing for the restarted stream' }
    $resumed = Get-Probe $rec $resumedFile.FullName (Join-Path $artifacts 'stream-received-smokekey5.probe')
    Expect-Within $resumed 'frames' 100 210
    Expect-Within $resumed 'av_sync_events' 3 7
    # MediaMTX starts the resumed recording at the keyframe, which can fall inside an AAC frame.
    # A beep at that instant then decodes faded in and reads late: resuming at 6 s measures
    # 29.1 ms for that beep and under 1 ms for the rest, at 4 s all six read under 1 ms.
    Expect-Within $resumed 'av_offset_ms' -10 10
    Expect-Within $resumed 'av_offset_max_ms' 0 35
    # HEVC needs a hardware encoder or the HEVC Video Extensions; GPU-less CI runners have neither.
    $hevcPath = Join-Path $artifacts 'hevc.mp4'
    $hevcRun = Invoke-Rec $rec @('record', '--test-pattern', '-r', '1080p', '-f', '30', '-t', '1', '--codec', 'hevc', '-o', $hevcPath)
    if ($hevcRun.Code -ne 0 -and $hevcRun.Text.Contains('HEVC encoder')) {
        Write-Host "HEVC skipped: $($hevcRun.Text)"
    } else {
        if ($hevcRun.Code -ne 0) { throw "HEVC recording failed: $($hevcRun.Text)" }
        $hevc = Get-Probe $rec $hevcPath (Join-Path $artifacts 'hevc.probe')
        Expect-Field $hevc 'codec' 'hevc'
        Expect-Field $hevc 'height' '1080'
        Expect-Field $hevc 'frames' '30'
        Expect-Text $hevcRun.Text 'written 30, dropped 0'
    }

    Expect-Failure 'already exists' @('record', '--test-pattern', '-t', '1', '-o', (Join-Path $artifacts 'h264.mp4'))
    Expect-Field (Get-Probe $rec (Join-Path $artifacts 'h264.mp4') (Join-Path $tmp 'recheck.probe')) 'frames' '60'

    if ($env:REC_E2E_CAPTURE -eq '1') {
        $screen = Record 'screen' 'screen.mp4' @('--display', '1', '-r', '1080p', '-f', '30', '-t', '3', '--codec', 'h264')
        Expect-Field $screen.Probe 'codec' 'h264'
        Expect-Field $screen.Probe 'frames' '90'
        $region = Record 'region' 'region.mp4' @('--display', '1', '--region', '0,0,640,360', '-f', '30', '-t', '3', '--codec', 'h264')
        Expect-Field $region.Probe 'width' '640'
        Expect-Field $region.Probe 'height' '360'
        Expect-Field $region.Probe 'frames' '90'
        Expect-Failure 'does not fit display 1' @('record', '--display', '1', '--region', '0,0,99999,100', '-t', '1', '-o', (Join-Path $tmp 'too-big.mp4'))
    }
    # The desktop app, driven through its accessibility interface.
    $app = Join-Path (Split-Path -Parent $rec) 'rec-app.exe'
    $appArtifacts = Join-Path $artifacts 'app'
    New-Item -ItemType Directory -Force $appArtifacts | Out-Null
    & (Join-Path $PSScriptRoot 'app-e2e.ps1') -Rec $rec -App $app -Artifacts $appArtifacts
    Write-Host 'smoke passed'
} finally {
    if ($artifacts -ne $tmp) { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
}
