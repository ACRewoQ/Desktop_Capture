<#
.SYNOPSIS
  P1-V3 measurement: desktop capture + hardware encoding, CPU load per configuration.

.DESCRIPTION
  Runs dc-capbench over a matrix of capture APIs, encoders and sizes (each with the moving test pattern on screen,
  unless noted) and writes a Markdown table plus the raw RESULT lines to a results folder.
  The pattern window covers the middle quarter of the primary monitor while each case runs; do not use the PC
  for anything heavy during the run (it takes about (Seconds + 5) x cases).

.EXAMPLE
  .\scripts\v3-bench.ps1
  .\scripts\v3-bench.ps1 -Seconds 60 -Config Release
  .\scripts\v3-bench.ps1 -Only 'wgc-h264-1080p'
#>
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [int]$Seconds = 30,
    [string]$OutDir = '',
    [string[]]$Only = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build/windows/tools/capbench/$Config/dc-capbench.exe"
if (-not (Test-Path $exe)) { throw "not built: $exe (cmake --build --preset windows-$($Config.ToLower()))" }
if (-not $OutDir) { $OutDir = Join-Path $root ("build/v3-results/" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# name, arguments
$cases = @(
    @('wgc-h264-1080p', '--source wgc --encoder h264_nvenc --size 1920x1080 --pattern window'),
    @('wgc-hevc-1080p', '--source wgc --encoder hevc_nvenc --size 1920x1080 --pattern window'),
    @('wgc-av1-1080p', '--source wgc --encoder av1_nvenc --size 1920x1080 --pattern window'),
    @('wgc-h264-1080p-lowlat', '--source wgc --encoder h264_nvenc --size 1920x1080 --pattern window --opt preset=p1 --opt tune=ull'),
    @('wgc-h264-1080p-static', '--source wgc --encoder h264_nvenc --size 1920x1080'),
    @('wgc-h264-1080p-full', '--source wgc --encoder h264_nvenc --size 1920x1080 --pattern full'),
    @('wgc-h264-1080p-mkv', "--source wgc --encoder h264_nvenc --size 1920x1080 --pattern window --output `"$OutDir\wgc-h264-1080p.mkv`""),
    @('wgc-none-1080p', '--source wgc --encoder none --size 1920x1080 --pattern window'),
    @('wgc-h264-native', '--source wgc --encoder h264_nvenc --size native --bitrate 40M --pattern window'),
    @('wgc-hevc-native', '--source wgc --encoder hevc_nvenc --size native --bitrate 40M --pattern window'),
    @('wgc-mf-h264-1080p', '--source wgc --encoder h264_mf --size 1920x1080 --pattern window'),
    @('dda-h264-1080p', '--source dda --encoder h264_nvenc --size 1920x1080 --pattern window'),
    @('dda-hevc-1080p', '--source dda --encoder hevc_nvenc --size 1920x1080 --pattern window'),
    @('dda-h264-native', '--source dda --encoder h264_nvenc --size native --bitrate 40M --pattern window')
)

& $exe list | Tee-Object -FilePath (Join-Path $OutDir 'list.txt')

$rows = @()
foreach ($c in $cases) {
    $name, $argLine = $c
    if ($Only.Count -and -not ($Only -contains $name)) { continue }
    Write-Host "`n=== $name ===" -ForegroundColor Cyan
    $csv = Join-Path $OutDir "$name.csv"
    $log = Join-Path $OutDir "$name.log"
    $cmd = "& `"$exe`" run $argLine --seconds $Seconds --csv `"$csv`""
    # FFmpeg logs to stderr; Windows PowerShell turns those lines into error records, which 'Stop' would make fatal.
    $ErrorActionPreference = 'Continue'
    Invoke-Expression "$cmd 2>&1" | ForEach-Object { "$_" } | Tee-Object -FilePath $log | Write-Host
    $ErrorActionPreference = 'Stop'
    $result = Get-Content $log | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1
    $kv = @{ case = $name }
    if ($result) {
        foreach ($m in [regex]::Matches($result, '(\w+)=("[^"]*"|\S+)')) { $kv[$m.Groups[1].Value] = $m.Groups[2].Value.Trim('"') }
    } else {
        $kv['error'] = (Get-Content $log | Select-Object -Last 1)
    }
    $rows += [pscustomobject]$kv
    Start-Sleep -Seconds 2
}

$cols = 'case', 'capture_fps', 'encode_fps', 'dropped', 'cpu_avg_pct', 'cpu_max_pct', 'cpu_core_avg_pct',
        'sys_cpu_avg_pct', 'gpu_3d_pct', 'gpu_videoproc_pct', 'gpu_encode_pct', 'mbps', 'latency_avg_ms',
        'latency_max_ms', 'working_set_mb', 'error'
$md = @(('| ' + ($cols -join ' | ') + ' |'), ('|' + (($cols | ForEach-Object { '---' }) -join '|') + '|'))
foreach ($r in $rows) { $md += '| ' + (($cols | ForEach-Object { $r.$_ }) -join ' | ') + ' |' }
$md | Set-Content -Encoding utf8 (Join-Path $OutDir 'summary.md')
$rows | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $OutDir 'summary.csv')
Write-Host ''
$md | Write-Host
Write-Host "`nresults: $OutDir" -ForegroundColor Green
