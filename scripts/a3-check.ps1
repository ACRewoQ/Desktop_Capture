<#
.SYNOPSIS
  Lane A3 on a real PC: per-track recording (WAV / FLAC) and the sampler, with mixd on WASAPI.

.DESCRIPTION
  1. Builds Release (mixd, dc-cli, dc-audio-check)
  2. Starts mixd with a -20 dBFS 1 kHz test tone on the master and sets up:
       - master recorded after the fader (it carries the tone)
       - track 1 = sampler (bank 1), recorded before the fader, muted; pad 1 loops a Windows sound
       - with -InputDevice: track 2 = that input device, recorded before the fader, muted
     (muted tracks keep the master's tone clean; the pre-fader recording is not affected)
  3. Recording, once per format (-Formats wav, flac): records -Minutes while track 1's fader moves
     every 300 ms (UI operation), then checks
       - every file of the take has the same length, and it matches /recorder/elapsed_ms
       - /recorder/dropped_frames = 0 and no new /system/xruns
       - the master file's tone has no glitch (dc-audio-check, sine fit per 100 ms)
  4. Sampler: loads up to 16 sounds from C:\Windows\Media into bank 2 (load time, errors), then
     checks the pad modes through /sampler/.../playing: oneshot ends by itself, toggle and loop stop
     on the second press, hold stops on release, loop keeps going past the sound's end.
  5. Writes build\a3\a3_<time>.md and prints the table.

  The test tone plays on the output device: turn the speakers down.

.EXAMPLE
  .\scripts\a3-check.ps1
  .\scripts\a3-check.ps1 -Minutes 10 -InputDevice '{0.0.1.00000000}.{...}'
  .\scripts\a3-check.ps1 -Mode exclusive -Block 128 -Device '{0.0.0.00000000}.{...}'
#>
param(
    [double]$Minutes = 2,
    [string[]]$Formats = @('wav', 'flac'),
    [string]$Mode = 'shared',
    [int]$Block = 256,
    [string]$Device = '',
    [string]$InputDevice = '',
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd dc-cli dc-audio-check
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$check = Join-Path $bin 'tools/audio_check/Release/dc-audio-check.exe'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path $root "build/a3/$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
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
function Invoke-Method([string]$method, [string[]]$kv = @()) {
    $r = & $cli --port $port call $method @kv
    if (-not ($r -like 'ok*')) { throw "call $method $kv -> $r" }
    return $r
}
function Wait-Until([scriptblock]$cond, [double]$seconds = 10) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

Write-Host "`n=== Devices ===" -ForegroundColor Cyan
& $mixd --list-devices

$log = Join-Path $out 'mixd.log'
$proc = Start-Process -FilePath $mixd -ArgumentList '--port', $port, '--test-tone', '1000', '--no-shm' `
    -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.err"
$rows = @()
$samplerRows = @()
try {
    if (-not (Wait-Until { (Get-Param '/system/clock_active') -like 'device/*' } 15)) {
        throw 'mixd did not start on an output device (see the log)'
    }
    Set-Param '/system/audio_mode' $Mode
    Set-Param '/system/block_size' $Block
    if ($Device) { Set-Param '/master/out/1/target' "device/$Device" }
    Start-Sleep -Seconds 2

    # --- setup
    $sounds = @(Get-ChildItem 'C:\Windows\Media' -Filter '*.wav' -ErrorAction SilentlyContinue | Sort-Object Length -Descending)
    if ($sounds.Count -eq 0) { throw 'no .wav files in C:\Windows\Media for the sampler' }
    $recDir = Join-Path $out 'takes'
    Set-Param '/recorder/directory' $recDir
    Set-Param '/master/record/enabled' 'true'
    Set-Param '/master/record/tap' 'post'
    Set-Param '/track/1/name' 'Sampler'
    Set-Param '/track/1/input/kind' 'sampler'
    Set-Param '/track/1/input/source' '1'
    Set-Param '/track/1/mute' 'true'
    Set-Param '/track/1/record/enabled' 'true'
    Set-Param '/track/1/record/tap' 'pre'
    Set-Param '/sampler/bank/1/pad/1/mode' 'loop'
    Set-Param '/sampler/bank/1/pad/1/file' $sounds[0].FullName
    if ($InputDevice) {
        Set-Param '/track/2/name' 'Input'
        Set-Param '/track/2/input/kind' 'device'
        Set-Param '/track/2/input/source' $InputDevice
        Set-Param '/track/2/mute' 'true'
        Set-Param '/track/2/record/enabled' 'true'
        Set-Param '/track/2/record/tap' 'pre'
    }
    if (-not (Wait-Until { (Get-Param '/sampler/bank/1/pad/1/state') -eq 'ready' } 30)) {
        throw "pad 1 did not load: $(Get-Param '/sampler/bank/1/pad/1/error')"
    }
    Invoke-Method '/sampler/trigger' @('pad=1', 'bank=1') | Out-Null

    # --- recording
    foreach ($format in $Formats) {
        Write-Host "`n=== recording: $format, $Mode, block $Block, $Minutes min ===" -ForegroundColor Cyan
        Set-Param '/recorder/format' $format
        $xruns0 = [int64](Get-Param '/system/xruns')
        $start = Invoke-Method '/recorder/start'
        $folder = Get-Param '/recorder/folder'
        $end = (Get-Date).AddMinutes($Minutes)
        $i = 0
        while ((Get-Date) -lt $end) {
            Set-Param '/track/1/volume_db' (@(0, -6, -12)[$i % 3])
            $i++
            Start-Sleep -Milliseconds 300
        }
        Invoke-Method '/recorder/stop' | Out-Null
        Wait-Until { (Get-Param '/recorder/state') -ne 'recording' } 10 | Out-Null
        $state = Get-Param '/recorder/state'
        $elapsed = [int64](Get-Param '/recorder/elapsed_ms')
        $dropped = [int64](Get-Param '/recorder/dropped_frames')
        $xruns = [int64](Get-Param '/system/xruns') - $xruns0

        $files = @(Get-ChildItem -LiteralPath $folder -File)
        $lines = @()
        foreach ($f in $files) {
            $tone = if ($f.Name -like '00_*') { @('--tone', '1000') } else { @() }
            $lines += (& $check @tone $f.FullName)
        }
        $lines | Out-File (Join-Path $out "audio-check_$format.txt")
        $frames = @($lines | ForEach-Object { if ($_ -match ' frames=(\d+)') { [int64]$Matches[1] } })
        $glitches = ($lines | Where-Object { $_ -like '*00_*' } | ForEach-Object { if ($_ -match 'glitches=(\d+)') { [int64]$Matches[1] } }) | Select-Object -First 1
        $sameLength = ($frames | Sort-Object -Unique).Count -eq 1
        $lengthOk = $sameLength -and [math]::Abs($frames[0] * 1000 / 48000 - $elapsed) -le 1
        $durationOk = [math]::Abs($elapsed - $Minutes * 60000) -le 2000
        $note = @()
        if ($state -ne 'idle') { $note += "state $state ($(Get-Param '/recorder/error'))" }
        if ($files.Count -ne ($(if ($InputDevice) { 3 } else { 2 }))) { $note += "$($files.Count) files" }
        if (-not $sameLength) { $note += "lengths differ: $($frames -join ', ')" }
        elseif (-not $lengthOk) { $note += "length $($frames[0]) frames vs elapsed $elapsed ms" }
        if (-not $durationOk) { $note += "elapsed $elapsed ms" }
        $pass = $state -eq 'idle' -and $lengthOk -and $durationOk -and $dropped -eq 0 -and $xruns -eq 0 -and $glitches -eq 0 -and $note.Count -eq 0
        $rows += [ordered]@{ format = $format; files = $files.Count; seconds = [math]::Round($elapsed / 1000, 1);
                             same_length = $sameLength; dropped = $dropped; xruns = $xruns; tone_glitches = $glitches;
                             result = $(if ($pass) { 'PASS' } else { 'FAIL' }); note = ($note -join '; ') }
        Write-Host ($rows[-1] | Out-String)
    }
    Invoke-Method '/sampler/stop' | Out-Null

    # --- sampler: loading and modes
    Write-Host "`n=== sampler ===" -ForegroundColor Cyan
    $pick = @($sounds | Sort-Object Name | Select-Object -First 16)
    $t0 = Get-Date
    for ($p = 1; $p -le $pick.Count; $p++) { Set-Param "/sampler/bank/2/pad/$p/file" $pick[$p - 1].FullName }
    Wait-Until { -not ((& $cli --port $port snapshot '/sampler/bank/2/') -match '/state = "loading"') } 60 | Out-Null
    $loadSeconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 2)
    $states = & $cli --port $port snapshot '/sampler/bank/2/' | Where-Object { $_ -match '/state = ' }
    $ready = @($states | Where-Object { $_ -match '"ready"' }).Count
    $errors = @($states | Where-Object { $_ -match '"error"' }).Count
    $samplerRows += [ordered]@{ check = "load $($pick.Count) files"; result = $(if ($errors -eq 0 -and $ready -eq $pick.Count) { 'PASS' } else { 'FAIL' });
                                note = "$ready ready, $errors errors, $loadSeconds s" }

    # the shortest ready sound for oneshot / loop
    $short = $null; $shortMs = [int64]::MaxValue
    for ($p = 1; $p -le $pick.Count; $p++) {
        $d = [int64](Get-Param "/sampler/bank/2/pad/$p/duration_ms")
        if ($d -gt 0 -and $d -lt $shortMs) { $short = $p; $shortMs = $d }
    }
    $playing = { param($pad) (Get-Param "/sampler/bank/2/pad/$pad/playing") -eq 'true' }
    function Add-Check([string]$name, [bool]$ok, [string]$note = '') {
        $script:samplerRows += [ordered]@{ check = $name; result = $(if ($ok) { 'PASS' } else { 'FAIL' }); note = $note }
    }
    Set-Param '/sampler/active_bank' 2
    Set-Param "/sampler/bank/2/pad/$short/mode" 'oneshot'
    Invoke-Method '/sampler/trigger' @("pad=$short") | Out-Null
    $on = Wait-Until { & $playing $short } 2
    $off = Wait-Until { -not (& $playing $short) } ($shortMs / 1000 + 2)
    Add-Check "oneshot ($shortMs ms)" ($on -and $off)

    Set-Param "/sampler/bank/2/pad/$short/mode" 'loop'
    Invoke-Method '/sampler/trigger' @("pad=$short") | Out-Null
    $on = Wait-Until { & $playing $short } 2
    Start-Sleep -Milliseconds ([math]::Min($shortMs * 2 + 500, 15000))
    $still = & $playing $short
    Invoke-Method '/sampler/trigger' @("pad=$short") | Out-Null
    $off = Wait-Until { -not (& $playing $short) } 2
    Add-Check 'loop' ($on -and $still -and $off) "playing past the end: $still"

    $long = 1; $longMs = 0
    for ($p = 1; $p -le $pick.Count; $p++) {
        $d = [int64](Get-Param "/sampler/bank/2/pad/$p/duration_ms")
        if ($d -gt $longMs) { $long = $p; $longMs = $d }
    }
    Set-Param "/sampler/bank/2/pad/$long/mode" 'toggle'
    Invoke-Method '/sampler/trigger' @("pad=$long") | Out-Null
    $on = Wait-Until { & $playing $long } 2
    Invoke-Method '/sampler/trigger' @("pad=$long") | Out-Null
    $off = Wait-Until { -not (& $playing $long) } 2
    Add-Check "toggle ($longMs ms)" ($on -and $off)

    Set-Param "/sampler/bank/2/pad/$long/mode" 'hold'
    Invoke-Method '/sampler/trigger' @("pad=$long") | Out-Null
    $on = Wait-Until { & $playing $long } 2
    Invoke-Method '/sampler/release' @("pad=$long") | Out-Null
    $off = Wait-Until { -not (& $playing $long) } 2
    Add-Check 'hold' ($on -and $off)

    # all 16 at once (voices), then stop the bank
    for ($p = 1; $p -le $pick.Count; $p++) { Set-Param "/sampler/bank/2/pad/$p/mode" 'loop' }
    for ($p = 1; $p -le $pick.Count; $p++) { Invoke-Method '/sampler/trigger' @("pad=$p") | Out-Null }
    Start-Sleep -Milliseconds 500
    $count = @(& $cli --port $port snapshot '/sampler/bank/2/' | Where-Object { $_ -match '/playing = true' }).Count
    $xruns0 = [int64](Get-Param '/system/xruns')
    Start-Sleep -Seconds 5
    $xruns = [int64](Get-Param '/system/xruns') - $xruns0
    Invoke-Method '/sampler/stop' @('bank=2') | Out-Null
    $off = Wait-Until { -not ((& $cli --port $port snapshot '/sampler/bank/2/') -match '/playing = true') } 2
    Add-Check "$($pick.Count) pads at once" ($count -eq $pick.Count -and $off -and $xruns -eq 0) "$count playing, xruns $xruns, cpu $(Get-Param '/system/cpu_load')"
} finally {
    Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue
}

$md = @("# A3 check $stamp", '', "mixd $Mode, block $Block, $Minutes min per format", '',
        '| format | files | seconds | same length | dropped | xruns | tone glitches | result | note |', '|---|---|---|---|---|---|---|---|---|')
foreach ($r in $rows) { $md += "| $($r.format) | $($r.files) | $($r.seconds) | $($r.same_length) | $($r.dropped) | $($r.xruns) | $($r.tone_glitches) | $($r.result) | $($r.note) |" }
$md += @('', '| sampler | result | note |', '|---|---|---|')
foreach ($r in $samplerRows) { $md += "| $($r.check) | $($r.result) | $($r.note) |" }
$report = Join-Path $root "build/a3/a3_$stamp.md"
$md | Out-File -Encoding utf8 $report
Write-Host "`n$($md -join "`n")"
Write-Host "`nReport: $report  (takes and logs in $out)"
$failed = @($rows + $samplerRows | Where-Object { $_.result -ne 'PASS' }).Count
if ($failed) { Write-Host "$failed check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'ALL A3 CHECKS PASSED' -ForegroundColor Green
