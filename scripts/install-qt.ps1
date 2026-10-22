<#
.SYNOPSIS
  Installs prebuilt Qt 6 (MSVC x64) without admin rights, using the standalone aqtinstall (aqt.exe).

.DESCRIPTION
  Downloads aqt.exe into <Root>\aqt, resolves the Qt version (newest 6.10.x by default; aqt 3.3 cannot read the
  per-arch repository layout Qt uses from 6.11 on), installs
  qtbase + qtdeclarative (aqt's base set) plus qtshadertools into <Root>\<version>\<msvc dir>,
  and sets the user environment variable DC_QT_DIR to that folder. CMake picks it up through
  cmake/DcDependencies.cmake. Shared by all Qt based targets (UI, P1 probes V5/V6).

.EXAMPLE
  .\scripts\install-qt.ps1
  .\scripts\install-qt.ps1 -Version 6.8.3
#>
param(
    [string]$Version = 'latest',
    [string]$Root = 'C:\dev\Qt'
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path (Join-Path $Root 'aqt') | Out-Null
$aqt = Join-Path $Root 'aqt\aqt.exe'
if (-not (Test-Path $aqt)) {
    Write-Host 'Downloading aqt.exe'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/miurahr/aqtinstall/releases/latest/download/aqt.exe' -OutFile $aqt
}

if ($Version -eq 'latest') {
    $Version = (& $aqt list-qt windows desktop --spec 6.10 --latest-version).Trim()
    if ($LASTEXITCODE -or -not $Version) { throw 'could not resolve the latest Qt 6 version' }
}
Write-Host "Qt version: $Version"

# Newest 64-bit MSVC build that is not a cross compile (e.g. win64_msvc2022_64).
$arches = (& $aqt list-qt windows desktop --arch $Version) -split '\s+' | Where-Object { $_ -match '^win64_msvc\d{4}_64$' }
$arch = $arches | Sort-Object | Select-Object -Last 1
if (-not $arch) { throw "no win64 MSVC build listed for Qt $Version" }
Write-Host "Arch: $arch"

& $aqt install-qt windows desktop $Version $arch -m qtshadertools --outputdir $Root
if ($LASTEXITCODE) { throw "aqt install-qt failed ($LASTEXITCODE)" }

$dir = Join-Path (Join-Path $Root $Version) ($arch -replace '^win64_', '')
if (-not (Test-Path (Join-Path $dir 'lib\cmake\Qt6\Qt6Config.cmake'))) { throw "Qt6Config.cmake not found under $dir" }
[Environment]::SetEnvironmentVariable('DC_QT_DIR', $dir, 'User')
$env:DC_QT_DIR = $dir
Write-Host "DC_QT_DIR = $dir (open a new PowerShell for other windows to see it)" -ForegroundColor Green
