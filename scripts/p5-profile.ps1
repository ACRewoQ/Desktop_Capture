<#
.SYNOPSIS
  P5-A on a real PC: profiles and mixer snapshots in mixd (protocol.md 5.10).

.DESCRIPTION
  1. Builds Release (mixd, capd, dc-cli, dc-ui, the DC Test Gain VST3) unless -NoBuild
  2. Starts mixd (real audio devices, VST3, a throw-away profile folder and bindings file) and capd, and
     sets up a profile: track settings, a fifth track, a built-in effect, the DC Test Gain VST3 on the
     master (its state holds the gain), a second scene on the program, capture and sampler settings
  3. Snapshot: take it, move the fader, recall it; a hotkey bound to "snapshot" recalls it too
  4. Autosave (/profile/modified goes back to false), then mixd is killed and started again: every
     value, the VST3 with its gain, the scene in capd and the snapshot come back
  5. A second profile from the fresh state, switching back and forth with capd connected, switching is
     refused while recording, rename and delete
  6. A broken profile file: the backup (.bak) is read
  7. dc-ui screenshot of the profile page
  Writes build\p5\<time>\p5.md and prints the table. Does not touch the real profiles.

.EXAMPLE
  .\scripts\p5-profile.ps1
#>
param(
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
[Console]::OutputEncoding = [Text.Encoding]::UTF8 # dc-cli prints UTF-8

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd capd dc-cli dc-ui dc_test_plugin_VST3
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$capd = Join-Path $bin 'apps/capd/Release/capd.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$ui = Join-Path $bin 'apps/ui/Release/dc-ui.exe'
$fixtureDir = Join-Path $bin 'tools/vst3_test_plugin/dc_test_plugin_artefacts/Release/VST3'
if ($env:DC_QT_DIR -and -not (Test-Path (Join-Path (Split-Path $ui) 'Qt6Core.dll'))) {
    $env:PATH = (Join-Path $env:DC_QT_DIR 'bin') + ';' + $env:PATH
}
foreach ($exe in @($mixd, $capd, $cli)) { if (-not (Test-Path $exe)) { throw "missing $exe (build first)" } }
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path $root "build/p5/$stamp"
$profiles = Join-Path $out 'profiles'
New-Item -ItemType Directory -Force $profiles | Out-Null
$port = 47893

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Keys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    static INPUT Key(ushort vk, bool up) { var i = new INPUT(); i.type = 1; i.ki.wVk = vk; i.ki.dwFlags = up ? 2u : 0u; return i; }
    public static void Press(ushort[] mods, ushort vk) {
        int size = Marshal.SizeOf(typeof(INPUT));
        foreach (var m in mods) SendInput(1, new[] { Key(m, false) }, size);
        SendInput(1, new[] { Key(vk, false) }, size);
        System.Threading.Thread.Sleep(60);
        SendInput(1, new[] { Key(vk, true) }, size);
        for (int k = mods.Length - 1; k >= 0; --k) SendInput(1, new[] { Key(mods[k], true) }, size);
    }
}
'@

function Get-Params([string]$prefix) {
    $map = [ordered]@{}
    foreach ($line in (& $cli --port $port snapshot $prefix)) {
        $i = $line.IndexOf(' = ')
        if ($i -lt 0) { continue }
        $v = $line.Substring($i + 3)
        if ($v.StartsWith('"')) { $v = $v.Substring(1, $v.Length - 2) }
        $map[$line.Substring(0, $i)] = $v
    }
    return $map
}
function Get-Param([string]$path) { return (Get-Params $path)[$path] }
function Set-Param([string]$path, $value) {
    $r = & $cli --port $port set $path $value
    if ($r -ne 'ok') { throw "set $path $value -> $r" }
}
function Invoke-Method([string]$method, [string[]]$kv = @()) { return (& $cli --port $port call $method @kv) -join ' ' }
function Wait-Until([scriptblock]$cond, [double]$seconds = 10) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 150
    }
    return $false
}
function Start-Hidden([string]$exe, [string[]]$arguments, [string]$name) {
    $log = Join-Path $out "$name.log"
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $null = $p.Handle
    return $p
}
function Stop-Quiet($p) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; $null = $p.WaitForExit(5000) } }
function Start-Mixd([string]$name, [string]$profile = '') {
    $a = @('--port', $port, '--local-only', '--profile-dir', "`"$profiles`"", '--plugin-cache', "`"$(Join-Path $out 'vst3-cache.json')`"",
           '--control-store', "`"$(Join-Path $out 'mixd-control.txt')`"")
    if ($profile) { $a += @('--profile', $profile) }
    $p = Start-Hidden $mixd $a $name
    if (-not (Wait-Until { (Get-Param '/system/clock_active') -like 'device/*' } 20)) { throw "mixd did not start (see $out\$name.log)" }
    return $p
}
function Start-Capd([string]$name) {
    $p = Start-Hidden $capd @('--port', $port) $name
    if (-not (Wait-Until { (Get-Param '/video/source/1/state') -eq 'ok' } 20)) { throw "capd did not open the monitor (see $out\$name.log)" }
    return $p
}
# the parameter key of a VST3 parameter by its name ("Gain" -> "p123")
function Get-PluginKey([string]$slot, [string]$name) {
    $r = & $cli --port $port call "$slot/params" 'count=64'
    $json = ($r -join '') -replace '^ok\s*', ''
    if ($json.StartsWith('"')) { $json = $json.Substring(1, $json.Length - 2) -replace '\\"', '"' }
    foreach ($p in ($json | ConvertFrom-Json)) { if ($p.name -eq $name) { return $p.key } }
    return $null
}

$rows = New-Object System.Collections.Generic.List[object]
function Add-Row([string]$check, [bool]$pass, [string]$detail) {
    $rows.Add([pscustomobject]@{ Check = $check; Result = $(if ($pass) { 'PASS' } else { 'FAIL' }); Detail = $detail })
    Write-Host ("{0,-4} {1} {2}" -f $(if ($pass) { 'PASS' } else { 'FAIL' }), $check, $detail) -ForegroundColor $(if ($pass) { 'Green' } else { 'Red' })
}

$procs = @{}
try {
    Write-Host "`n=== set up profile p5a ===" -ForegroundColor Cyan
    $procs.mixd = Start-Mixd 'mixd-1' 'p5a'
    $procs.capd = Start-Capd 'capd-1'
    Add-Row 'starts with --profile' ((Get-Param '/profile/current') -eq 'p5a') "current $(Get-Param '/profile/current'), folder $(Get-Param '/profile/directory')"

    Set-Param '/track/1/volume_db' -6.5
    Set-Param '/track/1/name' 's:P5 Mic'
    Set-Param '/track/2/pan' -0.4
    Set-Param '/capture/video/bitrate_kbps' 33000
    Set-Param '/sampler/bank/1/pad/1/name' 's:pon'
    $null = Invoke-Method '/mixer/track/add' @('name=s:Fifth')
    $null = Invoke-Method '/track/1/fx/add' @('type=eq4')
    Set-Param '/track/1/fx/1/param/output_db' -2.5
    $scene2 = (Invoke-Method '/video/scene/add' @('name=s:Two')) -replace '[^0-9]', ''
    $null = Invoke-Method "/video/scene/$scene2/layer/add" @('source=1')
    Set-Param "/video/scene/$scene2/layer/1/x" 0.25
    Set-Param '/video/mix/1/program_scene' $scene2

    Set-Param '/plugins/extra_dirs' $fixtureDir
    $null = Invoke-Method '/plugins/scan' @('rescan=true')
    Wait-Until { (Get-Param '/plugins/scan/state') -eq 'idle' } 120 | Out-Null
    $all = Get-Params '/plugins'
    $gainId = $null
    foreach ($k in $all.Keys) { if ($k -match '^/plugins/(\d+)/name$' -and $all[$k] -eq 'DC Test Gain') { $gainId = $all["/plugins/$($Matches[1])/id"] } }
    Add-Row 'test VST3 found' ([bool]$gainId) "id $gainId"
    $gainKey = $null
    if ($gainId) {
        $null = Invoke-Method '/master/fx/add' @("type=$gainId")
        Wait-Until { (Get-Param '/master/fx/1/status') -eq 'ok' } 20 | Out-Null
        $gainKey = Get-PluginKey '/master/fx/1' 'Gain'
        Set-Param "/master/fx/1/param/$gainKey" 0.3
    }
    Add-Row 'VST3 on the master' ((Get-Param '/master/fx/1/status') -eq 'ok' -and $gainKey) "status $(Get-Param '/master/fx/1/status'), gain key $gainKey"

    Write-Host "`n=== snapshot ===" -ForegroundColor Cyan
    $snap = (Invoke-Method '/snapshot/save' @('name=s:snapA')) -replace '[^0-9]', ''
    Set-Param '/track/1/volume_db' -20
    Set-Param '/track/1/fx/1/param/output_db' 0
    $r = Invoke-Method '/snapshot/recall' @("snapshot=$snap")
    Add-Row 'snapshot recall' ($r -like 'ok*' -and (Get-Param '/track/1/volume_db') -eq '-6.5' -and (Get-Param '/track/1/fx/1/param/output_db') -eq '-2.5') "$r, volume $(Get-Param '/track/1/volume_db'), eq output $(Get-Param '/track/1/fx/1/param/output_db')"
    Set-Param '/track/1/volume_db' -20
    $b = (Invoke-Method '/control/binding/add' @('trigger=key:ctrl+alt+shift+f8', 'action=snapshot', "target=s:$snap")) -replace '[^0-9]', ''
    Wait-Until { (Get-Param "/control/binding/$b/state") -eq 'ok' } | Out-Null
    [Keys]::Press([uint16[]]@(0x11, 0x12, 0x10), [uint16]0x77)
    $ok = Wait-Until { (Get-Param '/track/1/volume_db') -eq '-6.5' }
    Add-Row 'hotkey recalls the snapshot' $ok "binding $b $(Get-Param "/control/binding/$b/state"), volume $(Get-Param '/track/1/volume_db')"
    Set-Param '/track/1/volume_db' -7.5

    Write-Host "`n=== autosave and restart ===" -ForegroundColor Cyan
    $ok = Wait-Until { (Get-Param '/profile/modified') -eq 'false' } 15
    $file = Join-Path $profiles 'p5a.dcprofile'
    Add-Row 'autosave' ($ok -and (Test-Path $file)) "modified $(Get-Param '/profile/modified'), $file $((Get-Item $file -ErrorAction SilentlyContinue).Length) bytes"
    $text = Get-Content $file -Encoding UTF8
    Add-Row 'file has device names and the plugin state' ((@($text | Where-Object { $_ -like "dev`t*" }).Count -gt 0) -and (@($text | Where-Object { $_ -like "fx`t/master/fx/1`t*" }).Count -eq 1)) "$(@($text | Where-Object { $_ -like "dev`t*" }).Count) device lines"
    Stop-Quiet $procs.mixd
    Start-Sleep -Seconds 1
    $procs.mixd = Start-Mixd 'mixd-2'
    $checks = @{
        '/profile/current' = 'p5a'; '/track/1/volume_db' = '-7.5'; '/track/1/name' = 'P5 Mic'; '/track/2/pan' = '-0.4'
        '/mixer/track_count' = '5'; '/capture/video/bitrate_kbps' = '33000'; '/sampler/bank/1/pad/1/name' = 'pon'
        '/track/1/fx/1/type' = 'eq4'; '/track/1/fx/1/param/output_db' = '-2.5'; '/video/mix/1/program_scene' = $scene2
        "/video/scene/$scene2/layer/1/x" = '0.25'; "/snapshot/$snap/name" = 'snapA'
    }
    $bad = @()
    foreach ($k in $checks.Keys) { if ((Get-Param $k) -ne $checks[$k]) { $bad += "$k=$(Get-Param $k) (want $($checks[$k]))" } }
    Add-Row 'restored after restart' ($bad.Count -eq 0) $(if ($bad) { $bad -join ', ' } else { "$($checks.Count) values" })
    if ($gainKey) {
        $ok = Wait-Until { (Get-Param '/master/fx/1/status') -eq 'ok' } 20
        $g = [double](Get-Param "/master/fx/1/param/$gainKey")
        Add-Row 'VST3 back with its state' ($ok -and [math]::Abs($g - 0.3) -lt 0.001) "status $(Get-Param '/master/fx/1/status'), gain $g"
    }
    $ok = Wait-Until { (Get-Param '/video/source/1/state') -eq 'ok' } 15
    Add-Row 'capd follows the restored video' $ok "capd reconnected, source 1 $(Get-Param '/video/source/1/state')"

    Write-Host "`n=== second profile, switching ===" -ForegroundColor Cyan
    $r = Invoke-Method '/profile/new' @('name=s:p5b')
    Add-Row 'new profile from the fresh state' ($r -like 'ok*' -and (Get-Param '/mixer/track_count') -eq '4' -and -not (Get-Param '/master/fx/1/type')) "$r, tracks $(Get-Param '/mixer/track_count'), current $(Get-Param '/profile/current')"
    Set-Param '/track/3/mute' true
    $t0 = Get-Date
    $r = Invoke-Method '/profile/load' @('name=s:p5a')
    $ms = [int]((Get-Date) - $t0).TotalMilliseconds
    $ok = $r -like 'ok*' -and (Get-Param '/track/1/volume_db') -eq '-7.5' -and (Get-Param '/mixer/track_count') -eq '5'
    Add-Row 'switch back' $ok "$r in $ms ms, volume $(Get-Param '/track/1/volume_db')"
    if ($gainKey) {
        $ok = Wait-Until { (Get-Param '/master/fx/1/status') -eq 'ok' } 20
        Add-Row 'VST3 after switching' ($ok -and [math]::Abs([double](Get-Param "/master/fx/1/param/$gainKey") - 0.3) -lt 0.001) "gain $(Get-Param "/master/fx/1/param/$gainKey")"
    }
    $ok = Wait-Until { (Get-Param '/video/source/1/state') -eq 'ok' } 15
    Add-Row 'capd keeps going through the switch' ($ok -and -not $procs.capd.HasExited) "source 1 $(Get-Param '/video/source/1/state')"
    $r = Invoke-Method '/profile/load' @('name=s:p5b')
    Add-Row 'p5b kept its change' ((Get-Param '/track/3/mute') -eq 'true') "track 3 mute $(Get-Param '/track/3/mute')"

    $null = Invoke-Method '/capture/record/start'
    Wait-Until { (Get-Param '/capture/state') -eq 'recording' } 15 | Out-Null
    $r = Invoke-Method '/profile/load' @('name=s:p5a')
    Add-Row 'no switching while recording' ($r -like '*conflict*' -or $r -like '*8*') "$r (capture $(Get-Param '/capture/state'))"
    $null = Invoke-Method '/capture/record/stop'
    Wait-Until { (Get-Param '/capture/state') -eq 'idle' } 15 | Out-Null

    $r1 = Invoke-Method '/profile/rename' @('name=s:p5b', 'new_name=s:p5c')
    $r2 = Invoke-Method '/profile/load' @('name=s:p5a')
    $r3 = Invoke-Method '/profile/remove' @('name=s:p5c')
    Add-Row 'rename and delete' ($r1 -like 'ok*' -and $r3 -like 'ok*' -and -not (Test-Path (Join-Path $profiles 'p5c.dcprofile'))) "$r1 / $r2 / $r3"

    Write-Host "`n=== broken file ===" -ForegroundColor Cyan
    Stop-Quiet $procs.mixd
    Set-Content -Path $file -Value 'garbage' -Encoding ascii
    $procs.mixd = Start-Mixd 'mixd-3'
    $err = Get-Param '/profile/error'
    Add-Row 'broken file: the backup is read' ((Get-Param '/mixer/track_count') -eq '5' -and $err -like '*.bak*') "tracks $(Get-Param '/mixer/track_count'), error: $err"

    if (Test-Path $ui) {
        $shot = Join-Path $out 'profile-page.png'
        & $ui --connect "127.0.0.1:$port" --page profile --screenshot $shot | Out-Null
        Add-Row 'dc-ui page' (Test-Path $shot) $shot
    }
} finally {
    foreach ($p in $procs.Values) { Stop-Quiet $p }
}

$md = Join-Path $out 'p5.md'
$lines = @("# P5-A profiles and snapshots ($stamp)", '', '| Check | Result | Detail |', '|---|---|---|')
foreach ($r in $rows) { $lines += "| $($r.Check) | $($r.Result) | $($r.Detail) |" }
$lines | Set-Content -Encoding utf8 $md
$fails = @($rows | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host "`n$($rows.Count - $fails) / $($rows.Count) passed. $md" -ForegroundColor $(if ($fails) { 'Red' } else { 'Green' })
if ($fails) { exit 1 }
