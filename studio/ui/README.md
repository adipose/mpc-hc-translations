# ui/ — MFC Studio shell (Windows/MSVC)

UI + rendering only; all logic is in `../core` (libmpctrans). **Working** — builds to a single
static `Studio.exe` and drives the full translate loop against the real bundle.

## Classes
- **StudioApp** — `CWinApp`; creates MainFrame with the `IDR_MAINFRAME` menu.
- **MainFrame** — language picker + Checkout + status; surface tabs (Dialogs / Menus / All strings);
  left string list, center dialog picker + LivePreview, right EditPanel. Holds the checked-out
  language's 3 `PoFile`s + the bundled `ControlIndex`/`CoreEnrichment` + the language `LangPack`,
  and the pending-edit sets. `File > Submit PR` splices edits onto the latest upstream and opens
  the PR (device-flow auth on first use).
- **LivePreview** — loads `mpcresources.neutral.dll`, copies the `IDD_` template and patches
  `WS_POPUP`→`WS_CHILD` so it embeds, `CreateDialogIndirect`, substitutes each control's text from
  the `.po` via the control-index. Click-to-edit via `WM_PARENTNOTIFY` → `HitTest` (keyed by the
  English text captured pre-substitution). `DetectOverflow` measures text extent vs control rect.
  Also loads `RT_MENU` resources (`LoadRawMenu`) for the Menus surface.
- **Menus surface** — a resource picker (main menu bar `IDR_MAINFRAME` + the two context menus)
  drives three views of the real menus:
  - a **horizontal menu bar** strip (main menu only) — a translated `HMENU` on an *owned* popup
    window (`m_menuBar`) positioned over the top of the pane, because Windows only draws a menu
    bar on non-child windows;
  - a `CTreeCtrl` rendering the *entire* hierarchy with `.po` translations inline (commands
    resolve by numeric id, popup headers by English text; `\t` shortcuts preserved);
  - right-clicking a popup node pops the **real translated Win32 popup** (`TrackPopupMenu`) with
    native cascades.
  Selecting a tree node is click-to-edit; applying a menu edit rebuilds all three.
- **EditPanel** — English / editable translation / AI suggestion (Accept) / references / context /
  live `validate::check_entry` findings + overflow note. `OnCommit` → MainFrame applies the edit.

## Bundle resolution (`Bundle::Locate`)
Walks up from `Studio.exe`: a **release** layout has the artifacts beside the exe
(`control-index.json`, `mpcresources.neutral.dll`, `core-enrichment.sqlite`, `lang/`, `po/`); a
**dev** checkout resolves `<repo>/dist` + `<repo>/enrichment/lang` + the submodule `…/PO` as the
`.po` cache.

## Language loading (cache-first)
The bundle ships the `.po` at its pinned upstream SHA (`po/`; dev = the submodule), so selecting a
language **loads instantly from that cache** — no GitHub round-trip, and content shows on launch
(a default language auto-loads). The **Get latest** button refetches the current language from
upstream HEAD when you want the freshest base for a PR (Submit re-fetches + splices per file
regardless, so editing cached data is safe). The language list is drawn from the `.po` cache (the
translatable set), not the enrichment packs (a superset with AI-only entries). Switching languages
re-renders the active surface (dialog preview **or** menu bar+tree) and warns before discarding
pending edits.

## Verified (driven end-to-end on this machine)
Launch auto-loads `de` from cache → list + live preview of `IDD_ABOUTBOX` with German substitution
→ select a string (English + current + enrichment context populate) → edit + Apply → **preview
re-renders live** and the list updates → switch the language combo to `el` and the menu bar + tree
re-render in Greek instantly → status shows source (`cached @ <sha>` vs `latest via GitHub`) and the
pending-edit count.

## Build
Open `../Studio.sln` in VS 2022+ (v143, MFC static, static CRT), or:
`msbuild ..\Studio.sln /p:Configuration=Release /p:Platform=x64`. Vendor `../core/thirdparty/`
(sqlite3.c, json.hpp) first. Sources are UTF-8 → both projects compile with `/utf-8`.
Requires the bundle artifacts (dev: run the `bundle-build` scripts + `neutral-dll` script).

## Still TODO (polish, not blockers)
- Target-language font + `WS_EX_LAYOUTRTL` for ar/he in the preview (accurate overflow).
- Synthetic surfaces (message-box gallery, OSD strips, tooltips) + an Untranslated filter.
- Persist window layout; splitter bars between the three panes.
