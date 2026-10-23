<#
.SYNOPSIS
  Installs (or with -Unregister removes) the two prototype cameras. Needs elevation once; the
  script asks for it through UAC by itself.

  - copies build\bin to a new %ProgramData%\DesktopCapture\vcam\bin-<time> folder (the Frame
    Server's LOCAL SERVICE account can read it; user profile folders are not readable by it) and
    records it in vcam\current.txt
  - creates %ProgramData%\DesktopCapture\vcam-logs writable by services (MF source log)
  - regsvr32 vcam_mf.dll  -> HKLM COM class used by MFCreateVirtualCamera
  - regsvr32 vcam_ds.dll  -> DirectShow "video input device" filter
  The MF camera itself is created per user without elevation (vcam-mf-ctl run / install).
#>
param([switch]$Unregister)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Unregister) { $argList += '-Unregister' }
    $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $p.ExitCode
}

$root = Split-Path -Parent $PSScriptRoot
$base = Join-Path $env:ProgramData 'DesktopCapture\vcam'
$current = Join-Path $base 'current.txt'   # folder of the registered DLLs
$logs = Join-Path $env:ProgramData 'DesktopCapture\vcam-logs'
$result = Join-Path $base 'register-result.txt'

function Finish($text, $code) {
    $text | Set-Content $result
    Write-Host ''
    if ($code) { Write-Host "RESULT: $text" -ForegroundColor Red } else { Write-Host "RESULT: $text" -ForegroundColor Green }
    Read-Host 'Press Enter to close'
    exit $code
}

try {
    New-Item -ItemType Directory -Force $base, $logs | Out-Null
    $oldDir = $null
    if (Test-Path $current) { $oldDir = (Get-Content $current -Raw).Trim() }
    if ($Unregister) {
        foreach ($dll in 'vcam_mf.dll', 'vcam_ds.dll') {
            $path = Join-Path $oldDir $dll
            if ($oldDir -and (Test-Path $path)) { & regsvr32.exe /s /u $path; Write-Host "unregistered $dll ($LASTEXITCODE)" }
        }
        Remove-Item $current -ErrorAction SilentlyContinue
        Finish 'UNREGISTERED' 0
    }
    & icacls.exe $logs /grant '*S-1-5-19:(OI)(CI)M' '*S-1-5-11:(OI)(CI)M' | Out-Null  # LOCAL SERVICE, Authenticated Users

    # Our own helper processes keep the DLLs open; stop them. Camera apps and the Frame Server may
    # also hold the old DLL, so every install goes to a NEW folder and the old one is only removed
    # when nothing uses it any more. A failed install never leaves the cameras unregistered.
    Get-Process vcam-feed, vcam-mf-ctl, vcam-probe -ErrorAction SilentlyContinue | Stop-Process -Force
    $dest = Join-Path $base ('bin-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item (Join-Path $root 'build\bin\*') $dest -Force

    # MF virtual cameras exist from Windows 11 (build 22000); Windows 10 gets only the DirectShow one.
    $dlls = @('vcam_ds.dll')
    if ([Environment]::OSVersion.Version.Build -ge 22000) { $dlls = @('vcam_mf.dll', 'vcam_ds.dll') }
    $ok = $true
    foreach ($dll in $dlls) {
        & regsvr32.exe /s (Join-Path $dest $dll)
        Write-Host "regsvr32 $dll -> exit code $LASTEXITCODE (0 = OK)"
        if ($LASTEXITCODE) { $ok = $false }
    }
    if (-not $ok) { Finish "FAILED (regsvr32, see above; previous registration in $oldDir kept)" 1 }
    $dest | Set-Content $current
    # Clean up older install folders that are no longer locked (ignore the ones still in use).
    Get-ChildItem $base -Directory | Where-Object { $_.FullName -ne $dest -and ($_.Name -like 'bin-*' -or $_.Name -eq 'bin') } |
        ForEach-Object { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
    Finish "REGISTERED $dest" 0
} catch {
    Finish "FAILED $_" 1
}
