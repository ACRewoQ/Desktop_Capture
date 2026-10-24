<#
.SYNOPSIS
  P3 lane B1 check on a real PC: the video mixer (sources, scenes, layers, transitions, up to 4 mixes).

.DESCRIPTION
  1. "capd mixtest": checks pixels of colour / image / text layers, crop, fit, fill, opacity, chroma key, fade and
     cut, opens the primary monitor (WGC and DDA), a Notepad window, the first camera and a video file, runs four
     mixes at once (one at 4K) and changes a canvas size. Pictures and summary.md go to the output folder.
  2. The engine path: dc-mock-server --capd + capd. Adds a text layer over the monitor and a second scene through
     dc-cli, records 6 s while switching scenes with a fade, and checks that capd reported its sources and the
     monitor / camera lists into the tree.
  Results: build\b1-video\<date_time>\ (summary.md, PNGs, the recording, logs).

.EXAMPLE
  .\scripts\b1-video.ps1
  .\scripts\b1-video.ps1 -SkipBuild -Media C:\Users\me\Videos\clip.mp4
#>
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [string]$Media = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$bin = Join-Path $root 'build/windows'
if (-not $SkipBuild) {
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset "windows-$($Config.ToLower())" --target capd dc-mock-server dc-cli
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}
$capd = Join-Path $bin "apps/capd/$Config/capd.exe"
$mock = Join-Path $bin "tools/mock_server/$Config/dc-mock-server.exe"
$cli = Join-Path $bin "tools/dc_cli/$Config/dc-cli.exe"

$outDir = Join-Path $root ("build/b1-video/" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $outDir | Out-Null

if (-not $Media) {
    $videos = [Environment]::GetFolderPath('MyVideos')
    $found = Get-ChildItem -Path $videos -Include *.mp4 -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $Media = $found.FullName }
}

# --- 1. self-test ---
Write-Host "`n=== capd mixtest ===" -ForegroundColor Cyan
$ErrorActionPreference = 'Continue'
$mixArgs = @('mixtest', '--out', (Join-Path $outDir 'mixtest'))
if ($Media) { $mixArgs += @('--media', $Media) }
& $capd @mixArgs 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath (Join-Path $outDir 'mixtest.log')
$mixCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'

# --- 2. engine path (mock server delegates recording to capd) ---
Write-Host "`n=== engine path: dc-mock-server --capd + capd ===" -ForegroundColor Cyan
$port = 47833
$mockProc = Start-Process -FilePath $mock -ArgumentList '--port', $port, '--capd', '--quiet', '--no-meters' -PassThru -WindowStyle Hidden
$capdLog = Join-Path $outDir 'capd-serve.log'
$capdProc = Start-Process -FilePath $capd -ArgumentList '--port', $port -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $capdLog -RedirectStandardError (Join-Path $outDir 'capd-serve.err')
$engineLines = @()
function DcCli { param([Parameter(ValueFromRemainingArguments)]$a) $r = & $cli --port $port @a 2>&1 | ForEach-Object { "$_" }; $script:engineLines += "> dc-cli $($a -join ' ')"; $script:engineLines += $r; $r }
try {
    Start-Sleep -Seconds 3
    $ErrorActionPreference = 'Continue'
    DcCli set /capture/directory $outDir | Out-Null
    $src = (DcCli call /video/source/add kind=text) -replace '^ok ', ''
    DcCli set "/video/source/$src/text/content" "B1 engine path" | Out-Null
    DcCli set "/video/source/$src/text/background" 4278190335 | Out-Null
    $layer = (DcCli call /video/scene/1/layer/add source=$src) -replace '^ok ', ''
    DcCli set "/video/scene/1/layer/$layer/width" 0.4 | Out-Null
    DcCli set "/video/scene/1/layer/$layer/height" 0.2 | Out-Null
    $scene2 = (DcCli call /video/scene/add) -replace '^ok ', ''
    $color = (DcCli call /video/source/add kind=color) -replace '^ok ', ''
    DcCli set "/video/source/$color/color/value" 4278255360 | Out-Null
    DcCli call "/video/scene/$scene2/layer/add" source=$color | Out-Null
    DcCli set /video/mix/1/preview_scene $scene2 | Out-Null
    DcCli set /video/mix/1/transition/duration_ms 1000 | Out-Null
    DcCli call /capture/record/start | Out-Null
    Start-Sleep -Seconds 2
    DcCli call /video/mix/1/take | Out-Null
    Start-Sleep -Seconds 2
    DcCli call /video/mix/1/take | Out-Null
    Start-Sleep -Seconds 2
    DcCli call /capture/record/stop | Out-Null
    Start-Sleep -Seconds 2
    $state = DcCli snapshot /capture
    $sources = DcCli snapshot /video/source
    $devices = DcCli snapshot /video/devices
    $ErrorActionPreference = 'Stop'
} finally {
    Stop-Process -Id $capdProc.Id -ErrorAction SilentlyContinue
    Stop-Process -Id $mockProc.Id -ErrorAction SilentlyContinue
}
$engineLines | Set-Content (Join-Path $outDir 'engine-path.log') -Encoding utf8
$file = ($state | Where-Object { $_ -match '^/capture/file = "(.*)"' } | ForEach-Object { $Matches[1] -replace '\\\\', '\' }) | Select-Object -First 1
$probe = if ($file -and (Test-Path $file)) { $ErrorActionPreference = 'Continue'; & $capd probe $file 2>&1 | ForEach-Object { "$_" }; $ErrorActionPreference = 'Stop' } else { @("no recording ($file)") }
$probe | Set-Content (Join-Path $outDir 'engine-path-probe.txt') -Encoding utf8
$sourceOk = [bool]($sources | Where-Object { $_ -match '/video/source/1/state = "ok"' })
$textOk = [bool]($sources | Where-Object { $_ -match "/video/source/$src/state = `"(ok|none)`"" })
$displaysListed = [bool]($devices | Where-Object { $_ -match '/video/devices/display/1/id' })
$recorded = [bool]($probe | Where-Object { $_ -match 'video' })

$summary = Join-Path $outDir 'summary.md'
"# B1 video mixer check ($(Get-Date -Format 'yyyy-MM-dd HH:mm'), $Config)`n" | Set-Content $summary -Encoding utf8
"## capd mixtest (exit $mixCode)`n" | Add-Content $summary -Encoding utf8
$mt = Join-Path $outDir 'mixtest/summary.md'
if (Test-Path $mt) { Get-Content $mt | Select-Object -Skip 2 | Add-Content $summary -Encoding utf8 }
"`n## engine path`n" | Add-Content $summary -Encoding utf8
"| check | result |`n|---|---|" | Add-Content $summary -Encoding utf8
"| monitor source reported ok | $(if ($sourceOk) {'PASS'} else {'FAIL'}) |" | Add-Content $summary -Encoding utf8
"| text source reported | $(if ($textOk) {'PASS'} else {'FAIL'}) |" | Add-Content $summary -Encoding utf8
"| monitor list reported | $(if ($displaysListed) {'PASS'} else {'FAIL'}) |" | Add-Content $summary -Encoding utf8
"| recording with scene switches | $(if ($recorded) {'PASS'} else {'FAIL'}) ($file) |" | Add-Content $summary -Encoding utf8
"`n``````" | Add-Content $summary -Encoding utf8
$probe | Add-Content $summary -Encoding utf8
"``````" | Add-Content $summary -Encoding utf8

Write-Host "`nSummary: $summary" -ForegroundColor Cyan
Get-Content $summary | ForEach-Object { Write-Host $_ }
