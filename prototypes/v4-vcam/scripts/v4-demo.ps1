<#
.SYNOPSIS
  Hand check in real camera apps (Zoom, Teams, Windows Camera, browser, OBS ...).
  Keeps both prototype cameras alive with the moving test picture until you press Enter.
  Cameras: "Desktop Capture Camera" (Media Foundation; Windows adds a "(Windows ...)" suffix)
           "Desktop Capture Camera (DirectShow)"
  Run with -NoFeed to see the "no signal" picture instead.
#>
param([switch]$NoFeed)
$bin = Join-Path $env:ProgramData 'DesktopCapture\vcam\bin'
$cam = $null
if ([Environment]::OSVersion.Version.Build -ge 22000) {  # MF camera: Windows 11 only
    $cam = Start-Process -FilePath (Join-Path $bin 'vcam-mf-ctl.exe') -ArgumentList 'run' -PassThru -WindowStyle Hidden
}
$feed = $null
if (-not $NoFeed) {
    $feed = Start-Process -FilePath (Join-Path $bin 'vcam-feed.exe') -ArgumentList '--target', 'both' -PassThru -WindowStyle Hidden
}
Write-Host 'Cameras are live. Pick one of them in the camera app.'
Read-Host 'Press Enter to stop'
if ($feed) { Stop-Process -Id $feed.Id -ErrorAction SilentlyContinue }
if ($cam) { Stop-Process -Id $cam.Id -ErrorAction SilentlyContinue }
