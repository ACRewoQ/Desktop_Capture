<#
.SYNOPSIS
  C7 on a real PC: hotkeys and MIDI bindings in mixd (protocol.md 5.9).

.DESCRIPTION
  1. Builds Release (mixd, dc-cli, dc-ui)
  2. Starts mixd (fake audio devices, a throw-away bindings file) and checks, with key presses sent
     by this script (SendInput, the same path as a real keyboard):
       - a hotkey toggles /track/1/mute, a second binding on the same keys runs too
       - a step hotkey held down repeats (auto-repeat) and lowers the master volume several times
       - a combination another application holds (RegisterHotKey here) shows "conflict", and turns
         "ok" within a few seconds after it is released
       - learning: /control/learn source=key takes the next combination and swallows it
       - disabling a binding unregisters it (the key does nothing)
       - after a restart of mixd the bindings are back (saved file)
  3. MIDI: lists the MIDI inputs mixd sees. With -Midi, asks you to press a button or move a fader
     on a connected MIDI device within 60 s (learn), then to move it again (runs the binding).
  4. dc-ui screenshot of the "キー・MIDI" page against this mixd.
  5. Writes build\c7\<time>\c7.md and prints the table.

  Pressing keys while it runs may disturb the checks. It uses Ctrl+Alt+Shift+F9..F12, which nothing
  normally uses.

.EXAMPLE
  .\scripts\c7-control.ps1
  .\scripts\c7-control.ps1 -Midi
#>
param(
    [switch]$Midi,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $NoBuild) {
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set. See docs/build-and-test.md (section 1).' }
    cmake --preset windows
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    cmake --build --preset windows-release --target mixd dc-cli dc-ui
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root 'build/windows'
$mixd = Join-Path $bin 'apps/mixd/Release/mixd.exe'
$cli = Join-Path $bin 'tools/dc_cli/Release/dc-cli.exe'
$ui = Join-Path $bin 'apps/ui/Release/dc-ui.exe'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path $root "build/c7/$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$store = Join-Path $out 'mixd-control.txt'
$port = 47872

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Keys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool RegisterHotKey(IntPtr hwnd, int id, uint mods, uint vk);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool UnregisterHotKey(IntPtr hwnd, int id);
    static INPUT Key(ushort vk, bool up) {
        var i = new INPUT(); i.type = 1; i.ki.wVk = vk; i.ki.dwFlags = up ? 2u : 0u; return i;
    }
    // Presses the modifiers, then the key (held for holdMs, auto-repeat as Windows would send it), releases all.
    public static void Press(ushort[] mods, ushort vk, int holdMs) {
        int size = Marshal.SizeOf(typeof(INPUT));
        foreach (var m in mods) SendInput(1, new[] { Key(m, false) }, size);
        SendInput(1, new[] { Key(vk, false) }, size);
        for (int t = 500; t < holdMs; t += 33) {
            System.Threading.Thread.Sleep(t == 500 ? 500 : 33);
            SendInput(1, new[] { Key(vk, false) }, size);
        }
        System.Threading.Thread.Sleep(holdMs < 500 ? Math.Max(holdMs, 30) : 33);
        SendInput(1, new[] { Key(vk, true) }, size);
        for (int k = mods.Length - 1; k >= 0; --k) SendInput(1, new[] { Key(mods[k], true) }, size);
    }
}
'@

$VK_CTRL = [uint16]0x11; $VK_ALT = [uint16]0x12; $VK_SHIFT = [uint16]0x10
$mods = [uint16[]]@($VK_CTRL, $VK_ALT, $VK_SHIFT)
$F9 = [uint16]0x78; $F10 = [uint16]0x79; $F11 = [uint16]0x7A; $F12 = [uint16]0x7B

function Get-Param([string]$path) {
    $line = & $cli --port $port snapshot $path | Where-Object { $_ -like "$path = *" } | Select-Object -First 1
    if (-not $line) { return $null }
    $v = $line.Substring($path.Length + 3)
    if ($v.StartsWith('"')) { return $v.Trim('"') }
    return $v
}
function Set-Param([string]$path, $value) {
    $r = & $cli --port $port set $path $value
    if ($r -ne 'ok') { throw "set $path $value -> $r" }
}
function Invoke-Method([string]$method, [string[]]$kv = @()) {
    $r = & $cli --port $port call $method @kv
    if (-not ($r -like 'ok*')) { throw "call $method $kv -> $r" }
    return $r
}
function Add-Binding([string[]]$kv) {
    $r = Invoke-Method '/control/binding/add' $kv
    return [int]($r -replace '[^0-9]', '')
}
function Wait-Until([scriptblock]$cond, [double]$seconds = 5) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}
function Start-Mixd([string]$name) {
    $log = Join-Path $out "$name.log"
    $p = Start-Process -FilePath $mixd -ArgumentList '--port', $port, '--local-only', '--fake-devices', '--no-shm', '--no-plugins',
        '--control-store', "`"$store`"" -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not (Wait-Until { (Get-Param '/system/clock_active') -ne $null } 15)) { throw "mixd did not start (see $log)" }
    return $p
}

$rows = New-Object System.Collections.Generic.List[object]
function Add-Row([string]$check, [bool]$pass, [string]$detail) {
    $rows.Add([pscustomobject]@{ Check = $check; Result = $(if ($pass) { 'PASS' } else { 'FAIL' }); Detail = $detail })
    Write-Host ("{0,-4} {1} {2}" -f $(if ($pass) { 'PASS' } else { 'FAIL' }), $check, $detail) -ForegroundColor $(if ($pass) { 'Green' } else { 'Red' })
}
function State([int]$n) { Get-Param "/control/binding/$n/state" }

$proc = Start-Mixd 'mixd'
try {
    Write-Host "`n=== hotkeys ===" -ForegroundColor Cyan
    $b1 = Add-Binding @('trigger=key:ctrl+alt+shift+f9', 'action=toggle', 'target=/track/1/mute')
    $b2 = Add-Binding @('trigger=key:ctrl+alt+shift+f9', 'action=set', 'target=/sampler/active_bank', 'value=s:4')
    $ok = Wait-Until { (State $b1) -eq 'ok' -and (State $b2) -eq 'ok' }
    Add-Row 'binding registered' $ok "state $b1=$(State $b1), $b2=$(State $b2)"
    [Keys]::Press($mods, $F9, 60)
    $ok = Wait-Until { (Get-Param '/track/1/mute') -eq 'true' -and (Get-Param '/sampler/active_bank') -eq '4' }
    Add-Row 'hotkey runs both bindings' $ok "mute=$(Get-Param '/track/1/mute') bank=$(Get-Param '/sampler/active_bank')"
    [Keys]::Press($mods, $F9, 60)
    $ok = Wait-Until { (Get-Param '/track/1/mute') -eq 'false' }
    Add-Row 'second press toggles back' $ok "mute=$(Get-Param '/track/1/mute')"

    $b3 = Add-Binding @('trigger=key:ctrl+alt+shift+f10', 'action=step', 'target=/master/volume_db', 'value=s:-1')
    Wait-Until { (State $b3) -eq 'ok' } | Out-Null
    [Keys]::Press($mods, $F10, 1500) # held: about 30 repeats
    Start-Sleep -Milliseconds 300
    $vol = [double](Get-Param '/master/volume_db')
    Add-Row 'held step key repeats' ($vol -le -5) "master volume $vol dB after holding 1.5 s"

    Write-Host "`n=== conflict ===" -ForegroundColor Cyan
    $held = [Keys]::RegisterHotKey([IntPtr]::Zero, 77, 0x4007, 0x7A) # ctrl+alt+shift+f11, MOD_NOREPEAT
    $b4 = Add-Binding @('trigger=key:ctrl+alt+shift+f11', 'action=record_toggle')
    $ok = Wait-Until { (State $b4) -eq 'conflict' }
    Add-Row 'combination held elsewhere shows conflict' ($held -and $ok) "held by this script: $held, state $(State $b4) ($(Get-Param "/control/binding/$b4/error"))"
    [Keys]::UnregisterHotKey([IntPtr]::Zero, 77) | Out-Null
    $ok = Wait-Until { (State $b4) -eq 'ok' } 10
    Add-Row 'conflict clears after release' $ok "state $(State $b4)"

    Write-Host "`n=== learn ===" -ForegroundColor Cyan
    $b5 = Add-Binding @('action=set', 'target=/sampler/active_bank', 'value=s:6')
    Invoke-Method '/control/learn' @("binding=$b5", 'source=key') | Out-Null
    Start-Sleep -Milliseconds 300
    [Keys]::Press($mods, $F12, 60)
    $ok = Wait-Until { (Get-Param "/control/binding/$b5/trigger") -eq 'key:ctrl+alt+shift+f12' -and (Get-Param '/control/learn/binding') -eq '0' }
    Add-Row 'learn takes the combination' $ok "trigger $(Get-Param "/control/binding/$b5/trigger")"
    $bankBefore = Get-Param '/sampler/active_bank'
    Add-Row 'learning press does not run the binding' ($bankBefore -ne '6') "bank $bankBefore"
    Wait-Until { (State $b5) -eq 'ok' } | Out-Null
    [Keys]::Press($mods, $F12, 60)
    $ok = Wait-Until { (Get-Param '/sampler/active_bank') -eq '6' }
    Add-Row 'learned binding runs' $ok "bank $(Get-Param '/sampler/active_bank')"

    Write-Host "`n=== disable ===" -ForegroundColor Cyan
    Set-Param "/control/binding/$b1/enabled" 'false'
    Set-Param "/control/binding/$b2/enabled" 'false'
    Wait-Until { (State $b1) -eq 'off' -and (State $b2) -eq 'off' } | Out-Null
    $mute = Get-Param '/track/1/mute'
    [Keys]::Press($mods, $F9, 60)
    Start-Sleep -Milliseconds 500
    Add-Row 'disabled binding does nothing' ((Get-Param '/track/1/mute') -eq $mute) "mute stays $(Get-Param '/track/1/mute')"
    Set-Param "/control/binding/$b1/enabled" 'true'

    Write-Host "`n=== MIDI ===" -ForegroundColor Cyan
    $i = 1
    $devices = @()
    while ($null -ne ($n = Get-Param "/control/midi/device/$i/name")) {
        $devices += "$n ($(Get-Param "/control/midi/device/$i/state"))"
        ++$i
    }
    Add-Row 'MIDI inputs listed' $true $(if ($devices) { $devices -join ', ' } else { 'none connected' })
    if ($Midi) {
        if (-not $devices) {
            Add-Row 'MIDI learn' $false 'no MIDI input'
        } else {
            $b6 = Add-Binding @('action=toggle', 'target=/track/2/mute')
            Invoke-Method '/control/learn' @("binding=$b6", 'source=midi') | Out-Null
            Write-Host 'MIDI 機器のボタンを押すか、フェーダー・つまみを動かしてください（60 秒）' -ForegroundColor Yellow
            $ok = Wait-Until { (Get-Param "/control/binding/$b6/trigger") -like 'midi:*' } 60
            $trig = Get-Param "/control/binding/$b6/trigger"
            Add-Row 'MIDI learn' $ok "$trig from $(Get-Param "/control/binding/$b6/device")"
            if ($ok) {
                $m0 = Get-Param '/track/2/mute'
                Write-Host 'もう一度、同じボタンを押してください（30 秒）' -ForegroundColor Yellow
                $ok = Wait-Until { (Get-Param '/track/2/mute') -ne $m0 } 30
                Add-Row 'MIDI binding runs' $ok "track 2 mute $(Get-Param '/track/2/mute')"
            }
        }
    }

    Write-Host "`n=== saved ===" -ForegroundColor Cyan
    Start-Sleep -Milliseconds 1500 # the save follows the last change after 1 s
    Stop-Process -Id $proc.Id -Force
    $proc.WaitForExit()
    $proc = Start-Mixd 'mixd-restart'
    $ok = Wait-Until { (Get-Param "/control/binding/$b5/trigger") -eq 'key:ctrl+alt+shift+f12' -and (State $b5) -eq 'ok' }
    Add-Row 'bindings back after restart' $ok "binding $b5 $(Get-Param "/control/binding/$b5/trigger") $(State $b5); binding $b2 enabled $(Get-Param "/control/binding/$b2/enabled")"
    [Keys]::Press($mods, $F9, 60)
    $ok = Wait-Until { (Get-Param '/track/1/mute') -eq 'true' }
    Add-Row 'restored hotkey works' $ok "mute=$(Get-Param '/track/1/mute')"

    if (Test-Path $ui) {
        $shot = Join-Path $out 'control-page.png'
        & $ui --connect "127.0.0.1:$port" --page control --screenshot $shot | Out-Null
        Add-Row 'dc-ui page' (Test-Path $shot) $shot
    }
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}

$md = Join-Path $out 'c7.md'
$lines = @("# C7 hotkeys and MIDI ($stamp)", '', '| Check | Result | Detail |', '|---|---|---|')
foreach ($r in $rows) { $lines += "| $($r.Check) | $($r.Result) | $($r.Detail) |" }
$lines | Set-Content -Encoding utf8 $md
$fails = @($rows | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host "`n$($rows.Count - $fails) / $($rows.Count) passed. $md" -ForegroundColor $(if ($fails) { 'Red' } else { 'Green' })
if ($fails) { exit 1 }
