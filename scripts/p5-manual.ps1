<#
.SYNOPSIS
  P5-C items that need a person at the PC (gate G5, "untested" rows of the plan): a USB drive pulled out during a
  recording, the virtual camera in the desktop Discord / Teams apps, and one real GPU reset (TDR).

.DESCRIPTION
  Builds Release (mixd, capd, dc-ui, dc-cli) unless -NoBuild. Every case starts its own mixd (a 440 Hz test tone on
  the master, which goes to VB-CABLE so the speakers stay quiet) and capd, with:
    video mix 1  the primary monitor -> recording
    video mix 2  a coloured text -> virtual camera
  and the PC's own dc-ui on the preview of mix 1. Pop-ups (in Japanese) tell the person what to do, with plenty of time.

    usb    records to the USB drive (-UsbDrive E, or the only removable drive), asks to pull it out: the recording must
           stop safely (error, no crash), the engine, the virtual camera and the UI go on; asks to put it back: the
           file up to the pull plays, and a new recording on C: works.
    vcam   the virtual camera shows the text; asks to pick "Desktop Capture Camera" in Discord (Teams: not tested) and
           press OK when done. Logs every change of /video/vcam/state (active while an app has the camera open).
           Whether the picture showed is the person's answer.
    tdr    one real GPU reset with dxcap -forcetdr (admin: a UAC prompt; installs the Windows "Graphics Tools" feature
           first if dxcap is missing). The recording must go on in a new file, the virtual camera and the monitor
           source come back, mixd and dc-ui keep running. Every program using the GPU sees the reset: close
           games and video calls first.

  Writes build\p5m\<time>\p5m_<time>.md and prints it.

.EXAMPLE
  .\scripts\p5-manual.ps1 -Cases usb
  .\scripts\p5-manual.ps1 -NoBuild -Cases vcam,tdr
#>
param(
    [string[]]$Cases = @('usb', 'vcam', 'tdr'),
    [string]$UsbDrive = '',
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$Cases = @($Cases | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    $ErrorActionPreference = 'Continue'  # cmake warnings on stderr
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd capd dc-ui dc-cli
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
    $ErrorActionPreference = 'Stop'
}
$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$capd = Join-Path $bin 'apps/capd/Release/capd.exe'
$ui = Join-Path $bin 'apps/ui/Release/dc-ui.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
foreach ($exe in @($mixd, $capd, $ui, $cli)) { if (-not (Test-Path $exe)) { throw "missing $exe (build first)" } }
if ($env:DC_QT_DIR -and -not (Test-Path (Join-Path (Split-Path $ui) 'Qt6Core.dll'))) {
    $env:PATH = (Join-Path $env:DC_QT_DIR 'bin') + ';' + $env:PATH
}
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path $root "build/p5m/$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$port = 47883

# ---------------------------------------------------------------------------------------------------- helpers
function Get-Param([string]$path) {
    $line = & $cli --port $port snapshot $path | Where-Object { $_ -like "$path = *" } | Select-Object -First 1
    if (-not $line) { return $null }
    $v = $line.Substring($path.Length + 3)
    if ($v.StartsWith('"')) { return $v.Trim('"') -replace '\\\\', '\' }
    return $v
}
function Set-Param([string]$path, $value) {
    if ("$value" -eq '') { $value = '""' }
    $r = & $cli --port $port set $path $value
    if ($r -ne 'ok') { throw "set $path $value -> $r" }
}
function Call([string]$method, [string[]]$kv = @()) {
    $r = & $cli --port $port call $method @kv
    if ($r -notlike 'ok*') { throw "call $method $($kv -join ' ') -> $r" }
    return ($r -replace '^ok ?', '').Trim('"')
}
function Wait-Until([scriptblock]$cond, [double]$secs = 15) {
    $end = (Get-Date).AddSeconds($secs)
    do {
        try { if (& $cond) { return $true } } catch { }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $end)
    return $false
}
function Start-Logged([string]$exe, [string[]]$arguments, [string]$name, [switch]$Visible) {
    $log = Join-Path $out "$name.log"
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle $(if ($Visible) { 'Normal' } else { 'Hidden' }) `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle
    return $p
}
function Stop-Quiet($p) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue; $null = $p.WaitForExit(5000) } }
$checks = @()
function Add-Check([string]$group, [string]$what, [bool]$ok, [string]$detail) {
    $script:checks += [pscustomobject]@{ group = $group; what = $what; result = $(if ($ok) { 'PASS' } else { 'FAIL' }); detail = $detail }
    Write-Host ("{0}  [{1}] {2}: {3}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $group, $what, $detail) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}
function Add-Info([string]$group, [string]$what, [string]$detail) {
    $script:checks += [pscustomobject]@{ group = $group; what = $what; result = 'INFO'; detail = $detail }
    Write-Host "INFO  [$group] ${what}: $detail" -ForegroundColor Yellow
}
# A message box for the person at the PC. Returns the process: it ends when they press OK (or after $seconds).
function Popup([string]$text, [int]$seconds = 900) {
    # line breaks become [char]10 inside the command (a raw newline would split the argument)
    $t = ($text -replace "'", "''") -replace "`r?`n", "' + [char]10 + '"
    $p = Start-Process powershell -ArgumentList '-NoProfile', '-Command',
        "(New-Object -ComObject WScript.Shell).Popup(('$t'), $seconds, 'Desktop Capture P5', 64) | Out-Null" -PassThru -WindowStyle Hidden
    $null = $p.Handle
    return $p
}
function Probe([string]$file) {
    if (-not $file -or -not (Test-Path $file)) { return 'missing' }
    $o = & $capd probe $file 2>&1 | ForEach-Object { "$_" }
    $o | Add-Content -Encoding utf8 (Join-Path $out 'probe.txt')
    $v = $o | Where-Object { $_ -match 'video' } | Select-Object -First 1
    if ($v) { return "plays: $v" }
    return "does not play: $(($o | Select-Object -First 2) -join ' / ')"
}

$listing = & $mixd --list-devices
$cableIn = ''
$l = $listing | Where-Object { $_ -like 'out *' -and $_ -like '* ok *' -and $_ -like '*CABLE Input*' } | Select-Object -First 1
if ($l -and $l -match '\[(.+)\]$') { $cableIn = $Matches[1] }

$procs = @{}
function Start-Engine([string]$tag) {
    $script:procs = @{}
    $procs.mixd = Start-Logged $mixd @('--port', $port, '--bind', '127.0.0.1', '--no-discovery', '--no-profiles', '--no-hotkeys',
                                       '--test-tone', '440', '--lan-store', "`"$(Join-Path $out "mixd-lan-$tag.txt")`"") "mixd-$tag"
    if (-not (Wait-Until { Get-Param '/system/version' } 15)) { throw 'mixd did not start' }
    if ($cableIn) { Set-Param '/master/out/1/target' "vmic/$cableIn" }
    Set-Param '/capture/audio/1/source' 'master'
    $procs.capd = Start-Logged $capd @('--port', $port) "capd-$tag"
    if (-not (Wait-Until { Get-Param '/video/devices/display/1/id' } 15)) { throw 'capd did not report the monitors' }
    $mix2 = Call '/video/mix/add' @('name=s:vcam')
    $scene2 = Call '/video/scene/add' @('name=s:vcam')
    $txt = Call '/video/source/add' @('kind=text')
    Set-Param "/video/source/$txt/text/content" "Desktop Capture P5`n$tag"
    Set-Param "/video/source/$txt/text/size" 120
    Set-Param "/video/source/$txt/text/background" 4278190335
    Call "/video/scene/$scene2/layer/add" @("source=$txt") | Out-Null
    Set-Param "/video/mix/$mix2/program_scene" $scene2
    Set-Param '/video/vcam/mix' $mix2
    Set-Param '/video/preview/mix' 1
    $procs.ui = Start-Process -FilePath $ui -ArgumentList '--connect', "127.0.0.1:$port", '--page', 'video', '--demo', 'preview' -PassThru
    Start-Sleep -Seconds 3
}
function Stop-Engine {
    try { if ((Get-Param '/capture/state') -eq 'recording') { Call '/capture/record/stop' | Out-Null; $null = Wait-Until { (Get-Param '/capture/state') -ne 'recording' } 20 } } catch { }
    foreach ($k in @('ui', 'capd', 'mixd')) { Stop-Quiet $procs[$k] }
}
function Alive { return (-not $procs.mixd.HasExited) -and (-not $procs.capd.HasExited) -and (-not $procs.ui.HasExited) }

try {
    # ------------------------------------------------------------------------------------------------ usb
    if ($Cases -contains 'usb') {
        Write-Host "`n=== usb: the drive is pulled out during a recording ===" -ForegroundColor Cyan
        function Find-Usb {
            if ($UsbDrive) { return (Get-Volume -DriveLetter $UsbDrive.TrimEnd(':') -ErrorAction SilentlyContinue) }
            return Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -and $_.DriveLetter } | Select-Object -First 1
        }
        $vol = Find-Usb
        if (-not $vol) {
            $pp = Popup 'USB メモリを PC に挿してください（10 分待ちます）。' 600
            $null = Wait-Until { Find-Usb } 600
            Stop-Quiet $pp
            $vol = Find-Usb
        }
        if (-not $vol) { Add-Check 'usb' 'USB drive found' $false 'no removable drive' }
        else {
            $letter = $vol.DriveLetter
            $usbDir = "${letter}:\dc-p5-usb"
            New-Item -ItemType Directory -Force $usbDir | Out-Null
            Add-Info 'usb' 'drive' ("{0}: {1} {2:0.0} GB free ({3})" -f $letter, $vol.FileSystemLabel, ($vol.SizeRemaining / 1GB), $vol.FileSystem)
            Start-Engine 'usb'
            Set-Param '/capture/directory' $usbDir
            Call '/capture/record/start' | Out-Null
            $started = Wait-Until { (Get-Param '/capture/state') -eq 'recording' } 20
            $file = Get-Param '/capture/file'
            Start-Sleep -Seconds 20
            $x0 = [int64](Get-Param '/system/xruns')
            $pp = Popup "録画中です。USB メモリ（${letter}:）をそのまま引き抜いてください（取り外し操作はしないでください）。10 分待ちます。" 600
            $pulled = Wait-Until { -not (Test-Path "${letter}:\") } 600
            $pulledAt = Get-Date
            Stop-Quiet $pp
            $stopped = Wait-Until { (Get-Param '/capture/state') -ne 'recording' } 60
            $state = Get-Param '/capture/state'; $err = Get-Param '/capture/error'
            Start-Sleep -Seconds 5
            Add-Check 'usb' 'pulled out -> the recording stops safely' ($started -and $pulled -and $stopped -and (Alive)) ("pulled {0}, state {1} after {2:0} s, error '{3}', mixd/capd/ui alive {4}" -f $pulled, $state, ((Get-Date) - $pulledAt).TotalSeconds, $err, (Alive))
            Add-Check 'usb' 'the rest goes on (engine, vcam, preview)' ((Alive) -and [int64](Get-Param '/system/xruns') -eq $x0 -and (Get-Param '/video/vcam/state') -in 'waiting', 'active') ("xruns +{0}, vcam {1}, monitor source {2}" -f ([int64](Get-Param '/system/xruns') - $x0), (Get-Param '/video/vcam/state'), (Get-Param '/video/source/1/state'))
            # a new recording on C: right away
            Set-Param '/capture/directory' (Join-Path $out 'rec')
            New-Item -ItemType Directory -Force (Join-Path $out 'rec') | Out-Null
            Call '/capture/record/start' | Out-Null
            $again = Wait-Until { (Get-Param '/capture/state') -eq 'recording' } 20
            Start-Sleep -Seconds 10
            Call '/capture/record/stop' | Out-Null
            $null = Wait-Until { (Get-Param '/capture/state') -eq 'idle' } 20
            $file2 = Get-Param '/capture/file'
            Add-Check 'usb' 'a new recording on C: works' ($again -and (Probe $file2) -like 'plays*') "$(Split-Path -Leaf $file2): $(Probe $file2)"
            $pp = Popup "USB メモリ（${letter}:）を挿し直してください。録画が途中まで再生できるか確かめます（10 分待ちます）。" 600
            $back = Wait-Until { Test-Path $file } 600
            Stop-Quiet $pp
            Start-Sleep -Seconds 3
            $pr = Probe $file
            Add-Check 'usb' 'the file on the USB drive plays up to the pull' ($back -and $pr -like 'plays*') "$file : $pr"
            Stop-Engine
        }
    }

    # ------------------------------------------------------------------------------------------------ vcam
    if ($Cases -contains 'vcam') {
        Write-Host "`n=== vcam: Discord ===" -ForegroundColor Cyan
        Start-Engine 'vcam'
        $pp = Popup ("仮想カメラに青地の文字「Desktop Capture P5」を出しています。`n`nDiscord：ユーザー設定 → 音声・ビデオ → カメラで「Desktop Capture Camera」を選び、テストビデオに文字が映るか見てください。`n" +
                     "`n終わったら OK を押してください（20 分待ちます）。結果はスレッドで教えてください。") 1200
        $log = @(); $last = ''
        $start = Get-Date
        while (-not $pp.HasExited -and ((Get-Date) - $start).TotalMinutes -lt 20) {
            $s = "$(Get-Param '/video/vcam/state') $(Get-Param '/video/vcam/detail')"
            if ($s -ne $last) { $log += ('{0:HH:mm:ss} {1}' -f (Get-Date), $s); Write-Host $log[-1]; $last = $s }
            Start-Sleep -Milliseconds 500
        }
        $active = @($log | Where-Object { $_ -match ' active' }).Count
        Add-Check 'vcam' 'an app opened the virtual camera (state active)' ($active -ge 1) "$active time(s) active: $($log -join ' | ')"
        Add-Info 'vcam' 'picture in Discord' 'ask the person (the script cannot see inside the apps)'
        Stop-Engine
    }

    # ------------------------------------------------------------------------------------------------ tdr
    if ($Cases -contains 'tdr') {
        Write-Host "`n=== tdr: one real GPU reset ===" -ForegroundColor Cyan
        $dxcap = (Get-Command dxcap.exe -ErrorAction SilentlyContinue).Source
        if (-not $dxcap) {
            $pp = Popup "GPU リセットに使う Windows の「グラフィックス ツール」を入れます。次に出る管理者の確認（UAC）で「はい」を押してください。" 60
            $inst = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList '-NoProfile', '-Command',
                'Add-WindowsCapability -Online -Name Tools.Graphics.DirectX~~~~0.0.1.0 | Out-File -Encoding utf8 $env:TEMP\dc-graphics-tools.txt'
            Stop-Quiet $pp
            $dxcap = Join-Path $env:WINDIR 'System32\dxcap.exe'
        }
        if (-not (Test-Path $dxcap)) { Add-Check 'tdr' 'dxcap available' $false 'Graphics Tools could not be installed' }
        else {
            Start-Engine 'tdr'
            New-Item -ItemType Directory -Force (Join-Path $out 'rec') | Out-Null
            Set-Param '/capture/directory' (Join-Path $out 'rec')
            $watchFile = Join-Path $out 'watch-tdr.txt'
            $procs.watch = Start-Process -FilePath $cli -ArgumentList '--port', $port, 'watch' -PassThru -WindowStyle Hidden -RedirectStandardOutput $watchFile
            Call '/capture/record/start' | Out-Null
            $null = Wait-Until { (Get-Param '/capture/state') -eq 'recording' } 20
            $file0 = Get-Param '/capture/file'
            Start-Sleep -Seconds 10
            $x0 = [int64](Get-Param '/system/xruns')
            $pp = Popup "GPU リセットを 1 回起こします。管理者の確認（UAC）で「はい」を押してください。画面が数秒暗くなりますが、そのまま待ってください。" 120
            $t0 = Get-Date
            $tdr = Start-Process $dxcap -ArgumentList '-forcetdr' -Verb RunAs -Wait -PassThru
            Stop-Quiet $pp
            $seg = Wait-Until { Select-String -Path $watchFile -SimpleMatch 'capture.segment' -Quiet } 60
            Start-Sleep -Seconds 5
            $file1 = Get-Param '/capture/file'
            $vcamBack = Wait-Until { (Get-Param '/video/vcam/state') -in 'waiting', 'active' } 30
            $srcBack = Wait-Until { (Get-Param '/video/source/1/state') -eq 'ok' } 30
            Start-Sleep -Seconds 10
            $lost = @(Select-String -Path (Join-Path $out 'capd-tdr.log') -SimpleMatch 'GPU lost' -ErrorAction SilentlyContinue).Count
            Add-Info 'tdr' 'dxcap -forcetdr' ("exit {0}, {1:0.0} s" -f $tdr.ExitCode, ((Get-Date) - $t0).TotalSeconds)
            Add-Check 'tdr' 'GPU reset -> recording goes on in a new file' ($seg -and $file1 -ne $file0 -and (Get-Param '/capture/state') -eq 'recording') "capd 'GPU lost' lines $lost, segment event $seg, $(Split-Path -Leaf $file0) -> $(Split-Path -Leaf $file1), state $(Get-Param '/capture/state') $(Get-Param '/capture/error')"
            Add-Check 'tdr' 'outputs come back (vcam, monitor source)' ($vcamBack -and $srcBack) "vcam $(Get-Param '/video/vcam/state'), monitor source $(Get-Param '/video/source/1/state')"
            Add-Check 'tdr' 'mixd, capd, dc-ui survive' (Alive) ("alive {0}, xruns +{1}" -f (Alive), ([int64](Get-Param '/system/xruns') - $x0))
            Stop-Engine
            Stop-Quiet $procs.watch
            Add-Check 'tdr' 'both files play' ((Probe $file0) -like 'plays*' -and (Probe $file1) -like 'plays*') "$(Split-Path -Leaf $file0): $(Probe $file0); $(Split-Path -Leaf $file1): $(Probe $file1)"
        }
    }
} catch {
    Add-Check 'run' 'script' $false $_.Exception.Message
} finally {
    Stop-Engine
    if ($procs.watch) { Stop-Quiet $procs.watch }
}

$md = @("# P5 manual checks ($stamp)", '', "Cases: $($Cases -join ', ')", '', '| group | check | result | detail |', '|---|---|---|---|')
foreach ($c in $checks) { $md += "| $($c.group) | $($c.what) | $($c.result) | $($c.detail -replace '\|', '/') |" }
$md += ''; $md += "Logs: $out"
$report = Join-Path $out "p5m_$stamp.md"
$md | Set-Content -Encoding utf8 $report
Write-Host "`n$($md -join "`n")"
if (($checks | Where-Object { $_.result -eq 'FAIL' }).Count -eq 0) { Write-Host 'P5 MANUAL CHECKS PASSED' -ForegroundColor Green; exit 0 }
Write-Host 'P5 MANUAL CHECKS FAILED' -ForegroundColor Red
exit 1
