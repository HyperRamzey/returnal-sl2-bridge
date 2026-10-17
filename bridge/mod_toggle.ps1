# =============================================================================
#  mod_toggle.ps1 - enable/disable the RTX Encore / DLSS Enabler dxgi+version
#                  proxies in the game directory (fully reversible renames)
#
#  The mod ships as VERSION.dll (primary loader) and dxgi.dll (proxy), plus a
#  helper copy of nvngx_dlssg.dll under the name dlssg_to_fsr3_amd_is_better.dll.
#  With a genuine Streamline 2 stack in place none of them are needed, and the
#  proxies conflict with SL2's own DXGI/D3D12 hooks.
#
#  Usage:   powershell -File mod_toggle.ps1 -Off
#           powershell -File mod_toggle.ps1 -On
# =============================================================================
param(
    [switch]$Off,
    [switch]$On,
    [string]$Game = 'D:\SteamLibrary\steamapps\common\Returnal'
)

$ErrorActionPreference = 'Stop'
$binDir = Join-Path $Game 'Returnal\Binaries\Win64'

$names = @('VERSION.dll', 'dxgi.dll', 'd3d12.dll', 'dlssg_to_fsr3_amd_is_better.dll')

Write-Host "--- mod state ($binDir) ---"
foreach ($n in $names) {
    $live = Join-Path $binDir $n
    $offPath = "$live.mod-off"
    $state = if (Test-Path $live) { 'ENABLED' } elseif (Test-Path $offPath) { 'disabled' } else { 'absent' }
    '{0,-34} {1}' -f $n, $state
}

if ($Off) {
    if (Get-Process -Name 'Returnal' -ErrorAction SilentlyContinue) { throw 'game is running' }
    foreach ($n in $names) {
        $live = Join-Path $binDir $n
        if (Test-Path $live) {
            Move-Item $live "$live.mod-off" -Force
            Write-Host "disabled: $n"
        }
    }
    Write-Host '[mod] mod proxies disabled - genuine Streamline 2 + driver override path is now clean'
}

if ($On) {
    if (Get-Process -Name 'Returnal' -ErrorAction SilentlyContinue) { throw 'game is running' }
    foreach ($n in $names) {
        $offPath = Join-Path $binDir "$n.mod-off"
        if (Test-Path $offPath) {
            Move-Item $offPath (Join-Path $binDir $n) -Force
            Write-Host "re-enabled: $n"
        }
    }
    Write-Host '[mod] mod proxies re-enabled'
}
