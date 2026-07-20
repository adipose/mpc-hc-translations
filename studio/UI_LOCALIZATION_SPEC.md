# Localizing the Studio's own UI

Status: **proposal / not implemented.** The Translation Studio's interface is English-only. This
sketches what it would take to translate the tool itself, and flags the traps — several of which are
the same bugs our users report against MPC-HC.

## Why

Translators are, by definition, not necessarily comfortable in English. The tool asking them to
work in English is a small but real barrier. It also dogfoods the product: we'd be consuming the
same `.po` pipeline we ask contributors to fill.

## The one decision that shapes everything

**UI language and target language must be independent.**

A translator working on `ja` may want the Studio's chrome in `ja` — or in English, or in a third
language they read more comfortably. Binding the UI language to the language being edited would be
wrong, and would also make the app rewrite its own chrome every time you switch languages in the
picker. So:

- UI language = its own setting (persisted via `AfxGetApp()->WriteProfileInt/String`, alongside the
  existing `theme/mode` key), defaulting to the system UI language, falling back to English.
- Changing it is independent of `LoadLanguage()`.

## Current state

User-visible text is hardcoded as wide-string literals across the UI layer:

| File | Wide-string literals (rough) |
|---|---|
| `ui/MainFrame.cpp` | ~459 |
| `ui/EditPanel.cpp` | ~58 |
| `ui/AiSettingsDlg.cpp` | ~21 |
| `ui/LivePreview.cpp` | ~19 |
| `ui/Theme.cpp` | ~19 |
| `ui/SuggestFixDlg.cpp` | ~11 |

Those counts are an upper bound — many are not user-facing (class names, registry keys, msgctxt
literals like `"IDS_SUBRESYNC_CLN_PREVIEW"`, format fragments, `L""`). A realistic translatable
surface is likely **150–250 strings**. Someone has to trawl them by hand; there is no marker to
grep for, which is itself an argument for introducing one.

## Proposed approach

1. **Introduce a lookup macro** — e.g. `TR("Preview")` — that returns the translated `CString` or
   the English literal on miss. Keying by the *English source text* (rather than an invented id)
   means the code stays readable and there is no id/text drift, at the cost of duplicate keys for
   words that need different translations by context. Where that bites, use an explicit context:
   `TRC("menu", "Preview")`.
2. **Extract** the literals to `studio.pot` with a small script, mirroring `bundle-build/`'s
   existing tooling conventions. The extractor can key off the `TR(`/`TRC(` markers once step 1 is
   applied, so extraction is mechanical and repeatable.
3. **Ship** `studio.<lang>.po` in the bundle beside the existing `po/` directory; load the chosen UI
   language at startup and expose a picker (View menu is the natural home).
4. **Fall back** per string to English. Partial translations must be normal — never blank.

`libmpctrans` already has a `.po` reader (`mpctrans::PoFile`), so parsing is free; this is mostly
extraction plus a lookup table.

## Reusing MPC-HC's translations — tempting, mostly a trap

We already ship 44 languages of MPC-HC strings, and some Studio vocabulary overlaps exactly
("Preview", "Cancel", "Options", "Language"). Borrowing them looks like free localization.

It isn't, in general:

- **Context differs.** MPC-HC's `IDS_SUBRESYNC_CLN_PREVIEW` "Preview" is a *column header in the
  subtitle resync grid*. Our Preview button *opens a menu preview*. Same English word, potentially
  different target-language word (noun vs. verb, different gender/case).
- **Mnemonics and tails.** Many MPC-HC strings carry `&` accelerators and `\t` shortcut tails that
  are meaningless in our chrome and must be stripped — and stripping `&` correctly is not trivial
  (`&&` is a literal ampersand).
- **Coupling.** Our UI text would silently change whenever upstream retranslates an unrelated
  string.

**Recommendation:** don't auto-borrow. If we want the leverage, curate a small explicit map of
*hand-checked* reuses (a dozen or so unambiguous nouns), and treat everything else as our own
string. Reuse should be an opt-in per string, never a fallback rule.

(This is exactly why the menu Preview button is currently a plain English literal: reusing
`IDS_SUBRESYNC_CLN_PREVIEW` would have made it the only localized label in an otherwise English UI,
borrowing a word from an unrelated context.)

## The layout trap — our own dialogs will clip

`MainFrame::Layout()` positions controls with **hardcoded, DPI-scaled pixel widths**:

```cpp
m_langCombo.MoveWindow(M, S(6), S(200), comboDrop);
m_btnCheckout.MoveWindow(M + S(208), S(5), S(96), comboH);   // "Get latest" fits in 96px... in English
```

German and Japanese routinely run 30–50% longer than English. A localized UI would clip its own
buttons — **the identical bug class translators report against MPC-HC** (see issue #3: a
`BS_MULTILINE` radio clipping its caption, and our themed painter that ignored the style).

Two fixes, in order of preference:

1. **Measure and lay out.** Compute button widths from the rendered text
   (`DrawText`/`GetTextExtentPoint32` with the control's font, `DT_CALCRECT`) plus padding, then
   flow the row. MPC-HC does this upstream via `CMPCThemeUtil::AdjustDynamicWidgetPair`, already
   partially ported in `ui/LivePreview.cpp`.
2. **Generous fixed widths + ellipsis** as a stopgap: `SS_ENDELLIPSIS` on statics, wider buttons.
   Cheap, but it degrades in exactly the languages we most want to support.

Anything with a fixed pixel width and translated text needs an audit. The tabs
(`Dialogs`/`Menus`/`All strings`/`Untranslated`/`Review`) are the tightest constraint.

## Other traps

- **Mnemonics.** `&Get latest` — translated labels need their own mnemonics, and duplicates within a
  window must be avoided. Our own Review tab flags exactly this for MPC-HC.
- **Right-to-left.** Arabic/Hebrew are in our language list. Real RTL support (`WS_EX_LAYOUTRTL`,
  mirrored layout) is a much larger project; scoping RTL *out* initially is defensible, but say so.
- **Fonts.** The UI font is hardcoded Segoe UI 9pt (`m_font.CreatePointFont(90, L"Segoe UI")`),
  which lacks CJK coverage — Windows will font-link, but the result can look wrong. Prefer the
  system message font (`SPI_GETNONCLIENTMETRICS`), which `LivePreview` already uses for the dialog
  preview.
- **Live switching.** Changing UI language should ideally not require a restart. Every control's
  text would need resetting plus a `Layout()`; a restart prompt is an acceptable first version.

## Suggested increments

1. Add `TR()`/`TRC()` + the lookup table; convert **one** surface (the toolbar row + tabs) end to end.
2. Fix that surface's layout to measure text rather than assume widths.
3. Write the extractor, produce `studio.pot`, translate **one** language (German — long words, good
   stress test, and we have native reviewers in the issue tracker).
4. Ship it behind a UI-language picker defaulting to English; widen coverage from there.

Stopping after step 2 still leaves the codebase better: the layout stops assuming English.

## Open questions

- Where do `studio.<lang>.po` files live and who translates them — the same Transifex project, a
  separate one, or PRs against this repo?
- Do we localize AI-facing text (prompts, `AiSettingsDlg` explanations)? Prompts should almost
  certainly stay English for model quality; the settings *chrome* around them can be translated.
- Is RTL in or out of scope for v1?
