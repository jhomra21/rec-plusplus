# Shared helpers for the Windows E2E scripts.

function Find-CMake {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        foreach ($vs in & $vswhere -products * -property installationPath) {
            $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $bundled) { return $bundled }
        }
    }
    throw 'cmake not found: install Visual Studio Build Tools with the C++ workload'
}

function Build-Rec([string]$source, [string]$build) {
    $cmake = Find-CMake
    # Out-Host keeps build output out of the function's return value.
    & $cmake -S $source -B $build | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
    & $cmake --build $build --config Release --parallel | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    foreach ($candidate in @((Join-Path $build 'Release\rec.exe'), (Join-Path $build 'rec.exe'))) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "rec.exe not found under $build"
}

# Runs rec and returns its exit code and combined output. Windows PowerShell turns native
# stderr into error records, so the caller's error preference must not apply here.
function Invoke-Rec([string]$rec, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $lines = & $rec @arguments 2>&1 | ForEach-Object { "$_" }
    return @{ Code = $LASTEXITCODE; Text = ($lines -join "`n") }
}

function Get-Probe([string]$rec, [string]$movie, [string]$report, [string[]]$extra = @()) {
    $run = Invoke-Rec $rec (@('probe', $movie) + $extra)
    Set-Content -Path $report -Value $run.Text
    Write-Host ($run.Text -replace "`n", ' ')
    if ($run.Code -ne 0) { throw "rec probe $movie failed: $($run.Text)" }
    $fields = @{}
    foreach ($line in $run.Text -split "`n") {
        $pair = $line.Split('=', 2)
        if ($pair.Count -eq 2) { $fields[$pair[0].Trim()] = $pair[1].Trim() }
    }
    return $fields
}

function Expect-Field($probe, [string]$key, [string]$expected) {
    if ($probe[$key] -ne $expected) { throw "$key=$($probe[$key]), expected $expected" }
}

function Expect-Within($probe, [string]$key, [double]$low, [double]$high) {
    $value = $probe[$key]
    if (-not $value) { throw "$key is missing" }
    $number = [double]::Parse($value, [Globalization.CultureInfo]::InvariantCulture)
    if ($number -lt $low -or $number -gt $high) { throw "$key=$value is outside [$low, $high]" }
}

# A sample_N=R,G,B probe field: green, red, white, blue or black, with room for encoding error.
function Expect-Color($probe, [string]$key, [string]$color) {
    $value = $probe[$key]
    if (-not $value) { throw "$key is missing" }
    $r, $g, $b = $value.Split(',') | ForEach-Object { [int]$_ }
    $ok = switch ($color) {
        'green' { $g -gt 120 -and $g -gt $r + 60 -and $g -gt $b + 60 }
        'red' { $r -gt 170 -and $r -gt $g + 100 -and $r -gt $b + 100 }
        'white' { $r -gt 190 -and $g -gt 190 -and $b -gt 190 }
        'gray' { $r -ge 100 -and $r -le 160 -and $g -ge 100 -and $g -le 160 -and $b -ge 100 -and $b -le 160 -and [Math]::Max($r, [Math]::Max($g, $b)) - [Math]::Min($r, [Math]::Min($g, $b)) -lt 25 }
        'neutral' { [Math]::Max($r, [Math]::Max($g, $b)) - [Math]::Min($r, [Math]::Min($g, $b)) -lt 18 }
        'blue' { $b -gt 170 -and $b -gt $r + 100 -and $b -gt $g + 100 }
        'black' { $r -lt 40 -and $g -lt 40 -and $b -lt 40 }
        'yellow' { $r -gt 190 -and $g -gt 150 -and $b -lt $r - 70 }
        'light blue' { $b -gt 190 -and $b -gt $r + 70 }
    }
    if (-not $ok) { throw "$key=$value is not $color" }
}

# Compiles the mouse helper, which takes a second or more; call it before timed presses start.
function Initialize-Mouse {
    if ('Native.Mouse' -as [type]) { return }
    Add-Type -Name Mouse -Namespace Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
[DllImport("user32.dll")] public static extern bool GetPhysicalCursorPos(out System.Drawing.Point point);
'@ -ReferencedAssemblies System.Drawing
}

# Presses or releases a mouse button at x,y in physical pixels of the desktop, as a user would.
function Set-MouseButton([int]$x, [int]$y, [string]$button, [bool]$down) {
    Initialize-Mouse
    $previous = [Native.Mouse]::SetThreadDpiAwarenessContext([IntPtr]-4)  # Per-monitor aware v2.
    [Native.Mouse]::SetCursorPos($x, $y) | Out-Null
    $at = New-Object System.Drawing.Point
    [Native.Mouse]::GetPhysicalCursorPos([ref]$at) | Out-Null
    [Native.Mouse]::SetThreadDpiAwarenessContext($previous) | Out-Null
    if ([Math]::Abs($at.X - $x) -gt 4 -or [Math]::Abs($at.Y - $y) -gt 4) {
        throw "the pointer is at $($at.X),$($at.Y), not $x,$y; another program moved it"
    }
    $flag = if ($button -eq 'right') { if ($down) { 0x8 } else { 0x10 } } else { if ($down) { 0x2 } else { 0x4 } }
    [Native.Mouse]::mouse_event($flag, 0, 0, 0, [UIntPtr]::Zero)
}

# Waits for one of the E2E helper windows to publish its process window handle and title.
function Wait-TestWindow([System.Diagnostics.Process]$process, [string]$title) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) {
            throw "the $title window process exited before opening (exit code $($process.ExitCode))"
        }
        if ($process.MainWindowHandle -ne [IntPtr]::Zero -and $process.MainWindowTitle -eq $title) {
            Start-Sleep -Milliseconds 300
            return $process
        }
        Start-Sleep -Milliseconds 100
    }
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    throw "the $title window did not open within 30 seconds"
}
# A borderless, topmost window of one color at x,y in physical pixels of the main display,
# in its own process. Stop it with Stop-Process.
function Show-ColorWindow([string]$title, [int]$x, [int]$y, [int]$width, [int]$height, [string]$color, [string]$dir) {
    $script = Join-Path $dir "$title.ps1"
    Set-Content -Path $script -Value @"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Name Dpi -Namespace Native -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[Native.Dpi]::SetProcessDPIAware() | Out-Null
`$form = New-Object Windows.Forms.Form
`$form.Text = '$title'
`$form.FormBorderStyle = 'None'
`$form.StartPosition = 'Manual'
`$form.Bounds = New-Object Drawing.Rectangle $x, $y, $width, $height
`$form.TopMost = `$true
`$form.BackColor = [Drawing.Color]::$color
[Windows.Forms.Application]::Run(`$form)
"@
    $process = Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script) -PassThru -WindowStyle Hidden
    return Wait-TestWindow $process $title
}
# A borderless window of vertical black and white stripes, each $stripe pixels wide.
function Show-StripeWindow([string]$title, [int]$x, [int]$y, [int]$width, [int]$height, [int]$stripe, [string]$dir) {
    $script = Join-Path $dir "$title.ps1"
    Set-Content -Path $script -Value @"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Name Dpi -Namespace Native -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[Native.Dpi]::SetProcessDPIAware() | Out-Null
`$tile = New-Object Drawing.Bitmap ($stripe * 2), 1
`$colors = @([Drawing.Color]::Black, [Drawing.Color]::White)
for (`$i = 0; `$i -lt $stripe * 2; `$i++) { `$tile.SetPixel(`$i, 0, `$colors[[int][math]::Floor(`$i / $stripe)]) }
`$form = New-Object Windows.Forms.Form
`$form.Text = '$title'
`$form.FormBorderStyle = 'None'
`$form.StartPosition = 'Manual'
`$form.Bounds = New-Object Drawing.Rectangle $x, $y, $width, $height
`$form.TopMost = `$true
`$form.BackgroundImage = `$tile
`$form.BackgroundImageLayout = 'Tile'
[Windows.Forms.Application]::Run(`$form)
"@
    $process = Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script) -PassThru -WindowStyle Hidden
    return Wait-TestWindow $process $title
}
function Expect-Text([string]$text, [string]$expected) {
    if (-not $text.Contains($expected)) { throw "expected '$expected' in: $text" }
}

# Starts rec with a writable stdin, for pause/resume/stop commands. Read the result with
# Stop-RecProcess.
function Start-RecProcess([string]$rec, [string[]]$arguments) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $rec
    $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join ' '
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($info)
    # Read both pipes asynchronously so a full pipe can never stall the recorder.
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    return @{ Process = $process; Stdout = $stdout; Stderr = $stderr }
}

function Send-RecCommand($run, [string]$command) {
    $run.Process.StandardInput.WriteLine($command)
    $run.Process.StandardInput.Flush()
}

function Stop-RecProcess($run, [int]$timeoutSeconds = 60) {
    if (-not $run.Process.WaitForExit($timeoutSeconds * 1000)) { $run.Process.Kill(); throw 'rec did not exit' }
    $run.Process.WaitForExit()
    return @{ Code = $run.Process.ExitCode; Text = ($run.Stdout.Result + $run.Stderr.Result).Trim() }
}

function Get-ExecutionState {
    if (-not ('RecPowerInformation' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RecPowerInformation {
    [DllImport("powrprof.dll")]
    public static extern UInt32 CallNtPowerInformation(
        Int32 informationLevel, IntPtr inputBuffer, UInt32 inputBufferLength,
        out UInt32 outputBuffer, UInt32 outputBufferLength);
}
'@
    }
    [uint32]$state = 0
    $status = [RecPowerInformation]::CallNtPowerInformation(16, [IntPtr]::Zero, 0, [ref]$state, 4)
    if ($status -ne 0) { throw "CallNtPowerInformation(SystemExecutionState) failed: $status" }
    return $state
}

# MediaMTX, a local RTMP server for the streaming E2E: pinned release, checksum-verified.
$MediaMtxVersion = 'v1.21.2'
$MediaMtxSha256 = 'a84224f5a490855684e62ba6fb71e80ea2a5e16aed61883bb58380290e93e467'

function Get-MediaMTX([string]$cache) {
    $dir = Join-Path $cache "mediamtx-$MediaMtxVersion"
    $exe = Join-Path $dir 'mediamtx.exe'
    if (Test-Path $exe) { return $exe }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $zip = Join-Path $dir 'mediamtx.zip'
    $url = "https://github.com/bluenviron/mediamtx/releases/download/$MediaMtxVersion/mediamtx_$($MediaMtxVersion)_windows_amd64.zip"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
    $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
    if ($hash -ne $MediaMtxSha256) { Remove-Item $zip; throw "MediaMTX download checksum mismatch: $hash" }
    Expand-Archive -Path $zip -DestinationPath $dir -Force
    Remove-Item $zip
    return $exe
}

# A throwaway self-signed certificate for localhost and 127.0.0.1, written as PEM to $dir
# (cert.pem, key.pem) for MediaMTX's RTMPS listener. It is removed from the user's store at once.
function New-TestCertificate([string]$dir) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $cert = New-SelfSignedCertificate -Subject 'CN=localhost' -KeyAlgorithm RSA -KeyLength 2048 `
        -Provider 'Microsoft Software Key Storage Provider' -KeyExportPolicy Exportable `
        -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddDays(2) `
        -TextExtension @('2.5.29.17={text}DNS=localhost&IPAddress=127.0.0.1', '2.5.29.37={text}1.3.6.1.5.5.7.3.1')
    try {
        $rsa = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($cert)
        $key = $rsa.Key.Export([Security.Cryptography.CngKeyBlobFormat]::Pkcs8PrivateBlob)
        $pem = { param($label, $bytes) "-----BEGIN $label-----`n" + [Convert]::ToBase64String($bytes, 'InsertLineBreaks') + "`n-----END $label-----`n" }
        $certPath = Join-Path $dir 'cert.pem'
        $keyPath = Join-Path $dir 'key.pem'
        [IO.File]::WriteAllText($certPath, (& $pem 'CERTIFICATE' $cert.RawData))
        [IO.File]::WriteAllText($keyPath, (& $pem 'PRIVATE KEY' $key))
        return @{ Cert = $certPath; Key = $keyPath }
    } finally {
        Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
    }
}

# Starts MediaMTX on 127.0.0.1:1935 with RTMP, and WHIP on http://127.0.0.1:8889/<path>/whip
# (media over UDP 127.0.0.1:8189), recording every published stream as fragmented MP4 under
# $recordings. With $tls (from New-TestCertificate) it also serves RTMPS on 127.0.0.1:1936.
function Start-MediaMTX([string]$exe, [string]$recordings, [string]$work, $tls = $null) {
    New-Item -ItemType Directory -Force $recordings, $work | Out-Null
    $config = Join-Path $work 'mediamtx.yml'
    $target = ($recordings -replace '\\', '/') + '/%path/%Y-%m-%d_%H-%M-%S-%f'
    $secure = if ($tls) {
        "rtmpEncryption: optional`nrtmpsAddress: 127.0.0.1:1936`nrtmpServerKey: $($tls.Key -replace '\\', '/')`nrtmpServerCert: $($tls.Cert -replace '\\', '/')"
    } else { '' }
    Set-Content -Path $config -Encoding ASCII -Value @"
logLevel: info
api: no
metrics: no
pprof: no
playback: no
rtsp: no
rtmp: yes
rtmpAddress: 127.0.0.1:1935
$secure
hls: no
webrtc: yes
webrtcAddress: 127.0.0.1:8889
webrtcLocalUDPAddress: 127.0.0.1:8189
webrtcLocalTCPAddress: ''
webrtcIPsFromInterfaces: no
webrtcAdditionalHosts: [127.0.0.1]
webrtcICEServers2: []
srt: no
moq: no
pathDefaults:
  record: yes
  recordPath: $target
  recordFormat: fmp4
  recordPartDuration: 1s
  recordSegmentDuration: 1h
paths:
  all_others:
"@
    $log = Join-Path $work 'mediamtx.log'
    $process = Start-Process -FilePath $exe -ArgumentList "`"$config`"" -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError (Join-Path $work 'mediamtx.err.log')
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) { throw "MediaMTX exited: $(Get-Content -Raw $log)" }
        $client = New-Object Net.Sockets.TcpClient
        try { $client.Connect('127.0.0.1', 1935); return @{ Process = $process; Log = $log } } catch { Start-Sleep -Milliseconds 200 } finally { $client.Dispose() }
    }
    $process.Kill()
    throw 'MediaMTX did not open 127.0.0.1:1935'
}

function Stop-MediaMTX($server) {
    if (-not $server.Process.HasExited) { $server.Process.Kill(); $server.Process.WaitForExit() }
}

# Forwards 127.0.0.1:$port to 127.0.0.1:$target until Stop(), which also cuts every
# connection, so a test can lose one server while others keep running.
function Start-TcpRelay([int]$port, [int]$target) {
    if (-not ('RecRelay' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Threading;
public class RecRelay {
    readonly TcpListener listener;
    readonly List<TcpClient> clients = new List<TcpClient>();
    readonly int target;
    public RecRelay(int port, int target) {
        this.target = target;
        listener = new TcpListener(IPAddress.Loopback, port);
        listener.Start();
        new Thread(Accept) { IsBackground = true }.Start();
    }
    void Accept() {
        try {
            for (;;) {
                var a = listener.AcceptTcpClient();
                var b = new TcpClient("127.0.0.1", target);
                lock (clients) { clients.Add(a); clients.Add(b); }
                Pump(a, b);
                Pump(b, a);
            }
        } catch (Exception) {}
    }
    static void Pump(TcpClient from, TcpClient to) {
        new Thread(() => {
            try { from.GetStream().CopyTo(to.GetStream()); } catch (Exception) {}
            try { to.Client.Shutdown(SocketShutdown.Both); } catch (Exception) {}
        }) { IsBackground = true }.Start();
    }
    public void Stop() {
        listener.Stop();
        lock (clients) { foreach (var c in clients) { try { c.Client.Close(); } catch (Exception) {} } }
    }
}
'@
    }
    return New-Object RecRelay $port, $target
}

# The loudest peak (0..1) on the default output device over $seconds, from the endpoint's
# own meter. Zero when nothing plays.
function Get-OutputPeak([double]$seconds = 2) {
    if (-not ('RecOutputMeter' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class RecDeviceEnumerator {}
[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IRecDeviceEnumerator {
    int EnumAudioEndpoints(int flow, int mask, out IntPtr devices);
    int GetDefaultAudioEndpoint(int flow, int role, out IRecDevice device);
}
[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IRecDevice {
    int Activate(ref Guid iid, int context, IntPtr parameters, [MarshalAs(UnmanagedType.IUnknown)] out object result);
}
[ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IRecMeter { int GetPeakValue(out float peak); }
public static class RecOutputMeter {
    public static float Peak(int milliseconds) {
        var devices = (IRecDeviceEnumerator)new RecDeviceEnumerator();
        IRecDevice device;
        Marshal.ThrowExceptionForHR(devices.GetDefaultAudioEndpoint(0, 1, out device));
        Guid iid = typeof(IRecMeter).GUID;
        object meter;
        Marshal.ThrowExceptionForHR(device.Activate(ref iid, 23, IntPtr.Zero, out meter));
        float loudest = 0;
        for (int waited = 0; waited < milliseconds; waited += 20) {
            float peak;
            ((IRecMeter)meter).GetPeakValue(out peak);
            loudest = Math.Max(loudest, peak);
            System.Threading.Thread.Sleep(20);
        }
        return loudest;
    }
}
'@
    }
    return [RecOutputMeter]::Peak([int]($seconds * 1000))
}

# The phone camera's relay (support/phone-relay) on this computer, in wrangler's local Workers
# runtime, so the E2E checks this checkout's relay rather than the deployed one. Needs Node.js;
# the first run downloads wrangler. Close it with Stop-Relay.
function Start-Relay([string]$directory, [string]$log, [int]$port = 8787) {
    $env:WRANGLER_SEND_METRICS = 'false'
    $process = Start-Process -FilePath 'cmd.exe' -PassThru -NoNewWindow -WorkingDirectory $directory `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -ArgumentList @('/c', 'npx', '--yes', 'wrangler@4.151.3', 'dev', '--ip', '127.0.0.1', '--port', "$port", '--show-interactive-dev-session=false')
    $relay = @{ Process = $process; Url = "http://127.0.0.1:$port" }
    $deadline = (Get-Date).AddSeconds(180)
    while ($true) {
        try { if ((Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 "$($relay.Url)/").StatusCode -eq 200) { return $relay } } catch { }
        if ($process.HasExited -or (Get-Date) -gt $deadline) {
            Stop-Relay $relay
            throw "the relay did not start: $(Get-Content -Raw $log -ErrorAction SilentlyContinue) $(Get-Content -Raw "$log.err" -ErrorAction SilentlyContinue)"
        }
        Start-Sleep -Milliseconds 500
    }
}

function Stop-Relay($relay) {
    if ($relay -and -not $relay.Process.HasExited) { & taskkill.exe /T /F /PID $relay.Process.Id 2>&1 | Out-Null }
}

# Posts an offer to the relay with a wrong signature, as a relay or anyone who knows the room
# could; rec must refuse it. Returns the relay's reply and status.
function Send-ForgedOffer([string]$relay, [string]$token) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $room = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes("rec phone room`n$token")) | ForEach-Object { $_.ToString('x2') })
    return (curl.exe -s -w ' [%{http_code}]' -X POST -H 'Content-Type: application/sdp' -H "Rec-Mac: $('0' * 64)" --data 'v=0' "$relay/rooms/$room/offer")
}
# The base64 SHA-256 of the phone server's public key (its SubjectPublicKeyInfo), which is what
# Chrome's --ignore-certificate-errors-spki-list takes. A browser pinned to it refuses any other
# key, as a real phone does after its person confirmed the certificate warning once.
function Get-PhoneKeyPin([string]$url, [int]$seconds = 15) {
    $uri = [Uri]$url
    $deadline = (Get-Date).AddSeconds($seconds)
    while ($true) {
        $tcp = New-Object Net.Sockets.TcpClient
        try {
            $tcp.Connect($uri.Host, $uri.Port)
            $ssl = New-Object Net.Security.SslStream($tcp.GetStream(), $false, { $true })
            $ssl.AuthenticateAsClient($uri.Host, $null, [Security.Authentication.SslProtocols]::Tls12, $false)
            $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $ssl.RemoteCertificate
            # SubjectPublicKeyInfo of a P-256 key: the algorithm, then the point as a bit string.
            $spki = [byte[]](0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01, 0x06, 0x08, 0x2A,
                             0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00) + $certificate.PublicKey.EncodedKeyValue.RawData
            return [Convert]::ToBase64String([Security.Cryptography.SHA256]::Create().ComputeHash([byte[]]$spki))
        } catch {
            if ((Get-Date) -gt $deadline) { throw "no TLS answer from $($uri.Authority): $($_.Exception.Message)" }
            Start-Sleep -Milliseconds 250
        } finally { $tcp.Dispose() }
    }
}
# A headless Chrome or Edge with a synthetic camera (a green picture with a moving shape) that
# grants camera access and accepts the key rec's self-signed certificate has now, and no other:
# the phone in the phone camera E2E. Read the page with Get-PageText; close it with Stop-Phone.
function Start-Phone([string]$url, [int]$port = 9333) {
    $browser = @("$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
                 "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
                 "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $browser) { throw 'the phone camera E2E needs Chrome or Edge' }
    # Rec's own page needs its key pinned; a relay's page has a real certificate, or none.
    $pin = if ($url -match '^https://[0-9.]+:') { Get-PhoneKeyPin $url } else { '' }
    $profile = Join-Path $env:TEMP "rec-phone-$PID"
    # Chrome and Edge ask Windows whether apps may use the camera and microphone, and that can
    # create the user's switches set to Deny (a runner's default), which then keeps rec from
    # recording a microphone. Stop-Phone puts them back.
    $store = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore'
    $consent = foreach ($key in @("$store\microphone", "$store\microphone\NonPackaged", "$store\webcam", "$store\webcam\NonPackaged")) {
        @{ Key = $key; Existed = (Test-Path $key); Value = (Get-ItemProperty -Path $key -Name Value -ErrorAction SilentlyContinue).Value }
    }    $process = Start-Process -FilePath $browser -PassThru -ArgumentList @('--headless=new', '--use-fake-device-for-media-stream',
        '--use-fake-ui-for-media-stream', "--ignore-certificate-errors-spki-list=$pin", '--inprivate', '--incognito', '--no-first-run',
        '--disable-sync', "--remote-debugging-port=$port", "--user-data-dir=$profile", $url)
    return @{ Process = $process; Port = $port; Profile = $profile; Consent = $consent }
}

function Get-PageText($phone, [string]$expression) {
    $targets = Invoke-RestMethod "http://127.0.0.1:$($phone.Port)/json"
    $page = $targets | Where-Object { $_.type -eq 'page' -and $_.url -match '^https?:' } | Select-Object -First 1
    if (-not $page) { return $null }
    $socket = New-Object System.Net.WebSockets.ClientWebSocket
    $none = [Threading.CancellationToken]::None
    try {
        $socket.ConnectAsync([Uri]$page.webSocketDebuggerUrl, $none).Wait()
        $message = @{ id = 1; method = 'Runtime.evaluate'; params = @{ expression = $expression; returnByValue = $true } } | ConvertTo-Json -Compress -Depth 5
        $bytes = [Text.Encoding]::UTF8.GetBytes($message)
        $socket.SendAsync([ArraySegment[byte]]$bytes, 'Text', $true, $none).Wait()
        $buffer = New-Object byte[] 65536
        $reply = New-Object Text.StringBuilder
        do {
            $read = $socket.ReceiveAsync([ArraySegment[byte]]$buffer, $none)
            $read.Wait()
            [void]$reply.Append([Text.Encoding]::UTF8.GetString($buffer, 0, $read.Result.Count))
        } until ($read.Result.EndOfMessage)
        return ($reply.ToString() | ConvertFrom-Json).result.result.value
    } finally { $socket.Dispose() }
}

# Waits until the phone page's status reads $status; returns the last status seen.
function Wait-PhoneStatus($phone, [string]$status, [int]$seconds = 20) {
    $seen = $null
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        try { $seen = Get-PageText $phone "document.getElementById('status').textContent" } catch { }
        if ($seen -eq $status) { break }
        Start-Sleep -Milliseconds 250
    }
    return $seen
}

function Stop-Phone($phone) {
    Stop-Process -Id $phone.Process.Id -Force -ErrorAction SilentlyContinue
    $phone.Process.WaitForExit(5000) | Out-Null
    Start-Sleep -Milliseconds 500
    Remove-Item -Recurse -Force $phone.Profile -ErrorAction SilentlyContinue
    foreach ($entry in $phone.Consent) {
        if ($null -ne $entry.Value) { Set-ItemProperty -Path $entry.Key -Name Value -Value $entry.Value }
        elseif ($entry.Existed) { Remove-ItemProperty -Path $entry.Key -Name Value -ErrorAction SilentlyContinue }
        else { Remove-Item -Path $entry.Key -Recurse -Force -ErrorAction SilentlyContinue }
    }
}
