<#
.SYNOPSIS
  Installs (or with -Unregister removes) the two prototype cameras. Needs elevation once; the
  script asks for it through UAC by itself.

  - copies build\bin to %ProgramData%\DesktopCapture\vcam\bin (a folder the Frame Server's
    LOCAL SERVICE account can read; user profile folders are not readable by it)
  - creates %ProgramData%\DesktopCapture\vcam-logs writable by services (MF source log)
  - regsvr32 vcam_mf.dll  -> HKLM COM class used by MFCreateVirtualCamera
  - regsvr32 vcam_ds.dll  -> DirectShow "video input device" filter
  The MF camera itself is created per user without elevation (vcam-mf-ctl run / install).
#>
param([switch]$Unregister)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Unregister) { $args += '-Unregister' }
    $p = Start-Process powershell -Verb RunAs -ArgumentList $args -Wait -PassThru
    exit $p.ExitCode
}

$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $env:ProgramData 'DesktopCapture\vcam\bin'
$logs = Join-Path $env:ProgramData 'DesktopCapture\vcam-logs'
$result = Join-Path $env:ProgramData 'DesktopCapture\vcam\register-result.txt'

try {
    if ($Unregister) {
        foreach ($dll in 'vcam_mf.dll', 'vcam_ds.dll') {
            $path = Join-Path $dest $dll
            if (Test-Path $path) { & regsvr32.exe /s /u $path; Write-Host "unregistered $dll ($LASTEXITCODE)" }
        }
        'UNREGISTERED' | Set-Content $result
        exit 0
    }
    New-Item -ItemType Directory -Force $dest, $logs | Out-Null
    & icacls.exe $logs /grant '*S-1-5-19:(OI)(CI)M' '*S-1-5-11:(OI)(CI)M' | Out-Null  # LOCAL SERVICE, Authenticated Users
    foreach ($dll in 'vcam_mf.dll', 'vcam_ds.dll') {
        $old = Join-Path $dest $dll
        if (Test-Path $old) { & regsvr32.exe /s /u $old }
    }
    Copy-Item (Join-Path $root 'build\bin\*') $dest -Force
    $ok = $true
    foreach ($dll in 'vcam_mf.dll', 'vcam_ds.dll') {
        & regsvr32.exe /s (Join-Path $dest $dll)
        Write-Host "regsvr32 $dll -> $LASTEXITCODE"
        if ($LASTEXITCODE) { $ok = $false }
    }
    if ($ok) { 'REGISTERED' | Set-Content $result; exit 0 }
    'FAILED' | Set-Content $result
    exit 1
} catch {
    "FAILED $_" | Set-Content $result
    throw
}
