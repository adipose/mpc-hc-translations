# studio/ — native Translation Studio (one static `.exe`)

Path B: **no Python in the shipped app.** A portable C++ core lib + an MFC UI shell.

## core/ — `libmpctrans` (portable, no MFC)
- **PO serializer** (gettext-exact: escaping + 78-col wrapping) — surgical single-entry splice.
  - **Acceptance gate (mandatory):** 177-file byte-equivalence test vs polib output.
- **Validation rules** (format-specifier / `&`-mnemonic / `\n` parity, structural integrity, encoding) — shared with the CI `potool`.
- **GitHub client** — device-flow OAuth + Git Data API (WinHTTP + nlohmann/json); token via Windows Credential Manager/DPAPI.
- **Enrichment reader** — `sqlite3.c` amalgamation (zero external dep).

## ui/ — MFC shell
- Live-preview: instantiate neutral `IDD_*` templates (from the bundle's `mpcresources.neutral.dll`),
  `SetWindowText` from the in-memory `.po` via the precomputed control-index → instant in-language preview.
- Synthetic surfaces (message-box gallery, status/OSD strips, tooltip bubbles, button gallery, grid).
- "All strings / Untranslated" list (sourced from latest `.po`).
- Edit panel: English · current · AI suggestion · reference matches · context · overflow.

Ships as **one statically-linked native `.exe`** (≈1–3 MB). Frozen-Python sidecar = documented fallback only.

## Scaffold status (2026-07-05)
Structure + interfaces + VS solution are laid down; open `Studio.sln` (VS 2022, v143, MFC) to implement.
```
Studio.sln
core/  libmpctrans.vcxproj · include/mpctrans/{config,po,validate,github,enrichment,control_index}.h
       src/*.cpp · tests/{po_roundtrip_test,validate_conformance_test}.cpp · thirdparty/(vendor sqlite3.c, json.hpp)
ui/    Studio.vcxproj · StudioApp · MainFrame · LivePreview · EditPanel  (see ui/README.md for MFC TODO)
```
- **Implemented:** `config.h` (incl. `OAUTH_CLIENT_ID`), `validate.cpp` entry rules (faithful C++ port of potool — mirrors the conformance suite), `escape_po`/`unescape_po`, and the `LivePreview` substitution sketch (real Win32 flow).
- **TODO (the real v1 work):** PO parse/serialize/wrap/splice (fidelity-gated by `tests/po_roundtrip_test` over all 177 `.po`), file-level validation, WinHTTP GitHub client, sqlite readers, control-index JSON load, and the MFC plumbing (`pch`, `.rc`, layout, synthetic surfaces).
- **First build step:** vendor `core/thirdparty/` then build `libmpctrans` + run `validate_conformance_test` (should match `potool`), before the byte-equivalence gate.

