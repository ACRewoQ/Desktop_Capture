<#
.SYNOPSIS
  Builds the V4 virtual camera prototypes (Release, x64) into build\bin and runs the pattern test.
  No elevation needed. Run from anywhere: powershell -ExecutionPolicy Bypass -File scripts\v4-build.ps1
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
cmake -S $root -B $build -A x64
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
cmake --build $build --config Release --parallel
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
& (Join-Path $build 'bin\vcam-pattern-test.exe')
if ($LASTEXITCODE) { throw 'pattern test failed' }
Write-Host 'BUILD OK' -ForegroundColor Green
