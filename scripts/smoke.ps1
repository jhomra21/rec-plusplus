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
    Expect-Failure 'scaler must be' @('record', '--scaler', 'bicubic')
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
    # Diagnostics: what the system allows, one line each; Windows asks nothing for the screen.
    $permissions = Invoke-Rec $rec @('permissions')
    Set-Content -Path (Join-Path $artifacts 'permissions.txt') -Value $permissions.Text
    if ($permissions.Code -ne 0) { throw "rec permissions failed: $($permissions.Text)" }
    if ($permissions.Text -notmatch '(?m)^Screen recording: granted\r?$') { throw "screen permission: $($permissions.Text)" }
    foreach ($name in 'Microphone', 'Camera') {
        if ($permissions.Text -notmatch "(?m)^${name}: (granted|denied \(turn it on: rec permissions --open \w+\))\r?$") { throw "$name permission: $($permissions.Text)" }
    }
    Expect-Failure '--open takes screen, microphone or camera' @('permissions', '--open', 'speakers')
    Expect-Failure 'Windows asks no permission' @('permissions', '--open', 'screen')
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

    # Engine: `rec start` launches the background engine, which keeps recording after each
    # client exits; other clients pause, change and stop it. A short recording ends by itself.
    $env:REC_ENGINE_SOCKET = Join-Path $tmp 'engine.sock'
    Expect-Failure 'the engine is not running' @('status')
    $enginePath = Join-Path $artifacts 'engine.mp4'
    $engineLog = New-Object System.Collections.Generic.List[string]
    function Engine([string[]]$arguments) {
        $run = Invoke-Rec $rec $arguments
        $engineLog.Add("> rec $arguments [exit $($run.Code)]`n$($run.Text)")
        if ($run.Code -ne 0) { throw "rec $arguments failed: $($run.Text)" }
        return $run.Text
    }
    try {
        Expect-Text (Engine @('start', '--test-pattern', '--test-tone', '-f', '30', '--camera', 'test', '-o', $enginePath)) 'Recording to'
        Expect-Failure 'already recording' @('start', '--test-pattern', '-o', (Join-Path $tmp 'second.mp4'))
        Start-Sleep -Milliseconds 2000
        Expect-Text (Engine @('pause')) 'Paused at'
        Start-Sleep -Milliseconds 1000
        Expect-Text (Engine @('resume')) 'Resumed at'
        Expect-Text (Engine @('send', 'hide', '1')) 'Hiding camera 1'
        Expect-Failure 'unknown command' @('send', 'bogus')
        Expect-Text (Engine @('status')) 'state=recording'
        Start-Sleep -Milliseconds 1000
        $stopped = Engine @('stop')
        # Paused for the 1 s sleep plus the time the commands take on a busy machine.
        if ("$stopped" -notmatch 'Paused (0\.9|1\.[0-5]) s') { throw "expected a pause of about 1 s in: $stopped" }
        Expect-Text $stopped "Saved $enginePath"
        Expect-Text (Engine @('status')) "last_saved=$enginePath"
        $shortPath = Join-Path $artifacts 'engine-short.mp4'
        Engine @('start', '--test-pattern', '-f', '30', '-t', '2', '-o', $shortPath) | Out-Null
        Start-Sleep -Milliseconds 4000
        $idle = Engine @('status')
        Expect-Text $idle 'state=idle'
        Expect-Text $idle "last_saved=$shortPath"
        Expect-Text (Engine @('shutdown')) 'the engine stopped'
        Start-Sleep -Milliseconds 500
        Expect-Failure 'the engine is not running' @('status')
    } finally {
        Invoke-Rec $rec @('shutdown') | Out-Null
        Set-Content -Path (Join-Path $artifacts 'engine.client.log') -Value ($engineLog -join "`n")
        Copy-Item (Join-Path $tmp 'engine.log') (Join-Path $artifacts 'engine.log') -ErrorAction SilentlyContinue
        Remove-Item Env:REC_ENGINE_SOCKET -ErrorAction SilentlyContinue
    }
    $engine = Get-Probe $rec $enginePath (Join-Path $artifacts 'engine.probe')
    Expect-Within $engine 'frames' 80 110
    Expect-Within $engine 'av_sync_events' 2 4
    Expect-Within $engine 'av_offset_max_ms' 0 20
    $short = Get-Probe $rec $shortPath (Join-Path $artifacts 'engine-short.probe')
    Expect-Field $short 'frames' '60'
    # A camera that stops mid-recording, as an unplugged one does: the recording goes on with
    # its last frame, and the outage is reported when it happens.
    $env:REC_E2E_LOSE_CAMERA = '1000'
    try { $cameraLost = Record 'camera-lost' 'camera-lost.mp4' @('--test-pattern', '-f', '30', '-t', '3', '--camera', 'test') }
    finally { Remove-Item Env:REC_E2E_LOSE_CAMERA }
    if ($cameraLost.Log -notmatch 'Camera lost at 00:0[01]\.\d; it shows its last frame') { throw "the camera loss was not reported: $($cameraLost.Log)" }
    Expect-Text $cameraLost.Log 'Camera frames 30'
    Expect-Field $cameraLost.Probe 'frames' '90'
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
    # Camera shapes: over the red image filling the frame, the camera at 1152,324 576x324 is a
    # circle (the 324-pixel square against the right edge, from 1404) with a 12-pixel white
    # border and a shadow; mid-recording "style 2 rectangle,border=12,shadow" fills the box.
    # The test camera's white bar, about 29 pixels wide here, crosses at its own phase, so the
    # samples of the camera are wide enough that it cannot turn them white; "top" runs along
    # the square's top, below the border, corners included.
    Expect-Failure 'follows the --camera' @('record', '--test-pattern', '--camera-style', 'circle')
    Expect-Failure "unknown shape 'hexagon'" @('record', '--test-pattern', '--camera', 'test', '--camera-style', 'hexagon')
    Expect-Failure 'the border must be' @('record', '--test-pattern', '--camera', 'test', '--camera-style', 'circle,border=99')
    $shapePath = Join-Path $artifacts 'shape.mp4'
    $run = Start-RecProcess $rec @('record', '--test-pattern', '-r', '1080p', '-f', '30', '-t', '4', '--image', "$png@0,0,1,1",
        '--camera', 'test@0.6,0.3,0.3,0.3', '--camera-style', 'circle,border=12,shadow', '-o', $shapePath)
    Start-Sleep -Milliseconds 2500
    Send-RecCommand $run 'style 2 rectangle,border=12,shadow'
    $shaped = Stop-RecProcess $run
    Set-Content -Path (Join-Path $artifacts 'shape.record.log') -Value $shaped.Text
    Write-Host $shaped.Text
    if ($shaped.Code -ne 0) { throw "the shaped camera recording failed: $($shaped.Text)" }
    Expect-Text $shaped.Text 'Styled camera 2 rectangle,border=12:ffffff,shadow'
    Expect-Text $shaped.Text 'written 120, dropped 0'
    $shapeAt = @{ center = '1446,476,240,20'; box = '1170,476,190,20'; corner = '1408,340,8,8'; top = '1408,340,308,8'; border = '1560,326,10,6'; shadow = '1560,652,10,6' }
    $shape = @{}
    foreach ($part in $shapeAt.Keys) { $shape[$part] = Get-Probe $rec $shapePath (Join-Path $artifacts "shape-$part.probe") @('--sample', $shapeAt[$part]) }
    Expect-Color $shape.center 'sample_1' 'green'
    Expect-Color $shape.box 'sample_1' 'red'
    Expect-Color $shape.corner 'sample_1' 'red'
    Expect-Color $shape.border 'sample_1' 'white'
    $r, $g, $b = $shape.shadow['sample_1'].Split(',') | ForEach-Object { [int]$_ }
    if ($r -gt 200 -or $r -lt 120) { throw "no drop shadow below the camera: $($shape.shadow['sample_1'])" }
    Expect-Color $shape.center 'sample_3' 'green'
    Expect-Color $shape.box 'sample_3' 'green'
    Expect-Color $shape.top 'sample_3' 'green'
    # A second canvas: a vertical 720x1280 movie of the same recording. The middle third of the
    # test pattern fills it and the camera sits near the bottom; mid-recording
    # "place canvas2 1 0 ..." hides the camera there while the main movie keeps showing it.
    Expect-Failure 'need --canvas2' @('record', '--test-pattern', '--canvas2-output', (Join-Path $artifacts 'no-canvas.mp4'))
    Expect-Failure 'must be even' @('record', '--test-pattern', '-t', '1', '--canvas2', '721x1280', '-o', (Join-Path $artifacts 'odd-canvas.mp4'))
    Expect-Failure 'add the overlay first' @('record', '--test-pattern', '--canvas2', 'vertical', '--canvas2-place', '1:0,0,1,1')
    $mainPath = Join-Path $artifacts 'canvas2-main.mp4'
    $verticalPath = Join-Path $artifacts 'canvas2-vertical.mp4'
    $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '-r', '1080p', '-f', '30', '-t', '5', '--camera', 'test',
        '--canvas2', '720x1280', '--canvas2-place', 'screen:0,0,1,1:0.34,0,0.32,1', '--canvas2-place', '1:0.6,0.8,0.35,0.15',
        '--canvas2-output', $verticalPath, '-o', $mainPath)
    Start-Sleep -Milliseconds 2500
    Send-RecCommand $run 'place canvas2 1 0 0.6,0.8,0.35,0.15'
    $two = Stop-RecProcess $run
    Set-Content -Path (Join-Path $artifacts 'canvas2.record.log') -Value $two.Text
    Write-Host $two.Text
    if ($two.Code -ne 0) { throw "the two-canvas recording failed: $($two.Text)" }
    # On a slow CI GPU the second canvas may skip a few frames; the main canvas may not.
    if ($two.Text -notmatch 'Second canvas 720x1280: (14[5-9]|150) frames, skipped [0-5](\D|$)') { throw "the second canvas lost frames: $($two.Text)" }
    Expect-Text $two.Text 'written 150, dropped 0'
    $vertical = Get-Probe $rec $verticalPath (Join-Path $artifacts 'canvas2-vertical.probe')
    Expect-Field $vertical 'codec' 'h264'
    Expect-Field $vertical 'width' '720'
    Expect-Field $vertical 'height' '1280'
    Expect-Within $vertical 'frames' 145 150
    Expect-Field $vertical 'audio_codec' 'aac'
    Expect-Within $vertical 'av_sync_events' 5 5
    Expect-Within $vertical 'av_offset_max_ms' 0 20
    # The camera's area is 432,1024 252x192 in the vertical movie and 1402,772 480x270 in the main one.
    $verticalCamera = Get-Probe $rec $verticalPath (Join-Path $artifacts 'canvas2-vertical-camera.probe') @('--sample', '460,1050,190,140')
    Expect-Color $verticalCamera 'sample_1' 'green'
    Expect-Color $verticalCamera 'sample_4' 'white'
    $mainCamera = Get-Probe $rec $mainPath (Join-Path $artifacts 'canvas2-main-camera.probe') @('--sample', '1450,800,380,200')
    Expect-Color $mainCamera 'sample_1' 'green'
    Expect-Color $mainCamera 'sample_4' 'green'
    Expect-Field (Get-Probe $rec $mainPath (Join-Path $artifacts 'canvas2-main.probe')) 'width' '1920'    # Scene layers: the test pattern in the bottom middle, a blue window as a window layer top
    # left, and the part of the display it covers top right. Mid-recording "hide 1" hides the
    # window layer and "move screen" moves the pattern to the bottom left.
    Expect-Failure 'the screen area must be' @('record', '--test-pattern', '--screen-area', '0.5,0.5,0.6,0.5')
    Expect-Failure 'needs a display number' @('record', '--test-pattern', '--add-display', 'main')
    Expect-Failure 'display 9 not found' @('record', '--test-pattern', '-t', '1', '--add-display', '9', '-o', (Join-Path $artifacts 'no-display.mp4'))
    $blue = Show-ColorWindow 'rec-scene-blue' 100 100 640 360 'Blue' $tmp
    try {
        $scenePath = Join-Path $artifacts 'scene.mp4'
        $run = Start-RecProcess $rec @('record', '--test-pattern', '-r', '1080p', '-f', '30', '-t', '5', '--screen-area', '0.25,0.5,0.5,0.5',
            '--add-window', 'rec-scene-blue@0,0,0.5,0.5', '--add-display', '1:200,150,400,225@0.5,0,0.5,0.5', '-o', $scenePath)
        Start-Sleep -Milliseconds 2500
        Send-RecCommand $run 'hide 1'
        Send-RecCommand $run 'move screen 0,0.5,0.5,0.5'
        $scened = Stop-RecProcess $run
    } finally {
        Stop-Process -Id $blue.Id -Force -ErrorAction SilentlyContinue
    }
    Set-Content -Path (Join-Path $artifacts 'scene.record.log') -Value $scened.Text
    Write-Host $scened.Text
    if ($scened.Code -ne 0) { throw "scene recording failed: $($scened.Text)" }
    Expect-Text $scened.Text 'Hiding window 1'
    Expect-Text $scened.Text 'Moved the screen to 0,0.5,0.5,0.5'
    Expect-Text $scened.Text 'written 150, dropped 0'
    $window = Get-Probe $rec $scenePath (Join-Path $artifacts 'scene-window.probe') @('--sample', '280,170,400,200')
    $display = Get-Probe $rec $scenePath (Join-Path $artifacts 'scene-display.probe') @('--sample', '1240,170,400,200')
    $before = Get-Probe $rec $scenePath (Join-Path $artifacts 'scene-screen-before.probe') @('--sample', '1000,700,400,250')
    $after = Get-Probe $rec $scenePath (Join-Path $artifacts 'scene-screen-after.probe') @('--sample', '100,700,300,250')
    # A slow runner may not have a layer's first frame yet at 0 s; until then it is black.
    Expect-Color $window 'sample_1' 'blue'
    Expect-Color $window 'sample_4' 'black'
    foreach ($second in 1..4) { Expect-Color $display "sample_$second" 'blue' }
    Expect-Color $before 'sample_0' 'white'
    Expect-Color $before 'sample_4' 'black'
    Expect-Color $after 'sample_0' 'black'
    Expect-Color $after 'sample_4' 'white'
    # Click marks: the left button held over a test window, then the right one elsewhere. The
    # marks land where those points of display 1 fall in the test pattern's frame.
    $display = (& $rec devices) | Select-String '^\s+1: .*\((\d+)x(\d+), id' | Select-Object -First 1
    if (-not $display) { throw 'display 1 is not listed' }
    $screenW = [double]$display.Matches[0].Groups[1].Value
    $screenH = [double]$display.Matches[0].Groups[2].Value
    $fit = [Math]::Min(1920 / $screenW, 1080 / $screenH)
    $frameAt = { param($x, $y) "$([int]((1920 - $screenW * $fit) / 2 + $x * $fit) - 6),$([int]((1080 - $screenH * $fit) / 2 + $y * $fit) - 6),12,12" }
    $pad = Show-ColorWindow 'rec-click-pad' 100 100 800 600 'Gray' $tmp
    Initialize-Mouse
    try {
        $clicksPath = Join-Path $artifacts 'clicks.mp4'
        $run = Start-RecProcess $rec @('record', '--test-pattern', '--clicks', '-r', '1080p', '-f', '30', '-t', '9', '-o', $clicksPath)
        Start-Sleep -Milliseconds 1000
        Set-MouseButton 300 300 'left' $true
        Start-Sleep -Milliseconds 2500
        Set-MouseButton 300 300 'left' $false
        Start-Sleep -Milliseconds 1000
        Set-MouseButton 700 500 'right' $true
        Start-Sleep -Milliseconds 3000
        Set-MouseButton 700 500 'right' $false
        $clicked = Stop-RecProcess $run
    } finally {
        Stop-Process -Id $pad.Id -Force -ErrorAction SilentlyContinue
    }
    Set-Content -Path (Join-Path $artifacts 'clicks.record.log') -Value $clicked.Text
    if ($clicked.Code -ne 0) { throw "click recording failed: $($clicked.Text)" }
    Expect-Text $clicked.Text 'written 270, dropped 0'
    $left = Get-Probe $rec $clicksPath (Join-Path $artifacts 'clicks-left.probe') @('--sample', (& $frameAt 300 300))
    $right = Get-Probe $rec $clicksPath (Join-Path $artifacts 'clicks-right.probe') @('--sample', (& $frameAt 700 500))
    # Over the white flash: half yellow, then the pattern once the mark has faded.
    Expect-Color $left 'sample_2' 'yellow'
    Expect-Color $left 'sample_8' 'white'
    Expect-Color $right 'sample_2' 'white'
    Expect-Color $right 'sample_6' 'light blue'
    # Scaling quality: 1-pixel stripes beside 8-pixel stripes, a 1920x1080 region recorded at
    # 720p. Lanczos must turn the fine stripes into flat gray (no moire) and keep the coarse
    # ones sharp; the fast scaler's numbers are kept for comparison.
    if ($screenW -ge 1920 -and $screenH -ge 1080) {
        $fine = Show-StripeWindow 'rec-stripes-fine' 0 0 960 1080 1 $tmp
        $coarse = Show-StripeWindow 'rec-stripes-coarse' 960 0 960 1080 8 $tmp
        $spreads = @{}
        try {
            foreach ($scaler in 'lanczos', 'fast') {
                $scaled = Record "scaler-$scaler" "scaler-$scaler.mp4" @('--region', '0,0,1920,1080', '-r', '720p', '-f', '30', '-t', '2',
                    '--no-cursor', '--scaler', $scaler)
                if ($scaled.Probe.width -ne '1280' -or $scaled.Probe.height -ne '720') { throw "scaler-$scaler is $($scaled.Probe.width)x$($scaled.Probe.height)" }
                $finePath = Join-Path $artifacts "scaler-$scaler.mp4"
                $spreads["$scaler-fine"] = [double](Get-Probe $rec $finePath (Join-Path $artifacts "scaler-$scaler-fine.probe") @('--sample', '100,100,440,520'))['sample_1_spread']
                $spreads["$scaler-coarse"] = [double](Get-Probe $rec $finePath (Join-Path $artifacts "scaler-$scaler-coarse.probe") @('--sample', '740,100,440,520'))['sample_1_spread']
            }
        } finally {
            Stop-Process -Id $fine.Id, $coarse.Id -Force -ErrorAction SilentlyContinue
        }
        $summary = ($spreads.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$([Math]::Round($_.Value, 1))" }) -join ' '
        Set-Content -Path (Join-Path $artifacts 'scaler.txt') -Value $summary
        Write-Host "Luma spread (fine stripes should be flat, coarse ones sharp): $summary"
        if ($spreads['lanczos-fine'] -gt 4) { throw "Lanczos left moire in the fine stripes: $summary" }
        if ($spreads['lanczos-coarse'] -lt 90) { throw "Lanczos blurred the coarse stripes: $summary" }
    } else {
        Write-Host "Skipping the scaling-quality check: display 1 is ${screenW}x${screenH}, it needs 1920x1080"
    }    # Volume and mute act on the program audio. A lone tone, its only track, starts muted and
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
    Expect-Failure 'must start with rtmp://' @('record', '--test-pattern', '-t', '1', '--stream', 'ftp://127.0.0.1/live/key', '-o', (Join-Path $tmp 'bad-url.mp4'))
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

        # WHIP (WebRTC) beside RTMP from one encode: H.264 over SRTP with Opus audio, and the
        # bearer token after '#' kept out of the log.
        Expect-Failure 'could not connect to 127.0.0.1:1' @('record', '--test-pattern', '-t', '1', '--stream', 'http://127.0.0.1:1/live/key/whip',
            '-o', (Join-Path $tmp 'refused-whip.mp4'))
        $whip = Record 'stream-whip' 'stream-whip-local.mp4' @('--test-pattern', '--test-tone', '-r', '720p', '-f', '30', '-t', '4',
            '--stream', 'rtmp://127.0.0.1:1935/live/smokekey12', '--stream', 'http://127.0.0.1:8889/live/smokewhip/whip#smoketoken')
        Expect-Text $whip.Log 'Streaming to rtmp://127.0.0.1:1935/live/****, http://127.0.0.1:8889/****'
        Expect-Text $whip.Log 'http://127.0.0.1:8889/****: 120 frames'
        Expect-Text $whip.Log 'rtmp://127.0.0.1:1935/live/****: 120 frames'
        if ($whip.Log -match 'smokewhip|smoketoken|smokekey12') { throw 'a WHIP path or token was printed' }

        # Several servers: one encode, sent to each over its own connection.
        Expect-Failure 'at most 4 --stream' (@('record', '--test-pattern') + (1..5 | ForEach-Object { '--stream'; "rtmp://127.0.0.1:1935/live/many$_" }))
        Expect-Failure 'rtmp://127.0.0.1:1/live/****: could not connect' @('record', '--test-pattern', '-t', '1',
            '--stream', 'rtmp://127.0.0.1:1935/live/refusedpair', '--stream', 'rtmp://127.0.0.1:1/live/key', '-o', (Join-Path $tmp 'refused-pair.mp4'))
        $env:REC_TLS_TRUST_CERT = $tls.Cert
        try {
            $multi = Record 'stream-multi' 'stream-multi-local.mp4' @('--test-pattern', '--test-tone', '-r', '720p', '-f', '30', '-t', '4',
                '--stream', 'rtmp://127.0.0.1:1935/live/smokekey6', '--stream', 'rtmps://127.0.0.1:1936/live/smokekey7')
        } finally { Remove-Item Env:REC_TLS_TRUST_CERT }
        Expect-Text $multi.Log 'Streaming to rtmp://127.0.0.1:1935/live/****, rtmps://127.0.0.1:1936/live/****'
        Expect-Text $multi.Log 'rtmp://127.0.0.1:1935/live/****: 120 frames'
        Expect-Text $multi.Log 'rtmps://127.0.0.1:1936/live/****: 120 frames'
        if ($multi.Log.Contains('smokekey')) { throw 'a stream key was printed' }

        # Both canvases live at once, each to its own server: landscape and vertical.
        Expect-Failure 'at most 4 stream servers across both canvases' (@('record', '--test-pattern', '--canvas2', 'vertical') +
            (1..3 | ForEach-Object { '--stream'; "rtmp://127.0.0.1:1935/live/a$_" }) + (1..2 | ForEach-Object { '--canvas2-stream'; "rtmp://127.0.0.1:1935/live/b$_" }))
        $canvases = Record 'stream-canvas2' 'stream-canvas2-local.mp4' @('--test-pattern', '--test-tone', '-r', '720p', '-f', '30', '-t', '4',
            '--stream', 'rtmp://127.0.0.1:1935/live/smokekey10', '--canvas2', '720x1280', '--canvas2-place', 'screen:0,0,1,1:0.34,0,0.32,1',
            '--canvas2-stream', 'rtmp://127.0.0.1:1935/live/smokekey11')
        Expect-Text $canvases.Log 'Streaming to rtmp://127.0.0.1:1935/live/****; the second canvas: rtmp://127.0.0.1:1935/live/****'
        if ($canvases.Log -notmatch 'Second canvas 720x1280: (11[5-9]|120) frames, skipped [0-5](\D|$)') { throw "the second canvas lost frames: $($canvases.Log)" }
        if ($canvases.Log.Contains('smokekey')) { throw 'a stream key was printed' }
        # Losing one server ends only its stream.
        $relay = Start-TcpRelay 1937 1935
        $onePath = Join-Path $artifacts 'stream-one-lost.mp4'
        try {
            $run = Start-RecProcess $rec @('record', '--test-pattern', '--test-tone', '-r', '720p', '-f', '30', '-t', '5',
                '--stream', 'rtmp://127.0.0.1:1935/live/smokekey8', '--stream', 'rtmp://127.0.0.1:1937/live/smokekey9', '-o', $onePath)
            Start-Sleep -Seconds 2
        } finally { $relay.Stop() }
        $one = Stop-RecProcess $run
        Set-Content -Path (Join-Path $artifacts 'stream-one-lost.record.log') -Value $one.Text
        Write-Host $one.Text
        if ($one.Code -eq 0) { throw 'a lost server was not reported' }
        Expect-Text $one.Text 'Stream rtmp://127.0.0.1:1937/live/**** lost at 00:0'
        Expect-Text $one.Text 'rtmp://127.0.0.1:1935/live/****: 150 frames'
        Expect-Text $one.Text 'Stream stopped early: rtmp://127.0.0.1:1937/live/****: lost the connection'
        Expect-Text $one.Text 'written 150, dropped 0'

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
    foreach ($key in 'smokewhip', 'smokekey12') {
        $file = Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received "live\$key") | Select-Object -First 1
        if (-not $file) { throw "MediaMTX recorded nothing for $key" }
        $got = Get-Probe $rec $file.FullName (Join-Path $artifacts "stream-received-$key.probe")
        Expect-Field $got 'codec' 'h264'
        Expect-Field $got 'audio_codec' $(if ($key -eq 'smokewhip') { 'opus' } else { 'aac' })
        Expect-Within $got 'frames' 119 120
        Expect-Within $got 'av_sync_events' 4 4
        # MediaMTX writes no edit list for Opus either: its 312-sample pre-skip reads 6.5 ms late.
        Expect-Within $got 'av_offset_max_ms' 0 20
    }
    foreach ($key in 'smokekey6', 'smokekey7') {
        $file = Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received "live\$key") | Select-Object -First 1
        if (-not $file) { throw "MediaMTX recorded nothing for $key" }
        $both = Get-Probe $rec $file.FullName (Join-Path $artifacts "stream-received-$key.probe")
        Expect-Within $both 'frames' 119 120
        Expect-Within $both 'av_sync_events' 4 4
        Expect-Within $both 'av_offset_max_ms' 0 20
    }
    foreach ($key in 'smokekey10', 'smokekey11') {
        $file = Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received "live\$key") | Select-Object -First 1
        if (-not $file) { throw "MediaMTX recorded nothing for $key" }
        $canvas = Get-Probe $rec $file.FullName (Join-Path $artifacts "stream-received-$key.probe")
        Expect-Field $canvas 'width' $(if ($key -eq 'smokekey10') { '1280' } else { '720' })
        Expect-Field $canvas 'height' $(if ($key -eq 'smokekey10') { '720' } else { '1280' })
        # The second canvas (smokekey11) may skip up to 5 frames on a slow CI GPU.
        Expect-Within $canvas 'frames' $(if ($key -eq 'smokekey10') { 119 } else { 114 }) 120
        Expect-Within $canvas 'av_sync_events' 4 4
        Expect-Within $canvas 'av_offset_max_ms' 0 20
    }
    $file = Get-ChildItem -Recurse -Filter *.mp4 (Join-Path $received 'live\smokekey8') | Select-Object -First 1
    if (-not $file) { throw 'MediaMTX recorded nothing for the server that stayed' }
    Expect-Within (Get-Probe $rec $file.FullName (Join-Path $artifacts 'stream-received-smokekey8.probe')) 'frames' 149 150
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

    Expect-Failure 'ProRes needs macOS' @('record', '--test-pattern', '-t', '1', '--codec', 'prores', '-o', (Join-Path $tmp 'prores.mov'))
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
