<#
.SYNOPSIS
  Gate G4 (remote controller) on the engine PC: the real mixd and capd, the PC's own dc-ui, and several
  remotes (dc-ui --remote) at once, each with the preview and the monitor audio.

.DESCRIPTION
  Builds Release (mixd, capd, dc-ui, dc-cli) unless -NoBuild, then:

    mixd       on 0.0.0.0:47800 with --allow-remote (the remotes need no pairing in this test; pairing itself
               was tested in P3) and a 440 Hz test tone on the master. The master also goes to LAN send stream 1
               (Opus) = the remotes' monitor audio. Master output 1 goes to VB-CABLE when it is installed, so the
               PC's speakers stay quiet.
    capd       the preview of mix 1 (the primary monitor) to whoever watches.
    dc-ui      the PC's own UI (role local), on the mixer page.
    remotes    -Remotes copies of dc-ui --remote --connect <this PC's LAN address>, each one watching the preview
               and playing the monitor audio for -Seconds (dc-ui --measure). With -Here the first one runs the
               whole G4 check instead (dc-ui --g4: mixer, scene switch, recording settings and a recording from
               the remote, seen by a second connection, and the recording going on while that remote reconnects).

  With -HoldMinutes the engine stays up that long after the PC's remotes finished, for the Surface:
      dc-ui.exe --remote --connect <address>:47800 --g4 60 --monitor 1
  (the Surface makes the 4th remote when -Remotes is 3).

  Writes build\g4\g4_<time>.md and prints it.

.EXAMPLE
  .\scripts\g4-remote.ps1 -Here                       # everything on this PC: 3 remotes, the first runs --g4
  .\scripts\g4-remote.ps1 -NoBuild -HoldMinutes 20    # 3 PC remotes, then wait for the Surface's --g4
#>
param(
    [int]$Seconds = 60,
    [int]$Remotes = 3,
    [switch]$Here,
    [double]$HoldMinutes = 0,
    [string]$LanStore = '',
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $NoBuild) {
    cmake --preset windows | Out-Null
    cmake --build --preset windows-release --target mixd capd dc-ui dc-cli
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$capd = Join-Path $bin 'apps/capd/Release/capd.exe'
$ui = Join-Path $bin 'apps/ui/Release/dc-ui.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
# dc-ui needs the Qt DLLs: from DC_QT_DIR unless they were deployed next to it (as in scripts\test.ps1).
if ($env:DC_QT_DIR -and -not (Test-Path (Join-Path (Split-Path $ui) 'Qt6Core.dll'))) {
    $env:PATH = (Join-Path $env:DC_QT_DIR 'bin') + ';' + $env:PATH
}
$port = 47800
$out = Join-Path $root "build/g4/$((Get-Date).ToString('yyyyMMdd_HHmmss'))"
New-Item -ItemType Directory -Force -Path $out | Out-Null
if (-not $LanStore) { $LanStore = Join-Path $out 'mixd-lan.txt' }

# This PC's LAN address: the IPv4 address of the interface with the default route.
$ip = (Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } |
       Select-Object -First 1).IPv4Address.IPAddress
if (-not $ip) { throw 'no LAN address' }

function Get-Param([string]$path) {
    $line = & $cli --port $port snapshot $path | Where-Object { $_ -like "$path = *" } | Select-Object -First 1
    if (-not $line) { return $null }
    return ($line -replace '^[^=]+= ', '').Trim('"')
}
function Set-Param([string]$path, $value) {
    $r = & $cli --port $port set $path $value
    if ($LASTEXITCODE -ne 0) { Write-Warning "set $path $value : $r" }
}
function Wait-Until([scriptblock]$cond, [double]$seconds = 15) {
    $end = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $end) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 250 }
    return [bool](& $cond)
}
function Start-Logged([string]$exe, [string[]]$arguments, [string]$name, [switch]$Visible) {
    $style = if ($Visible) { 'Normal' } else { 'Hidden' }
    return Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle $style `
        -RedirectStandardOutput (Join-Path $out "$name.log") -RedirectStandardError (Join-Path $out "$name.err")
}
function Stop-Quiet($p) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue; $null = $p.WaitForExit(5000) } }

$procs = @{}
$report = New-Object System.Collections.Generic.List[string]
$report.Add("# G4 remote test $((Get-Date).ToString('yyyy-MM-dd HH:mm'))")
$report.Add('')
$report.Add("engine $ip`:$port, $Remotes PC remote(s) for $Seconds s$(if ($Here) { ', the first runs --g4' })")
try {
    Write-Host "`n=== setup ===" -ForegroundColor Cyan
    $procs.mixd = Start-Logged $mixd @('--port', $port, '--allow-remote', '--test-tone', '440', '--lan-store', "`"$LanStore`"") 'mixd'
    if (-not (Wait-Until { Get-Param '/system/version' } 15)) { throw 'mixd did not start' }
    # as scripts\g3-integration.ps1 picks it: "out  ok  ... CABLE Input ... [<id>]"
    $cable = & $mixd --list-devices | Where-Object { $_ -like 'out *' -and $_ -like '* ok *' -and $_ -like '*CABLE Input*' } |
             Select-Object -First 1
    if ($cable -and $cable -match '\[(.+)\]$') { Set-Param '/master/out/1/target' "vmic/$($Matches[1])" }
    else { Write-Warning 'VB-CABLE (CABLE Input) not found: the test tone plays on the default output' }
    Set-Param '/lan/stream/1/name' 'Monitor'
    Set-Param '/lan/stream/1/codec' 'opus'
    Set-Param '/master/out/2/target' 'lan/1'
    $procs.capd = Start-Logged $capd @('--port', $port) 'capd'
    if (-not (Wait-Until { Get-Param '/video/devices/display/1/id' } 15)) { throw 'capd did not report the monitors' }
    Set-Param '/video/preview/mix' 1
    $procs.local = Start-Logged $ui @('--connect', "127.0.0.1:$port") 'dc-ui-local' -Visible
    Start-Sleep -Seconds 3

    Write-Host "`n=== $Remotes remote(s) ===" -ForegroundColor Cyan
    $remoteProcs = @()
    for ($i = 1; $i -le $Remotes; $i++) {
        $mode = if ($Here -and $i -eq 1) { '--g4' } else { '--measure' }
        $remoteProcs += Start-Logged $ui @('--remote', '--connect', "$ip`:$port", $mode, $Seconds, '--monitor', '1') "remote$i" -Visible
        Start-Sleep -Seconds 2
    }
    $clients = Wait-Until { [int](Get-Param '/remote/clients') -ge $Remotes } 30
    $report.Add("remote clients connected at once: $(Get-Param '/remote/clients') (want $Remotes)")
    $report.Add("monitor stream subscribers: $(Get-Param '/lan/stream/1/subscribers'), preview subscriptions: " +
                ((& $cli --port $port snapshot /lan/sub | Where-Object { $_ -match 'video/preview' }).Count))
    foreach ($p in $remoteProcs) { $null = $p.WaitForExit(($Seconds + 180) * 1000) }

    $report.Add('')
    $failed = -not $clients
    for ($i = 1; $i -le $Remotes; $i++) {
        $log = Get-Content (Join-Path $out "remote$i.log") -Encoding UTF8 -ErrorAction SilentlyContinue
        $report.Add("## remote $i")
        $report.Add('```')
        $log | Where-Object { $_ -notmatch '^\s+\d+ s:' -and $_ -notmatch '^\s+monitor audio:' } | ForEach-Object { $report.Add($_) }
        $report.Add('```')
        if (-not ($log -match '(MEASURE|G4) PASSED')) { $failed = $true }
    }
    if ($HoldMinutes -gt 0) {
        Write-Host "`n=== engine stays up $HoldMinutes min: run the Surface now ===" -ForegroundColor Yellow
        Write-Host "  dc-ui.exe --remote --connect $ip`:$port --g4 60 --monitor 1"
        Start-Sleep -Seconds ([int]($HoldMinutes * 60))
    }
    $report.Add('')
    $report.Add($(if ($failed) { '**G4 REMOTE TEST FAILED**' } else { '**G4 REMOTE TEST PASSED**' }))
} finally {
    foreach ($k in @('local', 'capd', 'mixd')) { Stop-Quiet $procs[$k] }
    $file = Join-Path $out 'g4.md'
    $report | Set-Content -Path $file -Encoding UTF8
    Write-Host ''
    $report | ForEach-Object { Write-Host $_ }
    Write-Host "`nlogs: $out"
}
