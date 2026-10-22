<#
.SYNOPSIS
  P1/V5: measures the remote controller probe (dc-v5-probe) memory, CPU and multi-touch behaviour.

.DESCRIPTION
  Runs dc-v5-probe in several variants (UI style x track count x renderer), samples the process
  from outside every second (Working Set and Private Bytes, as Task Manager shows them), runs the
  injected multi-touch self test, and writes a summary table plus CSV / JSON into -OutDir.
  Works from the repository (after a Release build) and from the package made by v5-package.ps1.

.EXAMPLE
  .\scripts\v5-measure.ps1                 # in the repository, after building Release
  .\v5-measure.ps1                         # inside the copied package folder
  .\v5-measure.ps1 -Seconds 60 -Quick
#>
param(
    [string]$Exe = '',
    [int]$Seconds = 20,
    [string]$OutDir = '',
    [switch]$Quick,          # only the main variants (custom / controls / minimal at 16 and 64 tracks)
    [switch]$SkipTouch,
    [switch]$SkipConnected
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $Exe) {
    foreach ($c in @((Join-Path $here 'dc-v5-probe.exe'),
                     (Join-Path $here '..\build\windows\tools\v5_remote_probe\Release\dc-v5-probe.exe'))) {
        if (Test-Path $c) { $Exe = (Resolve-Path $c).Path; break }
    }
}
if (-not $Exe -or -not (Test-Path $Exe)) { throw 'dc-v5-probe.exe not found. Build Release first or pass -Exe.' }
$exeDir = Split-Path -Parent $Exe
# A repository build has no Qt DLLs next to it; use the installed Qt (scripts\install-qt.ps1).
if (-not (Test-Path (Join-Path $exeDir 'Qt6Core.dll')) -and $env:DC_QT_DIR) {
    $env:PATH = (Join-Path $env:DC_QT_DIR 'bin') + ';' + $env:PATH
}
$mockExe = @((Join-Path $exeDir 'dc-mock-server.exe'),
             (Join-Path $exeDir '..\..\mock_server\Release\dc-mock-server.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $OutDir) { $OutDir = Join-Path $exeDir ("v5-results-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# --- machine description -------------------------------------------------------------------------
Add-Type -Namespace V5 -Name Native -MemberDefinition '[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);'
$cs = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$gpu = (Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name }) -join ' / '
$machine = [ordered]@{
    computer       = $env:COMPUTERNAME
    os             = "$($os.Caption) $($os.Version)"
    cpu            = $cpu.Name.Trim()
    cores          = $cpu.NumberOfLogicalProcessors
    ram_total_mb   = [math]::Round($cs.TotalPhysicalMemory / 1MB)
    ram_free_mb    = [math]::Round($os.FreePhysicalMemory / 1KB)
    gpu            = $gpu
    screen         = "$([V5.Native]::GetSystemMetrics(0))x$([V5.Native]::GetSystemMetrics(1))"
    touch_points   = [V5.Native]::GetSystemMetrics(95)   # SM_MAXIMUMTOUCHES, 0 = no touch screen
    probe          = $Exe
}
Write-Host "Machine: $($machine.computer)  $($machine.os)  RAM $($machine.ram_total_mb) MB (free $($machine.ram_free_mb))  touch points $($machine.touch_points)" -ForegroundColor Cyan

# --- one run ---------------------------------------------------------------------------------------
function Invoke-Probe([string]$name, [string[]]$probeArgs, [int]$sec) {
    $report = Join-Path $OutDir "$name.json"
    $allArgs = @($probeArgs) + @('--report', $report)
    if ($probeArgs -notcontains '--touch-test') { $allArgs += @('--duration', $sec) }
    $p = Start-Process -FilePath $Exe -ArgumentList $allArgs -PassThru
    $samples = @()
    $deadline = (Get-Date).AddSeconds($sec + 30)
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 1000
        try {
            $p.Refresh()
            if (-not $p.HasExited) { $samples += [pscustomobject]@{ ws = $p.WorkingSet64; priv = $p.PrivateMemorySize64 } }
        } catch { }
    }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Host "  $name timed out" -ForegroundColor Red }
    $r = if (Test-Path $report) { Get-Content $report -Raw | ConvertFrom-Json } else { $null }
    # Steady state = average of the second half of the samples (start-up spikes excluded).
    $steady = if ($samples.Count -ge 2) { $samples[[int]($samples.Count / 2)..($samples.Count - 1)] } else { $samples }
    [pscustomobject]@{
        run            = $name
        ui             = if ($r) { $r.ui } else { '' }
        tracks         = if ($r) { $r.tracks } else { '' }
        ws_mb          = if ($steady) { [math]::Round((($steady | Measure-Object ws -Average).Average) / 1MB, 1) } else { $null }
        ws_max_mb      = if ($samples) { [math]::Round((($samples | Measure-Object ws -Maximum).Maximum) / 1MB, 1) } else { $null }
        private_mb     = if ($steady) { [math]::Round((($steady | Measure-Object priv -Average).Average) / 1MB, 1) } else { $null }
        cpu_pct        = if ($r) { [math]::Round($r.cpu_percent, 1) } else { $null }
        fps            = if ($r) { $r.fps } else { $null }
        qml_warnings   = if ($r) { $r.qml_warnings } else { $null }
        touch          = if ($r -and $r.touch_test) { if ($r.touch_test.pass) { 'PASS' } else { 'FAIL' } } else { '' }
        exit_code      = $p.ExitCode
    }
}

$runs = @(
    @{ n = 'minimal';            a = @('--ui', 'minimal') },
    @{ n = 'custom-16';          a = @('--ui', 'custom', '--tracks', '16') },
    @{ n = 'custom-64';          a = @('--ui', 'custom', '--tracks', '64') },
    @{ n = 'controls-16';        a = @('--ui', 'controls', '--tracks', '16') },
    @{ n = 'controls-64';        a = @('--ui', 'controls', '--tracks', '64') }
)
if (-not $Quick) {
    $runs += @(
        @{ n = 'custom-64-fullscreen'; a = @('--ui', 'custom', '--tracks', '64', '--fullscreen') },
        @{ n = 'custom-64-software';   a = @('--ui', 'custom', '--tracks', '64', '--backend', 'software') },
        @{ n = 'custom-64-opengl';     a = @('--ui', 'custom', '--tracks', '64', '--backend', 'opengl') }
    )
}

$results = @()
foreach ($run in $runs) {
    Write-Host "Running $($run.n) ($Seconds s)"
    $results += Invoke-Probe $run.n $run.a $Seconds
}

if (-not $SkipConnected -and $mockExe) {
    $port = 47815
    $mock = Start-Process -FilePath $mockExe -ArgumentList '--port', $port, '--quiet' -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 800
        Write-Host "Running custom-64-connected ($Seconds s, local dc-mock-server)"
        $results += Invoke-Probe 'custom-64-connected' @('--ui', 'custom', '--tracks', '64', '--connect', "127.0.0.1:$port") $Seconds
    } finally {
        Stop-Process -Id $mock.Id -ErrorAction SilentlyContinue
    }
}

if (-not $SkipTouch) {
    foreach ($ui in @('custom', 'controls')) {
        Write-Host "Running touch-$ui (injected touch; do not touch the mouse or screen for ~10 s)"
        $results += Invoke-Probe "touch-$ui" @('--ui', $ui, '--tracks', '32', '--touch-test') 20
    }
}

if ($results | Where-Object { $_.fps -eq 0 -and $_.ui -ne 'minimal' }) {
    Write-Host 'WARNING: some runs rendered 0 fps. The screen was probably locked or the window hidden,' -ForegroundColor Yellow
    Write-Host '         so their memory numbers miss the GPU work. Run again while the screen is on.' -ForegroundColor Yellow
}
$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$results | Export-Csv -NoTypeInformation -Encoding UTF8 (Join-Path $OutDir 'summary.csv')
[ordered]@{ machine = $machine; results = $results } | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $OutDir 'summary.json')
Write-Host "Results: $OutDir" -ForegroundColor Green
