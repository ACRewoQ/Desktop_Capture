<#
.SYNOPSIS
  Local build and test for Windows (there is no CI on GitHub; this script is the test gate).

.DESCRIPTION
  1. Configures with the "windows" CMake preset (vcpkg installs dependencies on first run)
  2. Builds all targets
  3. Runs every unit / integration test with ctest
  4. Smoke tests across real processes: dc-mock-server + dc-cli, dc-shm-tone writer + reader,
     dc-lan-tone sender + receiver

.EXAMPLE
  .\scripts\test.ps1
  .\scripts\test.ps1 -Config Release
  .\scripts\test.ps1 -SkipSmoke
#>
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [switch]$SkipSmoke
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$preset = "windows-$($Config.ToLower())"
$failures = @()

function Step($name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }
function Check($ok, $what) {
    if ($ok) { Write-Host "  OK   $what" -ForegroundColor Green }
    else { Write-Host "  FAIL $what" -ForegroundColor Red; $script:failures += $what }
}

if (-not $env:VCPKG_ROOT) {
    throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).'
}

Step 'Configure'
cmake --preset windows
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }

Step "Build ($Config)"
cmake --build --preset $preset
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

Step 'Unit and integration tests'
ctest --preset $preset
Check ($LASTEXITCODE -eq 0) 'ctest'

if (-not $SkipSmoke) {
    $bin = Join-Path $root 'build/windows'
    $mockExe = Join-Path $bin "tools/mock_server/$Config/dc-mock-server.exe"
    $cliExe = Join-Path $bin "tools/dc_cli/$Config/dc-cli.exe"
    $toneExe = Join-Path $bin "tools/shm_tone/$Config/dc-shm-tone.exe"

    Step 'Smoke: the three process skeletons start'
    foreach ($exe in @("apps/mixd/$Config/mixd.exe", "apps/capd/$Config/capd.exe", "apps/ui/$Config/dc-ui.exe")) {
        $out = & (Join-Path $bin $exe) --version
        Check ($LASTEXITCODE -eq 0 -and $out) "$exe --version -> $out"
    }

    Step 'Smoke: mock server <-> CLI client (separate processes)'
    $port = 47811
    $mock = Start-Process -FilePath $mockExe -ArgumentList '--port', $port, '--quiet' -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 800
        $snap = & $cliExe --port $port snapshot /track/1/name
        Check ($snap -match '"Mic"') 'snapshot contains /track/1/name = "Mic"'
        $set = & $cliExe --port $port set /track/1/volume_db -6
        Check ($set -eq 'ok') 'set /track/1/volume_db -6'
        $after = & $cliExe --port $port snapshot /track/1/volume_db
        Check ($after -match '= -6$') 'second client sees -6'
        $loop1 = & $cliExe --port $port set /track/1/out/2/target track/2
        $loop2 = & $cliExe --port $port set /track/2/out/2/target track/1
        Check ($loop1 -eq 'ok' -and $loop2 -match 'conflict') 'routing loop is rejected'
        $add = & $cliExe --port $port call /mixer/track/add
        Check ($add -eq 'ok 5') 'track add returns 5'
        $rec = & $cliExe --port $port call /capture/record/start
        Check ($rec -eq 'ok') 'record start'
    } finally {
        Stop-Process -Id $mock.Id -ErrorAction SilentlyContinue
    }

    Step 'Smoke: shared memory audio across processes'
    $writer = Start-Process -FilePath $toneExe -ArgumentList 'write', '--seconds', '4' -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 500
        $read = & $toneExe read --seconds 2 | Select-Object -Last 1
        Write-Host "  $read"
        Check ($read -match 'writer alive') 'reader sees a live writer'
        Check ($read -match 'overruns 0 ') 'no overruns'
        Check ($read -match ' 0=0\.2[0-9]') 'master slot carries the tone (peak ~0.25)'
        Check ($read -match ' 1=0\.2[0-9]') 'track 1 slot carries the tone'
    } finally {
        Stop-Process -Id $writer.Id -ErrorAction SilentlyContinue
    }

    Step 'Smoke: LAN audio stream across processes (UDP loopback)'
    $lanExe = Join-Path $bin "tools/lan_tone/$Config/dc-lan-tone.exe"
    $port = 47890
    $outFile = Join-Path $env:TEMP 'dc-lan-tone-recv.txt'
    $recv = Start-Process -FilePath $lanExe -ArgumentList 'recv', '--port', $port, '--seconds', '10' -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $outFile
    try {
        Start-Sleep -Milliseconds 500
        & $lanExe send --to "127.0.0.1:$port" --seconds 12 | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" }
        $recv.WaitForExit(5000) | Out-Null
        $verdict = Get-Content $outFile | Select-Object -Last 1
        Write-Host "  $verdict"
        Check ($verdict -match '^PASS') 'receiver played the tone without loss or clicks'
    } finally {
        Stop-Process -Id $recv.Id -ErrorAction SilentlyContinue
    }
}

Write-Host ''
if ($failures.Count -eq 0) {
    Write-Host 'ALL CHECKS PASSED' -ForegroundColor Green
    exit 0
}
Write-Host "$($failures.Count) CHECK(S) FAILED:" -ForegroundColor Red
$failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
exit 1
