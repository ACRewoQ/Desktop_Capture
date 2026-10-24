<#
.SYNOPSIS
  P3 lane A1 on a real machine: built-in effects and the VST3 host inside mixd.

.DESCRIPTION
  1. Builds Release (mixd, dc-cli, the DC Test Gain fixture)
  2. Starts mixd on the real output device with a -20 dBFS 1 kHz test tone on the master
  3. Built-in effects: puts all six on the master, moves their parameters for -Seconds, and checks
     /system/xruns and /system/cpu_load
  4. Scan: scans the standard VST3 folders plus the fixture's folder (one child process per file,
     with a fresh cache) and lists what crashed or timed out
  5. DC Test Gain on the master: loads, sets a parameter, opens its window for -Seconds (the window
     appears on screen) while the audio keeps running, closes it
  6. Unless -SkipInstalled: loads every installed effect plugin that scanned ok (up to -MaxPlugins)
     on the master one after another, opens and closes its window, and removes it; mixd must stay up
     and the audio must not drop out
  7. Writes build\a1\fx_<time>.md and prints the result

  A quiet 1 kHz tone plays on the default output device; turn the speakers down.

.EXAMPLE
  .\scripts\a1-fx.ps1
  .\scripts\a1-fx.ps1 -SkipInstalled -Seconds 20
#>
param(
    [int]$Seconds = 10,
    [int]$MaxPlugins = 30,
    [int]$ScanMinutes = 15,
    [switch]$SkipInstalled,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd dc-cli dc_test_plugin_VST3
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$fixtureDir = Join-Path $bin 'tools/vst3_test_plugin/dc_test_plugin_artefacts/Release/VST3'
$out = Join-Path $root 'build/a1'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$cache = Join-Path $out "vst3-cache_$stamp.json"
$log = Join-Path $out "mixd_$stamp.log"
$port = 47880
$checks = @()
$report = @()

function Check($ok, $what) {
    if ($ok) { Write-Host "  OK   $what" -ForegroundColor Green }
    else { Write-Host "  FAIL $what" -ForegroundColor Red }
    $script:checks += [pscustomobject]@{ ok = [bool]$ok; what = $what }
}
function Get-Params([string]$prefix) {
    $map = [ordered]@{}
    foreach ($line in (& $cli --port $port snapshot $prefix)) {
        $i = $line.IndexOf(' = ')
        if ($i -lt 0) { continue }
        $v = $line.Substring($i + 3)
        if ($v.StartsWith('"')) { $v = $v.Substring(1, $v.Length - 2) }
        $map[$line.Substring(0, $i)] = $v
    }
    return $map
}
function Get-Param([string]$path) { return (Get-Params $path)[$path] }
function Set-Param([string]$path, $value) {
    $r = & $cli --port $port set $path $value
    if ($r -ne 'ok') { throw "set $path $value -> $r" }
}
function Invoke-Method([string]$method, [string[]]$arguments = @()) {
    $r = & $cli --port $port call $method @arguments
    return ($r | Select-Object -First 1)
}
function Wait-Status([string]$slot, [int]$timeoutSec = 30) {
    $end = (Get-Date).AddSeconds($timeoutSec)
    do {
        $s = Get-Param "$slot/status"
        if ($s -and $s -ne 'loading') { return $s }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $end)
    return 'loading (timed out)'
}
function Clear-Master {
    while ((Get-Params '/master/fx/1/type').Count -gt 0) {
        $r = Invoke-Method '/master/fx/remove' @('index=1')
        if ($r -notlike 'ok*') { throw "remove -> $r" }
    }
}
# Runs for $sec seconds, sampling the CPU load; returns @{ xruns; cpu }.
function Measure-Run([int]$sec, [scriptblock]$each = $null) {
    $x0 = [int64](Get-Param '/system/xruns')
    $cpu = 0.0
    $end = (Get-Date).AddSeconds($sec)
    $n = 0
    while ((Get-Date) -lt $end) {
        Start-Sleep -Milliseconds 500
        if ($each) { & $each $n }
        $n++
        $c = [double](Get-Param '/system/cpu_load')
        if ($c -gt $cpu) { $cpu = $c }
        if ($script:proc.HasExited) { throw "mixd exited (see $log)" }
    }
    return @{ xruns = [int64](Get-Param '/system/xruns') - $x0; cpu = $cpu }
}

Write-Host "`n=== Start mixd ===" -ForegroundColor Cyan
$proc = Start-Process -FilePath $mixd -ArgumentList '--port', $port, '--test-tone', '1000', '--no-shm', '--plugin-cache', "`"$cache`"" `
    -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.err"
try {
    Start-Sleep -Milliseconds 2000
    $clock = ''
    for ($i = 0; $i -lt 40; $i++) {
        $clock = Get-Param '/system/clock_active'
        if ($clock -like 'device/*') { break }
        Start-Sleep -Milliseconds 250
    }
    Check ($clock -like 'device/*') "an output device clocks the engine ($clock)"
    Start-Sleep -Seconds 3
    $base = Measure-Run 5
    $report += "| no effects | $($base.xruns) | $($base.cpu) |"

    Write-Host "`n=== Built-in effects on the master ===" -ForegroundColor Cyan
    foreach ($t in @('eq4', 'comp', 'gate', 'reverb', 'delay', 'limiter')) {
        $r = Invoke-Method '/master/fx/add' @("type=$t")
        Check ($r -like 'ok*') "add $t -> $r"
    }
    Set-Param '/master/fx/3/param/threshold_db' -80 # the gate stays open for the tone
    $moves = {
        param($n)
        $v = if ($n % 2) { 6 } else { -6 }
        Set-Param '/master/fx/1/param/mid1_gain_db' $v
        Set-Param '/master/fx/5/param/time_ms' (200 + 50 * ($n % 4))
        Set-Param '/master/fx/4/param/size' (0.3 + 0.1 * ($n % 5))
    }
    $m = Measure-Run $Seconds $moves
    Check ($m.xruns -eq 0) "six built-in effects with moving parameters: xruns $($m.xruns), cpu max $($m.cpu)"
    $report += "| 6 built-in effects, parameters moving | $($m.xruns) | $($m.cpu) |"
    Set-Param '/master/fx/4/bypass' true
    Set-Param '/master/fx/4/bypass' false
    Clear-Master

    Write-Host "`n=== Scan (fresh cache) ===" -ForegroundColor Cyan
    Set-Param '/plugins/extra_dirs' $fixtureDir
    $t0 = Get-Date
    $r = Invoke-Method '/plugins/scan' @('rescan=true')
    Check ($r -like 'ok*') "scan starts -> $r"
    Start-Sleep -Milliseconds 500
    $end = (Get-Date).AddMinutes($ScanMinutes)
    while ((Get-Param '/plugins/scan/state') -eq 'scanning' -and (Get-Date) -lt $end) {
        Start-Sleep -Seconds 2
        Write-Host ("  {0:P0} {1}" -f [double](Get-Param '/plugins/scan/progress'), (Get-Param '/plugins/scan/current'))
        if ($proc.HasExited) { throw "mixd exited during the scan (see $log)" }
    }
    $scanSec = [int]((Get-Date) - $t0).TotalSeconds
    Check ((Get-Param '/plugins/scan/state') -eq 'idle') "scan finished in $scanSec s"
    $all = Get-Params '/plugins'
    $plugins = @()
    foreach ($k in $all.Keys) {
        if ($k -match '^/plugins/(\d+)/id$') {
            $b = "/plugins/$($Matches[1])"
            $plugins += [pscustomobject]@{ id = $all[$k]; name = $all["$b/name"]; vendor = $all["$b/vendor"];
                instrument = $all["$b/instrument"]; status = $all["$b/status"]; error = $all["$b/error"]; path = $all["$b/path"] }
        }
    }
    $okCount = @($plugins | Where-Object status -eq 'ok').Count
    Write-Host "  $($plugins.Count) plugin entries, $okCount ok"
    $plugins | Where-Object status -ne 'ok' | ForEach-Object { Write-Host "  $($_.status): $($_.path) $($_.error)" -ForegroundColor Yellow }
    $gain = $plugins | Where-Object name -eq 'DC Test Gain' | Select-Object -First 1
    Check ($gain -and $gain.status -eq 'ok') 'DC Test Gain is in the list'
    Check (Test-Path $cache) 'the scan cache was written'

    Write-Host "`n=== DC Test Gain on the master, window open ===" -ForegroundColor Cyan
    if ($gain) {
        $r = Invoke-Method '/master/fx/add' @("type=$($gain.id)")
        $st = Wait-Status '/master/fx/1'
        Check ($st -eq 'ok') "load -> $st ($(Get-Param '/master/fx/1/param_count') parameters)"
        $keys = @((Get-Params '/master/fx/1/param/').Keys)
        Check ($keys.Count -ge 1) "parameters in the tree: $($keys.Count)"
        if ($keys.Count -ge 1) { Set-Param $keys[0] 0.7 }
        Set-Param '/master/fx/1/editor' true
        Write-Host '  the DC Test Gain window should be on screen now'
        $m = Measure-Run $Seconds
        Check ($m.xruns -eq 0) "audio while the window is open: xruns $($m.xruns), cpu max $($m.cpu)"
        Check ((Get-Param '/master/fx/1/editor') -eq 'true') 'editor stays open'
        $report += "| DC Test Gain, window open | $($m.xruns) | $($m.cpu) |"
        Set-Param '/master/fx/1/editor' false
        Clear-Master
    }

    if (-not $SkipInstalled) {
        Write-Host "`n=== Installed plugins, one after another ===" -ForegroundColor Cyan
        $list = @($plugins | Where-Object { $_.status -eq 'ok' -and $_.instrument -ne 'true' -and $_.name -ne 'DC Test Gain' } |
            Select-Object -First $MaxPlugins)
        foreach ($p in $list) {
            $x0 = [int64](Get-Param '/system/xruns')
            $r = Invoke-Method '/master/fx/add' @("type=$($p.id)")
            $st = Wait-Status '/master/fx/1' 60
            $line = "| $($p.name) ($($p.vendor)) | $st"
            if ($st -eq 'ok') {
                $count = Get-Param '/master/fx/1/param_count'
                if ((Get-Param '/master/fx/1/editor') -ne $null) {
                    try { Set-Param '/master/fx/1/editor' true; Start-Sleep -Seconds 3; Set-Param '/master/fx/1/editor' false } catch { $line += " (window: $_)" }
                }
                Start-Sleep -Seconds 1
                $line += " | $count"
            } else {
                $line += " $(Get-Param '/master/fx/1/error') | "
            }
            $dx = [int64](Get-Param '/system/xruns') - $x0
            $line += " | $dx |"
            Write-Host "  $line"
            $report += $line
            Clear-Master
            if ($proc.HasExited) { throw "mixd exited with $($p.name) (see $log)" }
        }
        Check (-not $proc.HasExited) "mixd survived $($list.Count) installed plugins"
    }
} finally {
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue }
}

$failed = @($checks | Where-Object { -not $_.ok })
$md = @("# A1 effects and VST3 on $env:COMPUTERNAME ($stamp)", '', "Result: $(if ($failed.Count) { 'FAIL' } else { 'PASS' })", '', '## Checks', '')
$md += $checks | ForEach-Object { "- $(if ($_.ok) { 'OK' } else { 'FAIL' }) $($_.what)" }
$md += @('', '## Audio', '', '| case | xruns | cpu max |', '|---|---|---|')
$md += $report | Where-Object { ($_ -split '\|').Count -eq 5 }
if (-not $SkipInstalled) {
    $md += @('', '## Installed plugins', '', '| plugin | status | parameters | xruns |', '|---|---|---|---|')
    $md += $report | Where-Object { ($_ -split '\|').Count -eq 6 }
}
$md += @('', '## Scan', '', "$($plugins.Count) entries, $okCount ok, $scanSec s", '')
$md += $plugins | Where-Object status -ne 'ok' | ForEach-Object { "- $($_.status): $($_.path) $($_.error)" }
$md += @('', "mixd log: $log")
$file = Join-Path $out "fx_$stamp.md"
$md | Set-Content -Encoding UTF8 $file
Write-Host "`nReport: $file"
if ($failed.Count) { Write-Host "$($failed.Count) CHECK(S) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'ALL CHECKS PASSED' -ForegroundColor Green
