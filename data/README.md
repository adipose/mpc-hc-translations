# data/ — build inputs and community corrections

This directory documents the maintainer-side build inputs behind the AI suggestion pipeline, and
holds the one artifact that *is* community-editable (`research-corrections.jsonl`).

Two of the pipeline's build inputs are **private, local files — not distributed with this repo**:

- **`reference-corpus.sqlite.gz`** — a curated grounding corpus of commonly-used
  Windows-application translations, used only to help ground AI-generated suggestions. Never
  shipped to translators, never committed here.
- **`lab-snapshot.sqlite`** — a curated export of private per-string research and translation
  data used to help generate AI suggestions. Not committed here.

None of this is required to build or use the Studio — the committed enrichment packs
(`enrichment/lang/`) and core enrichment database (`dist/core-enrichment.sqlite`) are the derived,
distributable outputs; they're what the app actually reads.

## `research-corrections.jsonl` — community fixes to the per-string research

Studio users who spot an error in a string's research write-up (Type / Category / Where / Meaning /
Function, shown under the rendering) can submit a correction as a PR against **this** project
(`studio/core/src/corrections.cpp` builds the PR via the same GitHub Git Data flow used for
translation PRs — see `github::open_pr`). Accepted PRs land here.

Format: **JSONL** (one JSON object per line, UTF-8, no trailing comma, no comments — plain JSONL
can't carry them). **Append-only**: never edit or delete a line; a correction is retracted or revised
by appending a newer line for the same key. For a given `(msgctxt, msgid, field)` key, the **last**
line in the file wins.

Each line:

```json
{"msgctxt": "IDS_ARS_CHANNELS", "msgid": "Channels", "field": "functional_purpose", "text": "corrected write-up text", "note": "why this is wrong (optional)", "author": "Jane Doe <jane@example.com>", "date": "2026-07-10T14:32:07Z"}
```

- `msgctxt`, `msgid` — identify the string exactly as it appears in the `strings` table (same key
  `CoreEnrichment::by_key` looks up).
- `field` — `"semantic_purpose"` (the "Meaning" line) or `"functional_purpose"` (the "Function"
  line). These are the only two correctable fields.
- `text` — the corrected value for `field`.
- `note` — optional free-text explanation.
- `author`, `date` — attribution; `date` is ISO 8601 UTC.

This overlay is applied **last** in the committed `dist/core-enrichment.sqlite`, after the
union+alias `strings` rows: every row sharing the correction's `msgctxt` whose `msgid` is either
identical or normalizes to the same text (the escaping aliases) gets `field` updated, so a fix
reaches every alias row too. Unknown `(msgctxt, msgid)` pairs are skipped with a warning — the file
is never rejected outright.

## `translations` provenance model

`translations` is **authoritative-only**: one row per `(string_id, language_id)` representing the
shipped/accepted text. Authorship and authority are tracked separately (a human can accept an AI
suggestion verbatim, which is both AI-authored and human-authoritative):

- **`provenance`** (`'human' | 'ai_accepted' | 'imported'`), plus `accepted_by` / `accepted_at`,
  written when a translator accepts an `ai_suggestions` row (which then clears that cell's
  suggestions per the suppression rule below).
- **`ai_suggestions` table**: advisory, multiple rows allowed per `(string_id, language_id)` cell
  (e.g. one per model). Columns: `suggestion`, `model`, `prompt_version`, `confidence`, `agreement`,
  `fits_px`, `alternatives` (JSON), `back_translation_score`, `created_at`. Indexed on
  `(string_id, language_id)`.
- **Suppression rule** (enforced when the committed per-language packs in `enrichment/lang/` are
  built): an AI suggestion is emitted for a cell only if either (a) no authoritative `translations`
  row with non-empty `msgstr` exists for that cell, or (b) the authoritative row fails validation
  (`fits_in_control = 0`, or a placeholder/accelerator-parity failure). `NULL` validation columns
  are treated as "passes" until measured. An accepted translation that passes validation always
  suppresses/hides any AI suggestion for that cell.

## Enrichment recency signal

- **`language_fidelity`**: per main-PO language (installer POs excluded as noise), % of current
  translations set since 2024-01-01 — the regeneration-priority / sibling-grounding fidelity signal
  used by the suggestion pipeline, exposed to the Studio via `CoreEnrichment::language_fidelity()`.

## Glossary — evaluated, parked

A per-language domain-term glossary was prototyped and evaluated as an input to the AI suggestion
prompt. It was **parked before publishing**:

- **Low value as a translator reference** — competent per-language translators already know their
  conventions; the existing `.po` is their real reference.
- **Marginal for the AI prompt** — the terms it contained are ones any capable model already
  translates correctly and consistently; the terms that would actually matter (house-style choices,
  or domain terms absent from any grounding source) weren't reliably captured by the mining method.

There is **no glossary reader in the Studio**, it is **not exported to the bundle**, and the AI
prompt always emits an empty glossary. If a *specific* recurring house-style term ever needs
pinning, a handful of hand-curated override entries is a simpler answer than reviving a mined
termbase.
