<#
.SYNOPSIS
  Installs the virtual camera (or removes it with -Unregister). Asks for elevation once (UAC).

.DESCRIPTION
  - copies build\windows\vcam (vcam_mf.dll, vcam_ds.dll, dc-vcam-ctl.exe) to a NEW folder
    %ProgramData%\DesktopCapture\vcam\bin-<time> and records it in vcam\current.txt. The Frame Server's
    LOCAL SERVICE account can read %ProgramData% (not the user profile), and a camera app or the Frame Server may
    still hold the previous DLLs, so every install goes to a new folder (V4 findings 2 and 7).
  - creates %ProgramData%\DesktopCapture\vcam-logs writable by services (the MF source's log)
  - Windows 11: regsvr32 vcam_mf.dll (HKLM COM class), then, as the normal user, "dc-vcam-ctl install" creates
    the camera "Desktop Capture Camera" for this user. Windows shows it as "... (Windows 仮想カメラ)".
  - Windows 10: regsvr32 vcam_ds.dll (DirectShow video input device "Desktop Capture Camera").
  Only one of the two is installed: on Windows 11 the MF camera reaches DirectShow apps too (V4 finding 6).
  capd feeds whichever is installed; with no frames the camera shows a "no signal" picture.

.EXAMPLE
  .\scripts\vcam-register.ps1
  .\scripts\vcam-register.ps1 -Unregister
#>
param(
    [switch]$Unregister,
    [string]$From = '',
    [switch]$Elevated
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $From) { $From = Join-Path $root 'build\windows\vcam' }
$base = Join-Path $env:ProgramData 'DesktopCapture\vcam'
$current = Join-Path $base 'current.txt'
$logs = Join-Path $env:ProgramData 'DesktopCapture\vcam-logs'
$result = Join-Path $base 'register-result.txt'
$isWin11 = [Environment]::OSVersion.Version.Build -ge 22000

function Installed-Dir {
    if (Test-Path $current) { return (Get-Content $current -Raw).Trim() }
    return $null
}

# --- elevated part: copy and regsvr32 ---------------------------------------------------------------------------
if ($Elevated) {
    function Finish($text, $code) {
        New-Item -ItemType Directory -Force $base | Out-Null
        $text | Set-Content $result
        exit $code
    }
    try {
        New-Item -ItemType Directory -Force $base, $logs | Out-Null
        $oldDir = Installed-Dir
        if ($Unregister) {
            foreach ($dll in 'vcam_mf.dll', 'vcam_ds.dll') {
                if ($oldDir -and (Test-Path (Join-Path $oldDir $dll))) { & regsvr32.exe /s /u (Join-Path $oldDir $dll) }
            }
            Remove-Item $current -ErrorAction SilentlyContinue
            Finish 'UNREGISTERED' 0
        }
        & icacls.exe $logs /grant '*S-1-5-19:(OI)(CI)M' '*S-1-5-11:(OI)(CI)M' | Out-Null  # LOCAL SERVICE, Authenticated Users
        $dest = Join-Path $base ('bin-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory -Force $dest | Out-Null
        Copy-Item (Join-Path $From '*') $dest -Force
        $dll = if ($isWin11) { 'vcam_mf.dll' } else { 'vcam_ds.dll' }
        & regsvr32.exe /s (Join-Path $dest $dll)
        if ($LASTEXITCODE) { Finish "FAILED regsvr32 $dll exit $LASTEXITCODE (previous registration in $oldDir kept)" 1 }
        # A previous install of the other kind (prototype, or an upgrade from Windows 10) is removed.
        $other = if ($isWin11) { 'vcam_ds.dll' } else { 'vcam_mf.dll' }
        if ($oldDir -and (Test-Path (Join-Path $oldDir $other))) { & regsvr32.exe /s /u (Join-Path $oldDir $other) }
        $dest | Set-Content $current
        # Older folders that nothing holds any more are removed; the ones still in use stay until next time.
        Get-ChildItem $base -Directory | Where-Object { $_.FullName -ne $dest -and ($_.Name -like 'bin-*' -or $_.Name -eq 'bin') } |
            ForEach-Object { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
        Finish "REGISTERED $dest" 0
    } catch {
        Finish "FAILED $_" 1
    }
}

# --- normal part ------------------------------------------------------------------------------------------------
if (-not $Unregister -and -not (Test-Path (Join-Path $From 'vcam_mf.dll'))) {
    throw "no camera DLLs in $From (build first: cmake --build --preset windows-release)"
}
$ctl = $null
if ($Unregister -and $isWin11) {
    $dir = Installed-Dir
    if ($dir -and (Test-Path (Join-Path $dir 'dc-vcam-ctl.exe'))) { $ctl = Join-Path $dir 'dc-vcam-ctl.exe' }
    if ($ctl) { & $ctl uninstall }
}
# capd keeps the DirectShow sender DLL loaded; stop it so an old folder can be removed.
Get-Process capd, dc-vcam-probe -ErrorAction SilentlyContinue | Stop-Process -Force
$argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated', '-From', "`"$From`"")
if ($Unregister) { $argList += '-Unregister' }
$p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden
$text = if (Test-Path $result) { Get-Content $result -Raw } else { "FAILED (no result; UAC declined?)" }
if ($p.ExitCode) { Write-Host "RESULT: $text" -ForegroundColor Red; exit 1 }
if (-not $Unregister -and $isWin11) {
    $ctl = Join-Path (Installed-Dir) 'dc-vcam-ctl.exe'
    & $ctl install
    if ($LASTEXITCODE) { Write-Host "RESULT: registered, but dc-vcam-ctl install failed ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }
}
Write-Host "RESULT: $($text.Trim())" -ForegroundColor Green
