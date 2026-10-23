<#
.SYNOPSIS
  Gate G2 (audio engine): no dropouts with a 128 or 256 sample buffer.

.DESCRIPTION
  1. Builds Release (mixd, dc-cli, dc-engine-check)
  2. For each mode (shared, exclusive) and block size (128, 256): starts mixd with a -20 dBFS 1 kHz
     test tone on the master, waits until the output device clocks the engine, and runs it for
     -Minutes.
       - shared mode: dc-engine-check analyses the device's loopback for dropouts (sine fit per 100 ms)
       - both modes: mixd's own counter /system/xruns (device underflows, missed periods, clock
         bridge underruns/overruns) and the peak /system/cpu_load
     With -WithInput, track 1 takes the default input device (muted, so it does not disturb the
     tone) to exercise an input stream and its clock bridge.
  3. Writes build\g2\engine_<time>.md and prints the table.

  A quiet 1 kHz tone plays on the output device; turn the speakers down (the loopback is taken before
  them) and avoid other sounds on that device during the test (they count as dropouts in shared mode).

.EXAMPLE
  .\scripts\g2-engine.ps1 -Minutes 5
  .\scripts\g2-engine.ps1 -Minutes 10 -WithInput
  .\scripts\g2-engine.ps1 -Modes exclusive -Blocks 128 -Minutes 30 -Device '{0.0.0.00000000}.{...}'
#>
param(
    [double]$Minutes = 5,
    [string[]]$Modes = @('shared', 'exclusive'),
    [int[]]$Blocks = @(128, 256),
    [string]$Device = '',
    [switch]$WithInput,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd dc-cli dc-engine-check
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$check = Join-Path $bin 'tools/engine_check/Release/dc-engine-check.exe'
$out = Join-Path $root 'build/g2'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$port = 47870

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

Write-Host "`n=== Devices ===" -ForegroundColor Cyan
& $mixd --list-devices

$rows = @()
foreach ($mode in $Modes) {
    foreach ($block in $Blocks) {
        Write-Host "`n=== $mode, block $block, $Minutes min ===" -ForegroundColor Cyan
        $log = Join-Path $out "mixd_${stamp}_${mode}_$block.log"
        $proc = Start-Process -FilePath $mixd -ArgumentList '--port', $port, '--test-tone', '1000', '--no-shm' `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        $row = [ordered]@{ mode = $mode; block = $block; clock = ''; latency_ms = ''; xruns = '';
                           cpu_max = ''; loopback = ''; result = 'FAIL'; note = '' }
        try {
            Start-Sleep -Milliseconds 1500
            if ($Device) {
                Set-Param '/master/out/1/target' "device/$Device"
                Set-Param '/system/clock_device' $Device
            }
            Set-Param '/system/block_size' $block
            Set-Param '/system/audio_mode' $mode
            if ($WithInput) {
                $inId = Get-Param '/devices/input/1/id'
                Set-Param '/track/1/mute' true # processed but silent, so the tone stays clean
                Set-Param '/track/1/input/kind' 'device'
                Set-Param '/track/1/input/source' $inId
            }
            $clock = ''
            for ($i = 0; $i -lt 40; $i++) {
                Start-Sleep -Milliseconds 250
                $clock = Get-Param '/system/clock_active'
                if ($clock -like 'device/*') { break }
            }
            $row.clock = $clock
            if ($clock -notlike 'device/*') {
                $row.note = "no output device clocks the engine (clock: $clock); see $log"
                $rows += [pscustomobject]$row
                continue
            }
            if ($WithInput) {
                # the input opens right after the clock; give it a moment before calling it missing
                $inState = ''
                for ($i = 0; $i -lt 20; $i++) {
                    $inState = Get-Param '/track/1/input/state'
                    if ($inState -eq 'ok') { break }
                    Start-Sleep -Milliseconds 250
                }
                if ($inState -ne 'ok') { $row.note = "input stream did not open (see device error in $log); " }
            }
            Start-Sleep -Seconds 3 # warm-up: bridges lock, first periods settle
            $row.latency_ms = Get-Param '/system/latency_ms'
            $x0 = [int64](Get-Param '/system/xruns')

            $checker = $null
            $checkOut = Join-Path $out "check_${stamp}_${mode}_$block.txt"
            if ($mode -eq 'shared') {
                $devId = $clock.Substring(7)
                $checker = Start-Process -FilePath $check -ArgumentList '--device', "`"$devId`"", '--seconds', ($Minutes * 60), '--warmup', '1' `
                    -PassThru -WindowStyle Hidden -RedirectStandardOutput $checkOut
            }
            $cpuMax = 0.0
            $end = (Get-Date).AddMinutes($Minutes).AddSeconds(2)
            while ((Get-Date) -lt $end) {
                Start-Sleep -Seconds 5
                $c = [double](Get-Param '/system/cpu_load')
                if ($c -gt $cpuMax) { $cpuMax = $c }
                if ($proc.HasExited) { throw "mixd exited (see $log)" }
            }
            $x1 = [int64](Get-Param '/system/xruns')
            $row.xruns = $x1 - $x0
            $row.cpu_max = $cpuMax
            $ok = ($x1 - $x0) -eq 0
            if ($checker) {
                $checker.WaitForExit(30000) | Out-Null
                $summary = Get-Content $checkOut | Where-Object { $_ -like 'blocks *' } | Select-Object -Last 1
                if ($summary -match 'glitches (\d+)') {
                    $row.loopback = "$($Matches[1]) / $(($summary -split ' ')[1]) blocks"
                    $ok = $ok -and ([int]$Matches[1] -eq 0)
                } else {
                    $row.loopback = 'no result'
                    $ok = $false
                }
            } else {
                $row.loopback = 'n/a (exclusive)'
            }
            $row.result = if ($ok) { 'PASS' } else { 'FAIL' }
            if ($x1 -ne $x0) { $row.note += "xrun sources in $log; " }
        } catch {
            $row.note += $_.Exception.Message
        } finally {
            Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 500
        }
        $rows += [pscustomobject]$row
        $rows[-1] | Format-List
    }
}

$md = @("# G2 audio engine check ($stamp)", '', "Minutes per case: $Minutes. With input: $WithInput.", '',
        '| mode | block | clock | latency ms | xruns | cpu max | loopback glitches | result | note |',
        '|---|---|---|---|---|---|---|---|---|')
foreach ($r in $rows) {
    $md += "| $($r.mode) | $($r.block) | $($r.clock) | $($r.latency_ms) | $($r.xruns) | $($r.cpu_max) | $($r.loopback) | $($r.result) | $($r.note) |"
}
$report = Join-Path $out "engine_$stamp.md"
$md | Set-Content -Encoding utf8 $report
Write-Host "`n$($md -join "`n")"
Write-Host "`nreport: $report"
if (($rows | Where-Object { $_.result -ne 'PASS' }).Count -eq 0) { Write-Host 'G2 ENGINE PASSED' -ForegroundColor Green; exit 0 }
Write-Host 'G2 ENGINE FAILED' -ForegroundColor Red
exit 1
