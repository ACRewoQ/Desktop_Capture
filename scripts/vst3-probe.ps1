<#
.SYNOPSIS
  V7: build the JUCE-based VST3 probe, run its tests, and probe the VST3 plugins on this PC.

.DESCRIPTION
  1. Configures the "windows-juce" preset (first run fetches JUCE 9 with git, a few minutes)
  2. Builds dc-vst3-probe and the "DC Test Gain" fixture plugin (Release)
  3. Runs the vst3_* ctest cases (fixture probe, crash and hang isolation)
  4. Scans VST3 plugins, each in its own child process, and writes a report

  Each plugin's editor opens for a moment during the scan; leave the windows alone.

.EXAMPLE
  .\scripts\vst3-probe.ps1                          # standard VST3 folders
  .\scripts\vst3-probe.ps1 -Dir C:\dev\plugins      # only this folder (repeatable: -Dir a,b)
  .\scripts\vst3-probe.ps1 -SkipScan
#>
param(
    [string[]]$Dir = @(),
    [double]$EditorSeconds = 2,
    [int]$Timeout = 90,
    [string]$Out = 'out/vst3',
    [switch]$SkipScan
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }

Write-Host "`n=== Configure (windows-juce) ===" -ForegroundColor Cyan
cmake --preset windows-juce
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }

Write-Host "`n=== Build (Release) ===" -ForegroundColor Cyan
cmake --build --preset windows-juce-release --target dc-vst3-probe
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

Write-Host "`n=== Tests (vst3_*) ===" -ForegroundColor Cyan
ctest --preset windows-juce-release -R vst3
$testsOk = $LASTEXITCODE -eq 0

$probe = Join-Path $root 'build/windows-juce/tools/vst3_probe/dc-vst3-probe_artefacts/Release/dc-vst3-probe.exe'
$scanOk = $true
if (-not $SkipScan) {
    Write-Host "`n=== Scan ===" -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    $scanArgs = @('scan', '--editor-seconds', $EditorSeconds, '--timeout', $Timeout,
                  '--json', "$Out/report.json", '--md', "$Out/report.md")
    foreach ($d in $Dir) { $scanArgs += @('--dir', $d) }
    & $probe @scanArgs
    $scanOk = $LASTEXITCODE -eq 0
    Write-Host "report: $Out/report.md, $Out/report.json"
}

if ($testsOk) { Write-Host "`nVST3 TESTS PASSED" -ForegroundColor Green }
else { Write-Host "`nVST3 TESTS FAILED" -ForegroundColor Red; exit 1 }
if (-not $scanOk) { Write-Host 'some plugins failed the probe (see the report); this is a finding, not a build failure' -ForegroundColor Yellow }
