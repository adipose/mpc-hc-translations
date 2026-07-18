# bundle-build/ — SHA-pinned bundle pipeline

Turns one pinned `upstream/` (clsid2/mpc-hc) snapshot into a Studio bundle. **All stages read
the same pinned SHA ⇒ no inter-stage drift.** Outputs are gitignored (Release artifacts).

## Inputs (split by trust)
- **Public** (from `upstream/` @ pinned SHA): `mpc-hc.rc`, `resource.h`, `mpcresources.vcxproj`. CI-buildable; no private inputs needed.
- **Committed** (this repo): the `enrichment/` per-language packs, refreshed by hand (manual cadence)
  from private, non-distributed source data.

## Stages
1. **(Python)** parse `resource.h` → numeric↔symbol map; run upstream `TranslationDataRC(mpc-hc.rc)`
   → **control→entry index**, joined with resource.h so it is keyed by
   `(dialogNumericID, controlNumericID, EnglishText)` (+ CAPTION + POPUP + `excludedStrings`). Deterministic.
2. **(Windows/MSVC)** `mpcresources.vcxproj` (neutral config) → `mpcresources.neutral.dll`; build `Studio.exe`.
3. **Package** + stamp the upstream SHA → GitHub Release.

## Outputs (→ `dist/`, gitignored)
`Studio.exe` · `mpcresources.neutral.dll` · `control-index.json` · `core-enrichment.sqlite` · manifest (SHA-stamped).

## TODO
- [ ] Add a **"neutral" config** to `upstream/.../mpcresources.vcxproj` (or a thin wrapper) compiling `mpc-hc.rc`.
- [ ] `build_index.py`, `package.py`.
- [ ] Determinism check: same SHA + same private inputs ⇒ identical outputs.
