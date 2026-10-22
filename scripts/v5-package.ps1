<#
.SYNOPSIS
  P1/V5: builds dc-v5-probe (Release) and makes a self-contained folder + zip for the remote device.

.DESCRIPTION
  The folder holds dc-v5-probe.exe, dc-mock-server.exe, the Qt DLLs and QML modules (windeployqt),
  the MSVC runtime DLLs, v5-measure.ps1 and measure.cmd. Copy it to the device and double-click
  measure.cmd; nothing needs to be installed there.

.EXAMPLE
  .\scripts\v5-package.ps1
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $env:DC_QT_DIR) { throw 'DC_QT_DIR is not set. Run scripts\install-qt.ps1 first.' }

cmake --preset windows
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
cmake --build --preset windows-release --target dc-v5-probe dc-mock-server
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

$out = Join-Path $root 'build\v5-package\dc-v5-probe'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item 'build\windows\tools\v5_remote_probe\Release\dc-v5-probe.exe' $out
Copy-Item 'build\windows\tools\mock_server\Release\dc-mock-server.exe' $out

& (Join-Path $env:DC_QT_DIR 'bin\windeployqt.exe') --release --qmldir tools\v5_remote_probe\qml `
    --no-translations --no-opengl-sw (Join-Path $out 'dc-v5-probe.exe')
if ($LASTEXITCODE) { throw "windeployqt failed ($LASTEXITCODE)" }

# App-local MSVC runtime, so the device needs no Visual C++ Redistributable.
foreach ($dll in 'msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    $src = Join-Path $env:SystemRoot "System32\$dll"
    if (Test-Path $src) { Copy-Item $src $out }
}

Copy-Item 'scripts\v5-measure.ps1' $out
Set-Content -Encoding ASCII (Join-Path $out 'measure.cmd') @'
@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0v5-measure.ps1" %*
pause
'@
Set-Content -Encoding ASCII (Join-Path $out 'touch-try.cmd') @'
@echo off
rem Opens the probe full screen with 16 strips for trying it by hand. Close with Alt+F4.
start "" "%~dp0dc-v5-probe.exe" --ui custom --tracks 16 --fullscreen
'@

$zip = Join-Path $root 'build\v5-package\dc-v5-probe.zip'
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $out -DestinationPath $zip
$size = [math]::Round(((Get-ChildItem $out -Recurse | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host "Package: $out ($size MB)" -ForegroundColor Green
Write-Host "Zip:     $zip" -ForegroundColor Green
