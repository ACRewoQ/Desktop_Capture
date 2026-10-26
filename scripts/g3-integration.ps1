<#
.SYNOPSIS
  Gate G3 integration: every P3 lane at once on the real engine, capture process and UI, and the rows of the
  design doc's disconnect tables (audio and video) on the real PC.

.DESCRIPTION
  Builds Release (mixd, capd, dc-ui, dc-cli, dc-av-flash, dc-loopback-probe) unless -NoBuild, then:

  Setup (one recording runs through all of it):
    - mixd A (the engine under test, --test-pulse 2: a 1 kHz burst on the master every 2 s, shared mode)
        track 1 "Mic"   : -InputDevice (the UR22 input by default), recorded before the fader, fader closed
        track 2 "Game"  : process input dc-g3-app.exe (a copy of dc-loopback-probe playing 700 Hz quietly)
        track 3 "Vmic"  : input "CABLE Output" (what the virtual mic receives), no output: its meter checks the vmic
        track 4 "LAN"   : LAN input from mixd B (a second engine on this PC with fake devices and a 500 Hz test
                          tone on its master, sent as LAN stream 1), fader -40 dB
        master          : out 1 = -Device (the UR22 output by default, also the clock), out 2 = vmic "CABLE Input"
        recording       : audio streams master, track/1, track/2, track/4; video mix 1
    - capd with DC_CAPD_TEST_DIR (test switches for a simulated GPU reset and a full disk)
    - video mix 1 (recorded): the primary monitor, a test window (a PowerShell form titled dc-g3-window) and the
      camera "Desktop Capture Camera"; video mix 2 (a text) goes to the virtual camera, so the camera source in
      mix 1 shows mix 2 through the real Media Foundation camera path
    - dc-av-flash flashes the top-left of the primary monitor with the bursts (A/V offset, as in G2)

  G3-2 (recording + virtual camera + virtual mic at once), for -Minutes:
    xruns, dropped frames, vcam active, camera source ok, the vmic meter, each track's state.

  G3-4 (disconnect rows), one after the other in the same recording:
    process    the game app quits -> track 2 disconnected; starts again -> ok
    lan        mixd B stops -> track 4 disconnected; starts again -> ok (paired before, reconnects by itself)
    window     the test window closes -> source disconnected; opens again with the same title -> ok
    camera     dc-vcam-ctl uninstall -> camera source not ok; install -> ok
    display    (-ChangeResolution) the primary monitor goes to 1920x1080 for 10 s -> the source stays ok with
               the new size, the recording goes on
    remote     dc-ui with the preview open, then closed and opened again: the recording goes on, the preview
               subscription comes and goes
    gpu        simulated GPU reset (the device is recreated) -> a new file segment, the recording goes on
    unplug     (not with -SkipUnplug) asks you, with a pop-up, to unplug -Device / -InputDevice and plug it back:
               the clock moves to another device, the vmic output keeps going, track 1 disconnected -> ok
    disk       the free space drops below the reserve (test switch) -> the recording stops safely, the file plays
    engine     a new recording, then mixd A is killed: what capd does with the recording (design: goes on silent)

  Then the recording: audio streams (capd levels: each track holds its own audio), A/V offset (capd avsync on
  the master), and capd probe of every file. Writes build\g3\g3_<time>.md and prints it.

  Turn the speakers down (bursts every 2 s), leave the top-left of the primary monitor uncovered, and do not use
  the PC's audio meanwhile. Needs VB-CABLE and the virtual camera (scripts\vcam-register.ps1) installed.

.EXAMPLE
  .\scripts\g3-integration.ps1
  .\scripts\g3-integration.ps1 -Minutes 10 -ChangeResolution
  .\scripts\g3-integration.ps1 -NoBuild -SkipUnplug -Cases record,process,lan
#>
param(
    [double]$Minutes = 3,
    [string[]]$Cases = @('record', 'process', 'lan', 'window', 'camera', 'display', 'remote', 'gpu', 'unplug', 'disk', 'engine'),
    [string]$DeviceName = 'UR22',  # picks the input and output whose name contains this (unless -Device / -InputDevice)
    [string]$Device = '',          # master output / clock device id
    [string]$InputDevice = '',     # track 1 input device id
    [ValidateSet('shared', 'exclusive')][string]$Mode = 'shared',
    [switch]$ChangeResolution,
    [switch]$SkipUnplug,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8  # dc-cli and capd print UTF-8 (device names in Japanese)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$Cases = @($Cases | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
function Has-Case([string]$c) { return $Cases -contains $c }

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd capd dc-ui dc-cli dc-av-flash dc-loopback-probe
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$capd = Join-Path $bin 'apps/capd/Release/capd.exe'
$ui = Join-Path $bin 'apps/ui/Release/dc-ui.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$flash = Join-Path $bin 'tools/av_flash/Release/dc-av-flash.exe'
$probe = Join-Path $bin 'tools/loopback_probe/Release/dc-loopback-probe.exe'
foreach ($exe in @($mixd, $capd, $ui, $cli, $flash, $probe)) { if (-not (Test-Path $exe)) { throw "missing $exe (build first)" } }
if ($env:DC_QT_DIR -and -not (Test-Path (Join-Path (Split-Path $ui) 'Qt6Core.dll'))) {
    $env:PATH = (Join-Path $env:DC_QT_DIR 'bin') + ';' + $env:PATH
}
$vcamDir = $null
$vcamCurrent = Join-Path $env:ProgramData 'DesktopCapture\vcam\current.txt'
if (Test-Path $vcamCurrent) { $vcamDir = (Get-Content $vcamCurrent -Raw).Trim() }
$vcamCtl = if ($vcamDir) { Join-Path $vcamDir 'dc-vcam-ctl.exe' } else { '' }

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path $root "build/g3/$stamp"
$recDir = Join-Path $out 'rec'
$hooks = Join-Path $out 'hooks'
New-Item -ItemType Directory -Force $out, $recDir, $hooks | Out-Null
$port = 47881      # mixd A
$portB = 47891     # mixd B
$mediaB = 47892

# ---------------------------------------------------------------------------------------------------- helpers
function Get-Param([string]$path, [int]$p = $port) {
    $line = & $cli --port $p snapshot $path | Where-Object { $_ -like "$path = *" } | Select-Object -First 1
    if (-not $line) { return $null }
    $v = $line.Substring($path.Length + 3)
    if ($v.StartsWith('"')) { return $v.Trim('"') -replace '\\\\', '\' }
    return $v
}
function Set-Param([string]$path, $value, [int]$p = $port) {
    # Windows PowerShell 5.1 drops an empty argument to a native program: pass "" literally
    if ("$value" -eq '') { $value = '""' }
    $r = & $cli --port $p set $path $value
    if ($r -ne 'ok') { throw "set $path $value -> $r" }
}
function Call([string]$method, [string[]]$kv = @(), [int]$p = $port) {
    $r = & $cli --port $p call $method @kv
    if ($r -notlike 'ok*') { throw "call $method $($kv -join ' ') -> $r" }
    return ($r -replace '^ok ?', '').Trim('"')
}
function Wait-Until([scriptblock]$cond, [double]$seconds = 15) {
    $end = (Get-Date).AddSeconds($seconds)
    do {
        try { if (& $cond) { return $true } } catch { }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $end)
    return $false
}
function Wait-Param([string]$path, [string]$want, [double]$seconds = 15, [int]$p = $port) {
    return Wait-Until { (Get-Param $path $p) -eq $want } $seconds
}
function Start-Hidden([string]$exe, [string[]]$arguments, [string]$name) {
    $log = Join-Path $out "$name.log"
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle
    return $p
}
function Stop-Quiet($p) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue; $null = $p.WaitForExit(5000) } }

$watchFile = Join-Path $out 'watch.txt'
function Has-Event([string]$name, [string]$text = '', [double]$seconds = 15, [int]$after = 0) {
    # true when more than $after matching events have arrived (pass Event-Count from before the action)
    return Wait-Until {
        @(Select-String -Path $watchFile -SimpleMatch "event $name" -ErrorAction SilentlyContinue | Where-Object { $_.Line -like "*$text*" }).Count -gt $after
    } $seconds
}
function Event-Count([string]$name, [string]$text = '') {
    return @(Select-String -Path $watchFile -SimpleMatch "event $name" -ErrorAction SilentlyContinue | Where-Object { $_.Line -like "*$text*" }).Count
}
# peak (dB) of a meter id over the newest meter lines (about 1 s each)
function Meter-Db([int]$id, [int]$lines = 2) {
    $all = @(Get-Content $watchFile -ErrorAction SilentlyContinue | Where-Object { $_ -like 'meters seq*' } | Select-Object -Last $lines)
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
function Popup([string]$text, [int]$seconds) {
    # a message for the person at the PC that does not block the script
    $t = $text -replace "'", "''"
    Start-Process powershell -ArgumentList '-NoProfile', '-Command', "(New-Object -ComObject WScript.Shell).Popup('$t', $seconds, 'G3 test', 64) | Out-Null" -WindowStyle Hidden | Out-Null
}
function Xruns { return [int64](Get-Param '/system/xruns') }
function Capture-State { return Get-Param '/capture/state' }

# The test window: a PowerShell form with a fixed title (a window source matches on exe + title).
$windowTitle = 'dc-g3-window'
function Start-TestWindow {
    $cmd = "Add-Type -AssemblyName System.Windows.Forms; `$f = New-Object Windows.Forms.Form; `$f.Text = '$windowTitle'; " +
           "`$f.Width = 640; `$f.Height = 360; `$f.StartPosition = 'Manual'; `$f.Left = 900; `$f.Top = 300; `$f.BackColor = 'SteelBlue'; " +
           "`$f.ShowDialog() | Out-Null"
    $p = Start-Process powershell -ArgumentList '-NoProfile', '-Command', $cmd -PassThru
    $null = $p.Handle
    return $p
}

# ---------------------------------------------------------------------------------------------------- devices
$listing = & $mixd --list-devices
$listing | Set-Content -Encoding utf8 (Join-Path $out 'devices.txt')
function Pick([string]$prefix, [string]$like) {
    $l = $listing | Where-Object { $_ -like "$prefix *" -and $_ -like "* ok *" -and $_ -like "*$like*" } | Select-Object -First 1
    if ($l -and $l -match '\[(.+)\]$') { return $Matches[1] }
    return ''
}
if (-not $Device) { $Device = Pick 'out' $DeviceName }
if (-not $InputDevice) { $InputDevice = Pick 'in ' $DeviceName }
if (-not $InputDevice) { $InputDevice = ($listing | Where-Object { $_ -like 'in  ok *' } | Select-Object -First 1) -replace '^.*\[(.+)\]$', '$1' }
$cableIn = Pick 'out' 'CABLE Input'
$cableOut = Pick 'in ' 'CABLE Output'
$info = [ordered]@{ device = $Device; input = $InputDevice; cable_in = $cableIn; cable_out = $cableOut; mode = $Mode }
Write-Host "output/clock: $Device`ninput: $InputDevice`nvmic: $cableIn / $cableOut"
if (-not $cableIn -or -not $cableOut) { Add-Check 'setup' 'VB-CABLE installed' $false 'CABLE Input / CABLE Output not found (install VB-CABLE)' }
if (-not $vcamCtl -or -not (Test-Path $vcamCtl)) { Add-Info 'setup' 'virtual camera' "not installed ($vcamCurrent); camera checks skipped" }

# ---------------------------------------------------------------------------------------------------- run
$procs = @{}
$files = New-Object System.Collections.Generic.List[string]
function Note-File {
    $f = Get-Param '/capture/file'
    if ($f -and -not $files.Contains($f)) { $files.Add($f) }
}
$gameApp = Join-Path $out 'dc-g3-app.exe'
Copy-Item $probe $gameApp -Force
function Start-Game { return Start-Process -FilePath $gameApp -ArgumentList 'tone', '--freq', '700', '--amp', '0.005', '--seconds', '3600' -PassThru -WindowStyle Hidden }
$storeA = Join-Path $out 'mixd-a-lan.txt'
$storeB = Join-Path $out 'mixd-b-lan.txt'
function Start-MixdB {
    $p = Start-Hidden $mixd @('--port', $portB, '--media-port', $mediaB, '--bind', '127.0.0.1', '--no-discovery', '--fake-devices', '--no-shm',
                              '--no-plugins', '--lan-store', "`"$storeB`"", '--test-tone', '500') "mixd-b-$((Get-Date).ToString('HHmmss'))"
    if (-not (Wait-Until { (& $cli --port $portB snapshot /lan/id) -match '/lan/id = "' } 10)) { throw 'mixd B did not start' }
    Set-Param '/master/out/2/target' 'lan/1' $portB
    return $p
}
$flashStop = Join-Path $out 'flash.stop'
$env:DC_CAPD_TEST_DIR = $hooks
$recordingAlive = $false

try {
    Write-Host "`n=== setup ===" -ForegroundColor Cyan
    $procs.mixdA = Start-Hidden $mixd @('--port', $port, '--bind', '127.0.0.1', '--no-discovery', '--test-pulse', '2', '--lan-store', "`"$storeA`"") 'mixd-a'
    if (-not (Wait-Until { (Get-Param '/system/version') } 10)) { throw 'mixd A did not start' }
    $procs.watch = Start-Process -FilePath $cli -ArgumentList '--port', $port, 'watch', '--meters' -PassThru -WindowStyle Hidden -RedirectStandardOutput $watchFile
    Set-Param '/system/audio_mode' $Mode
    if ($Device) {
        Set-Param '/master/out/1/target' "device/$Device"
        Set-Param '/system/clock_device' $Device
    }
    if ($cableIn) { Set-Param '/master/out/2/target' "vmic/$cableIn" }
    for ($t = 1; $t -le 4; $t++) { Set-Param "/track/$t/out/1/target" $(if ($t -eq 3) { '' } else { 'master' }) }
    Set-Param '/track/1/name' 'Mic'
    Set-Param '/track/1/input/kind' 'device'
    Set-Param '/track/1/input/source' $InputDevice
    Set-Param '/track/1/record/tap' 'pre'
    Set-Param '/track/1/volume_db' -144
    Set-Param '/track/2/name' 'Game'
    Set-Param '/track/2/input/kind' 'process'
    Set-Param '/track/2/input/source' 'dc-g3-app.exe'
    Set-Param '/track/2/record/tap' 'pre'
    Set-Param '/track/3/name' 'Vmic'
    if ($cableOut) {
        Set-Param '/track/3/input/kind' 'device'
        Set-Param '/track/3/input/source' $cableOut
    }
    Set-Param '/track/4/name' 'LAN'
    Set-Param '/track/4/record/tap' 'pre'
    Set-Param '/track/4/volume_db' -40
    $meter = @{}
    for ($t = 1; $t -le 4; $t++) { $meter[$t] = [int](Get-Param "/track/$t/meter_id") }

    $procs.game = Start-Game

    # mixd B: pair A with B (B accepts with a code), then track 4 takes B's LAN stream 1
    $procs.mixdB = Start-MixdB
    $idB = Get-Param '/lan/id' $portB
    $code = Call '/remote/pairing/start' @() $portB
    # a peer added by address is listed as addr:<address> until it is paired
    Call '/lan/peers/add' @("address=127.0.0.1:$portB") | Out-Null
    if (-not (Wait-Until { (& $cli --port $port snapshot /lan/peers) -match "addr:127.0.0.1:$portB" } 10)) { throw 'mixd A does not list mixd B' }
    Call '/lan/peers/pair' @("id=addr:127.0.0.1:$portB", "code=s:$code") | Out-Null
    $peerOk = Wait-Until { (& $cli --port $port snapshot /lan/peers) -match '/state = "connected"' } 15
    Set-Param '/track/4/input/kind' 'lan'
    Set-Param '/track/4/input/source' "$idB/1"

    # recording: master and three tracks as separate streams
    Set-Param '/capture/directory' $recDir
    Set-Param '/capture/audio/1/source' 'master'
    Set-Param '/capture/audio/2/source' 'track/1'
    Set-Param '/capture/audio/3/source' 'track/2'
    Set-Param '/capture/audio/4/source' 'track/4'

    $procs.capd = Start-Hidden $capd @('--port', $port) 'capd'
    $procs.flash = Start-Hidden $flash @('--period', '2', '--stop-file', "`"$flashStop`"", '--seconds', '7200') 'flash'
    $procs.window = Start-TestWindow
    if (-not (Wait-Until { (Get-Param '/video/devices/display/1/id') } 15)) { throw 'capd did not report the monitors' }

    # video: mix 1 = monitor (default layer) + test window + camera; mix 2 = text -> virtual camera
    $win = Call '/video/source/add' @('kind=window')
    Set-Param "/video/source/$win/window/exe" 'powershell.exe'
    Set-Param "/video/source/$win/window/title" $windowTitle
    Set-Param "/video/source/$win/window/match" 'title'
    $l = Call '/video/scene/1/layer/add' @("source=$win")
    Set-Param "/video/scene/1/layer/$l/x" 0.55; Set-Param "/video/scene/1/layer/$l/y" 0.55
    Set-Param "/video/scene/1/layer/$l/width" 0.4; Set-Param "/video/scene/1/layer/$l/height" 0.4
    $cam = 0
    if ($vcamCtl -and (Test-Path $vcamCtl)) {
        # No id or name: capd takes the first camera. Reo's PC has no other camera, so that is the virtual camera
        # (setting its id or its Japanese name from here did not match capd's list).
        $info.camera = (& $cli --port $port snapshot /video/devices/camera | Where-Object { $_ -match '/name = ' }) -join '; '
        $cam = Call '/video/source/add' @('kind=camera')
        $l = Call '/video/scene/1/layer/add' @("source=$cam")
        Set-Param "/video/scene/1/layer/$l/x" 0.55; Set-Param "/video/scene/1/layer/$l/y" 0.05
        Set-Param "/video/scene/1/layer/$l/width" 0.4; Set-Param "/video/scene/1/layer/$l/height" 0.4
    }
    $mix2 = Call '/video/mix/add'
    $scene2 = Call '/video/scene/add'
    $txt = Call '/video/source/add' @('kind=text')
    Set-Param "/video/source/$txt/text/content" 'G3 virtual camera'
    Set-Param "/video/source/$txt/text/size" 120
    Set-Param "/video/source/$txt/text/background" 4278190335
    Call "/video/scene/$scene2/layer/add" @("source=$txt") | Out-Null
    Set-Param "/video/mix/$mix2/program_scene" $scene2
    Set-Param '/video/vcam/mix' $mix2

    Start-Sleep -Seconds 3
    Add-Check 'setup' 'mixd A clocked by the output device' ((Get-Param '/system/clock_active') -eq "device/$Device") "clock $(Get-Param '/system/clock_active')"
    Add-Check 'setup' 'LAN peer paired and connected' $peerOk "mixd B $idB"
    Call '/capture/record/start' | Out-Null
    if (-not (Wait-Param '/capture/state' 'recording' 20)) { throw "recording did not start: $(Get-Param '/capture/error')" }
    $recordingAlive = $true
    Note-File
    Start-Sleep -Seconds 3

    # ------------------------------------------------------------------------------------------- G3-2
    if (Has-Case 'record') {
        Write-Host "`n=== G3-2: recording with vcam and vmic for $Minutes min ===" -ForegroundColor Cyan
        $x0 = Xruns
        $vmicMin = 999.0; $vcamBad = @(); $camBad = @(); $trackBad = @(); $samples = 0
        $end = (Get-Date).AddMinutes($Minutes)
        while ((Get-Date) -lt $end) {
            Start-Sleep -Seconds 5
            $samples++
            foreach ($k in $procs.Keys) { if ($k -in 'mixdA', 'mixdB', 'capd' -and $procs[$k].HasExited) { throw "$k exited early" } }
            $vmicMin = [math]::Min($vmicMin, (Meter-Db $meter[3] 3))
            $vs = Get-Param '/video/vcam/state'; if ($vs -ne 'active') { $vcamBad += $vs }
            if ($cam) { $cs = Get-Param "/video/source/$cam/state"; if ($cs -ne 'ok') { $camBad += "$cs $(Get-Param "/video/source/$cam/error")" } }
            foreach ($t in 1, 2, 4) { $s = Get-Param "/track/$t/input/state"; if ($s -ne 'ok') { $trackBad += "track $t $s" } }
        }
        $x1 = Xruns
        Add-Check 'G3-2' 'engine: no dropouts' ($x1 -eq $x0) "xruns +$($x1 - $x0), cpu $(Get-Param '/system/cpu_load'), latency $(Get-Param '/system/latency_ms') ms"
        Add-Check 'G3-2' 'recording kept going' ((Capture-State) -eq 'recording') "state $(Capture-State), $samples samples"
        Add-Check 'G3-2' 'inputs ok (mic, game, LAN)' ($trackBad.Count -eq 0) $(if ($trackBad) { ($trackBad | Select-Object -Unique) -join ', ' } else { 'all ok every 5 s' })
        Add-Check 'G3-2' 'virtual camera active' ($vcamBad.Count -eq 0) $(if ($vcamBad) { "not active: $(($vcamBad | Select-Object -Unique) -join ', ')" } else { "active (consumer: the camera source), $(Get-Param '/video/vcam/detail')" })
        if ($cam) { Add-Check 'G3-2' 'camera source (vcam through MF) ok' ($camBad.Count -eq 0) $(if ($camBad) { ($camBad | Select-Object -Unique) -join ', ' } else { "ok $(Get-Param "/video/source/$cam/width")x$(Get-Param "/video/source/$cam/height")" }) }
        if ($cableOut) { Add-Check 'G3-2' 'virtual mic carries the master' ($vmicMin -gt -60) "CABLE Output meter, lowest of the 3 s peaks: $vmicMin dB" }
        $lanDelay = Get-Param '/track/4/input/lan_delay_ms'
        Add-Info 'G3-3' 'LAN input buffer (loopback, mixd B -> A)' "$lanDelay ms (target $(Get-Param '/track/4/input/lan_buffer_ms'), 0 = auto)"
    }

    # ------------------------------------------------------------------------------------------- G3-4
    if (Has-Case 'process') {
        Write-Host "`n=== process: the game quits and starts again ===" -ForegroundColor Cyan
        Stop-Quiet $procs.game
        $down = Wait-Param '/track/2/input/state' 'disconnected' 15
        $rec = (Capture-State) -eq 'recording'
        $procs.game = Start-Game
        $up = Wait-Param '/track/2/input/state' 'ok' 20
        Start-Sleep -Seconds 3
        $db = Meter-Db $meter[2]
        Add-Check 'G3-4' 'process: quit -> silent, restart -> back' ($down -and $up -and $rec -and $db -gt -70) "disconnected $down, recording $rec, ok again $up, meter $db dB"
    }
    if (Has-Case 'lan') {
        Write-Host "`n=== lan: mixd B stops and starts again ===" -ForegroundColor Cyan
        Stop-Quiet $procs.mixdB
        $down = Wait-Param '/track/4/input/state' 'disconnected' 20
        Start-Sleep -Seconds 1
        $quiet = Meter-Db $meter[4] 1
        $rec = (Capture-State) -eq 'recording'
        $procs.mixdB = Start-MixdB
        $up = Wait-Param '/track/4/input/state' 'ok' 40
        Start-Sleep -Seconds 3
        $db = Meter-Db $meter[4]
        Add-Check 'G3-4' 'LAN input: stops -> silent, back -> restored' ($down -and $up -and $rec -and $db -gt -80) "disconnected $down (meter $quiet dB), recording $rec, ok again $up (meter $db dB), buffer $(Get-Param '/track/4/input/lan_delay_ms') ms"
    }
    if (Has-Case 'window') {
        Write-Host "`n=== window: the test window closes and opens again ===" -ForegroundColor Cyan
        $before = Get-Param "/video/source/$win/state"
        Stop-Quiet $procs.window
        $down = Wait-Until { (Get-Param "/video/source/$win/state") -ne 'ok' } 15
        $mid = Get-Param "/video/source/$win/state"
        $procs.window = Start-TestWindow
        $up = Wait-Param "/video/source/$win/state" 'ok' 30
        Add-Check 'G3-4' 'window: closed -> transparent, reopened -> reconnected' ($before -eq 'ok' -and $down -and $up -and (Capture-State) -eq 'recording') "before $before, closed $mid, reopened $(Get-Param "/video/source/$win/state")"
    }
    if ((Has-Case 'camera') -and $cam) {
        Write-Host "`n=== camera: the virtual camera is removed and installed again ===" -ForegroundColor Cyan
        & $vcamCtl uninstall | Out-Host
        $down = Wait-Until { (Get-Param "/video/source/$cam/state") -ne 'ok' } 20
        $mid = "$(Get-Param "/video/source/$cam/state") $(Get-Param "/video/source/$cam/error")"
        & $vcamCtl install | Out-Host
        $up = Wait-Param "/video/source/$cam/state" 'ok' 40
        Add-Check 'G3-4' 'camera: removed -> transparent, back -> reconnected' ($down -and $up -and (Capture-State) -eq 'recording') "removed: $mid; back: $(Get-Param "/video/source/$cam/state")"
    }
    if ((Has-Case 'display') -and $ChangeResolution) {
        Write-Host "`n=== display: the primary monitor at 1920x1080 for 10 s ===" -ForegroundColor Cyan
        $w0 = Get-Param '/video/source/1/width'
        $res = @'
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Threading;
public static class Disp {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public char[] dmDeviceName;
    public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public char[] dmFormName;
    public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency, dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettingsExW(string dev, ref DEVMODE mode, IntPtr hwnd, int flags, IntPtr param);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettingsExW(string dev, IntPtr mode, IntPtr hwnd, int flags, IntPtr param);
  // the primary monitor at w x h (temporary, CDS_FULLSCREEN) for some seconds, then back to the saved mode
  public static string Run(int w, int h, int seconds) {
    var m = new DEVMODE { dmDeviceName = new char[32], dmFormName = new char[32] };
    m.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
    bool ok = EnumDisplaySettingsW(null, -1, ref m);
    string r = "current " + ok + " " + m.dmPelsWidth + "x" + m.dmPelsHeight + "@" + m.dmDisplayFrequency + " size " + m.dmSize;
    m.dmPelsWidth = w; m.dmPelsHeight = h; m.dmFields = 0x80000 | 0x100000;
    r += ", change " + ChangeDisplaySettingsExW(null, ref m, IntPtr.Zero, 4, IntPtr.Zero);
    Thread.Sleep(seconds * 1000);
    r += ", restore " + ChangeDisplaySettingsExW(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero);
    return r;
  }
}
"@
[Disp]::Run(1920, 1080, 10)
'@
        $job = Start-Job -ScriptBlock ([scriptblock]::Create($res))
        Start-Sleep -Seconds 6
        $w1 = Get-Param '/video/source/1/width'; $s1 = Get-Param '/video/source/1/state'
        $null = Wait-Job $job -Timeout 30
        $jobOut = (Receive-Job $job) -join ', '
        Start-Sleep -Seconds 4
        $w2 = Get-Param '/video/source/1/width'; $s2 = Get-Param '/video/source/1/state'
        Add-Check 'G3-4' 'monitor: resolution change -> picture follows' ($s1 -eq 'ok' -and $s2 -eq 'ok' -and $w1 -ne $w0 -and $w2 -eq $w0 -and (Capture-State) -eq 'recording') "width $w0 -> $w1 ($s1) -> $w2 ($s2); $jobOut"
    }
    if (Has-Case 'remote') {
        Write-Host "`n=== remote: dc-ui with the preview, closed and opened again ===" -ForegroundColor Cyan
        $x0 = Xruns
        # with its window up: the preview subscribes only while it is on screen
        $procs.ui = Start-Process -FilePath $ui -ArgumentList '--connect', "127.0.0.1:$port", '--page', 'video', '--demo', 'preview' -PassThru
        $sub1 = Wait-Until { (& $cli --port $port snapshot /lan/sub) -match 'video/preview' } 20
        Start-Sleep -Seconds 5
        Stop-Process -Id $procs.ui.Id -Force   # an unclean disconnect
        $gone = Wait-Until { -not ((& $cli --port $port snapshot /lan/sub) -match 'video/preview') } 15
        $rec = (Capture-State) -eq 'recording'
        $procs.ui = Start-Process -FilePath $ui -ArgumentList '--connect', "127.0.0.1:$port", '--page', 'video', '--demo', 'preview' -PassThru
        $sub2 = Wait-Until { (& $cli --port $port snapshot /lan/sub) -match 'video/preview' } 20
        Start-Sleep -Seconds 3
        Stop-Quiet $procs.ui
        Add-Check 'G3-4' 'remote: disconnect does not touch the recording; preview comes back' ($sub1 -and $gone -and $sub2 -and $rec -and (Capture-State) -eq 'recording') "preview subscribed $sub1, dropped on kill $gone, recording $rec, subscribed again $sub2, xruns +$((Xruns) - $x0)"
    }
    if (Has-Case 'gpu') {
        Write-Host "`n=== gpu: simulated GPU reset ===" -ForegroundColor Cyan
        $segments0 = Event-Count 'capture.segment'
        $file0 = Get-Param '/capture/file'
        New-Item -ItemType File -Force (Join-Path $hooks 'gpu_reset') | Out-Null
        $seg = Has-Event 'capture.segment' 'gpu_reset' 20 $segments0
        Start-Sleep -Seconds 3
        Note-File
        $file1 = Get-Param '/capture/file'
        $vcamBack = Wait-Param '/video/vcam/state' 'active' 20
        $srcBack = Wait-Param '/video/source/1/state' 'ok' 20
        $lost = @(Select-String -Path (Join-Path $out 'capd.log') -SimpleMatch 'GPU lost' -ErrorAction SilentlyContinue).Count
        Add-Check 'G3-4' 'GPU reset: new file, recording goes on, outputs come back' ($seg -and $file1 -ne $file0 -and (Capture-State) -eq 'recording' -and $vcamBack -and $srcBack) "capd 'GPU lost' lines $lost, segment event $seg ($segments0 before), file $(Split-Path -Leaf $file0) -> $(Split-Path -Leaf $file1), vcam $(Get-Param '/video/vcam/state'), monitor source $(Get-Param '/video/source/1/state')"
    }
    if ((Has-Case 'unplug') -and -not $SkipUnplug) {
        Write-Host "`n=== unplug: the clock / input / output device goes away and comes back ===" -ForegroundColor Cyan
        Popup "G3 test: unplug the USB cable of the $DeviceName now (within 5 minutes)." 300
        Write-Host "Unplug the $DeviceName now (waiting up to 5 min)..." -ForegroundColor Yellow
        $lost0 = Event-Count 'device.lost'
        $lost = Has-Event 'device.lost' '' 300 $lost0
        Start-Sleep -Seconds 5
        $clockOff = Get-Param '/system/clock_active'
        $t1 = Get-Param '/track/1/input/state'
        $vmicDb = if ($cableOut) { Meter-Db $meter[3] 3 } else { 'n/a' }
        $rec = (Capture-State) -eq 'recording'
        Popup "G3 test: plug the $DeviceName back in now (within 5 minutes)." 300
        Write-Host "Plug it back in now (waiting up to 5 min)..." -ForegroundColor Yellow
        $back = Wait-Until { (Get-Param '/track/1/input/state') -eq 'ok' -and (Get-Param '/system/clock_active') -like 'device/*' } 300
        Start-Sleep -Seconds 5
        $clockBack = Get-Param '/system/clock_active'
        Add-Check 'G3-4' 'clock device: moves to another device' ($lost -and $clockOff -ne "device/$Device" -and $clockOff) "unplugged -> clock $clockOff"
        Add-Check 'G3-4' 'input device: silent, recording goes on' ($t1 -eq 'disconnected' -and $rec) "track 1 $t1, recording $rec"
        if ($cableOut) { Add-Check 'G3-4' 'output device: only that output stops (vmic keeps going)' ([double]$vmicDb -gt -60) "vmic meter $vmicDb dB while unplugged" }
        Add-Check 'G3-4' 'replug: restored' ($back -and (Capture-State) -eq 'recording') "track 1 $(Get-Param '/track/1/input/state') (source $(Get-Param '/track/1/input/source')), clock $clockBack, master out 1 $(Get-Param '/master/out/1/target')"
    }
    Note-File
    if (Has-Case 'disk') {
        Write-Host "`n=== disk: free space below the reserve ===" -ForegroundColor Cyan
        Set-Content -Path (Join-Path $hooks 'disk_free') -Value '104857600' -Encoding ascii  # 100 MB
        $stopped = Wait-Until { (Capture-State) -ne 'recording' } 30
        $err = Get-Param '/capture/error'
        Remove-Item (Join-Path $hooks 'disk_free') -ErrorAction SilentlyContinue
        $vcam = Get-Param '/video/vcam/state'
        $recordingAlive = $false
        Start-Sleep -Seconds 2
        $last = $files[$files.Count - 1]
        $probeOut = if ($last -and (Test-Path $last)) { & $capd probe $last 2>&1 | ForEach-Object { "$_" } } else { @('missing') }
        $plays = [bool]($probeOut | Where-Object { $_ -match 'video' })
        Add-Check 'G3-4' 'disk full: safe stop, file plays, vcam goes on' ($stopped -and $err -match 'disk' -and $plays -and $vcam -eq 'active') "state $(Capture-State), error '$err', last file plays $plays, vcam $vcam"
    }
    if ($recordingAlive) {
        Call '/capture/record/stop' | Out-Null
        $null = Wait-Until { (Capture-State) -eq 'idle' } 20
        $recordingAlive = $false
    }
    if (Has-Case 'engine') {
        Write-Host "`n=== engine: mixd stops during a recording ===" -ForegroundColor Cyan
        Call '/capture/record/start' | Out-Null
        $started = Wait-Param '/capture/state' 'recording' 20
        $f = Get-Param '/capture/file'
        Start-Sleep -Seconds 10
        Stop-Process -Id $procs.mixdA.Id -Force
        Start-Sleep -Seconds 8
        $capdLog = Get-Content (Join-Path $out 'capd.log') -Encoding UTF8 -ErrorAction SilentlyContinue
        $lostLine = $capdLog | Where-Object { $_ -like '*engine connection lost*' } | Select-Object -Last 1
        $stoppedLine = $capdLog | Where-Object { $_ -like 'stopped *' } | Select-Object -Last 1
        $plays = $f -and (Test-Path $f) -and [bool](& $capd probe $f 2>&1 | Where-Object { "$_" -match 'video' })
        $goesOn = -not ($stoppedLine -and $stoppedLine -like "*$(Split-Path -Leaf $f)*")
        Add-Check 'G3-4' 'engine stop: file is kept and plays' ($started -and $plays) "file $(Split-Path -Leaf $f), plays $plays"
        Add-Check 'G3-4' 'engine stop: recording goes on silent (design doc)' $goesOn "capd: '$lostLine' / '$stoppedLine'"
    }
} catch {
    Add-Check 'run' 'script' $false $_.Exception.Message
} finally {
    if ($recordingAlive) { try { & $cli --port $port call /capture/record/stop | Out-Null; Start-Sleep -Seconds 3 } catch { } }
    New-Item -ItemType File -Force $flashStop | Out-Null
    Start-Sleep -Seconds 2
    foreach ($k in @('ui', 'game', 'window', 'flash', 'capd', 'mixdB', 'watch', 'mixdA')) { Stop-Quiet $procs[$k] }
    if ($vcamCtl -and (Test-Path $vcamCtl)) { & $vcamCtl install | Out-Null }  # the camera stays installed whatever happened
    Remove-Item Env:\DC_CAPD_TEST_DIR -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------------------------------- files
$stopLines = Get-Content (Join-Path $out 'capd.log') -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -like 'stopped *' }
foreach ($line in $stopLines) {
    if ($line -match '\((\d+) ms, (\d+) frames, (\d+) dropped\)') { Add-Check 'files' 'no dropped video frames' ([int]$Matches[3] -eq 0) "$line" }
}
$levelsOut = @()
foreach ($f in $files) {
    if (-not (Test-Path $f)) { Add-Check 'files' 'file exists' $false $f; continue }
    $leaf = Split-Path -Leaf $f
    $probeOut = & $capd probe $f 2>&1 | ForEach-Object { "$_" }
    $levelsOut += "## $leaf"; $levelsOut += $probeOut
    $l700 = & $capd levels $f --tone 700 2>&1 | ForEach-Object { "$_" }
    $l500 = & $capd levels $f --tone 500 2>&1 | ForEach-Object { "$_" }
    $levelsOut += $l700; $levelsOut += ($l500 | ForEach-Object { "500 Hz: $_" })
    $st = @{}
    foreach ($line in $l700) {
        if ($line -match 'STREAM index=(\d+) title="([^"]*)" seconds=([\d.]+) rms_db=(-?[\d.]+) tone_db=(-?[\d.]+) blocks=(\d+) tone_blocks=(\d+) silent_blocks=(\d+)') {
            $st[[int]$Matches[1]] = @{ title = $Matches[2]; seconds = [double]$Matches[3]; rms = [double]$Matches[4]; tone = [double]$Matches[5]; blocks = [int]$Matches[6]; toneBlocks = [int]$Matches[7]; silent = [int]$Matches[8] }
        }
    }
    $lan = @{}
    foreach ($line in $l500) {
        if ($line -match 'STREAM index=(\d+) .* tone_db=(-?[\d.]+) blocks=(\d+) tone_blocks=(\d+)') { $lan[[int]$Matches[1]] = @{ tone = [double]$Matches[2]; blocks = [int]$Matches[3]; toneBlocks = [int]$Matches[4] } }
    }
    if ($st.Count -lt 4) { Add-Check 'files' "$leaf : four audio streams" $false "$($st.Count) streams"; continue }
    $titles = ($st.Keys | Sort-Object | ForEach-Object { $st[$_].title }) -join ', '
    Add-Check 'G3-2' "$leaf : master, mic, game and LAN as separate streams" ($st.Count -eq 4) $titles
    $game = $st[2]; $mic = $st[1]; $master = $st[0]
    Add-Check 'G3-2' "$leaf : game stream holds the game (700 Hz)" ($game.toneBlocks -ge 0.8 * $game.blocks) "$($game.toneBlocks)/$($game.blocks) s with the tone, $($game.tone) dBFS"
    Add-Check 'G3-2' "$leaf : mic stream does not (separate track)" ($mic.tone -le $game.tone - 20) "700 Hz in mic $($mic.tone) dB vs game $($game.tone) dB; mic level $($mic.rms) dBFS"
    Add-Check 'G3-2' "$leaf : master holds the mix" ($master.tone -ge $game.tone - 6) "700 Hz in master $($master.tone) dB"
    if ($lan.ContainsKey(3)) { Add-Check 'G3-2' "$leaf : LAN stream holds mixd B's 500 Hz" ($lan[3].toneBlocks -ge 0.8 * $lan[3].blocks) "$($lan[3].toneBlocks)/$($lan[3].blocks) s, $($lan[3].tone) dBFS" }
}
$levelsOut | Set-Content -Encoding utf8 (Join-Path $out 'files.txt')
if ($files.Count -gt 0 -and (Test-Path $files[0])) {
    $syncOut = & $capd avsync $files[0] --csv (Join-Path $out 'avsync.csv') 2>&1 | ForEach-Object { "$_" }
    $syncOut | Add-Content -Encoding utf8 (Join-Path $out 'files.txt')
    $s = @{}
    $line = $syncOut | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1
    if ($line) { foreach ($m in [regex]::Matches($line, '(\w+)=(\S+)')) { $s[$m.Groups[1].Value] = [double]$m.Groups[2].Value } }
    if ($s.Count) {
        Add-Check 'G3-2' 'A/V: audio within 45 ms of the picture (first file)' ([math]::Abs($s.median_ms) -le 45 -and $s.pairs -ge 10) ("{0} pairs, median {1:+0.0;-0.0} ms, min {2:+0.0;-0.0}, max {3:+0.0;-0.0}" -f $s.pairs, $s.median_ms, $s.min_ms, $s.max_ms)
        # The offset flips between two values a frame apart (where the flash lands between two 60 fps ticks), so the
        # median of a tenth can jump by a frame without any drift: compare the means of the first and last tenth.
        $rows = @(Import-Csv (Join-Path $out 'avsync.csv') | ForEach-Object { [double]$_.offset_ms })
        $tenth = [math]::Max(1, [int][math]::Floor($rows.Count / 10))
        $firstMean = ($rows | Select-Object -First $tenth | Measure-Object -Average).Average
        $lastMean = ($rows | Select-Object -Last $tenth | Measure-Object -Average).Average
        $drift = $lastMean - $firstMean
        Add-Check 'G3-4' 'A/V: no drift through the disconnects (first file)' ([math]::Abs($drift) -le 10) ("mean of the last tenth - first tenth {0:+0.0;-0.0} ms over {1:0} s (median-based {2:+0.0;-0.0} ms)" -f $drift, $s.duration_s, $s.drift_ms)
    } else { Add-Check 'G3-2' 'A/V analysis' $false 'capd avsync gave no result' }
}

# ---------------------------------------------------------------------------------------------------- report
$md = @("# G3 integration check ($stamp)", '',
        "Output/clock: $($info.device). Input: $($info.input). VB-CABLE: $($info.cable_in) / $($info.cable_out). Mode: $Mode. Minutes: $Minutes. Camera: $($info.camera).",
        "Cases: $($Cases -join ', ')$(if ($SkipUnplug) { ' (unplug skipped)' })$(if (-not $ChangeResolution) { ' (display skipped: -ChangeResolution)' })",
        "Files: $($files -join ', ')", '',
        '| group | check | result | detail |', '|---|---|---|---|')
foreach ($c in $checks) { $md += "| $($c.group) | $($c.what) | $($c.result) | $($c.detail -replace '\|', '/') |" }
$md += ''
$md += "Logs and files.txt (probe, levels, avsync): $out"
$report = Join-Path $out "g3_$stamp.md"
$md | Set-Content -Encoding utf8 $report
Write-Host "`n$($md -join "`n")"
Write-Host "`nreport: $report"
if (($checks | Where-Object { $_.result -eq 'FAIL' }).Count -eq 0) { Write-Host 'G3 INTEGRATION PASSED' -ForegroundColor Green; exit 0 }
Write-Host 'G3 INTEGRATION FAILED' -ForegroundColor Red
exit 1
