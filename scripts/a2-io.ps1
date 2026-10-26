<#
.SYNOPSIS
  P3 lane A2 (audio inputs and outputs): process audio, devices at other rates, ASIO, VB-CABLE,
  unplugging and replugging.

.DESCRIPTION
  Builds Release (mixd, dc-cli, dc-engine-check, dc-loopback-probe) unless -NoBuild, then runs the
  cases in -Cases and writes build\a2\a2_<time>.md.

    list     mixd --list-devices: devices with their rate and api (wasapi / vmic / asio), applications
    process  a test application (a copy of dc-loopback-probe named dc-a2-app.exe) plays a 700 Hz tone;
             two tracks take it as process input. Checks: both tracks "ok" with one capture, the track
             meter shows the tone, the application quits -> "disconnected" and device.lost, it starts
             again -> "ok" and device.restored, the tone is back.
    rate     mixed rates: runs scripts\g2-engine.ps1 with -Device (an output that is NOT at 48 kHz, e.g.
             the UR22 switched to 44.1 kHz in its driver panel) and -InputDevice (an input at 48 kHz),
             shared and exclusive, block 256. Passes when the engine runs without xruns and the mixd
             log has the device note that the device is converted.
    asio     -Asio "<driver name>": test tone to the driver's outputs 1-2 (the engine's clock) and input
             1 on a muted track, -Minutes, xruns must stay 0.
    vmic     VB-CABLE: test tone to "CABLE Input" (shared), dropouts measured on its loopback.
    unplug   -InputDevice: asks you to unplug the device and plug it back within 60 s each; checks
             device.lost / device.restored and the track state.

.EXAMPLE
  .\scripts\a2-io.ps1 -Cases list,process
  .\scripts\a2-io.ps1 -Cases rate -Device '{0.0.0.00000000}.{...}' -InputDevice '{0.0.1.00000000}.{...}' -Minutes 10
  .\scripts\a2-io.ps1 -Cases asio -Asio 'Yamaha Steinberg USB ASIO' -Minutes 10
#>
param(
    [string[]]$Cases = @('list', 'process'),
    [double]$Minutes = 5,
    [string]$Device = '',
    [string]$InputDevice = '',
    [string]$Asio = '',
    [switch]$NoBuild
)

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# 'Stop' only after the build: with output redirected, CMake's warnings on stderr would stop the script
if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd dc-cli dc-engine-check dc-loopback-probe
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$ErrorActionPreference = 'Stop'

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$check = Join-Path $bin 'tools/engine_check/Release/dc-engine-check.exe'
$probe = Join-Path $bin 'tools/loopback_probe/Release/dc-loopback-probe.exe'
$out = Join-Path $root 'build/a2'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$port = 47871
$rows = @()

function Get-Param([string]$path) {
    $line = & $cli --port $port snapshot $path | Where-Object { $_ -like "$path = *" } | Select-Object -First 1
    if (-not $line) { return $null }
    $v = $line.Substring($path.Length + 3)
    if ($v.StartsWith('"')) { return $v.Trim('"') }
    return $v
}
function Set-Param([string]$path, $value) {
    $r = & $cli --port $port set $path $value
    if ($r -ne 'ok') { throw "set $path $value -> $r" }
}
function Wait-Param([string]$path, [string]$want, [int]$seconds = 10) {
    for ($i = 0; $i -lt $seconds * 4; $i++) {
        if ((Get-Param $path) -eq $want) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}
function Start-Mixd([string]$name, [string[]]$extra = @()) {
    $log = Join-Path $out "mixd_${stamp}_$name.log"
    $p = Start-Process -FilePath $mixd -ArgumentList (@('--port', $port, '--no-shm', '--no-profiles') + $extra) -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    Start-Sleep -Milliseconds 1500
    return @{ proc = $p; log = $log }
}
# dc-cli watch --meters in the background: events and a meter line every ~1 s
function Start-Watch([string]$name) {
    $file = Join-Path $out "watch_${stamp}_$name.txt"
    $p = Start-Process -FilePath $cli -ArgumentList '--port', $port, 'watch', '--meters' -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $file
    Start-Sleep -Milliseconds 500
    return @{ proc = $p; file = $file }
}
function Has-Event($watch, [string]$name, [string]$id, [int]$seconds = 15) {
    for ($i = 0; $i -lt $seconds * 2; $i++) {
        if (Select-String -Path $watch.file -SimpleMatch "event $name" -Quiet) {
            $hit = Select-String -Path $watch.file -SimpleMatch "event $name" | Where-Object { $_.Line -like "*$id*" }
            if ($hit) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
# peak (dB) of meter id in the newest meter line
function Meter-Db($watch, [int]$id) {
    $line = Get-Content $watch.file | Where-Object { $_ -like 'meters seq*' } | Select-Object -Last 1
    if ($line -match " $id=(-?[\d.]+)dB") { return [double]$Matches[1] }
    return -999
}
function Add-Row([string]$case, [bool]$ok, [string]$detail) {
    $script:rows += [pscustomobject]@{ case = $case; result = $(if ($ok) { 'PASS' } else { 'FAIL' }); detail = $detail }
    Write-Host "$case : $(if ($ok) { 'PASS' } else { 'FAIL' })  $detail" -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}

# powershell -File passes 'list,process' as one string
$Cases = @($Cases | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

foreach ($case in $Cases) {
    Write-Host "`n=== $case ===" -ForegroundColor Cyan
    switch ($case) {
        'list' {
            $listing = & $mixd --list-devices
            $listing | Write-Host
            $listing | Set-Content -Encoding utf8 (Join-Path $out "devices_$stamp.txt")
            Add-Row 'list' ($LASTEXITCODE -eq 0) "$(@($listing | Where-Object { $_ -like 'in *' -or $_ -like 'out *' }).Count) devices, $(@($listing | Where-Object { $_ -like 'app *' }).Count) apps"
        }
        'process' {
            $app = Join-Path $out 'dc-a2-app.exe'
            Copy-Item $probe $app -Force
            $m = Start-Mixd 'process'
            $w = Start-Watch 'process'
            $tone = $null
            $notes = @()
            $ok = $true
            try {
                Set-Param '/track/1/input/kind' 'process'
                Set-Param '/track/1/input/source' 'dc-a2-app.exe'
                Set-Param '/track/2/input/kind' 'process'
                Set-Param '/track/2/input/source' 'DC-A2-APP.EXE'
                Set-Param '/track/2/mute' true
                $tone = Start-Process -FilePath $app -ArgumentList 'tone', '--freq', '700', '--amp', '0.1', '--seconds', '600' -PassThru -WindowStyle Hidden
                if (-not (Wait-Param '/track/1/input/state' 'ok' 15)) { $ok = $false; $notes += 'track 1 not ok after start' }
                if ((Get-Param '/track/2/input/state') -ne 'ok') { $ok = $false; $notes += 'track 2 not ok' }
                Start-Sleep -Seconds 3
                $db = Meter-Db $w 1
                $notes += "meter $db dB"
                if ($db -lt -40) { $ok = $false; $notes += 'tone not on track 1' }
                Stop-Process -Id $tone.Id
                if (-not (Wait-Param '/track/1/input/state' 'disconnected' 15)) { $ok = $false; $notes += 'not disconnected after exit' }
                if (-not (Has-Event $w 'device.lost' 'dc-a2-app.exe')) { $ok = $false; $notes += 'no device.lost' }
                $tone = Start-Process -FilePath $app -ArgumentList 'tone', '--freq', '700', '--amp', '0.1', '--seconds', '600' -PassThru -WindowStyle Hidden
                if (-not (Wait-Param '/track/1/input/state' 'ok' 20)) { $ok = $false; $notes += 'not ok after restart' }
                if (-not (Has-Event $w 'device.restored' 'dc-a2-app.exe')) { $ok = $false; $notes += 'no device.restored' }
                Start-Sleep -Seconds 3
                $db2 = Meter-Db $w 1
                $notes += "after restart $db2 dB"
                if ($db2 -lt -40) { $ok = $false; $notes += 'tone not back' }
                $streams = @(Select-String -Path $m.log -Pattern 'process:dc-a2-app.exe pid').Count
                if ($streams -gt 0) { $notes += "capture lines in log: $streams" }
            } catch {
                $ok = $false
                $notes += $_.Exception.Message
            } finally {
                if ($tone -and -not $tone.HasExited) { Stop-Process -Id $tone.Id -ErrorAction SilentlyContinue }
                Stop-Process -Id $w.proc.Id -ErrorAction SilentlyContinue
                Stop-Process -Id $m.proc.Id -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 500
            }
            Add-Row 'process' $ok ($notes -join '; ')
        }
        'rate' {
            if (-not $Device) { Add-Row 'rate' $false 'needs -Device (an output not at 48 kHz)'; break }
            $g2 = @{ NoBuild = $true; Minutes = $Minutes; Blocks = @(256); Device = $Device }
            if ($InputDevice) { $g2.WithInput = $true; $g2.InputDevice = $InputDevice }
            & (Join-Path $PSScriptRoot 'g2-engine.ps1') @g2
            $passed = $LASTEXITCODE -eq 0
            $logs = Get-ChildItem (Join-Path $root 'build/g2') -Filter 'mixd_*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 2
            # the "device note" mixd logs for a converted device: "<name>: 44100Hz ... 48kHz ..."
            $converted = @($logs | ForEach-Object { Select-String -Path $_.FullName -Pattern 'device note: .*[0-9]+Hz .*48kHz' }).Count -gt 0
            Add-Row 'rate' ($passed -and $converted) "g2-engine $(if ($passed) { 'passed' } else { 'failed' }); conversion note in log: $converted"
        }
        'asio' {
            if (-not $Asio) { Add-Row 'asio' $false 'needs -Asio "<driver name>" (see the list case)'; break }
            $m = Start-Mixd 'asio' @('--test-tone', '1000')
            $notes = @()
            $ok = $true
            try {
                Set-Param '/system/block_size' 256
                Set-Param '/master/out/1/target' "device/asio:${Asio}:1-2"
                Set-Param '/track/1/mute' true
                Set-Param '/track/1/input/kind' 'device'
                Set-Param '/track/1/input/source' "asio:$Asio"
                if (-not (Wait-Param '/system/clock_active' "device/asio:${Asio}:1-2" 15)) {
                    $ok = $false
                    $notes += "clock: $(Get-Param '/system/clock_active')"
                }
                if (-not (Wait-Param '/track/1/input/state' 'ok' 10)) { $ok = $false; $notes += 'input not ok' }
                Start-Sleep -Seconds 3
                $notes += "latency $(Get-Param '/system/latency_ms') ms"
                $x0 = [int64](Get-Param '/system/xruns')
                $end = (Get-Date).AddMinutes($Minutes)
                while ((Get-Date) -lt $end) {
                    Start-Sleep -Seconds 5
                    if ($m.proc.HasExited) { throw 'mixd exited' }
                }
                $x = [int64](Get-Param '/system/xruns') - $x0
                $notes += "xruns $x"
                if ($x -ne 0) { $ok = $false }
            } catch {
                $ok = $false
                $notes += $_.Exception.Message
            } finally {
                Stop-Process -Id $m.proc.Id -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 500
            }
            Add-Row 'asio' $ok ($notes -join '; ')
        }
        'vmic' {
            $cable = & $mixd --list-devices | Where-Object { $_ -like 'out *vmic*CABLE Input*' } | Select-Object -First 1
            if (-not $cable -or $cable -notmatch '\[(.+)\]$') { Add-Row 'vmic' $false 'VB-CABLE ("CABLE Input") not found'; break }
            $id = $Matches[1]
            & (Join-Path $PSScriptRoot 'g2-engine.ps1') -NoBuild -Minutes $Minutes -Modes shared -Blocks 256 -Device $id
            Add-Row 'vmic' ($LASTEXITCODE -eq 0) "CABLE Input $id, shared, block 256"
        }
        'unplug' {
            if (-not $InputDevice) { Add-Row 'unplug' $false 'needs -InputDevice'; break }
            $m = Start-Mixd 'unplug'
            $w = Start-Watch 'unplug'
            $ok = $true
            $notes = @()
            try {
                Set-Param '/track/1/input/kind' 'device'
                Set-Param '/track/1/input/source' $InputDevice
                if (-not (Wait-Param '/track/1/input/state' 'ok' 10)) { $ok = $false; $notes += 'not ok at start' }
                Write-Host 'Unplug the input device now (60 s)...' -ForegroundColor Yellow
                if (-not (Has-Event $w 'device.lost' '' 60)) { $ok = $false; $notes += 'no device.lost' }
                if ((Get-Param '/track/1/input/state') -ne 'disconnected') { $ok = $false; $notes += 'not disconnected' }
                Write-Host 'Plug it back in now (60 s)...' -ForegroundColor Yellow
                if (-not (Has-Event $w 'device.restored' '' 60) -and -not (Has-Event $w 'device.rematched' '' 1)) { $ok = $false; $notes += 'no device.restored / rematched' }
                if (-not (Wait-Param '/track/1/input/state' 'ok' 10)) { $ok = $false; $notes += 'not ok again' }
                $notes += "source now $(Get-Param '/track/1/input/source')"
            } catch {
                $ok = $false
                $notes += $_.Exception.Message
            } finally {
                Stop-Process -Id $w.proc.Id -ErrorAction SilentlyContinue
                Stop-Process -Id $m.proc.Id -ErrorAction SilentlyContinue
            }
            Add-Row 'unplug' $ok ($notes -join '; ')
        }
        default { Add-Row $case $false 'unknown case' }
    }
}

$md = @("# A2 audio I/O check ($stamp)", '', "Minutes: $Minutes. Device: $Device. Input: $InputDevice. ASIO: $Asio", '',
        '| case | result | detail |', '|---|---|---|')
foreach ($r in $rows) { $md += "| $($r.case) | $($r.result) | $($r.detail) |" }
$report = Join-Path $out "a2_$stamp.md"
$md | Set-Content -Encoding utf8 $report
Write-Host "`n$($md -join "`n")"
Write-Host "`nreport: $report"
if (($rows | Where-Object { $_.result -ne 'PASS' }).Count -eq 0) { Write-Host 'A2 PASSED' -ForegroundColor Green; exit 0 }
Write-Host 'A2 FAILED' -ForegroundColor Red
exit 1
