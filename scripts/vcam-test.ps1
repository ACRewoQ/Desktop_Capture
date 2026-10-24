<#
.SYNOPSIS
  Checks the installed virtual camera (run vcam-register.ps1 first) with capd as the producer. No elevation.

.DESCRIPTION
  Windows 11 (Media Foundation camera) / Windows 10 (DirectShow camera):
    1. the camera is listed
    2. without capd it delivers the "no signal" picture
    3. "capd vcam --test-pattern": stamped frames, fps >= 27, frames from the producer, latency <= 100 ms
       (Windows 11: 1080p and 720p through MF, and the DirectShow apps' view of the MF camera)
    4. "capd vcam": the primary monitor through the real GPU path (VcamOutput), fps >= 27
  The camera apps themselves (Chrome, Teams, Zoom...) are checked by hand afterwards.
#>
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [double]$Seconds = 10
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8  # the probe prints device names as UTF-8
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build/windows'
$capd = Join-Path $bin "apps/capd/$Config/capd.exe"
$probe = Join-Path $bin "tools/vcam_probe/$Config/dc-vcam-probe.exe"
$win11 = [Environment]::OSVersion.Version.Build -ge 22000
$api = if ($win11) { 'mf' } else { 'ds' }
$name = 'Desktop Capture Camera'

$failures = @()
function Step($n) { Write-Host "`n=== $n ===" -ForegroundColor Cyan }
function Check($ok, $what) {
    if ($ok) { Write-Host "  OK   $what" -ForegroundColor Green } else { Write-Host "  FAIL $what" -ForegroundColor Red; $script:failures += $what }
}
function Result($lines) { ($lines | Where-Object { $_ -match '^RESULT' } | Select-Object -Last 1) }
function Field($line, $f) { if ($line -match "$f=([0-9.]+)") { [double]$Matches[1] } else { -1 } }
function Probe([string]$how, [string[]]$extra = @()) {
    $out = & $probe $how --name $name --seconds $Seconds @extra 2>&1 | ForEach-Object { "$_" }
    $out | Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
    return (Result $out)
}

Step 'Environment'
Write-Host ((Get-CimInstance Win32_OperatingSystem).Caption + ' build ' + [Environment]::OSVersion.Version.Build)
$installed = Join-Path $env:ProgramData 'DesktopCapture\vcam\current.txt'
Check (Test-Path $installed) "camera installed ($installed)"
Get-Process capd -ErrorAction SilentlyContinue | Stop-Process -Force

Step 'Device list'
$list = & $probe list
$list | ForEach-Object { Write-Host "  $_" }
Check ($list -match "^\s+${api}: $name") "the camera is listed ($api)"

Step 'No producer: the no-signal picture keeps flowing'
$r = Probe $api @()
Check ((Field $r 'frames') -gt ($Seconds * 20) -and (Field $r 'stamped') -eq 0) 'frames flow without capd and carry no stamp'

Step 'capd vcam --test-pattern'
$feed = Start-Process -FilePath $capd -ArgumentList 'vcam', '--test-pattern', '--seconds', ($Seconds * 4 + 20) -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Seconds 2
    $cases = if ($win11) { @(@{ how = 'mf'; extra = @('--width', '1920', '--height', '1080'); label = 'MF 1080p NV12' },
                             @{ how = 'mf'; extra = @('--width', '1280', '--height', '720'); label = 'MF 720p (source scales)' },
                             @{ how = 'ds'; extra = @(); label = 'DirectShow apps see the MF camera' }) }
             else { @(@{ how = 'ds'; extra = @(); label = 'DirectShow 1080p' }) }
    foreach ($c in $cases) {
        Write-Host "  -- $($c.label)"
        $r = Probe $c.how $c.extra
        Check ((Field $r 'fps') -ge 27) "$($c.label): fps >= 27"
        Check ((Field $r 'unique') -ge (Field $r 'frames') * 0.9) "$($c.label): frames come from capd"
        Check ((Field $r 'avg') -ge 0 -and (Field $r 'avg') -le 100) "$($c.label): average latency <= 100 ms"
    }
} finally {
    Stop-Process -Id $feed.Id -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 1

Step 'capd vcam (primary monitor through the GPU path)'
$log = Join-Path $env:TEMP 'dc-vcam-test-capd.log'
$feed = Start-Process -FilePath $capd -ArgumentList 'vcam', '--seconds', ($Seconds + 8) -PassThru -WindowStyle Hidden -RedirectStandardOutput $log
try {
    Start-Sleep -Seconds 3
    $r = Probe $api @()
    Check ((Field $r 'fps') -ge 27) 'monitor picture: fps >= 27'
    $feed.WaitForExit(($Seconds + 15) * 1000) | Out-Null
} finally {
    Stop-Process -Id $feed.Id -ErrorAction SilentlyContinue
}
$capdOut = Get-Content $log -ErrorAction SilentlyContinue
$capdOut | Select-Object -Last 4 | ForEach-Object { Write-Host "  capd: $_" }
$cr = Result $capdOut
Check ((Field $cr 'frames') -gt $Seconds * 25) 'capd handed the frames to the camera'
Check ($capdOut -match '\| active') 'capd saw the camera app reading'

$mflog = Join-Path $env:ProgramData 'DesktopCapture\vcam-logs\vcam-mf.log'
if ($win11 -and (Test-Path $mflog)) { Step 'MF source log (last 10 lines)'; Get-Content $mflog -Tail 10 | ForEach-Object { Write-Host "  $_" } }

Write-Host ''
if ($failures.Count -eq 0) { Write-Host 'ALL VCAM CHECKS PASSED' -ForegroundColor Green; exit 0 }
Write-Host "$($failures.Count) CHECK(S) FAILED:" -ForegroundColor Red
$failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
exit 1
