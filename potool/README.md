# potool/ — PO validation core

Single source of truth for MPC-HC translation validation rules. Used by:
1. the **mpc-hc CI PR gate** (workstream E wires it into a `clsid2/mpc-hc` Action), and
2. the **Studio** edit-panel + pre-submit pre-flight (v1 **C++ core lib mirrors these rules**).

`tests/test_potool.py` is the **conformance contract** the C++ core must reproduce 1:1.

## Rules
**HARD (error — blocks merge):**
- encoding: UTF-8, no BOM
- `msgfmt -c` well-formedness (syntax / dup keys / header), if gettext present
- **format-specifier parity** — multiset (+ order for non-positional printf), *enforced only when the
  source has a specifier* (a literal `%` in a label doesn't police translations)
- **structural integrity** (PR mode, `--base`): identical `(msgctxt,msgid)` key set;
  `POT-Creation-Date` / `Plural-Forms` header unchanged ⇒ only `msgstr` may change

**SOFT (warning/info — advisory, never blocks):**
- ampersand-mnemonic parity, newline parity (translators legitimately vary these per language)
- leading/trailing whitespace mismatch, length ratio > 2.0, empty msgstr (info)

## Usage
```bash
python3 potool/potool.py <file.po>              # standalone checks
python3 potool/potool.py <pr.po> --base <base.po>   # PR mode (+ structural integrity)
python3 potool/tests/test_potool.py             # conformance suite
```
Exit 1 on any error (or warning with `--fail-on-warning`).

## Why the rules look the way they do (validated against the real corpus)
Sweeping all **177** committed `.po` drove three false-positive fixes and two hard→soft
downgrades, so the gate now yields **0 errors on the entire committed corpus** yet still catches
genuine breakage (dropped/changed `%d`/`%s`):
- dropped the printf **space flag** (`"100% spyware"` → bogus `% s`)
- dropped bare **`%<digits>`** matching (tr/eu write percentages `%25`, `%100`)
- format parity **only when the source has a spec** (Malay `%s animasi`, Hungarian `%-a` on `%`-literal labels)
- **`\n` and `&` parity → warnings** (translators reflow lines / add-move accelerators legitimately)
