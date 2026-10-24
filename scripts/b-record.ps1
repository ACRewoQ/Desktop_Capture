<#
.SYNOPSIS
  P2 lane B check on a real PC: capd records the desktop and the shared-memory audio into files.

.DESCRIPTION
  Runs a few short recordings with dc-shm-tone as the engine stand-in and the dc-capbench test pattern as moving
  content, then reads every file back with "capd probe" (codec, packets, first/last time of each stream).
  Results go to build\b-record\<date_time>\ (the recordings and summary.md).
  Do not run it while other measurements run on the same PC.

.EXAMPLE
  .\scripts\b-record.ps1
  .\scripts\b-record.ps1 -Seconds 20 -Config Debug
  .\scripts\b-record.ps1 -Only x264,keyframes     # only the cases whose name contains one of these
#>
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [int]$Seconds = 10,
    [switch]$SkipBuild,
    [string[]]$Only = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$bin = Join-Path $root 'build/windows'
if (-not $SkipBuild) {
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset "windows-$($Config.ToLower())" --target capd dc-shm-tone dc-capbench
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}
$capd = Join-Path $bin "apps/capd/$Config/capd.exe"
$tone = Join-Path $bin "tools/shm_tone/$Config/dc-shm-tone.exe"
$bench = Join-Path $bin "tools/capbench/$Config/dc-capbench.exe"

$outDir = Join-Path $root ("build/b-record/" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $outDir | Out-Null
$summary = Join-Path $outDir 'summary.md'

$ErrorActionPreference = 'Continue'
& $capd list 2>$null | Tee-Object -FilePath (Join-Path $outDir 'list.txt')
$ErrorActionPreference = 'Stop'

$cases = @(
    @{ name = 'wgc-h264-1080p60';     engine = $true;  args = @('--size', '1920x1080', '--fps', '60', '--audio', 'master,track/1,track/2') },
    @{ name = 'wgc-4k60-auto-hevc';   engine = $true;  args = @('--size', '3840x2160', '--fps', '60', '--bitrate', '40000') },
    @{ name = 'dda-h264-1080p60';     engine = $true;  args = @('--source', 'dda', '--size', '1920x1080') },
    @{ name = 'wgc-av1-cqp-1080p30';  engine = $true;  args = @('--codec', 'av1', '--rc', 'cqp', '--quality', '28', '--fps', '30', '--mix-fps', '60') },
    @{ name = 'wgc-h264-cbr-mp4-opus'; engine = $true; args = @('--rc', 'cbr', '--bitrate', '12000', '--container', 'mp4', '--audio-codec', 'opus') },
    @{ name = 'wgc-mf-h264-pcm';      engine = $true;  args = @('--encoder', 'mf', '--audio-codec', 'pcm') },
    @{ name = 'no-engine-silence';    engine = $false; args = @() },
    # P3 B2+B3: software encoder, and the preview stream's encoder options (low latency, forced keyframes).
    @{ name = 'x264-h264-1080p60';    engine = $true;  args = @('--encoder', 'x264', '--size', '1920x1080', '--fps', '60') },
    @{ name = 'x264-crf-720p30';      engine = $true;  args = @('--encoder', 'x264', '--rc', 'cqp', '--quality', '23', '--size', '1280x720', '--fps', '30') },
    @{ name = 'nvenc-lowlat-keyframes'; engine = $true; args = @('--low-latency', '--gop', '600', '--force-keyframes', '0.5') },
    @{ name = 'x264-lowlat-keyframes';  engine = $true; args = @('--encoder', 'x264', '--low-latency', '--gop', '600', '--force-keyframes', '0.5') }
)
if ($Only.Count) { $cases = @($cases | Where-Object { $n = $_.name; @($Only | Where-Object { $n -like "*$_*" }).Count }) }

"# capd recording check ($(Get-Date -Format 'yyyy-MM-dd HH:mm'), $Seconds s each, $Config)`n" | Set-Content $summary -Encoding utf8
"| case | result | streams |`n|---|---|---|" | Add-Content $summary -Encoding utf8

foreach ($c in $cases) {
    Write-Host "`n=== $($c.name) ===" -ForegroundColor Cyan
    $ext = if ($c.args -contains 'mp4') { 'mp4' } else { 'mkv' }
    $file = Join-Path $outDir "$($c.name).$ext"
    $writer = $null
    if ($c.engine) { $writer = Start-Process -FilePath $tone -ArgumentList 'write' -PassThru -WindowStyle Hidden }
    $pattern = Start-Process -FilePath $bench -ArgumentList 'pattern', '--seconds', ($Seconds + 6) -PassThru
    try {
        Start-Sleep -Milliseconds 1500
        # Windows PowerShell 5.1 turns a native command's stderr into errors under 'Stop'; FFmpeg warns there.
        $ErrorActionPreference = 'Continue'
        $log = & $capd record --seconds $Seconds --output $file @($c.args) 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        $log | Set-Content (Join-Path $outDir "$($c.name).log") -Encoding utf8
        $log | Select-Object -First 6 | ForEach-Object { Write-Host "  $_" }
        $result = ($log | Where-Object { $_ -match '^RESULT' } | Select-Object -Last 1)
        if (-not $result) { $result = "exit $code : $(($log | Select-Object -Last 1))" }
        Write-Host "  $result"
    } finally {
        if ($writer) { Stop-Process -Id $writer.Id -ErrorAction SilentlyContinue }
        Stop-Process -Id $pattern.Id -ErrorAction SilentlyContinue
    }
    $ErrorActionPreference = 'Continue'
    $probe = if (Test-Path $file) { & $capd probe $file 2>&1 | ForEach-Object { "$_" } } else { @('no file') }
    $ErrorActionPreference = 'Stop'
    $probe | ForEach-Object { Write-Host "  $_" }
    $streams = ($probe | Where-Object { $_ -match '^STREAM' } | ForEach-Object { $_ -replace '^STREAM ', '' }) -join '<br>'
    "| $($c.name) | $($result -replace '\|', '/') | $streams |" | Add-Content $summary -Encoding utf8
    Start-Sleep -Seconds 2
}

Write-Host "`nsummary: $summary" -ForegroundColor Green
