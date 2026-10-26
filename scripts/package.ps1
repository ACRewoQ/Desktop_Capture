<#
.SYNOPSIS
  P5-B: builds Release and puts everything that runs on this PC into one folder (and optionally a zip, or installs it).

.DESCRIPTION
  The folder (default build\package\DesktopCapture) holds:
    dc-tray.exe   the resident program in the notification area (starts mixd and capd, opens dc-ui)
    mixd.exe, capd.exe, dc-ui.exe, dc-cli.exe and their DLLs (FFmpeg, Opus, FLAC), the Qt DLLs and QML modules
    (windeployqt) and the MSVC runtime, so nothing has to be installed on the PC
    vcam\         the virtual camera (vcam_mf.dll, vcam_ds.dll, dc-vcam-ctl.exe); vcam-install.cmd registers it
                  (one UAC prompt, scripts\vcam-register.ps1)
    README.txt    how to use it (Japanese)
  Start dc-tray.exe from the folder; the logon item in its menu makes Windows start it at logon.

  -Install copies the folder to -InstallDir (default %LOCALAPPDATA%\Programs\DesktopCapture): a running dc-tray from
  that folder is stopped first (capd closes a recording cleanly) and started again afterwards. Keep the installed
  folder in one place: the logon entry points to it.

.EXAMPLE
  .\scripts\package.ps1
  .\scripts\package.ps1 -Zip
  .\scripts\package.ps1 -Install
  .\scripts\package.ps1 -NoBuild -Install -InstallDir 'D:\Apps\DesktopCapture'
#>
param(
    [string]$Out = '',
    [switch]$Zip,
    [switch]$NoBuild,
    [switch]$Install,
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\DesktopCapture')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $Out) { $Out = Join-Path $root 'build\package\DesktopCapture' }
if (-not $env:DC_QT_DIR) { throw 'DC_QT_DIR is not set. Run scripts\install-qt.ps1 first (dc-ui needs Qt).' }

if (-not $NoBuild) {
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd capd dc-ui dc-tray dc-cli vcam_mf vcam_ds dc-vcam-ctl
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root 'build\windows'
$exes = [ordered]@{
    'dc-tray.exe' = 'apps\tray\Release'
    'mixd.exe'    = 'apps\mixd\Release'
    'capd.exe'    = 'apps\capd\Release'
    'dc-ui.exe'   = 'apps\ui\Release'
    'dc-cli.exe'  = 'tools\dc_cli\Release'
}
foreach ($e in $exes.GetEnumerator()) {
    $src = Join-Path $bin "$($e.Value)\$($e.Key)"
    if (-not (Test-Path $src)) { throw "missing $src (build Release first, or leave out -NoBuild)" }
}

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
foreach ($e in $exes.GetEnumerator()) {
    $dir = Join-Path $bin $e.Value
    Copy-Item (Join-Path $dir $e.Key) $Out
    # vcpkg copies the DLLs each program needs next to it (FFmpeg, Opus, FLAC); they are the same files everywhere.
    Get-ChildItem $dir -Filter '*.dll' | Copy-Item -Destination $Out -Force
}

& (Join-Path $env:DC_QT_DIR 'bin\windeployqt.exe') --release --qmldir apps\ui\qml --no-translations --no-opengl-sw `
    (Join-Path $Out 'dc-ui.exe')
if ($LASTEXITCODE) { throw "windeployqt failed ($LASTEXITCODE)" }

# App-local MSVC runtime, so the PC needs no Visual C++ Redistributable.
foreach ($dll in 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll') {
    $src = Join-Path $env:SystemRoot "System32\$dll"
    if (Test-Path $src) { Copy-Item $src $Out -Force }
}

# Virtual camera: registered from this folder by vcam-install.cmd (the script copies it to %ProgramData% first).
$vcamSrc = Join-Path $bin 'vcam'
if (Test-Path (Join-Path $vcamSrc 'vcam_mf.dll')) {
    $vcamOut = Join-Path $Out 'vcam'
    New-Item -ItemType Directory -Force -Path $vcamOut | Out-Null
    foreach ($f in 'vcam_mf.dll', 'vcam_ds.dll', 'dc-vcam-ctl.exe') {
        if (Test-Path (Join-Path $vcamSrc $f)) { Copy-Item (Join-Path $vcamSrc $f) $vcamOut }
    }
    Copy-Item 'scripts\vcam-register.ps1' $vcamOut
    Set-Content -Encoding ASCII (Join-Path $Out 'vcam-install.cmd') @'
@echo off
rem Registers the virtual camera "Desktop Capture Camera" (asks for administrator rights once).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0vcam\vcam-register.ps1" -From "%~dp0vcam"
pause
'@
    Set-Content -Encoding ASCII (Join-Path $Out 'vcam-uninstall.cmd') @'
@echo off
rem Removes the virtual camera.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0vcam\vcam-register.ps1" -Unregister
pause
'@
} else {
    Write-Host "vcam not built ($vcamSrc): the package has no virtual camera" -ForegroundColor Yellow
}

$version = (& (Join-Path $Out 'mixd.exe') --version) -join ' '
$commit = (git rev-parse --short HEAD 2>$null)
Set-Content -Encoding UTF8 (Join-Path $Out 'version.txt') "$version`r`ncommit $commit`r`npackaged $(Get-Date -Format 'yyyy-MM-dd HH:mm')"

# README (scripts\package-readme.txt), written with a BOM so that Notepad and "type" read the UTF-8 right.
$readme = [IO.File]::ReadAllText((Join-Path $root 'scripts\package-readme.txt'), [Text.Encoding]::UTF8)
[IO.File]::WriteAllText((Join-Path $Out 'README.txt'), ($readme -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding $true))

$size = [math]::Round(((Get-ChildItem $Out -Recurse | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host "Package: $Out ($size MB)" -ForegroundColor Green

if ($Zip) {
    $zipFile = "$Out.zip"
    if (Test-Path $zipFile) { Remove-Item $zipFile }
    Compress-Archive -Path $Out -DestinationPath $zipFile
    Write-Host "Zip:     $zipFile" -ForegroundColor Green
}

if ($Install) {
    $tray = Join-Path $InstallDir 'dc-tray.exe'
    $wasRunning = $false
    if (Test-Path $tray) {
        $wasRunning = [bool](Get-Process dc-tray -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $tray })
        if ($wasRunning) {
            Write-Host 'Stopping the installed dc-tray (and capd, mixd)...'
            $p = Start-Process -FilePath $tray -ArgumentList '--quit' -PassThru -Wait
            if ($p.ExitCode) { throw "dc-tray --quit failed ($($p.ExitCode))" }
        }
    }
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    # Mirror, so files dropped from the package do not linger (robocopy: exit codes below 8 are success).
    robocopy $Out $InstallDir /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    Write-Host "Installed: $InstallDir" -ForegroundColor Green
    if ($wasRunning) {
        Start-Process -FilePath $tray -ArgumentList '--background'
        Write-Host 'dc-tray started again'
    }
}
