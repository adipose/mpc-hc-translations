# enrichment/ — Studio enrichment data

The Studio's read-only context data (committed to this repo).

## lang/<code>.sqlite — per-language packs (committed, small: 6–30 KB)
Downloaded on "check out a language." Tables (keyed by `string_id`):
- `ai_suggestions` — `string_id, msgstr, method, confidence` (AI proposals; the editor shows as suggestion/default)
- `reference_matches` — filtered commonly-used-translation references for that language (~200–535 rows)

## Core enrichment — NOT committed here
The language-independent core (strings catalog + research + `dialog_mapping` + control-index,
~558 KB gz) is a **bundle-build output** shipped IN the Studio bundle (Release), not stored in this
dir. See `../bundle-build/`; the committed artifact lives at `../dist/core-enrichment.sqlite`.

> `msgid→string_id` is resolved so packs key on `string_id`.
