# Translation Studio — "Review Queue" feature spec

Status: proposed (2026-07-12). Audience: the Studio implementer. Self-contained — no prior context
assumed. Earlier standalone research (fit measurement, suggestion pipeline) informed this spec, but
**the production feature should use the Studio's own code, not any of that throwaway analysis
tooling** (see Reuse).

## 1. Purpose

A Studio view that surfaces **potentially bad translations** for human review — starting with
translations that **don't fit their UI control**, later expanding to suspected **poor translations**.
It applies to *all* translations, human/imported included: measurement over the current corpus found
**259 single-line label/button overflows, 242 of them human** (imported from `.po`) — real, visible UI
defects shipping today (worst: Bengali 51, Arabic 17, pt_BR/Bulgarian 15). This is not just an
AI-suggestion gate; it's QA on what's already shipped.

## 2. Design principle — precision-ordered tiers

Order flags by **precision**, not by how interesting they are. Deterministic flags (fit, placeholders)
are ~100% precise → they lead. Semantic "poor translation" flags are genuinely noisy (a divergence from
a model is often a valid paraphrase, not an error) → they come later, always labeled **"suspected,"**
with evidence, for the human to judge. Leading with noisy flags trains translators to ignore the queue.

## 3. Tier 1 — deterministic flags (build first)

### 3a. Fit / overflow  ← the initial shipment

Flag a translation whose rendered width exceeds the space its control allows.

- **Scope: single-line, width-constrained controls only** — labels and buttons. **Exclude** tooltips
  (render in an auto-sizing popup) and multi-line / wrapping statics (they word-wrap; width doesn't
  constrain a single line). Discriminator: control **height ≤ ~10 DU** (one line of 9pt) ⇒ single-line;
  taller ⇒ wraps. Buttons are always single-line.
- **Group-aware (important):** some controls are flexible-width and come in **pairs/groups sharing a
  total span** (e.g. `label + edit + unit` on one row). For these the constraint is
  `sum(rendered widths of the group) ≤ sum(available widths)`, not each control vs its own rect — a wide
  label is fine if its sibling has slack, and two individually-fitting controls can jointly overflow.
- **Two severities:** **hard** = rendered > available (physically clips); **tight** = 90–100% of
  available (at-risk; a soft "verify," given measurement is approximate). Present hard first.
- **Measurement engine — reuse `studio/ui/LivePreview.cpp` `DetectOverflow(HWND)`.** It already renders
  the real dialog templates with the target strings using GDI (`GetTextExtentPoint32W` /
  `DrawTextW DT_CALCRECT`, the system message font Segoe UI) and reports per-control overflow — this is
  the accurate, production path and it already word-wraps (`DT_WORDBREAK`), which correctly handles the
  wrapping controls the earlier analysis tooling could not. Do **not** port the scratchpad PIL measurer;
  it was an approximation for analysis only, not production-accurate. Scope `DetectOverflow`'s output to
  single-line width-constrained controls for this flag.

### 3b. Placeholder mismatch

Flag when the translation's printf placeholders differ from the English source in **count or type**:
`%s %d %u %ld %i64d %1 %2 …`. A missing/extra/retyped placeholder is a crash-or-garbage bug. Deterministic
diff of the placeholder multiset between `msgid` and `msgstr`.

### 3c. Accelerator problems

Flag missing `&` mnemonic where the English has one, and **duplicate** `&`-accelerator letters within
the same dialog/menu (two controls claiming the same Alt+key). Deterministic.

## 4. Tier 2 — suspected poor translation (later, precision-tuned)

Surface as **"suspected,"** never as "wrong," each with its evidence:

- **Back-translation drift** — translate the target back to English (a cheap model) and score similarity
  to the original `msgid`; low similarity ⇒ likely mistranslation. This approach has been prototyped
  previously; the score has a home in the schema (`translations.validation_reverse_translate_score` /
  `validation_reverse_translation`). Most reliable semantic flag — add this one first.
- **AI-vs-accepted divergence** — generate the suggestion (§Reuse) and flag large divergence from the
  accepted translation. **Noisy** (valid paraphrases diverge too, e.g. `Schaltflächen` vs `Knöpfe`) →
  soft signal only, shown with both texts side by side.
- **Fuzzy / stale** — existing `is_fuzzy`, or translation older than a change to its source string.

## 5. Backing data

Per `(string, language)` cell, a set of flags computed by a validation pass and surfaced in the queue:

- Fit: `translation_width_px`, `available_width_px`, `overflow_px`, `fits_in_control`, `single_line`,
  `width_constrained`, and group columns `group_available_width_px`, `group_rendered_width_px`,
  `group_overflow_px`, `fits_in_group` (semantics defined in §3a; canonical = hard `overflow>0` plus a
  90% soft margin). Control geometry comes from `mpc-hc.rc` (dialog-unit rects; `px = width_du ×
  avg_char_width / 4`), joined to strings via the `dialog_mapping` table (`string_id → dialog_id,
  control_id`). Strings with no dialog control (menus, stringtable messages) have no width concept →
  `available_width_px = NULL`, never fit-flagged.
- Placeholder / accelerator: computed on demand from `msgid` vs `msgstr`.
- Semantic: `validation_reverse_translate_score`, and the AI suggestion + its confidence.

The AI **fix suggestion** is stored/served separately from the accepted translation (see the
`ai_suggestions` design in `../data/README.md`) — the queue reads it, never overwrites the accepted
string until the human accepts.

## 6. UX

- Filterable/sortable list **per language**, grouped by flag type, deterministic flags on top.
- Each row shows: **English**, **current translation**, the **flag(s) + evidence** (e.g. "overflows by
  104px" with a rendered preview; "placeholder `%d` missing"; "back-translation: '…' ≠ original"), the
  **enrichment Meaning** (why this sense — reuse `CoreEnrichment`), and an inline **AI fix suggestion**
  the translator can **accept / edit / dismiss**.
- The fix suggestion for overflow cases is specifically prompted to be **shorter while preserving
  meaning** (see the v3 prompt in Reuse) and is itself re-checked by the fit measurement before being
  offered — never suggest a fix that also overflows.
- Accepting a fix updates the accepted translation with provenance `ai_accepted` and clears the flag.
- Corrections integrate with the existing PR flow (`corrections.cpp` / `research-corrections.jsonl`).

## 7. Reuse — what already exists in this codebase

- **`studio/ui/LivePreview.cpp` → `DetectOverflow(HWND)`** — accurate GDI fit/overflow measurement.
  This *is* the fit engine; wire it into the flag computation.
- **`studio/core/src/ai_client.cpp`** — the AI provider layer (GPT/GLM/OpenRouter/Claude) already used
  for on-demand suggestions with the user's key. Reuse for the fix suggestion. Recommended generation
  prompt is the **v3 prompt** proven in earlier research (enrichment-first, "MEANING is authoritative,
  prefer it over reference wordings," **be maximally concise**, match source punctuation, preserve
  placeholders/`&`) — see `MainFrame.cpp`'s `AiSystemPrompt()` for the current production wording.
- **`CoreEnrichment`** — the per-string Meaning/Function/UI-role research to show as review context.

## 8. Phasing

1. **Fit-review queue** (3a) + **placeholder/accelerator** (3b/3c) + inline **AI fix suggestion** — all
   deterministic or already-built; this is the shippable v1.
2. **Back-translation drift** (2a) as the first semantic flag, once v1 proves useful.
3. AI-vs-accepted divergence and fuzzy/stale as lower-priority soft flags.

## 9. Caveats / open items

- **Use `DetectOverflow` (GDI), not PIL-measured widths from earlier analysis — this is not optional.**
  That earlier analysis measured every language in **Segoe UI**, but Segoe UI lacks glyphs for many
  scripts (Gurmukhi, Bengali, Devanagari, Thai, …); its fallback width differs from the real render
  font, so it **mis-flags non-Latin overflows in both directions**. Proven on Punjabi: of 11
  Segoe-flagged overflows, **7 actually fit** when measured in **Nirmala UI** (Windows' real Gurmukhi
  font). GDI does correct per-script font fallback automatically; the PIL numbers for non-Latin
  scripts are not trustworthy. Latin/Cyrillic/Greek (Segoe-covered) are fine. If these numbers are
  ever recomputed outside the Studio, select the font per script (Nirmala UI for Indic, Leelawadee UI
  for Thai, etc.) — but prefer GDI.
- **RTL** (ar/he): `LivePreview.cpp` has a TODO for `WS_EX_LAYOUTRTL` + target-language font — accurate
  RTL overflow needs that.
- **Semantic flags are advisory.** Keep them clearly separated from deterministic bugs; tune thresholds
  for precision so the queue stays trustworthy.
