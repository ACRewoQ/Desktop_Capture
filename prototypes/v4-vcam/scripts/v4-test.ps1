<#
.SYNOPSIS
  Automated V4 checks against the installed prototype cameras (run v4-register.ps1 first).
  No elevation needed. Prints RESULT lines and a summary; the camera apps (Zoom etc.) are checked
  by hand afterwards, see docs/verification/v4-virtual-camera.md.
#>
param([double]$Seconds = 10)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8  # vcam-probe prints device names as UTF-8
$bin = Join-Path $env:ProgramData 'DesktopCapture\vcam\bin'
$ctl = Join-Path $bin 'vcam-mf-ctl.exe'
$feed = Join-Path $bin 'vcam-feed.exe'
$probe = Join-Path $bin 'vcam-probe.exe'
$failures = @()
function Step($n) { Write-Host "`n=== $n ===" -ForegroundColor Cyan }
function Check($ok, $what) {
    if ($ok) { Write-Host "  OK   $what" -ForegroundColor Green } else { Write-Host "  FAIL $what" -ForegroundColor Red; $script:failures += $what }
}
function Result($lines) { ($lines | Where-Object { $_ -match '^RESULT' } | Select-Object -Last 1) }
function Field($line, $name) { if ($line -match "$name=([0-9.]+)") { [double]$Matches[1] } else { -1 } }

Step 'Environment'
Write-Host ((Get-CimInstance Win32_OperatingSystem).Caption + ' build ' + [Environment]::OSVersion.Version.Build)
& $ctl supported

Step 'MF camera: start (session lifetime)'
$cam = Start-Process -FilePath $ctl -ArgumentList 'run', '--seconds', ($Seconds * 4 + 30) -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3
$list = & $probe list
$list | ForEach-Object { Write-Host "  $_" }
# Windows appends a localized suffix to MF virtual cameras: "(Windows Virtual Camera)", localized on Japanese Windows.
$mfName = '^\s+mf: Desktop Capture Camera( \(Windows .+\))?$'
Check ($list -match $mfName) 'MF camera is listed by Media Foundation'
Check ($list -match '^\s+ds: Desktop Capture Camera \(DirectShow\)$') 'DirectShow camera is listed by DirectShow'
$mfInDs = [bool]($list -match '^\s+ds: Desktop Capture Camera( \(Windows .+\))?$')
Write-Host "  info: MF camera visible to DirectShow apps: $mfInDs"

try {
    Step 'MF camera without producer (no-signal picture keeps flowing)'
    $out = & $probe mf --name 'Desktop Capture Camera' --seconds 4
    $out | ForEach-Object { Write-Host "  $_" }
    $r = Result $out
    Check ((Field $r 'frames') -gt 60 -and (Field $r 'stamped') -eq 0) 'frames flow and carry no stamp'

    $feedProc = Start-Process -FilePath $feed -ArgumentList '--target', 'both', '--seconds', ($Seconds * 3 + 20) -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2

    Step 'MF camera with producer, via Media Foundation (1080p NV12)'
    $out = & $probe mf --name 'Desktop Capture Camera' --seconds $Seconds
    $out | ForEach-Object { Write-Host "  $_" }
    $r = Result $out
    Check ((Field $r 'fps') -ge 27) 'fps >= 27'
    Check ((Field $r 'unique') -ge (Field $r 'frames') * 0.9) 'frames come from the producer (stamped and unique)'
    Check ((Field $r 'avg') -ge 0 -and (Field $r 'avg') -le 100) 'average latency <= 100 ms'

    if ($mfInDs) {
        Step 'MF camera via DirectShow (Windows 11 bridge)'
        $out = & $probe ds --name 'Desktop Capture Camera (Windows' --seconds $Seconds
        $out | ForEach-Object { Write-Host "  $_" }
        $r = Result $out
        Check ((Field $r 'fps') -ge 27 -and (Field $r 'unique') -gt 0) 'DirectShow app receives the MF camera'
    }

    Step 'DirectShow camera via DirectShow'
    $out = & $probe ds --name 'Desktop Capture Camera (DirectShow)' --seconds $Seconds
    $out | ForEach-Object { Write-Host "  $_" }
    $r = Result $out
    Check ((Field $r 'fps') -ge 27) 'fps >= 27'
    Check ((Field $r 'unique') -ge (Field $r 'frames') * 0.9) 'frames come from the producer'
    Check ((Field $r 'avg') -ge 0 -and (Field $r 'avg') -le 100) 'average latency <= 100 ms'
} finally {
    if ($feedProc) { Stop-Process -Id $feedProc.Id -ErrorAction SilentlyContinue }
    Stop-Process -Id $cam.Id -ErrorAction SilentlyContinue
}

$log = Join-Path $env:ProgramData 'DesktopCapture\vcam-logs\vcam-mf.log'
if (Test-Path $log) { Step 'MF source log (last 15 lines)'; Get-Content $log -Tail 15 | ForEach-Object { Write-Host "  $_" } }

Write-Host ''
if ($failures.Count -eq 0) { Write-Host 'ALL V4 CHECKS PASSED' -ForegroundColor Green; exit 0 }
Write-Host "$($failures.Count) CHECK(S) FAILED:" -ForegroundColor Red
$failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
exit 1
