<#
.SYNOPSIS
  V6 (preview playback) measurement matrix. Builds nothing; run scripts\test.ps1 -Config Release first
  (or cmake --build --preset windows-release).

.DESCRIPTION
  Runs dc-v6-bench and dc-v6-preview with hardware (D3D11/DXVA) and software decoding at 1080p60 and 720p30,
  plus the TCP receiver mode over loopback, and writes every report to build\v6-results\<timestamp>\.
  While the hardware runs are going it samples the GPU "Video Decode" engine utilisation to prove that
  DXVA is really in use, and it takes a screenshot of the preview window (on-screen clock vs. the time
  burned into the frame) as an independent latency cross-check.

.EXAMPLE
  .\scripts\v6-measure.ps1
  .\scripts\v6-measure.ps1 -Seconds 30 -Config Debug
#>
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [int]$Seconds = 20
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build/windows/tools/v6_preview/$Config"
$bench = Join-Path $bin 'dc-v6-bench.exe'
$preview = Join-Path $bin 'dc-v6-preview.exe'
if (-not (Test-Path $bench)) { throw "missing $bench (build the $Config configuration first)" }
$havePreview = Test-Path $preview
if (-not $havePreview) { Write-Warning "dc-v6-preview.exe not built (Qt not found?) - only bench runs" }

$out = Join-Path $root ("build/v6-results/" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $out | Out-Null
Write-Host "results -> $out"

function Sample-VideoDecode([int]$seconds) {
    # Average utilisation of all "VideoDecode" GPU engines over the window (percent, summed over engines).
    try {
        $s = Get-Counter '\GPU Engine(*engtype_VideoDecode)\Utilization Percentage' -SampleInterval 1 `
            -MaxSamples $seconds -ErrorAction Stop
        $vals = $s | ForEach-Object { ($_.CounterSamples | Measure-Object CookedValue -Sum).Sum }
        return [math]::Round(($vals | Measure-Object -Average).Average, 1)
    } catch { return 'n/a' }
}

function Take-Screenshot([string]$path) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

$summary = @()

function Run-Case([string]$name, [string]$exe, [string[]]$caseArgs, [switch]$Gpu, [switch]$Shot) {
    Write-Host "`n=== $name ===" -ForegroundColor Cyan
    $log = Join-Path $out "$name.txt"
    $csv = Join-Path $out "$name.csv"
    $all = $caseArgs + @('--seconds', $Seconds, '--csv', $csv, '--label', $name)
    $p = Start-Process -FilePath $exe -ArgumentList $all -RedirectStandardOutput $log `
        -RedirectStandardError "$log.err" -PassThru -NoNewWindow
    $null = $p.Handle  # cache the handle so ExitCode is available after exit
    $decodeLoad = ''
    if ($Gpu) {
        Start-Sleep -Seconds 4
        $decodeLoad = Sample-VideoDecode ([math]::Max(3, $Seconds - 8))
    }
    if ($Shot) {
        Start-Sleep -Seconds 2
        Take-Screenshot (Join-Path $out "$name.png")
    }
    $p.WaitForExit()
    Get-Content $log | Write-Host
    $err = Get-Content "$log.err" -ErrorAction SilentlyContinue
    if ($err) { $err | Write-Host -ForegroundColor Yellow }
    $total = (Get-Content $log | Where-Object { $_ -match '^TOTAL' }) -join ''
    $cpu = (Get-Content $log | Where-Object { $_ -match '^process CPU' }) -join ''
    $script:summary += [pscustomobject]@{
        case = $name; exit = $p.ExitCode; total = $total; cpu = $cpu; videoDecodeEngine = $decodeLoad
    }
}

$hd = @('--size', '1920x1080', '--fps', '60', '--kbps', '8000')
$sd = @('--size', '1280x720', '--fps', '30', '--kbps', '3000')

Run-Case 'bench-1080p60-hw' $bench ($hd + @('--decoder', 'hw')) -Gpu
Run-Case 'bench-1080p60-sw' $bench ($hd + @('--decoder', 'sw'))
if ($havePreview) {
    Run-Case 'preview-1080p60-hw' $preview ($hd + @('--decoder', 'hw')) -Gpu -Shot
    Run-Case 'preview-1080p60-sw' $preview ($hd + @('--decoder', 'sw'))
    Run-Case 'preview-720p30-hw' $preview ($sd + @('--decoder', 'hw')) -Gpu
    Run-Case 'preview-720p30-sw' $preview ($sd + @('--decoder', 'sw'))

    # Network mode over loopback: receiver window + sender, as it would run between two PCs.
    Write-Host "`n=== net-loopback-720p30-hw ===" -ForegroundColor Cyan
    $rlog = Join-Path $out 'net-loopback-receiver.txt'
    $recv = Start-Process -FilePath $preview -ArgumentList ($sd + @('--decoder', 'hw', '--listen', '47860',
            '--seconds', ($Seconds + 6), '--label', 'net-loopback-receiver')) `
        -RedirectStandardOutput $rlog -RedirectStandardError "$rlog.err" -PassThru -NoNewWindow
    Start-Sleep -Seconds 3
    Run-Case 'net-loopback-720p30-sender' $bench ($sd + @('--send', '127.0.0.1:47860'))
    $recv.WaitForExit()
    Get-Content $rlog | Write-Host
}

Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$summary | Format-List | Out-String | Tee-Object -FilePath (Join-Path $out 'summary.txt') | Write-Host
