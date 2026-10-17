# FINDINGS — what was done, in order, with evidence

Goal: make Returnal (which ships **Streamline 1.3.3** and refuses newer runtimes) run on
a **genuine Streamline 2.14** stack so that **DLSS Multi-Frame Generation (up to 6x)**
becomes available.

Method: an SL 1.3.3 → SL 2.x **compatibility bridge** (`bridge/sl1_bridge.cpp`) that is
installed as the game's `sl.interposer.dll`, keeps the name and the complete export
surface the game expects, loads the genuine SL2 interposer from `sl2\` by absolute path
and translates every call. No game code flow is modified; the only patch to game-side
binaries is a one-time 3-byte loader-gate flip (documented below, reversible).

## Architecture

```
Returnal.exe + Returnal-Streamline* modules          (SL 1.3.3 API expectations)
        |
        v
sl.interposer.dll == sl1_bridge.dll                  (this project)
        |   LoadLibraryW("...\Win64\sl2\sl.interposer.dll") + dynamic bindings
        v
sl2\sl.interposer.dll                                (genuine Streamline 2.14.16)
        |
        v
sl.common / sl.dlss_g / sl.reflex / sl.pcl  ->  NGX (nvapp_override builds) -> driver
```

* The bridge re-exports all 195 names of the stock SL 1.3.3 interposer (39 carry real
  work, 156 are verified-unused stubs — confirmed against a PE-import scan of every
  game module).
* The D3D/DXGI creation functions that the game resolves *from the interposer*
  (`D3D12CreateDevice`, `CreateDXGIFactory1/2`, …) are forwarded to the genuine SL2
  proxies, so SL2's swapchain/device interception is installed exactly as it would be
  in a native SL2 title.
* SL1 is a single-view world here: all tagged viewports collapse onto canonical
  viewport `0`, and SL1 "sticky" tags are replayed every frame.

## Problems found & fixed (each with the log evidence that identified it)

### 1. Loader gate — SL2 rejected outright

`Returnal-StreamlineRHIPreInit-Win64-Shipping.dll` verifies the embedded signature of
the Streamline DLLs it loads; the stock SL 2.14 interposer fails that check and the
game never uses it.

Fix: `bridge/patch_gate.py` flips `?slVerifyEmbeddedSignature@@YA_NAEBVFString@@Z`
(RVA `0x3380`) to `mov al,1 ; ret` — three bytes (`B0 01 C3`). Patched module sha256
`92F9DB712DA8F702…`; the installer keeps the pristine backup and `-Remove` restores it.

### 2. Crash on the failed DLSS-G options path (T+17 s)

With DLSS-G rejected at runtime, the game crashed ~17 seconds after launch when its
`slSetFeatureConstants(DLSSG)` path failed. The bridge now fails soft on that path
(returns success / no-op translation instead of an error), which turned crash-loops
into stable multi-minute runs.

### 3. NGX disabled over the application id — the root cause of "no DLSS-G"

SL2 production builds substitute `kTemporaryAppId` (100721531) when the application id
is `0`, then disable all NGX features:

```
[00-38-13][streamline][error][tid:31284][7s:241ms:984us]commonEntry.cpp:1658[slOnPluginStartup]
  Please provide correct application id when calling slInit - NGX based features will be disabled
```

Consequences observed in that state:

* `Failed to initialize NGX` → `sl_dlss_g` never stays mapped;
* `slIsFeatureSupported(kFeatureDLSS_G = 1000)` returned `32` (`eErrorFeatureNotSupported`);
* every `slDLSSGSetOptions` failed — ~287,000 `slDLSSGSetOptions failed: -255` lines in a
  10-minute bridge log.

Fix: the bridge **relays the game's real application id** — captured by logging it at
runtime (`195018064`) — into `sl::Preferences` for SL2/NGX. `AppId` / `ProjectId` ini
knobs cover other titles (force a value, or use the NGX `Init_with_ProjectID` path,
which skips the app-id check entirely).

After the fix: NGX initialises, `sl.dlss_g` stays loaded,
`slIsFeatureSupported(feature=1000) -> 0 (eOk)`, zero setOptions failures.

### 4. Every `slEvaluateFeature` needs a ViewportHandle — 384,774 rejects per run

```
[00-38-26][streamline][error][tid:19580][19s:572ms:634us]commonEntry.cpp:704[slEvaluateFeature]
  Missing viewport handle, did you forget to chain it up in the slEvaluateFeature inputs?
```

SL2 rejects evaluate calls that do not chain a `ViewportHandle` in their input list
(`eErrorMissingInputParameter`). The bridge now chains the canonical viewport into
every marker/DLSS-G/generic evaluate. Errors: **384,774 → 0**.

### 5. Reflex markers: SL2's reflex plugin has *no* `slEvaluateFeature` callback

With (4) fixed, marker calls reached the plugin dispatch and returned
`eErrorMissingOrInvalidAPI` (28) — SL2's reflex plugin registers its marker input as
`slSetData`, and the sanctioned submission API is the feature function
**`slReflexSetMarker(PCLMarker, FrameToken)`** (`reflexEntry.cpp:868`,
`SL_EXPORT_FUNCTION(slReflexSetMarker)`).

Observable effect before the fix — subtle and important: DLSS-G was *enabled* everywhere
(mode `1` accepted, plugin loaded, no errors) but **frame generation never generated**:
FrameView showed no MFG indicator and fps stayed flat with FG on or off.

After rewiring markers through `slReflexSetMarker`: FrameView immediately reported
**MFG x2** with the game's own setting (marker fix was the difference between
"FG on, nothing happens" and real generation), and forced multipliers do the rest.
The bridge logs `reflexSetMarker=1` among its resolved feature functions.

### 6. Per-frame duplicate DLSSG options → `Repeated slDLSSGSetOptions` warning per frame

The game submits DLSS-G constants once per viewport; with the canonical viewport both
collapse onto viewport 0, so SL2 saw two identical calls per frame. A same-frame,
byte-identical duplicate is now skipped. Warnings: one per frame → **0**.

### 7. Instrumentation: 302 MB of logs in ten minutes

Every per-frame call was logged once; steady-state runs produced a 302 MB / 3,472,128-line
`sl_bridge.log` (top offenders: `slIsFeatureSupported` 1.89 M lines, DLSS-G constants,
Reflex constants, tag replay). The bridge now uses a `Throttle()` helper (first N
occurrences, then every Nth, plus log-on-change) at every noisy site: a 150 s run now
produces ~**50 KB / ~600 lines**.

## Before / after

| signal | before | after |
| --- | --- | --- |
| `Please provide correct application id` | present → NGX off | **0** |
| `Failed to initialize NGX` | present | **0** |
| `sl.dlss_g` plugin loaded | no | **yes (stays mapped)** |
| `slIsFeatureSupported(kFeatureDLSS_G)` | `32` (not supported) | **`0` (eOk)** |
| `slDLSSGSetOptions failed: -255` | ~287,000 / 10 min | **0** |
| `Missing viewport handle` | 384,774 / run | **0** |
| `Repeated slDLSSGSetOptions` | 1 per frame | **0** |
| bridge log, 10 min | 302 MB / 3,472,128 lines | ~50 KB / ~600 lines (150 s run) |
| crash at ~T+17 s | yes | no (stable runs, clean exit) |
| FrameView with FG on | no MFG indicator, fps flat | **MFG 5x, 281 fps** |

## The decisive experiment — MFG 5x

With the game's own auto/dynamic behavior the stack generates the 2x baseline
(FrameView: "MFG x2"). For a fixed multiplier the ini forces non-dynamic mode:

```ini
ForceMode=1        ; eOn — fixed multiplier, not dynamic
ForceNumFrames=4   ; 1 real + 4 generated = 5x
```

Result in the live run:

```
DLSS-G options: mode=1(app) frames=4 uir=0 flags=0x10 mvecDepth=1708x964 color=2560x1440 (app)
DLSS-G state:   status=0x0 presented=5 max=5 dynamic=1 vram=0MB
```

`status=0x0` = `DLSSGStatus::eOk`, `numFramesToGenerateMax=5` = the stack supports up
to a 6x multiplier on this system. NVIDIA FrameView then reports **FPS 281 · MFG 5x**
while the game renders ~56 fps at 2560×1440
([screenshot](evidence/frameview-mfg5x-281fps.png)).

Note on the driver override: the NVIDIA-App override machinery is detected and engaged
for other paths (NGX built as `nvapp_override`, global DLSS-SR override applied, DLSS-G
override status callback registered, `sl_dlss_g_override_0` loaded) — but on this system
it did not raise the FG multiplier on its own (the game's request stayed the 2x
baseline). The verified 5x is produced by the bridge's forced fixed multiplier.

## Residual warnings (benign, documented for completeness)

* 5× `getNGXFeatureRequirements ngxResult failed 0xbad00005` in the first second —
  NGX requirement queries before full init; SL falls back to defaults
  (`Failed to obtain DLSS-G min spec requirements from NGX, using SL defaults`).
* 5× `Failed to find global tag 'kBufferTypeDepth'/'kBufferTypeMotionVectors'` exactly
  around an in-game resolution change (1080p → 1440p) — a transient re-tagging race
  that does not recur (SL's own message: "this needs to be fixed if occurring every
  frame").
* Clean shutdown path: `Destroyed DXGIFactory proxy …`, `Delayed destroy resource list
  count 0`.

## Environment & hashes

| item | value |
| --- | --- |
| Game | Returnal (Steam), DX12, single-view |
| Streamline (new) | 2.14.16 `b742680bf` PRODUCTION (Aug 28 2026 build), host SDK v1.5.0 |
| Streamline (stock) | 1.3.3 (backed up by the installer) |
| GPU class | RTX 50-series (NGX arch store `0B1`, `numFramesToGenerateMax=5`) |
| Bridge | `sl1_bridge.dll` — 264,192 bytes, sha256 `423B08934278C5A3A27BB588747BD130F59962B065D9A095C9102CABF423E737` |
| Gate DLL (patched) | `Returnal-StreamlineRHIPreInit-Win64-Shipping.dll` sha256 `92F9DB712DA8F70264AFD1B49B3D21C08F690420D6DF2EF7A3A4490209F7C352` (`B0 01 C3` at RVA `0x3380`) |
| Gate patch (bytes) | `?slVerifyEmbeddedSignature@@YA_NAEBVFString@@Z` → `mov al,1 ; ret` |

## Reproduce

1. Build + install per [README.md](README.md) (`bridge/install.ps1`).
2. Launch the game; confirm in `sl_bridge.log`:
   * `app identity: game appId=… -> applicationId=…`
   * `feature functions resolved: … reflexSetMarker=1`
   * `DLSS-G options: … frames=4 …` and later `DLSS-G state: status=0x0 … max=5 …`
3. Confirm in `sl2logs\<pid>\sl.log`: no `Please provide correct application id`,
   no `Missing viewport handle`, `sl.dlss_g` loaded, clean shutdown.
4. Enable the NVIDIA overlay: FrameView should read an MFG multiplier ≥ 2 with FG on,
   and `MFG 5x` with the shipped forced configuration.
