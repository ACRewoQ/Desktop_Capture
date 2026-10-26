<#
.SYNOPSIS
  P5-B on the real PC: dc-tray from the package folder starts, watches, restarts and stops mixd and capd; the mixer
  window opens; starting at logon turns on and off.

.DESCRIPTION
  Builds the package with scripts\package.ps1 unless -NoPackage, then, with the real engine on the default port:
    start      dc-tray --background: mixd and capd run (tray-status.txt), mixd answers on 47800, capd connects
    single     starting dc-tray again does not make a second tray
    ui         dc-tray --open opens dc-ui with a window; --open again does not start a second one
    crash      mixd killed -> notification, restarted with a new pid, capd connects again; capd killed -> restarted
    record     a recording is running when dc-tray --quit stops everything: the file is complete (capd probe)
    stop       dc-tray --quit: capd and mixd end cleanly (exit 0, "stop requested" in their logs), dc-ui closes
    giveup     mixd that cannot start (a bad argument) is restarted 3 times, then left stopped (state failed)
    autostart  --autostart on / off / status, Windows' "turned off" mark; the earlier state is put back
  Writes build\p5b\<time>\p5b.md and prints it. No mixd, capd or dc-tray may run before it starts.

.EXAMPLE
  .\scripts\p5b-tray.ps1
  .\scripts\p5b-tray.ps1 -NoPackage -Cases start,crash,stop
#>
param(
    [switch]$NoPackage,
    [string]$Package = '',
    [string[]]$Cases = @('start', 'single', 'ui', 'crash', 'record', 'stop', 'giveup', 'autostart')
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $Package) { $Package = Join-Path $root 'build\package\DesktopCapture' }
if (-not $NoPackage) {
    & (Join-Path $PSScriptRoot 'package.ps1') -Out $Package
}
$tray = Join-Path $Package 'dc-tray.exe'
$cli = Join-Path $Package 'dc-cli.exe'
$capd = Join-Path $Package 'capd.exe'
foreach ($exe in @($tray, $cli, $capd)) { if (-not (Test-Path $exe)) { throw "missing $exe" } }

$running = Get-Process mixd, capd, dc-tray -ErrorAction SilentlyContinue
if ($running) { throw "stop these first: $(($running | ForEach-Object { "$($_.Name) $($_.Id)" }) -join ', ')" }

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path $root "build/p5b/$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$logs = Join-Path $env:LOCALAPPDATA 'DesktopCapture\logs'
$statusFile = Join-Path $env:LOCALAPPDATA 'DesktopCapture\tray-status.txt'

$results = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$case, [string]$what, [bool]$ok, [string]$detail = '') {
    $results.Add([pscustomobject]@{ Case = $case; What = $what; Ok = $ok; Detail = $detail })
    $mark = if ($ok) { 'OK  ' } else { 'FAIL' }
    Write-Host ("  {0} [{1}] {2}  {3}" -f $mark, $case, $what, $detail) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}
function Has-Case([string]$c) { return $Cases -contains $c }
function Wait-Until([scriptblock]$cond, [double]$seconds = 15) {
    $end = (Get-Date).AddSeconds($seconds)
    do {
        try { if (& $cond) { return $true } } catch { }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $end)
    return $false
}
function Status {
    $s = @{}
    if (Test-Path $statusFile) {
        foreach ($line in Get-Content $statusFile) { $k, $v = $line -split '=', 2; if ($k) { $s[$k] = $v } }
    }
    return $s
}
function Read-Log([string]$name) {
    $f = Join-Path $logs "$name.log"
    if (-not (Test-Path $f)) { return '' }
    # mixd and capd still write to it: open it shared
    $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite, Delete')
    try { return (New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $fs.Dispose() }
}
function Count([string]$text, [string]$pattern) { return ([regex]::Matches($text, [regex]::Escape($pattern))).Count }
function Tray([string[]]$arguments, [int]$timeoutSec = 60) {
    $p = Start-Process -FilePath $tray -ArgumentList $arguments -PassThru -RedirectStandardOutput "$out\tray-out.txt"
    $null = $p.Handle
    if (-not $p.WaitForExit($timeoutSec * 1000)) { return @{ Code = -1; Output = '' } }
    return @{ Code = $p.ExitCode; Output = (Get-Content "$out\tray-out.txt" -Raw) }
}
function Start-Tray([string[]]$arguments) {
    Remove-Item $statusFile -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $tray -ArgumentList $arguments -PassThru
    $null = $p.Handle
    $ok = Wait-Until { (Status)['tray_pid'] -eq "$($p.Id)" } 10
    return @{ Process = $p; Ok = $ok }
}
function Running([string]$name) { return (Status)["$name.state"] -eq 'running' }
function Port-Open([int]$port) {
    $c = New-Object Net.Sockets.TcpClient
    try { $c.Connect('127.0.0.1', $port); return $true } catch { return $false } finally { $c.Dispose() }
}
function Get-Param([string]$path) {
    $line = & $cli snapshot $path | Where-Object { $_ -like "$path = *" } | Select-Object -First 1
    if (-not $line) { return $null }
    $v = $line.Substring($path.Length + 3)
    if ($v.StartsWith('"')) { return $v.Trim('"') -replace '\\\\', '\' }
    return $v
}
function Stop-Tray {
    $r = Tray @('--quit') 90
    return $r.Code -eq 0
}

# Starting at logon: remember what was there, put it back at the end.
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$approvedKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run'
$runBefore = (Get-ItemProperty $runKey -Name DesktopCapture -ErrorAction SilentlyContinue).DesktopCapture
$approvedBefore = (Get-ItemProperty $approvedKey -Name DesktopCapture -ErrorAction SilentlyContinue).DesktopCapture

$trayRun = $null
try {
    # ------------------------------------------------------------------------------------------------ start
    Write-Host "`n=== start ===" -ForegroundColor Cyan
    $t = Start-Tray @('--background')
    $trayRun = $t.Process
    Add-Check 'start' 'dc-tray runs and writes its state' $t.Ok "pid $($trayRun.Id)"
    $up = Wait-Until { (Running 'mixd') -and (Running 'capd') } 30
    $s = Status
    Add-Check 'start' 'mixd and capd running' $up "mixd $($s['mixd.state']) pid $($s['mixd.pid']), capd $($s['capd.state']) pid $($s['capd.pid'])"
    Add-Check 'start' 'mixd answers on 47800' (Wait-Until { Port-Open 47800 } 30)
    Add-Check 'start' 'capd connected to the engine' (Wait-Until { (Count (Read-Log 'capd') 'connected to the engine') -ge 1 } 30)
    $mixdLog = Read-Log 'mixd'
    Add-Check 'start' 'mixd log has the --stop-event line' ($mixdLog -match 'started by dc-tray: .*--stop-event Local\\DesktopCapture\.stop\.mixd') ''
    $children = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($trayRun.Id)" | Select-Object -ExpandProperty Name
    Add-Check 'start' 'mixd and capd are children of dc-tray' (($children -contains 'mixd.exe') -and ($children -contains 'capd.exe')) ($children -join ', ')
    $ws = [math]::Round((Get-Process -Id $trayRun.Id).WorkingSet64 / 1MB, 1)
    Add-Check 'start' 'dc-tray is small (working set < 30 MB)' ($ws -lt 30) "$ws MB"

    # ------------------------------------------------------------------------------------------------ single
    if (Has-Case 'single') {
        Write-Host "`n=== single ===" -ForegroundColor Cyan
        $r = Tray @('--background') 15
        Start-Sleep -Seconds 1
        $trays = @(Get-Process dc-tray -ErrorAction SilentlyContinue)
        Add-Check 'single' 'a second start hands over and exits' ($r.Code -eq 0 -and $trays.Count -eq 1 -and (Status)['tray_pid'] -eq "$($trayRun.Id)") "exit $($r.Code), dc-tray processes $($trays.Count)"
    }

    # ------------------------------------------------------------------------------------------------ ui
    if (Has-Case 'ui') {
        Write-Host "`n=== ui ===" -ForegroundColor Cyan
        $r = Tray @('--open') 15
        $opened = Wait-Until { @(Get-Process dc-ui -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }).Count -ge 1 } 20
        Add-Check 'ui' 'dc-tray --open opens the mixer window' ($r.Code -eq 0 -and $opened) "exit $($r.Code)"
        $r = Tray @('--open') 15
        Start-Sleep -Seconds 3
        $uis = @(Get-Process dc-ui -ErrorAction SilentlyContinue)
        Add-Check 'ui' 'opening again raises it (no second dc-ui)' ($uis.Count -eq 1) "dc-ui processes $($uis.Count)"
    }

    # ------------------------------------------------------------------------------------------------ crash
    if (Has-Case 'crash') {
        Write-Host "`n=== crash ===" -ForegroundColor Cyan
        $old = [int](Status)['mixd.pid']
        $capdConnects = Count (Read-Log 'capd') 'connected to the engine'
        Stop-Process -Id $old -Force
        $back = Wait-Until { (Running 'mixd') -and [int](Status)['mixd.pid'] -ne $old } 20
        $s = Status
        Add-Check 'crash' 'mixd killed -> restarted' ($back -and $s['mixd.restarts'] -eq '1') "pid $old -> $($s['mixd.pid']), restarts $($s['mixd.restarts']), last exit $($s['mixd.last_exit'])"
        Add-Check 'crash' 'the killed run''s log is kept (mixd.1.log)' (Test-Path (Join-Path $logs 'mixd.1.log'))
        Add-Check 'crash' 'mixd answers again' (Wait-Until { Port-Open 47800 } 30)
        Add-Check 'crash' 'capd connects to the new engine' (Wait-Until { (Count (Read-Log 'capd') 'connected to the engine') -gt $capdConnects } 30) "capd pid $((Status)['capd.pid']) (not restarted)"

        $old = [int](Status)['capd.pid']
        Stop-Process -Id $old -Force
        $back = Wait-Until { (Running 'capd') -and [int](Status)['capd.pid'] -ne $old } 20
        $s = Status
        Add-Check 'crash' 'capd killed -> restarted' ($back -and $s['capd.restarts'] -eq '1') "pid $old -> $($s['capd.pid'])"
        Add-Check 'crash' 'the new capd connects' (Wait-Until { (Count (Read-Log 'capd') 'connected to the engine') -ge 1 } 30)
        $trayLog = Get-Content (Join-Path $logs 'tray.log') -Encoding UTF8 -Tail 20
        Add-Check 'crash' 'tray.log has the restarts' ([bool]($trayLog | Where-Object { $_ -match 'mixd: restarting' }) -and [bool]($trayLog | Where-Object { $_ -match 'capd: restarting' }))
    }

    # ------------------------------------------------------------------------------------------------ record + stop
    $recFile = $null
    if (Has-Case 'record') {
        Write-Host "`n=== record ===" -ForegroundColor Cyan
        $recDir = Join-Path $out 'rec'
        New-Item -ItemType Directory -Force $recDir | Out-Null
        & $cli set /capture/directory $recDir | Out-Null
        $r = & $cli call /capture/record/start
        $rec = Wait-Until { (Get-Param '/capture/state') -eq 'recording' } 20
        Add-Check 'record' 'recording started' $rec "$r"
        Start-Sleep -Seconds 6
        $recFile = Get-Param '/capture/file'
    }
    if (Has-Case 'stop') {
        Write-Host "`n=== stop ===" -ForegroundColor Cyan
        $mixdPid = [int](Status)['mixd.pid']
        $capdPid = [int](Status)['capd.pid']
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $ok = Stop-Tray
        $sw.Stop()
        $gone = Wait-Until { -not (Get-Process -Id $mixdPid, $capdPid -ErrorAction SilentlyContinue) } 5
        Add-Check 'stop' 'dc-tray --quit returns after everything stopped' ($ok -and $gone -and $trayRun.HasExited) ("{0:N1} s" -f $sw.Elapsed.TotalSeconds)
        $mixdLog = Read-Log 'mixd'
        $capdLog = Read-Log 'capd'
        Add-Check 'stop' 'mixd stopped cleanly' (($mixdLog -match 'stop requested') -and ($mixdLog -match 'mixd stopped')) ''
        Add-Check 'stop' 'capd stopped cleanly' ($capdLog -match 'stop requested') ''
        $trayLog = Get-Content (Join-Path $logs 'tray.log') -Encoding UTF8 -Tail 10
        $clean = $trayLog | Where-Object { $_ -match '(mixd|capd): stopped, .* 0$' }
        Add-Check 'stop' 'exit code 0 for both' (@($clean).Count -eq 2) (($trayLog | Where-Object { $_ -match ': stopped' }) -join ' | ')
        Add-Check 'stop' 'no termination needed' (-not ($trayLog | Where-Object { $_ -match 'terminating' }))
        Add-Check 'stop' 'state file says stopped' ((Status)['tray_pid'] -eq '0' -and (Status)['mixd.state'] -eq 'stopped')
        if (Has-Case 'ui') { Add-Check 'stop' 'dc-ui closed' (Wait-Until { -not (Get-Process dc-ui -ErrorAction SilentlyContinue) } 5) }
        if ($recFile) {
            $probe = & $capd probe $recFile 2>&1 | ForEach-Object { "$_" }
            $probe | Set-Content (Join-Path $out 'probe.txt')
            $video = $probe | Where-Object { $_ -match 'type=video' } | Select-Object -First 1
            $end = if ($video -match 'end=([\d.]+)') { [double]$Matches[1] } else { 0 }
            Add-Check 'record' 'the recording is complete after --quit' ($video -and $end -ge 5) "$(Split-Path -Leaf $recFile): video end $end s"
        }
    }
    $trayRun = $null

    # ------------------------------------------------------------------------------------------------ giveup
    if (Has-Case 'giveup') {
        Write-Host "`n=== giveup (about 20 s) ===" -ForegroundColor Cyan
        $t = Start-Tray @('--background', '--no-capd', '--mixd-args', '--no-such-option')
        $trayRun = $t.Process
        $failed = Wait-Until { (Status)['mixd.state'] -eq 'failed' } 40
        $s = Status
        Add-Check 'giveup' 'mixd that cannot start: 3 restarts, then failed' ($failed -and $s['mixd.restarts'] -eq '3' -and $s['mixd.last_exit'] -eq '0x00000002') "state $($s['mixd.state']), restarts $($s['mixd.restarts']), last exit $($s['mixd.last_exit'])"
        Add-Check 'giveup' 'audio only: no capd' (-not $s.ContainsKey('capd.state'))
        Add-Check 'giveup' '--quit with nothing running' (Stop-Tray)
        $trayRun = $null
    }

    # ------------------------------------------------------------------------------------------------ autostart
    if (Has-Case 'autostart') {
        Write-Host "`n=== autostart ===" -ForegroundColor Cyan
        $want = "`"$tray`" --background"
        $r = Tray @('--autostart', 'on')
        $v = (Get-ItemProperty $runKey -Name DesktopCapture -ErrorAction SilentlyContinue).DesktopCapture
        Add-Check 'autostart' 'on: Run entry for this folder' ($r.Code -eq 0 -and $r.Output -match 'autostart=on' -and $v -eq $want) "$v"
        Set-ItemProperty $approvedKey -Name DesktopCapture -Value ([byte[]](3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)) -Type Binary -ErrorAction SilentlyContinue
        if (-not (Test-Path $approvedKey)) { New-Item $approvedKey -Force | Out-Null; Set-ItemProperty $approvedKey -Name DesktopCapture -Value ([byte[]](3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)) -Type Binary }
        $r = Tray @('--autostart', 'status')
        Add-Check 'autostart' 'turned off in Windows settings -> disabled' ($r.Output -match 'autostart=disabled') "$($r.Output.Trim())"
        $r = Tray @('--autostart', 'on')
        $mark = (Get-ItemProperty $approvedKey -Name DesktopCapture -ErrorAction SilentlyContinue).DesktopCapture
        Add-Check 'autostart' 'on again clears the mark' ($r.Output -match 'autostart=on' -and -not $mark) "$($r.Output.Trim())"
        $r = Tray @('--autostart', 'off')
        $v = (Get-ItemProperty $runKey -Name DesktopCapture -ErrorAction SilentlyContinue).DesktopCapture
        Add-Check 'autostart' 'off: entry removed' ($r.Output -match 'autostart=off' -and -not $v) "$($r.Output.Trim())"
    }
} finally {
    if ($trayRun -and -not $trayRun.HasExited) { Stop-Tray | Out-Null }
    if ($null -ne $runBefore) { Set-ItemProperty $runKey -Name DesktopCapture -Value $runBefore }
    else { Remove-ItemProperty $runKey -Name DesktopCapture -ErrorAction SilentlyContinue }
    if ($null -ne $approvedBefore) { Set-ItemProperty $approvedKey -Name DesktopCapture -Value $approvedBefore -Type Binary }
    else { Remove-ItemProperty $approvedKey -Name DesktopCapture -ErrorAction SilentlyContinue }
    Copy-Item (Join-Path $logs '*.log') $out -ErrorAction SilentlyContinue
}

$failed = @($results | Where-Object { -not $_.Ok })
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# P5-B dc-tray ($stamp)")
$md.Add('')
$md.Add("Package: $Package")
$md.Add('')
$md.Add('| Case | Check | Result | Detail |')
$md.Add('|---|---|---|---|')
foreach ($r in $results) { $md.Add("| $($r.Case) | $($r.What) | $(if ($r.Ok) { 'OK' } else { '**FAIL**' }) | $($r.Detail -replace '\|', '/') |") }
$md.Add('')
$md.Add($(if ($failed.Count) { "**$($failed.Count) FAILED**" } else { 'ALL CHECKS PASSED' }))
$report = Join-Path $out 'p5b.md'
$md | Set-Content -Encoding UTF8 $report
Write-Host ''
$md | ForEach-Object { Write-Host $_ }
Write-Host "`nReport: $report"
if ($failed.Count) { exit 1 }
