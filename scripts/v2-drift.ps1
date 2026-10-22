<#
.SYNOPSIS
  Verification V2 (clock drift correction between two audio devices).

.DESCRIPTION
  1. Builds Release (dc-drift-sim, dc-drift-test)
  2. Runs the offline simulation for every scenario (-SimMinutes, default 60 simulated minutes each)
  3. Lists the output devices
  4. Unless -SimOnly: runs the real test between devices A and B for -Minutes (default 60) and keeps the
     log and CSV under build\v2\

.EXAMPLE
  .\scripts\v2-drift.ps1 -SimOnly
  .\scripts\v2-drift.ps1 -A 0 -B 2 -Minutes 60
#>
param(
    [int]$A = -1,
    [int]$B = -1,
    [double]$Minutes = 60,
    [double]$SimMinutes = 60,
    [switch]$SimOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }

cmake --preset windows
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
cmake --build --preset windows-release --target dc-drift-sim dc-drift-test
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

$bin = Join-Path $root 'build/windows/tools'
$sim = Join-Path $bin 'drift_sim/Release/dc-drift-sim.exe'
$test = Join-Path $bin 'drift_test/Release/dc-drift-test.exe'
$out = Join-Path $root 'build/v2'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'

Write-Host "`n=== Simulation ($SimMinutes min per scenario) ===" -ForegroundColor Cyan
& $sim --minutes $SimMinutes | Tee-Object -FilePath (Join-Path $out "sim_$stamp.txt")
$simOk = $LASTEXITCODE -eq 0

Write-Host "`n=== Output devices ===" -ForegroundColor Cyan
& $test --list

if ($SimOnly) {
    if ($simOk) { Write-Host 'SIMULATION PASSED' -ForegroundColor Green; exit 0 }
    Write-Host 'SIMULATION FAILED' -ForegroundColor Red; exit 1
}

Write-Host "`n=== Real devices ($Minutes min) ===" -ForegroundColor Cyan
$testArgs = @('--minutes', $Minutes, '--csv', (Join-Path $out "real_$stamp.csv"))
if ($A -ge 0) { $testArgs += @('--a', $A) }
if ($B -ge 0) { $testArgs += @('--b', $B) }
& $test @args | Tee-Object -FilePath (Join-Path $out "real_$stamp.txt")
$realOk = $LASTEXITCODE -eq 0

if ($simOk -and $realOk) { Write-Host 'V2 ALL PASSED' -ForegroundColor Green; exit 0 }
Write-Host "V2 FAILED (simulation $simOk, real $realOk)" -ForegroundColor Red
exit 1
