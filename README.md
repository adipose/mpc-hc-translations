# mpc-hc-translations

Backend and tooling for **MPC-HC Translation Studio** — a native Windows application that lets
translators edit [MPC-HC](https://github.com/clsid2/mpc-hc)'s UI strings against a **live preview
of the real dialogs and menus**, get AI-generated translation suggestions, and open pull requests
**straight to upstream `clsid2/mpc-hc`**.

> **License:** GPLv3 (this project derives from MPC-HC). See `COPYING.txt`.

## What's here

This repo holds the Studio's source and the backend that feeds it:

- **`studio/`** — the native Translation Studio app: a portable C++ core library
  (`studio/core`, `libmpctrans`) plus an MFC UI shell (`studio/ui`). Ships as a single
  statically-linked `Studio.exe` — no Python, no external DLLs at runtime.
- **`enrichment/`** — read-only context data shipped with the Studio: per-language packs
  (AI suggestions + reference matches) plus a core enrichment database (string catalog, dialog
  layout, research notes).
- **`bundle-build/`** — the SHA-pinned pipeline that turns one pinned `upstream/` snapshot into a
  distributable Studio bundle (neutral resource DLL, control-index, packaged release).
- **`potool/`** — the shared PO validation ruleset, used both by CI (against `clsid2/mpc-hc`) and
  mirrored in the Studio's C++ core so edits are checked before a PR is opened.
- **`upstream/`** — a git submodule pinned to a `clsid2/mpc-hc` commit; the coherence anchor that
  the bundle's dialog templates, control-index, and enrichment all derive from.

## How it fits together

- **Canonical translations live upstream**, in `clsid2/mpc-hc`
  (`src/mpc-hc/mpcresources/PO/*.po`). Pull requests from the Studio go straight there.
- `upstream/` is pinned to one commit so the bundle's dialog templates, control-index, and
  enrichment data all derive from a single coherent snapshot. A translator's edits are applied
  onto the **latest** upstream `.po` at PR time, so PRs never conflict — the pin only governs
  preview/render coherence.
- The Studio renders **real MPC-HC dialogs and menus** from a neutral (English) resource DLL,
  built from the pinned upstream `.rc`, and substitutes each control's text from the checked-out
  language's `.po` — so what a translator sees while editing is the actual live layout.
- **AI suggestions** come from two places. A small set of pre-generated, per-language suggestions
  ships as read-only defaults (in the committed enrichment packs) that a translator can accept,
  edit, or ignore. The Studio can also generate a suggestion **on demand** ("Suggest with AI"),
  calling a hosted AI provider (Claude, GPT, GLM, or OpenRouter) directly with **your own API key**
  — stored locally via Windows Credential Manager, never sent anywhere but the provider you chose.

## Private build inputs

A couple of the build inputs behind the committed enrichment packs are **not distributed** with
this repo — private, maintainer-local resources used to generate that data, not something end
users or contributors need:

- A private reference corpus of commonly-used Windows-application translations, used to help
  ground AI suggestions.
- A private snapshot of per-string research and translation data.

Everything a translator or contributor actually needs — the Studio source, the per-language
enrichment packs, and the committed core enrichment/control-index artifacts — is in this repo.
Regenerating the private inputs from scratch isn't required to build or use the Studio.

## Building

The Studio is a native Windows app built with **Visual Studio 2022 (or later), MSVC v143, x64**,
using MFC (static linkage).

1. Clone with submodules:
   ```
   git clone --recurse-submodules <this-repo-url>
   ```
   (or `git submodule update --init` after a plain clone)
2. Vendor the header-only/amalgamation dependencies in `studio/core/thirdparty/` (see
   `studio/core/thirdparty/README.md` — SQLite amalgamation + nlohmann/json, both single-file).
3. Build the neutral resource DLL and control-index (see `bundle-build/neutral-dll/README.md` and
   `bundle-build/README.md`) so the Studio has something to render against, or use a bundle
   produced by the `bundle-build` GitHub Actions workflow.
4. Open `studio/Studio.sln` in Visual Studio and build (`Release`/`x64`), or from a Developer
   PowerShell:
   ```
   msbuild studio/Studio.sln /p:Configuration=Release /p:Platform=x64
   ```

The full bundle (Studio.exe + neutral DLL + control-index + enrichment) is assembled by
`.github/workflows/bundle-build.yml`, which re-pins `upstream/` and builds everything from a
clean checkout.

## Contributing

- Translation fixes and additions belong upstream, in `clsid2/mpc-hc`'s `.po` files — the Studio
  is a tool for producing those PRs, not a place to store translations.
- Studio/tooling changes: open a PR against this repo. `potool/` is the single source of truth
  for PO validation rules; `studio/core` mirrors it in C++, and `potool/tests/test_potool.py` is
  the conformance contract the C++ port has to match.
- If you spot an error in a string's research write-up (shown alongside the translation in the
  Studio), you can submit a correction — see `data/README.md` for the format.

## Layout

```
studio/              native Translation Studio: core/ = libmpctrans (portable C++) · ui/ = MFC shell
bundle-build/         SHA-pinned pipeline: neutral DLL + control-index + core enrichment
enrichment/lang/      per-language packs (AI suggestions + reference matches), committed
data/                research-corrections.jsonl + docs; the private build inputs
                     described above are not stored here
potool/              shared PO validation rules (CI + Studio core)
upstream/            git submodule → clsid2/mpc-hc @ pinned SHA
.github/workflows/   bundle-build.yml
```
