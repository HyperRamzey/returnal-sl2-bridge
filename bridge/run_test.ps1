# =============================================================================
#  run_test.ps1 - one launch cycle of the bridge test
#
#    1. (optionally) disables the third-party proxies  (VERSION.dll = DLSS
#       Enabler/RTX Encore, dxgi.dll = ReShade, dlssg_to_fsr3... = OptiScaler
#       primed copy)
#    2. starts the window poller in the background  (names any error dialog)
#    3. launches Returnal through Steam
#    4. waits, then prints: poller log, bridge log, SL2 log, enabler log tail
#
#  Usage:  powershell -File run_test.ps1 [-Mods Off|On] [-Wait 105]
# =============================================================================
param(
    [ValidateSet('Off', 'On', 'Keep')]
    [string]$Mods = 'Off',
    [int]$Wait = 105,
    [string]$Game = 'D:\SteamLibrary\steamapps\common\Returnal',
    [switch]$Direct,
    [string]$GameArgs = ''
)

$ErrorActionPreference = 'Stop'
$src  = 'C:\Users\admin\AppData\Local\Temp\opencode\bridge'
$plug = Join-Path $Game 'Engine\Plugins\Streamline\Binaries\ThirdParty\Win64'
$bin  = Join-Path $Game 'Returnal\Binaries\Win64'

if (Get-Process -Name 'Returnal' -ErrorAction SilentlyContinue) {
    Write-Host '[run] game already running - close it first'; exit 1
}

if ($Mods -ne 'Keep') {
    if ($Mods -eq 'Off') { & (Join-Path $src 'mod_toggle.ps1') -Off }
    if ($Mods -eq 'On')  { & (Join-Path $src 'mod_toggle.ps1') -On }
}

# clear per-run logs so the evidence is unambiguous
Remove-Item "$plug\sl_bridge.log" -ErrorAction SilentlyContinue
Remove-Item "$plug\sl2logs\*" -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "[run] window poller for $Wait s ..."
$pollOut = Join-Path $src 'windows.log'
Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $src 'pollwin.ps1'), '-Seconds', "$Wait", '-Out', $pollOut) -WindowStyle Hidden

Write-Host '[run] launching Returnal ...'
if ($Direct) {
    $exe = Join-Path $bin 'Returnal-Win64-Shipping.exe'
    Write-Host "[run] direct: $exe $GameArgs"
    if ([string]::IsNullOrWhiteSpace($GameArgs)) {
        Start-Process -FilePath $exe -WorkingDirectory $bin
    } else {
        Start-Process -FilePath $exe -ArgumentList $GameArgs -WorkingDirectory $bin
    }
} else {
    Write-Host '[run] via Steam'
    Start-Process 'steam://rungameid/1649240'
}
Start-Sleep -Seconds $Wait

Write-Host ''
Write-Host '================ PROCESS ================'
Get-Process -Name Returnal -ErrorAction SilentlyContinue | Select-Object Id, @{n='CPU_s';e={[math]::Round($_.CPU,1)}}, @{n='WS_MB';e={[math]::Round($_.WorkingSet64/1MB)}}, Responding | Format-Table -AutoSize

Write-Host '================ DIALOGS / WINDOWS ================'
Get-Content $pollOut -ErrorAction SilentlyContinue | Select-Object -First 25

Write-Host '================ BRIDGE LOG (first 25) ================'
Get-Content "$plug\sl_bridge.log" -ErrorAction SilentlyContinue | Select-Object -First 25

Write-Host '================ BRIDGE LOG (last 45) ================'
Get-Content "$plug\sl_bridge.log" -ErrorAction SilentlyContinue | Select-Object -Last 45

Write-Host '================ SL2 LOG (errors/warnings) ================'
$sl2log = Get-ChildItem "$plug\sl2logs" -Recurse -Filter 'sl.log' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($sl2log) { Write-Host "[run] newest SL2 log: $($sl2log.FullName)" } else { Write-Host '[run] NO SL2 LOG FILE FOUND' }
if ($sl2log) {
    Select-String -Path $sl2log.FullName -Pattern '\[error\]|\[warn\]' -ErrorAction SilentlyContinue | Select-Object -Last 25 | ForEach-Object { $_.Line }
}

Write-Host '================ SL2 LOG (last 20) ================'
if ($sl2log) { Get-Content $sl2log.FullName -ErrorAction SilentlyContinue | Select-Object -Last 20 }
