<#
.SYNOPSIS
  P5-C long run (gate G5): 64 tracks with effects, 4 video mixes, a recording and 4 remotes for hours on the
  real PC, and the design doc's performance targets measured on the way.

.DESCRIPTION
  Builds Release (mixd, capd, dc-ui, dc-cli, dc-av-flash, dc-loopback-probe, dc-audio-check, dc-v6-preview)
  unless -NoBuild, then:

    mixd A     the engine under test on 0.0.0.0:47800 (--allow-remote: the remotes need no pairing here),
               --test-pulse 2 (a 1 kHz burst on the master every 2 s, for capd avsync), -Mode, -BlockSize.
                 track 1 "Mic"   -InputDevice (UR22), recorded before the fader, fader closed
                 track 2 "Game"  process dc-p5-app.exe (a copy of dc-loopback-probe playing 700 Hz at -30 dBFS:
                                 dc-audio-check fits blocks above -40 dBFS only), also
                                 recorded to WAV by the track recorder (glitch check over the whole run)
                 track 3 "LAN"   LAN input from mixd B (500 Hz test tone), -40 dB
                 track 4 "Vmic"  CABLE Output (what the virtual mic receives), no output: its meter checks the vmic
                 tracks 5..64    the same game app, -60 dB, each with EQ + compressor (+ reverb on every 4th,
                                 + delay on every 8th), low cut / high cut on, panned
                 master          limiter; out 1 = -Device (UR22, the clock), out 2 = vmic CABLE Input,
                                 out 3 = LAN stream 1 (Opus) = the remotes' monitor audio
    mixd B     a second engine on this PC (fake devices, 500 Hz tone) sending LAN stream 1 to A
    capd       4 video mixes, all used at once:
                 mix 1 -> recording   the primary monitor + the virtual camera (= mix 2) as a camera source
                 mix 2 -> virtual camera  a text on a colour
                 mix 3 -> remote preview  the primary monitor + a text; its scene changes every -ActionMinutes
                 mix 4 -> LAN video send  the primary monitor, watched by dc-v6-preview --stream video/lan
               recording: master, mic, game, LAN as 4 audio streams (1080p60 H.264 by default)
    dc-ui      the PC's own UI, plus -Remotes copies of dc-ui --remote --measure (preview + monitor audio). The
               Surface is the 4th remote, started separately:
                 dc-ui.exe --remote --connect <this PC>:47800 --measure <seconds> --monitor 1
    dc-av-flash  the top-left of the primary monitor flashes with the bursts (A/V offset over the whole file)

  Every -SampleSeconds: xruns, engine CPU, recording state, vcam / sources / inputs, remote count, and the CPU
  and memory of every process (samples.csv). Every -ActionMinutes: some faders move, mix 3 switches scenes, a
  snapshot is taken and recalled (what a person at the remote would do).

  At the end: the recording (dropped frames, capd levels per stream, capd avsync over the whole file), the WAV
  of track 2 (dc-audio-check --tone 700: glitches), memory growth per process, capd CPU (design target <= 5%),
  the remotes' preview delay and memory. Writes build\p5\<time>\p5_<time>.md and prints it.

  Turn the speakers down (bursts every 2 s), leave the top-left of the primary monitor uncovered and do not use
  the PC meanwhile. Needs VB-CABLE and the virtual camera installed. Free disk: about 10 GB per hour.

.EXAMPLE
  .\scripts\p5-soak.ps1 -Hours 3
  .\scripts\p5-soak.ps1 -NoBuild -Hours 0.1 -SampleSeconds 10 -ActionMinutes 1   # a quick dry run
#>
param(
    [double]$Hours = 3,
    [int]$Remotes = 3,
    [int]$Tracks = 64,
    [ValidateSet('shared', 'exclusive')][string]$Mode = 'shared',
    [ValidateSet(128, 256, 512)][int]$BlockSize = 256,
    [string]$DeviceName = 'UR22',
    [string]$Device = '',
    [string]$InputDevice = '',
    [int]$SampleSeconds = 30,
    [double]$ActionMinutes = 5,
    [int]$BitrateKbps = 20000,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    # cmake writes warnings to stderr: with 'Stop' a redirected run (*>&1 | Tee-Object) would turn them into errors
    $ErrorActionPreference = 'Continue'
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd capd dc-ui dc-cli dc-av-flash dc-loopback-probe dc-audio-check dc-v6-preview
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
    $ErrorActionPreference = 'Stop'
}

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$capd = Join-Path $bin 'apps/capd/Release/capd.exe'
$ui = Join-Path $bin 'apps/ui/Release/dc-ui.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$flash = Join-Path $bin 'tools/av_flash/Release/dc-av-flash.exe'
$probe = Join-Path $bin 'tools/loopback_probe/Release/dc-loopback-probe.exe'
$audioCheck = Join-Path $bin 'tools/audio_check/Release/dc-audio-check.exe'
$v6 = Join-Path $bin 'tools/v6_preview/Release/dc-v6-preview.exe'
foreach ($exe in @($mixd, $capd, $ui, $cli, $flash, $probe, $audioCheck, $v6)) { if (-not (Test-Path $exe)) { throw "missing $exe (build first)" } }
if ($env:DC_QT_DIR -and -not (Test-Path (Join-Path (Split-Path $ui) 'Qt6Core.dll'))) {
    $env:PATH = (Join-Path $env:DC_QT_DIR 'bin') + ';' + $env:PATH
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path $root "build/p5/$stamp"
$recDir = Join-Path $out 'rec'
$takeDir = Join-Path $out 'takes'
New-Item -ItemType Directory -Force $out, $recDir, $takeDir | Out-Null
$port = 47800
$portB = 47891
$mediaB = 47892
$seconds = [int]($Hours * 3600)

$ip = (Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } |
       Select-Object -First 1).IPv4Address.IPAddress
if (-not $ip) { throw 'no LAN address' }
$freeGb = [math]::Round((Get-PSDrive -Name (Split-Path -Qualifier $out).TrimEnd(':')).Free / 1GB, 1)
$needGb = [math]::Ceiling($Hours * ($BitrateKbps / 8000.0 * 3600 / 1024 + 1.2) + 5)
if ($freeGb -lt $needGb) { throw "free disk $freeGb GB, needs about $needGb GB for $Hours h" }

# ---------------------------------------------------------------------------------------------------- helpers
function Get-Param([string]$path, [int]$p = $port) {
    $line = & $cli --port $p snapshot $path | Where-Object { $_ -like "$path = *" } | Select-Object -First 1
    if (-not $line) { return $null }
    $v = $line.Substring($path.Length + 3)
    if ($v.StartsWith('"')) { return $v.Trim('"') -replace '\\\\', '\' }
    return $v
}
function Set-Param([string]$path, $value, [int]$p = $port) {
    if ("$value" -eq '') { $value = '""' }  # Windows PowerShell 5.1 drops an empty native argument
    $r = & $cli --port $p set $path $value
    if ($r -ne 'ok') { throw "set $path $value -> $r" }
}
function Call([string]$method, [string[]]$kv = @(), [int]$p = $port) {
    $r = & $cli --port $p call $method @kv
    if ($r -notlike 'ok*') { throw "call $method $($kv -join ' ') -> $r" }
    return ($r -replace '^ok ?', '').Trim('"')
}
function Wait-Until([scriptblock]$cond, [double]$secs = 15) {
    $end = (Get-Date).AddSeconds($secs)
    do {
        try { if (& $cond) { return $true } } catch { }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $end)
    return $false
}
function Start-Logged([string]$exe, [string[]]$arguments, [string]$name, [switch]$Visible) {
    $log = Join-Path $out "$name.log"
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle $(if ($Visible) { 'Normal' } else { 'Hidden' }) `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle  # keeps the exit code readable after exit
    return $p
}
function Stop-Quiet($p) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue; $null = $p.WaitForExit(5000) } }

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class P5Win {
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);
}
'@
# The monitor must stay on and the PC unlocked for the whole run (the recording captures it): ES_CONTINUOUS |
# ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED for as long as this script runs, without touching the power settings.
$null = [P5Win]::SetThreadExecutionState([uint32]2147483651)  # 0x80000003 (a hex literal is Int32 in PS 5.1)
# Windows go to the right half of the primary monitor, away from the flash in the top-left quarter.
Add-Type -AssemblyName System.Windows.Forms
$screen = [Windows.Forms.Screen]::PrimaryScreen.Bounds
function Place-Window($p, [int]$slot) {
    if (-not (Wait-Until { $p.Refresh(); $p.MainWindowHandle -ne [IntPtr]::Zero } 20)) { return }
    $w = [int]($screen.Width * 0.3); $h = [int]($screen.Height * 0.38)
    $x = [int]($screen.Width * 0.52) + ($slot % 2) * [int]($screen.Width * 0.16)
    $y = [int]($screen.Height * 0.08) + [math]::Floor($slot / 2) * [int]($screen.Height * 0.28)
    $null = [P5Win]::MoveWindow($p.MainWindowHandle, $x, $y, $w, $h, $true)
}

$watchFile = Join-Path $out 'watch.txt'
function Meter-Db([int]$id, [int]$lines = 3) {
    $all = @(Get-Content $watchFile -Tail 400 -ErrorAction SilentlyContinue | Where-Object { $_ -like 'meters seq*' } | Select-Object -Last $lines)
    $best = -999.0
    foreach ($l in $all) { if ($l -match " $id=(-?[\d.]+)dB") { $best = [math]::Max($best, [double]$Matches[1]) } }
    return $best
}

$checks = @()
function Add-Check([string]$group, [string]$what, [bool]$ok, [string]$detail) {
    $script:checks += [pscustomobject]@{ group = $group; what = $what; result = $(if ($ok) { 'PASS' } else { 'FAIL' }); detail = $detail }
    Write-Host ("{0}  [{1}] {2}: {3}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $group, $what, $detail) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}
function Add-Info([string]$group, [string]$what, [string]$detail) {
    $script:checks += [pscustomobject]@{ group = $group; what = $what; result = 'INFO'; detail = $detail }
    Write-Host "INFO  [$group] ${what}: $detail" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------------------------------- devices
$listing = & $mixd --list-devices
$listing | Set-Content -Encoding utf8 (Join-Path $out 'devices.txt')
function Pick([string]$prefix, [string]$like) {
    $l = $listing | Where-Object { $_ -like "$prefix *" -and $_ -like '* ok *' -and $_ -like "*$like*" } | Select-Object -First 1
    if ($l -and $l -match '\[(.+)\]$') { return $Matches[1] }
    return ''
}
if (-not $Device) { $Device = Pick 'out' $DeviceName }
if (-not $InputDevice) { $InputDevice = Pick 'in ' $DeviceName }
$cableIn = Pick 'out' 'CABLE Input'
$cableOut = Pick 'in ' 'CABLE Output'
if (-not $Device -or -not $InputDevice) { throw "$DeviceName not found (see devices.txt)" }
if (-not $cableIn -or -not $cableOut) { throw 'VB-CABLE (CABLE Input / CABLE Output) not found' }
$vcamCurrent = Join-Path $env:ProgramData 'DesktopCapture\vcam\current.txt'
if (-not (Test-Path $vcamCurrent)) { throw 'the virtual camera is not installed (scripts\vcam-register.ps1)' }
Write-Host "engine $ip`:$port, output/clock $Device, input $InputDevice, $Mode $BlockSize, $Tracks tracks, $Hours h"

# ---------------------------------------------------------------------------------------------------- run
$procs = [ordered]@{}
$gameApp = Join-Path $out 'dc-p5-app.exe'
Copy-Item $probe $gameApp -Force
$flashStop = Join-Path $out 'flash.stop'
$storeA = Join-Path $out 'mixd-a-lan.txt'
$storeB = Join-Path $out 'mixd-b-lan.txt'
$recording = $false
$samplesCsv = Join-Path $out 'samples.csv'
$file = ''
$take = ''
$remoteLogs = @()
$crashed = @()
$runStart = Get-Date

try {
    Write-Host "`n=== setup ===" -ForegroundColor Cyan
    $procs.mixdA = Start-Logged $mixd @('--port', $port, '--allow-remote', '--no-discovery', '--no-profiles', '--test-pulse', '2',
                                        '--lan-store', "`"$storeA`"") 'mixd-a'
    if (-not (Wait-Until { Get-Param '/system/version' } 15)) { throw 'mixd A did not start' }
    $procs.watch = Start-Process -FilePath $cli -ArgumentList '--port', $port, 'watch', '--meters' -PassThru -WindowStyle Hidden -RedirectStandardOutput $watchFile
    Set-Param '/system/audio_mode' $Mode
    Set-Param '/system/block_size' $BlockSize
    Set-Param '/master/out/1/target' "device/$Device"
    Set-Param '/system/clock_device' $Device
    Set-Param '/master/out/2/target' "vmic/$cableIn"
    Set-Param '/lan/stream/1/name' 'Monitor'
    Set-Param '/lan/stream/1/codec' 'opus'
    Set-Param '/master/out/3/target' 'lan/1'
    Call '/master/fx/add' @('type=s:limiter') | Out-Null

    $procs.game = Start-Process -FilePath $gameApp -ArgumentList 'tone', '--freq', '700', '--amp', '0.03', '--seconds', ($seconds + 3600) -PassThru -WindowStyle Hidden

    # mixd B, paired with A, sends its 500 Hz to LAN stream 1
    $procs.mixdB = Start-Logged $mixd @('--port', $portB, '--media-port', $mediaB, '--bind', '127.0.0.1', '--no-discovery', '--fake-devices',
                                        '--no-shm', '--no-plugins', '--no-hotkeys', '--lan-store', "`"$storeB`"", '--test-tone', '500') 'mixd-b'
    if (-not (Wait-Until { (& $cli --port $portB snapshot /lan/id) -match '/lan/id = "' } 10)) { throw 'mixd B did not start' }
    Set-Param '/master/out/2/target' 'lan/1' $portB
    $idB = Get-Param '/lan/id' $portB
    $code = Call '/remote/pairing/start' @() $portB
    Call '/lan/peers/add' @("address=127.0.0.1:$portB") | Out-Null
    if (-not (Wait-Until { (& $cli --port $port snapshot /lan/peers) -match "addr:127.0.0.1:$portB" } 10)) { throw 'mixd A does not list mixd B' }
    Call '/lan/peers/pair' @("id=addr:127.0.0.1:$portB", "code=s:$code") | Out-Null
    $peerOk = Wait-Until { (& $cli --port $port snapshot /lan/peers) -match '/state = "connected"' } 15

    # tracks
    while ([int](Get-Param '/mixer/track_count') -lt $Tracks) { Call '/mixer/track/add' | Out-Null }
    $setup = @(
        @{ t = 1; name = 'Mic'; kind = 'device'; source = $InputDevice; vol = -144; tap = 'pre' },
        @{ t = 2; name = 'Game'; kind = 'process'; source = 'dc-p5-app.exe'; vol = 0; tap = 'pre' },
        @{ t = 3; name = 'LAN'; kind = 'lan'; source = "$idB/1"; vol = -40; tap = 'pre' },
        @{ t = 4; name = 'Vmic'; kind = 'device'; source = $cableOut; vol = 0; tap = 'post' })
    foreach ($s in $setup) {
        Set-Param "/track/$($s.t)/name" $s.name
        Set-Param "/track/$($s.t)/input/kind" $s.kind
        Set-Param "/track/$($s.t)/input/source" $s.source
        Set-Param "/track/$($s.t)/volume_db" $s.vol
        Set-Param "/track/$($s.t)/record/tap" $s.tap
        Set-Param "/track/$($s.t)/out/1/target" $(if ($s.t -eq 4) { '' } else { 'master' })
    }
    Set-Param '/track/2/record/enabled' true
    for ($t = 1; $t -le $Tracks; $t++) {
        if ($t -ge 5) {
            Set-Param "/track/$t/name" "T$t"
            Set-Param "/track/$t/input/kind" 'process'
            Set-Param "/track/$t/input/source" 'dc-p5-app.exe'
            Set-Param "/track/$t/volume_db" -60
            Set-Param "/track/$t/pan" ([math]::Round((($t % 9) - 4) / 4.0, 2))
            Set-Param "/track/$t/out/1/target" 'master'
        }
        Set-Param "/track/$t/lowcut/enabled" true
        Set-Param "/track/$t/highcut/enabled" true
        Call "/track/$t/fx/add" @('type=s:eq4') | Out-Null
        Call "/track/$t/fx/add" @('type=s:comp') | Out-Null
        if ($t % 4 -eq 0) { Call "/track/$t/fx/add" @('type=s:reverb') | Out-Null }
        if ($t % 8 -eq 0) { Call "/track/$t/fx/add" @('type=s:delay') | Out-Null }
    }
    $fxCount = @(& $cli --port $port snapshot /track | Where-Object { $_ -match '^/track/\d+/fx/\d+/type = ' }).Count
    $meterVmic = [int](Get-Param '/track/4/meter_id')

    # recording: master and three tracks as separate streams; the track recorder follows it (WAV of track 2)
    Set-Param '/capture/directory' $recDir
    Set-Param '/capture/video/bitrate_kbps' $BitrateKbps
    Set-Param '/capture/audio/1/source' 'master'
    Set-Param '/capture/audio/2/source' 'track/1'
    Set-Param '/capture/audio/3/source' 'track/2'
    Set-Param '/capture/audio/4/source' 'track/3'
    Set-Param '/recorder/directory' $takeDir
    Set-Param '/recorder/with_capture' true

    $procs.capd = Start-Logged $capd @('--port', $port) 'capd'
    if (-not (Wait-Until { Get-Param '/video/devices/display/1/id' } 15)) { throw 'capd did not report the monitors' }
    $procs.flash = Start-Logged $flash @('--period', '2', '--stop-file', "`"$flashStop`"", '--seconds', ($seconds + 3600)) 'flash'

    # video: 4 mixes
    $cam = Call '/video/source/add' @('kind=camera')    # the first camera = the virtual camera on this PC
    $l = Call '/video/scene/1/layer/add' @("source=$cam")
    Set-Param "/video/scene/1/layer/$l/x" 0.55; Set-Param "/video/scene/1/layer/$l/y" 0.55
    Set-Param "/video/scene/1/layer/$l/width" 0.4; Set-Param "/video/scene/1/layer/$l/height" 0.4
    function New-Text([string]$text, [int64]$bg) {
        $s = Call '/video/source/add' @('kind=text')
        Set-Param "/video/source/$s/text/content" $text
        Set-Param "/video/source/$s/text/size" 96
        Set-Param "/video/source/$s/text/background" $bg
        return $s
    }
    $mix2 = Call '/video/mix/add' @('name=s:vcam')
    $scene2 = Call '/video/scene/add' @('name=s:vcam')
    Call "/video/scene/$scene2/layer/add" @("source=$(New-Text 'P5 virtual camera' 4278190335)") | Out-Null
    Set-Param "/video/mix/$mix2/program_scene" $scene2
    Set-Param '/video/vcam/mix' $mix2

    $mix3 = Call '/video/mix/add' @('name=s:preview')
    $scene3a = Call '/video/scene/add' @('name=s:preview A')
    Call "/video/scene/$scene3a/layer/add" @('source=1') | Out-Null
    $l = Call "/video/scene/$scene3a/layer/add" @("source=$(New-Text 'P5 preview A' 4278255360)")
    Set-Param "/video/scene/$scene3a/layer/$l/width" 0.5; Set-Param "/video/scene/$scene3a/layer/$l/height" 0.2
    $scene3b = Call '/video/scene/add' @('name=s:preview B')
    Call "/video/scene/$scene3b/layer/add" @("source=$(New-Text 'P5 preview B' 4294901760)") | Out-Null
    $l = Call "/video/scene/$scene3b/layer/add" @('source=1')
    Set-Param "/video/scene/$scene3b/layer/$l/x" 0.25; Set-Param "/video/scene/$scene3b/layer/$l/y" 0.25
    Set-Param "/video/scene/$scene3b/layer/$l/width" 0.5; Set-Param "/video/scene/$scene3b/layer/$l/height" 0.5
    Set-Param "/video/mix/$mix3/program_scene" $scene3a
    Set-Param "/video/mix/$mix3/preview_scene" $scene3b
    Set-Param '/video/preview/mix' $mix3

    $mix4 = Call '/video/mix/add' @('name=s:lan')
    $scene4 = Call '/video/scene/add' @('name=s:lan')
    Call "/video/scene/$scene4/layer/add" @('source=1') | Out-Null
    Set-Param "/video/mix/$mix4/program_scene" $scene4
    Set-Param '/video/lan/mix' $mix4

    # viewers: the LAN video, the PC's own UI, the PC's remotes
    $procs.lanview = Start-Logged $v6 @('--engine', "127.0.0.1:$port", '--stream', 'video/lan', '--seconds', ($seconds + 120)) 'lan-viewer' -Visible
    Place-Window $procs.lanview 5
    $procs.local = Start-Logged $ui @('--connect', "127.0.0.1:$port") 'dc-ui-local' -Visible
    Place-Window $procs.local 4
    for ($i = 1; $i -le $Remotes; $i++) {
        $procs["remote$i"] = Start-Logged $ui @('--remote', '--connect', "$ip`:$port", '--measure', $seconds, '--monitor', '1') "remote$i" -Visible
        $remoteLogs += Join-Path $out "remote$i.log"
        Place-Window $procs["remote$i"] ($i - 1)
        Start-Sleep -Seconds 2
    }

    Start-Sleep -Seconds 5
    Add-Check 'setup' "$Tracks tracks with effects" ([int](Get-Param '/mixer/track_count') -eq $Tracks) "$(Get-Param '/mixer/track_count') tracks, $fxCount effects, master limiter"
    Add-Check 'setup' 'LAN peer (mixd B) connected' $peerOk "mixd B $idB, track 3 $(Get-Param '/track/3/input/state')"
    Add-Check 'setup' 'clocked by the output device' ((Get-Param '/system/clock_active') -eq "device/$Device") "clock $(Get-Param '/system/clock_active'), latency $(Get-Param '/system/latency_ms') ms"
    $active = @(1..4 | Where-Object { (Get-Param "/video/mix/$_/active") -eq 'true' }).Count
    Add-Check 'setup' '4 video mixes in use' ($active -eq 4) "$active active (record 1, vcam $mix2, preview $mix3, LAN $mix4)"

    Call '/capture/record/start' | Out-Null
    if (-not (Wait-Until { (Get-Param '/capture/state') -eq 'recording' } 20)) { throw "recording did not start: $(Get-Param '/capture/error')" }
    $recording = $true
    $file = Get-Param '/capture/file'
    $null = Wait-Until { (Get-Param '/recorder/state') -eq 'recording' } 10
    $take = Get-Param '/recorder/folder'
    Write-Host "recording $file`ntake $take`nSurface: dc-ui.exe --remote --connect $ip`:$port --measure $($seconds - 300) --monitor 1" -ForegroundColor Yellow

    # ------------------------------------------------------------------------------------------- the long run
    Write-Host "`n=== $Hours h ===" -ForegroundColor Cyan
    'time,elapsed_s,xruns,engine_cpu,latency_ms,capture,elapsed_rec_s,recorder_dropped,vcam,cam_src,remotes,vmic_db,inputs_bad,' +
        (($procs.Keys | Where-Object { $_ -ne 'watch' } | ForEach-Object { "${_}_cpu_pct,${_}_private_mb,${_}_handles" }) -join ',') |
        Set-Content -Encoding utf8 $samplesCsv
    $cores = [Environment]::ProcessorCount
    $lastCpu = @{}; $lastT = Get-Date
    foreach ($k in $procs.Keys) { try { $lastCpu[$k] = $procs[$k].TotalProcessorTime.TotalSeconds } catch { $lastCpu[$k] = 0 } }
    $x0 = [int64](Get-Param '/system/xruns')
    $start = Get-Date
    $end = $start.AddSeconds($seconds)
    $nextAction = $start.AddMinutes($ActionMinutes)
    $actions = 0; $actionErrors = @()
    $badStates = @{}
    $maxRemotes = 0
    while ((Get-Date) -lt $end) {
        Start-Sleep -Seconds $SampleSeconds
        $now = Get-Date
        foreach ($k in @('mixdA', 'mixdB', 'capd', 'local', 'lanview', 'game')) {
            if ($procs[$k].HasExited -and -not ($crashed -contains $k)) { $crashed += $k; Add-Check 'run' "$k kept running" $false "exited at $([int]($now - $start).TotalMinutes) min, code $($procs[$k].ExitCode)" }
        }
        if ($crashed -contains 'mixdA' -or $crashed -contains 'capd') { break }
        $row = @($now.ToString('HH:mm:ss'), [int]($now - $start).TotalSeconds)
        $cap = Get-Param '/capture/state'
        $vcam = Get-Param '/video/vcam/state'; $cs = Get-Param "/video/source/$cam/state"
        $rc = [int](Get-Param '/remote/clients'); $maxRemotes = [math]::Max($maxRemotes, $rc)
        $inBad = @(1..4 | Where-Object { (Get-Param "/track/$_/input/state") -ne 'ok' } | ForEach-Object { "t$_" }) -join ' '
        foreach ($b in @(@('capture', $cap, 'recording'), @('vcam', $vcam, 'active'), @('camera source', $cs, 'ok'))) {
            if ($b[1] -ne $b[2]) { $badStates[$b[0]] = "$($badStates[$b[0]]) $($b[1])@$([int]($now - $start).TotalMinutes)m" }
        }
        if ($inBad) { $badStates['inputs'] = "$($badStates['inputs']) $inBad@$([int]($now - $start).TotalMinutes)m" }
        $row += @((Get-Param '/system/xruns'), (Get-Param '/system/cpu_load'), (Get-Param '/system/latency_ms'), $cap,
                  [int]([int64](Get-Param '/capture/elapsed_ms') / 1000), (Get-Param '/recorder/dropped_frames'), $vcam, $cs, $rc,
                  (Meter-Db $meterVmic), "`"$inBad`"")
        $dt = ($now - $lastT).TotalSeconds; $lastT = $now
        foreach ($k in $procs.Keys) {
            if ($k -eq 'watch') { continue }
            $p = $procs[$k]
            if ($p.HasExited) { $row += @('', '', ''); continue }
            $p.Refresh()
            $c = $p.TotalProcessorTime.TotalSeconds
            $row += @(('{0:0.00}' -f (100.0 * ($c - $lastCpu[$k]) / $dt / $cores)), ('{0:0.0}' -f ($p.PrivateMemorySize64 / 1MB)), $p.HandleCount)
            $lastCpu[$k] = $c
        }
        ($row -join ',') | Add-Content -Encoding utf8 $samplesCsv
        if ($cap -ne 'recording') { $recording = $false; Add-Check 'run' 'recording kept going' $false "state $cap at $([int]($now - $start).TotalMinutes) min: $(Get-Param '/capture/error')"; break }

        if ($now -ge $nextAction) {
            # what a person at the remote does now and then
            $nextAction = $now.AddMinutes($ActionMinutes); $actions++
            try {
                foreach ($t in (Get-Random -InputObject (5..$Tracks) -Count 8)) { Set-Param "/track/$t/volume_db" (Get-Random -Minimum -70 -Maximum -50) }
                Set-Param '/track/3/volume_db' $(if ($actions % 2) { -42 } else { -40 })
                Call "/video/mix/$mix3/take" | Out-Null
                $snap = Call '/snapshot/save' @('name=s:soak')
                Call '/snapshot/recall' @("snapshot=$snap") | Out-Null
                Call '/snapshot/remove' @("snapshot=$snap") | Out-Null
            } catch { $actionErrors += "$($_.Exception.Message)" }
            Write-Host ("{0:HH:mm} {1,4} min  xruns +{2}  rec {3} s  remotes {4}  {5}" -f $now, [int]($now - $start).TotalMinutes,
                        ([int64](Get-Param '/system/xruns') - $x0), [int]([int64](Get-Param '/capture/elapsed_ms') / 1000), $rc, $inBad)
        }
    }
    $runSeconds = ((Get-Date) - $start).TotalSeconds
    $x1 = [int64](Get-Param '/system/xruns')
    Add-Check 'G5' 'no audio dropouts in the engine (xruns)' ($x1 -eq $x0) ("xruns +{0} in {1:0.0} h, engine CPU now {2}" -f ($x1 - $x0), ($runSeconds / 3600), (Get-Param '/system/cpu_load'))
    Add-Check 'G5' 'no crash' ($crashed.Count -eq 0) $(if ($crashed) { $crashed -join ', ' } else { 'mixd A, mixd B, capd, dc-ui, the LAN viewer all ran to the end' })
    Add-Check 'run' 'states stayed ok (recording, vcam, camera source, inputs 1-4)' ($badStates.Count -eq 0) $(if ($badStates.Count) { ($badStates.GetEnumerator() | ForEach-Object { "$($_.Key):$($_.Value)" }) -join '; ' } else { "every $SampleSeconds s" })
    Add-Check 'run' 'remote-like changes went through' ($actionErrors.Count -eq 0) "$actions rounds (8 faders, mix 3 scene take, snapshot save + recall + remove)$(if ($actionErrors) { ': ' + (($actionErrors | Select-Object -Unique) -join '; ') })"
    Add-Info 'run' 'remotes connected at once (max)' "$maxRemotes (want 4 with the Surface)"
    Add-Check 'run' 'track recorder kept up' ([int64](Get-Param '/recorder/dropped_frames') -eq 0) "dropped frames $(Get-Param '/recorder/dropped_frames')"
} catch {
    Add-Check 'run' 'script' $false $_.Exception.Message
} finally {
    if ($recording) { try { & $cli --port $port call /capture/record/stop | Out-Null; $null = Wait-Until { (Get-Param '/capture/state') -eq 'idle' } 30 } catch { } }
    New-Item -ItemType File -Force $flashStop | Out-Null
    Start-Sleep -Seconds 2
    foreach ($k in @('lanview', 'local', 'game', 'flash', 'capd', 'mixdB', 'watch', 'mixdA')) { Stop-Quiet $procs[$k] }
    foreach ($k in @($procs.Keys | Where-Object { $_ -like 'remote*' })) { $null = $procs[$k].WaitForExit(240000); Stop-Quiet $procs[$k] }
}

# ---------------------------------------------------------------------------------------------------- results
$details = @()
# the recording
$stopLines = Get-Content (Join-Path $out 'capd.log') -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -like 'stopped *' }
foreach ($line in $stopLines) {
    if ($line -match '\((\d+) ms, (\d+) frames, (\d+) dropped\)') { Add-Check 'G5' 'no dropped video frames' ([int]$Matches[3] -eq 0) "$line" }
}
$recFiles = @(Get-ChildItem $recDir -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
Add-Info 'files' 'recording files' (($recFiles | ForEach-Object { '{0} ({1:0.0} GB)' -f $_.Name, ($_.Length / 1GB) }) -join ', ')
foreach ($f in $recFiles) {
    $leaf = $f.Name
    $details += "## $leaf"
    $details += (& $capd probe $f.FullName 2>&1 | ForEach-Object { "$_" })
    $l700 = & $capd levels $f.FullName --tone 700 2>&1 | ForEach-Object { "$_" }
    $l500 = & $capd levels $f.FullName --tone 500 2>&1 | ForEach-Object { "$_" }
    $details += $l700; $details += ($l500 | ForEach-Object { "500 Hz: $_" })
    $st = @{}
    foreach ($line in $l700) {
        if ($line -match 'STREAM index=(\d+) title="([^"]*)" seconds=([\d.]+) rms_db=(-?[\d.]+) tone_db=(-?[\d.]+) blocks=(\d+) tone_blocks=(\d+) silent_blocks=(\d+)') {
            $st[[int]$Matches[1]] = @{ seconds = [double]$Matches[3]; tone = [double]$Matches[5]; blocks = [int]$Matches[6]; toneBlocks = [int]$Matches[7]; silent = [int]$Matches[8] }
        }
    }
    if ($st.ContainsKey(2)) { Add-Check 'G5' "$leaf : game stream holds its tone throughout" ($st[2].toneBlocks -ge $st[2].blocks - 2) ("{0}/{1} s with the tone, {2} silent s, {3:0.0} h" -f $st[2].toneBlocks, $st[2].blocks, $st[2].silent, ($st[2].seconds / 3600)) }
    foreach ($line in $l500) {
        if ($line -match 'STREAM index=3 .* blocks=(\d+) tone_blocks=(\d+) silent_blocks=(\d+)') {
            Add-Check 'G5' "$leaf : LAN stream holds mixd B's tone throughout" ([int]$Matches[2] -ge [int]$Matches[1] - 5) "$($Matches[2])/$($Matches[1]) s with the tone, $($Matches[3]) silent s"
        }
    }
    Write-Host "capd avsync $leaf (a few minutes per hour of video)..."
    $csv = Join-Path $out "avsync_$($f.BaseName).csv"
    $syncOut = & $capd avsync $f.FullName --csv $csv 2>&1 | ForEach-Object { "$_" }
    $details += $syncOut
    $s = @{}
    $line = $syncOut | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1
    if ($line) { foreach ($m in [regex]::Matches($line, '(\w+)=(\S+)')) { $s[$m.Groups[1].Value] = [double]$m.Groups[2].Value } }
    if ($s.Count) {
        $rows = @(Import-Csv $csv | ForEach-Object { [double]$_.offset_ms })
        $tenth = [math]::Max(1, [int][math]::Floor($rows.Count / 10))
        $drift = ($rows | Select-Object -Last $tenth | Measure-Object -Average).Average - ($rows | Select-Object -First $tenth | Measure-Object -Average).Average
        $expected = [math]::Floor($s.duration_s / 2) - 2
        Add-Check 'G5' "$leaf : audio within 45 ms of the picture" ([math]::Abs($s.median_ms) -le 45 -and $s.pairs -ge 0.9 * $expected) ("{0} pairs (about {1} bursts), median {2:+0.0;-0.0} ms, min {3:+0.0;-0.0}, max {4:+0.0;-0.0}" -f $s.pairs, $expected, $s.median_ms, $s.min_ms, $s.max_ms)
        Add-Check 'G5' "$leaf : no A/V drift over the run" ([math]::Abs($drift) -le 10) ("last tenth - first tenth {0:+0.0;-0.0} ms over {1:0.0} h" -f $drift, ($s.duration_s / 3600))
    } else { Add-Check 'G5' "$leaf : A/V analysis" $false 'capd avsync gave no result' }
}
# the WAV of track 2
$wavs = @(if ($take -and (Test-Path $take)) { Get-ChildItem $take -Filter *.wav })
foreach ($w in $wavs) {
    $r = & $audioCheck --tone 700 $w.FullName 2>&1 | ForEach-Object { "$_" }
    $details += $r
    if ($r[0] -match 'seconds=([\d.]+) .*glitches=(\d+) min_sinad_db=(-?[\d.]+)') {
        Add-Check 'G5' "track WAV $($w.Name): no glitches in the 700 Hz" ([int]$Matches[2] -eq 0) ("{0:0.0} h, glitches {1}, min SINAD {2} dB" -f ([double]$Matches[1] / 3600), $Matches[2], $Matches[3])
    }
}
if (-not $wavs) { Add-Check 'G5' 'track WAV' $false "no WAV in '$take'" }

# processes: CPU and memory over the run
$rows = @(Import-Csv $samplesCsv -ErrorAction SilentlyContinue)
if ($rows.Count -ge 4) {
    $skip = [math]::Min([int](600 / $SampleSeconds), [int]($rows.Count / 4))  # the first 10 min warm up
    $steady = $rows | Select-Object -Skip $skip
    foreach ($k in @($procs.Keys | Where-Object { $_ -ne 'watch' })) {
        $vals = @($steady | Where-Object { $_."${k}_private_mb" } | ForEach-Object { [double]$_."${k}_private_mb" })
        $cpu = @($steady | Where-Object { $_."${k}_cpu_pct" } | ForEach-Object { [double]$_."${k}_cpu_pct" })
        if ($vals.Count -lt 2) { continue }
        $n = [math]::Max(1, [int]($vals.Count / 10))
        $first = ($vals | Select-Object -First $n | Measure-Object -Average).Average
        $last = ($vals | Select-Object -Last $n | Measure-Object -Average).Average
        $cpuAvg = ($cpu | Measure-Object -Average).Average; $cpuMax = ($cpu | Measure-Object -Maximum).Maximum
        $detail = '{0:0.0} -> {1:0.0} MB private ({2:+0.0;-0.0} MB), CPU avg {3:0.00}% max {4:0.00}% of the PC ({5} logical cores)' -f $first, $last, ($last - $first), $cpuAvg, $cpuMax, [Environment]::ProcessorCount
        if ($k -in 'mixdA', 'capd', 'local') { Add-Check 'G5' "$k memory does not grow" (($last - $first) -le [math]::Max(30, 0.15 * $first)) $detail }
        else { Add-Info 'procs' $k $detail }
        if ($k -eq 'capd') {
            Add-Check 'target' 'capd CPU while recording <= 5% (GPU encode)' ($cpuAvg -le 5) ('avg {0:0.00}% / max {1:0.00}% of the PC = {2:0.0}% of one core on average (4 mixes, recording + vcam + preview + LAN video)' -f $cpuAvg, $cpuMax, ($cpuAvg * [Environment]::ProcessorCount))
        }
    }
    $eng = @($steady | ForEach-Object { [double]$_.engine_cpu })
    Add-Info 'target' 'engine audio thread load' ('avg {0:0.000} max {1:0.000} ({2} tracks, {3} {4} samples)' -f ($eng | Measure-Object -Average).Average, ($eng | Measure-Object -Maximum).Maximum, $Tracks, $Mode, $BlockSize)
    Add-Info 'target' 'output buffer latency reported by the engine' "$($rows[-1].latency_ms) ms ($Mode, $BlockSize samples; in->out not measured: no loopback cable)"
}
# the remotes
foreach ($log in $remoteLogs) {
    $lines = Get-Content $log -Encoding UTF8 -ErrorAction SilentlyContinue
    $sum = $lines | Where-Object { $_ -like 'summary:*' -or $_ -like 'memory*' -or $_ -match '^\s+(OK|FAIL)' }
    $details += "## $(Split-Path -Leaf $log)"; $details += $sum
    $p95 = ($lines | Where-Object { $_ -match 'delay p50 ([\d.]+) ms, p95 ([\d.]+) ms' } | Select-Object -Last 1)
    $mem = ($lines | Where-Object { $_ -match 'private max ([\d.]+) MB' } | Select-Object -Last 1)
    if ($p95 -match 'p50 ([\d.]+) ms, p95 ([\d.]+) ms') { Add-Check 'target' "$(Split-Path -Leaf $log): preview delay p95 <= 200 ms" ([double]$Matches[2] -le 200) "p50 $($Matches[1]) ms, p95 $($Matches[2]) ms" }
    else { Add-Check 'target' "$(Split-Path -Leaf $log): preview measured" $false (($lines | Select-Object -Last 3) -join ' / ') }
    if ($mem -match 'working set max ([\d.]+) MB, private max ([\d.]+) MB') { Add-Check 'target' "$(Split-Path -Leaf $log): memory about 200 MB" ([double]$Matches[2] -le 260) "with the preview for the whole run: working set max $($Matches[1]) MB, private max $($Matches[2]) MB" }
}
$details += '## LAN video viewer'; $details += (Get-Content (Join-Path $out 'lan-viewer.log') -Encoding UTF8 -ErrorAction SilentlyContinue | Select-Object -Last 25)
$details | Set-Content -Encoding utf8 (Join-Path $out 'details.txt')

# ---------------------------------------------------------------------------------------------------- report
$md = @("# P5 long run ($stamp)", '',
        "Engine $ip`:$port, output/clock $Device, input $InputDevice, $Mode $BlockSize samples, $Tracks tracks, $Remotes PC remotes (+ the Surface), $Hours h planned, started $($runStart.ToString('yyyy-MM-dd HH:mm')).",
        "Recording $file (1080p60, $BitrateKbps kbps), take $take", '',
        '| group | check | result | detail |', '|---|---|---|---|')
foreach ($c in $checks) { $md += "| $($c.group) | $($c.what) | $($c.result) | $($c.detail -replace '\|', '/') |" }
$md += ''
$md += "samples.csv (every $SampleSeconds s), details.txt (probe, levels, avsync, remotes), logs: $out"
$report = Join-Path $out "p5_$stamp.md"
$md | Set-Content -Encoding utf8 $report
Write-Host "`n$($md -join "`n")"
Write-Host "`nreport: $report"
if (($checks | Where-Object { $_.result -eq 'FAIL' }).Count -eq 0) { Write-Host 'P5 LONG RUN PASSED' -ForegroundColor Green; exit 0 }
Write-Host 'P5 LONG RUN FAILED' -ForegroundColor Red
exit 1
