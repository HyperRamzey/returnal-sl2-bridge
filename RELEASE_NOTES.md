# v1.0.0 — DLSS Multi-Frame Generation in Returnal via a Streamline 1.x → 2.x bridge

First public build of the compatibility bridge that runs Returnal (a Streamline 1.3.3
integration) on a genuine **Streamline 2.14** stack and enables **DLSS MFG**.

**Verified 2026-10-18:** NVIDIA FrameView reports **MFG 5x at 281 FPS** while the game
renders ~56 fps at 2560×1440 (screenshot and raw logs in the repository's `evidence/`).

## Assets

| asset | sha256 |
| --- | --- |
| `sl1_bridge.dll` (install **renamed as** `sl.interposer.dll`) | `423B08934278C5A3A27BB588747BD130F59962B065D9A095C9102CABF423E737` |
| `sl_bridge.ini` (ships as the verified MFG 5x configuration) | — |

## Requirements

* Returnal (Steam) with its Streamline plugin folder.
* A genuine **Streamline 2.x payload** (`sl.interposer.dll`, `sl.common.dll`,
  `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, …) placed in an `sl2\` folder next to
  the bridge.
* The one-time loader-gate patch for
  `Returnal-StreamlineRHIPreInit-Win64-Shipping.dll` — `bridge/patch_gate.py`
  flips 3 bytes and is fully reversible.

## Install (short)

1. Back up `…\Engine\Plugins\Streamline\Binaries\ThirdParty\Win64\sl.interposer.dll`.
2. Copy `sl1_bridge.dll` into that folder **as** `sl.interposer.dll`.
3. Copy `sl_bridge.ini` next to it; create `sl2\` with the genuine SL 2.x files.
4. `python bridge/patch_gate.py "…\Returnal\Binaries\Win64\Returnal-StreamlineRHIPreInit-Win64-Shipping.dll"`
5. Launch the game (Steam).

Full instructions, configuration reference and the complete engineering log
(seven root causes with before/after log evidence) are in the repository:
`README.md` and `FINDINGS.md`.

## Provenance

The released DLL is compiled from `bridge/sl1_bridge.cpp` in this repository
(source sha256 `EBF17B5A0D365E3F84AFB42480F40D980BC2A04D547B04166F6A5DCD7E6B84DC`).
A clean rebuild from the repository sources reproduces the release binary byte-for-byte
except the two standard MSVC link-timestamp fields (6 bytes total);
all code, data and export tables are identical. Method in `README.md` →
"Source ↔ binary correspondence".

## What was verified

* NGX initialises with the game's real application id; `sl.dlss_g` stays loaded;
  `slIsFeatureSupported(kFeatureDLSS_G)` returns `eOk`.
* Frame generation actually generates: FrameView **MFG x2** with the game's own
  setting, **MFG 5x** with `ForceMode=1` + `ForceNumFrames=4`.
* Stable multi-minute runs, clean exits, no crash dumps.
* 384,774 per-run viewport rejects, ~287 k failed option calls, per-frame warnings and
  302 MB/10 min logging reduced to zero / ~50 KB respectively as part of the fixes.
