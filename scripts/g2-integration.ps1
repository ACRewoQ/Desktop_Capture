<#
.SYNOPSIS
  Gate G2 integration: the real engine, capture process and UI together (mixd + capd + dc-ui).

.DESCRIPTION
  1. Builds Release (mixd, capd, dc-ui, dc-cli, dc-av-flash).
  2. Starts mixd with a test pulse (a 50 ms 1 kHz burst on the master at every multiple of -Period
     seconds of QPC time), sets the audio mode, block size and output device, and waits until the
     output device clocks the engine.
  3. Starts capd (follows the engine) and dc-av-flash (the top-left quarter of the primary monitor
     turns white at the same instants as the bursts).
  4. Starts dc-ui with its window up: it picks the first track's input from the device list (as the
     input picker does), starts the recording, keeps moving every track's fader, pan and filters for
     -Minutes (at the UI's 60 Hz send rate), then stops the recording.
  5. Checks:
       - dc-ui: input opened, recording started and stopped, no request refused, screen kept drawing
       - mixd: /system/xruns did not grow while the UI was busy
       - capd: no dropped video frames
       - the recording (capd avsync): every flash has its burst; the audio sits within 45 ms of the
         picture (median), every pair within 34 ms (two frames at 60 fps) of that median, and the
         offset drifts less than 10 ms from the start to the end
  6. Writes build\g2\integration_<time>.md and prints it.

  A burst plays on the output device every -Period seconds: turn the speakers down. Leave the top-left
  of the primary monitor uncovered (dc-av-flash stays on top) and do not use the PC's audio meanwhile.

.EXAMPLE
  .\scripts\g2-integration.ps1 -Minutes 5
  .\scripts\g2-integration.ps1 -Minutes 60 -Mode exclusive -Block 128
  .\scripts\g2-integration.ps1 -Minutes 10 -Device '{0.0.0.00000000}.{...}' -InputDevice '{0.0.1.00000000}.{...}'
#>
param(
    [double]$Minutes = 5,
    [ValidateSet('shared', 'exclusive')][string]$Mode = 'exclusive',
    [ValidateSet(128, 256)][int]$Block = 128,
    [string]$Device = '',            # output device id for the master (default: Windows default device)
    [string]$InputDevice = 'first',  # input device id for track 1, or 'first'
    [int]$InputChannel = 1,
    [double]$Period = 2,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd capd dc-ui dc-cli dc-av-flash
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$capd = Join-Path $bin 'apps/capd/Release/capd.exe'
$ui = Join-Path $bin 'apps/ui/Release/dc-ui.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$flash = Join-Path $bin 'tools/av_flash/Release/dc-av-flash.exe'
foreach ($exe in @($mixd, $capd, $ui, $cli, $flash)) { if (-not (Test-Path $exe)) { throw "missing $exe (build first)" } }
# dc-ui is a Qt program: Qt DLLs from DC_QT_DIR unless they were copied next to it.
if ($env:DC_QT_DIR -and -not (Test-Path (Join-Path (Split-Path $ui) 'Qt6Core.dll'))) {
    $env:PATH = (Join-Path $env:DC_QT_DIR 'bin') + ';' + $env:PATH
}

$out = Join-Path $root 'build/g2'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$port = 47871

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
function Start-Hidden([string]$exe, [string[]]$arguments, [string]$log) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle # Windows PowerShell 5.1: without this ExitCode stays empty
    return $p
}
function Get-Result([string]$file) {
    # key=value pairs of the last "RESULT ..." line of a log
    $line = Get-Content $file -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1
    $h = @{}
    if ($line) {
        foreach ($m in [regex]::Matches($line, '(\w+)=("([^"]*)"|\S+)')) {
            $h[$m.Groups[1].Value] = if ($m.Groups[3].Success) { $m.Groups[3].Value } else { $m.Groups[2].Value }
        }
    }
    return $h
}

$mixdLog = Join-Path $out "integration_${stamp}_mixd.log"
$capdLog = Join-Path $out "integration_${stamp}_capd.log"
$flashLog = Join-Path $out "integration_${stamp}_flash.log"
$uiLog = Join-Path $out "integration_${stamp}_ui.log"
$syncCsv = Join-Path $out "integration_${stamp}_avsync.csv"
$flashStop = Join-Path $out "integration_${stamp}_flash.stop"

$checks = @()
function Add-Check([string]$what, [bool]$ok, [string]$detail) {
    $script:checks += [pscustomobject]@{ what = $what; result = $(if ($ok) { 'PASS' } else { 'FAIL' }); detail = $detail }
    Write-Host ("{0}  {1}: {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $detail) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}

$procs = @()
$info = [ordered]@{}
$file = ''
try {
    Write-Host "`n=== mixd ($Mode, block $Block, pulse every $Period s) ===" -ForegroundColor Cyan
    $procs += Start-Hidden $mixd @('--port', $port, '--local-only', '--no-profiles', '--test-pulse', $Period) $mixdLog
    Start-Sleep -Milliseconds 1500
    if ($Device) {
        Set-Param '/master/out/1/target' "device/$Device"
        Set-Param '/system/clock_device' $Device
    }
    Set-Param '/system/block_size' $Block
    Set-Param '/system/audio_mode' $Mode
    $clock = ''
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        $clock = Get-Param '/system/clock_active'
        if ($clock -like 'device/*') { break }
    }
    $info.clock = $clock
    $info.master_out = Get-Param '/master/out/1/target'
    if ($clock -notlike 'device/*') { throw "no output device clocks the engine (clock: $clock); see $mixdLog" }

    Write-Host "`n=== capd, dc-av-flash ===" -ForegroundColor Cyan
    $procs += Start-Hidden $capd @('--port', $port) $capdLog
    $flashProc = Start-Hidden $flash @('--period', $Period, '--stop-file', "`"$flashStop`"", '--seconds', [int]($Minutes * 60 + 600)) $flashLog
    $procs += $flashProc
    Start-Sleep -Seconds 3 # capd connects, bridges lock
    $info.latency_ms = Get-Param '/system/latency_ms'
    $x0 = [int64](Get-Param '/system/xruns')

    Write-Host "`n=== dc-ui: input, record, busy controls for $Minutes min ===" -ForegroundColor Cyan
    $uiArgs = @('--connect', "127.0.0.1:$port", '--exercise', $Minutes, '--record', '--input', "`"$InputDevice`"", '--input-channel', $InputChannel)
    $uiProc = Start-Process -FilePath $ui -ArgumentList $uiArgs -PassThru -RedirectStandardOutput $uiLog -RedirectStandardError "$uiLog.err"
    $null = $uiProc.Handle
    $cpuMax = 0.0
    $deadline = (Get-Date).AddMinutes($Minutes + 3)
    while (-not $uiProc.HasExited) {
        Start-Sleep -Seconds 5
        $c = Get-Param '/system/cpu_load'
        if ($c -and [double]$c -gt $cpuMax) { $cpuMax = [double]$c }
        foreach ($p in $procs) { if ($p.HasExited) { throw "$($p.ProcessName) exited early (see build\g2\integration_${stamp}_*.log)" } }
        if ((Get-Date) -gt $deadline) { Stop-Process -Id $uiProc.Id -ErrorAction SilentlyContinue; throw 'dc-ui did not finish' }
    }
    $x1 = [int64](Get-Param '/system/xruns')
    $info.cpu_max = $cpuMax
    New-Item -ItemType File -Force $flashStop | Out-Null # dc-av-flash prints its result and exits
    $null = $flashProc.WaitForExit(([int]$Period + 3) * 1000)

    Get-Content $uiLog -Encoding UTF8 | ForEach-Object { Write-Host $_ }
    $uiRes = Get-Result $uiLog
    $file = $uiRes['file']
    $uiPassed = (Get-Content $uiLog -Encoding UTF8 -Raw) -match 'EXERCISE PASSED'
    Add-Check 'UI: input, recording, controls' $uiPassed ("input `"$($uiRes['input'])`", $($uiRes['steps']) steps in $($uiRes['seconds']) s, refused $($uiRes['errors'])")
    Add-Check 'Engine: no dropouts while the UI was busy' ($x1 -eq $x0) ("xruns +$($x1 - $x0) (since mixd started: $x1), cpu max $cpuMax, latency $($info.latency_ms) ms")
} catch {
    Add-Check 'Run' $false $_.Exception.Message
} finally {
    foreach ($p in $procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue } }
    Start-Sleep -Milliseconds 500
}

# capd's own count of dropped frames ("stopped FILE (N ms, F frames, D dropped)")
$stopLine = Get-Content $capdLog -ErrorAction SilentlyContinue | Where-Object { $_ -like 'stopped *' } | Select-Object -Last 1
if ($stopLine -match '\((\d+) ms, (\d+) frames, (\d+) dropped\)') {
    Add-Check 'Recording: no dropped video frames' ([int]$Matches[3] -eq 0) "$($Matches[2]) frames, $($Matches[3]) dropped, $([int]$Matches[1] / 1000) s"
} elseif ($file) {
    Add-Check 'Recording: no dropped video frames' $false "no 'stopped' line in $capdLog"
}

if ($file -and (Test-Path $file)) {
    Write-Host "`n=== $file ===" -ForegroundColor Cyan
    & $capd probe $file | ForEach-Object { Write-Host $_ }
    $syncOut = & $capd avsync $file --csv $syncCsv
    $syncOut | ForEach-Object { Write-Host $_ }
    $s = @{}
    $line = $syncOut | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1
    if ($line) { foreach ($m in [regex]::Matches($line, '(\w+)=(\S+)')) { $s[$m.Groups[1].Value] = [double]$m.Groups[2].Value } }
    if ($s.Count -gt 0) {
        $expected = [math]::Floor($s.duration_s / $Period) - 2
        $pairsOk = $s.pairs -ge [math]::Max(1, 0.95 * $expected) -and $s.pairs -ge 0.95 * $s.flashes -and $s.pairs -ge 0.95 * $s.pulses
        Add-Check 'A/V: every flash has its burst' $pairsOk "pairs $($s.pairs), flashes $($s.flashes), bursts $($s.pulses), expected about $expected"
        $spread = [math]::Max($s.max_ms - $s.median_ms, $s.median_ms - $s.min_ms)
        Add-Check 'A/V: audio within 45 ms of the picture' ([math]::Abs($s.median_ms) -le 45) ("median {0:+0.0;-0.0} ms, mean {1:+0.0;-0.0} ms (positive = audio later)" -f $s.median_ms, $s.mean_ms)
        Add-Check 'A/V: steady (each pair within 34 ms of the median)' ($spread -le 34) ("min {0:+0.0;-0.0} ms, max {1:+0.0;-0.0} ms" -f $s.min_ms, $s.max_ms)
        Add-Check 'A/V: no drift over the recording' ([math]::Abs($s.drift_ms) -le 10) ("drift {0:+0.0;-0.0} ms over {1:0} s" -f $s.drift_ms, $s.duration_s)
    } else {
        Add-Check 'A/V analysis' $false 'capd avsync gave no result'
    }
} else {
    Add-Check 'Recording file' $false "no file ($file)"
}
$flashRes = Get-Result $flashLog

$md = @("# G2 integration check ($stamp)", '',
        "mixd + capd + dc-ui, $Minutes min, $Mode, block $Block, test pulse every $Period s.",
        "Clock: $($info.clock). Master out: $($info.master_out). Input: $InputDevice (ch $InputChannel).",
        "Device notes from mixd: $((Get-Content $mixdLog -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'device note' }) -join ' / ')",
        "File: $file",
        "dc-av-flash: $($flashRes['flashes']) flashes, painted $($flashRes['paint_late_ms_median']) ms (median) / $($flashRes['paint_late_ms_max']) ms (max) after the instant.", '',
        '| check | result | detail |', '|---|---|---|')
foreach ($c in $checks) { $md += "| $($c.what) | $($c.result) | $($c.detail) |" }
$md += ''
$md += "Logs: build\g2\integration_${stamp}_*.log, pairs: $(Split-Path -Leaf $syncCsv)"
$report = Join-Path $out "integration_$stamp.md"
$md | Set-Content -Encoding utf8 $report
Write-Host "`n$($md -join "`n")"
Write-Host "`nreport: $report"
if ($checks.Count -gt 0 -and ($checks | Where-Object { $_.result -ne 'PASS' }).Count -eq 0) {
    Write-Host 'G2 INTEGRATION PASSED' -ForegroundColor Green
    exit 0
}
Write-Host 'G2 INTEGRATION FAILED' -ForegroundColor Red
exit 1
