# =============================================================================
#  install.ps1 - deploy the Streamline 1.3.3 -> 2.14.16 bridge into Returnal
#
#  What it does:
#    1. backs up the pristine stock files (once) into .\backup
#    2. installs sl1_bridge.dll as the game's sl.interposer.dll
#    3. copies the genuine Streamline 2.14 stack into <plugin>\sl2
#    4. writes sl_bridge.ini (only if missing)
#    5. patches the signature gate in Returnal-StreamlineRHIPreInit-Win64-Shipping.dll
#       (mov al,1; ret at ?slVerifyEmbeddedSignature@@YA_NAEBVFString@@Z)
#
#  Run with -Remove to restore the two pristine game files.
# =============================================================================
param(
    [switch]$Remove,
    [string]$Game = 'D:\SteamLibrary\steamapps\common\Returnal'
)

$ErrorActionPreference = 'Stop'
$src      = 'C:\Users\admin\AppData\Local\Temp\opencode\bridge'
$ref      = 'C:\1\CONTROL Resonant (2026)\CONTROL Resonant'
$py       = 'C:\Program Files\Python313\python.exe'
$plug     = Join-Path $Game 'Engine\Plugins\Streamline\Binaries\ThirdParty\Win64'
$binDir   = Join-Path $Game 'Returnal\Binaries\Win64'
$preinit  = Join-Path $binDir 'Returnal-StreamlineRHIPreInit-Win64-Shipping.dll'
$bakDir   = Join-Path $src 'backup'
$bakInter = Join-Path $bakDir 'sl.interposer.stock-1.3.3.dll'
$bakPre   = Join-Path $bakDir 'Returnal-StreamlineRHIPreInit.stock.dll'

function Kill-Check {
    $p = Get-Process -Name 'Returnal' -ErrorAction SilentlyContinue
    if ($p) { throw 'Returnal is running - close the game before installing/removing.' }
}

function Show-File($f) {
    if (Test-Path $f) {
        $h = (Get-FileHash $f -Algorithm SHA256).Hash.Substring(0, 16)
        '{0,-46} {1,10} bytes  sha256:{2}' -f (Split-Path $f -Leaf), (Get-Item $f).Length, $h
    }
}

if ($Remove) {
    Kill-Check
    if (-not (Test-Path $bakInter)) { throw "backup missing: $bakInter" }
    if (-not (Test-Path $bakPre))   { throw "backup missing: $bakPre" }
    Copy-Item $bakInter (Join-Path $plug 'sl.interposer.dll') -Force
    Copy-Item $bakPre $preinit -Force
    Remove-Item (Join-Path $plug 'sl_bridge.ini') -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $plug 'sl_bridge.log') -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $plug 'sl2') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $plug 'sl2logs') -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host '[install] RESTORED pristine stock Streamline 1.3.3:'
    Show-File (Join-Path $plug 'sl.interposer.dll')
    Show-File $preinit
    & $py (Join-Path $src 'patch_gate.py') $preinit --check
    exit 0
}

Kill-Check

if (-not (Test-Path $src\sl1_bridge.dll)) { throw "build missing: $src\sl1_bridge.dll (run build.bat)" }
if (-not (Test-Path $ref\sl.interposer.dll)) { throw "SL2 reference stack missing: $ref" }

New-Item -ItemType Directory -Force -Path $bakDir  | Out-Null
New-Item -ItemType Directory -Force -Path $plug\sl2 | Out-Null
New-Item -ItemType Directory -Force -Path $plug\sl2logs | Out-Null

# 1. one-time backups of the pristine files
if (-not (Test-Path $bakInter)) {
    Copy-Item (Join-Path $plug 'sl.interposer.dll') $bakInter -Force
    Write-Host "[install] backed up stock sl.interposer.dll"
} else { Write-Host "[install] stock interposer backup already present" }

if (-not (Test-Path $bakPre)) {
    Copy-Item $preinit $bakPre -Force
    Write-Host "[install] backed up pre-init module"
} else { Write-Host "[install] pre-init backup already present" }

# 2. the bridge becomes the interposer
Copy-Item $src\sl1_bridge.dll (Join-Path $plug 'sl.interposer.dll') -Force

# 3. config (never overwrite an existing ini - it may be tuned)
if (-not (Test-Path (Join-Path $plug 'sl_bridge.ini'))) {
    Copy-Item $src\sl_bridge.ini (Join-Path $plug 'sl_bridge.ini') -Force
    Write-Host "[install] wrote sl_bridge.ini (first time)"
}

# 4. genuine SL2 stack into sl2\
$sl2files = @('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll',
              'sl.dlss.dll','sl.dlss_d.dll','nvngx_dlssg.dll','nvngx_dlss.dll','nvngx_dlssd.dll')
foreach ($f in $sl2files) {
    $s = Join-Path $ref $f
    if (Test-Path $s) { Copy-Item $s (Join-Path $plug "sl2\$f") -Force }
    else { Write-Host "[install] note: $f not found in reference stack, skipped" }
}

# 5. patch the signature gate
& $py (Join-Path $src 'patch_gate.py') $preinit
if ($LASTEXITCODE -ne 0) { throw 'gate patch failed' }

Write-Host ''
Write-Host '[install] deployed:'
Show-File (Join-Path $plug 'sl.interposer.dll')
Show-File (Join-Path $plug 'sl2\sl.interposer.dll')
Show-File (Join-Path $plug 'sl2\sl.dlss_g.dll')
Show-File $preinit
Write-Host ''
Write-Host "[install] bridge log will be written to $plug\sl_bridge.log"
Write-Host "[install] SL2 log will be written to $plug\sl2logs\sl.log"
