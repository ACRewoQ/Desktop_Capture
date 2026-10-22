<#
.SYNOPSIS
  P1 verification V1: per-application audio capture (WASAPI process loopback).

.DESCRIPTION
  Builds dc-loopback-probe and runs "info" and "selftest". The self-test plays three quiet test tones
  (-20 dBFS, about 2.5 s) on the default output device from child processes and checks that each
  process can be captured on its own. Needs a working default output device. Not part of test.ps1
  because it depends on audio hardware.

.EXAMPLE
  .\scripts\v1-loopback.ps1
  .\scripts\v1-loopback.ps1 -Extra 60
#>
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [int]$Extra = 30
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

cmake --preset windows
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
cmake --build --preset "windows-$($Config.ToLower())" --target dc-loopback-probe
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

$probe = Join-Path $root "build/windows/tools/loopback_probe/$Config/dc-loopback-probe.exe"
Write-Host "`n=== info ===" -ForegroundColor Cyan
& $probe info
Write-Host "`n=== list ===" -ForegroundColor Cyan
& $probe list
Write-Host "`n=== selftest ===" -ForegroundColor Cyan
& $probe selftest --extra $Extra
exit $LASTEXITCODE
