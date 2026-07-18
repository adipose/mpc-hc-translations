#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
AI-fill translation suggestions for the untranslated cells of near-complete languages.

For each language at least --threshold %% translated (default 85), find its empty (untranslated)
cells that DON'T already have an AI suggestion, translate each via the Anthropic API grounded in the
string's Meaning + translator-hint + fidelity-ranked sibling translations (the same context the
Studio's own suggestion path uses), QA the result (placeholder / mnemonic preservation), and store it
in enrichment/lang/<lang>.sqlite (method 'ai:<model>'). Idempotent: re-running only fills the few new
cells since last time -- the "few new per build" top-up. Skips en_GB (British English overrides only).

Backend: Claude Code CLI by default (your subscription -- NO api key); --backend api uses ANTHROPIC_API_KEY.
  python bundle-build/ai_fill.py --threshold 85                       # via Claude Code (no key)
  python bundle-build/ai_fill.py --backend api --regenerate --langs de fr
"""
import argparse, glob, os, re, sqlite3, sys, json, time
import polib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PO   = os.path.join(ROOT, "upstream/src/mpc-hc/mpcresources/PO")
REF  = ["de", "fr", "ru", "ja", "pl", "it", "es", "zh_CN"]   # sibling grounding (high-fidelity, diverse)
SKIP = {"en_GB"}                                              # British English = US-English overrides only

LANG_NAME = {
 "de":"German","fr":"French","ru":"Russian","ja":"Japanese","pl":"Polish","it":"Italian","es":"Spanish",
 "zh_CN":"Simplified Chinese","zh_TW":"Traditional Chinese","nl":"Dutch","tr":"Turkish","pt_PT":"European Portuguese",
 "pt_BR":"Brazilian Portuguese","bg":"Bulgarian","cs":"Czech","hu":"Hungarian","ko":"Korean","ro":"Romanian",
 "sk":"Slovak","el":"Greek","id":"Indonesian","vi":"Vietnamese","uk":"Ukrainian","ca":"Catalan","sl":"Slovenian",
 "bs_BA":"Bosnian (Latin script)","da":"Danish","ar":"Arabic","pa":"Punjabi (Gurmukhi script)","be":"Belarusian",
 "eu":"Basque","gl":"Galician","hr":"Croatian","ms_MY":"Malay","sv":"Swedish","fi":"Finnish","bn":"Bangla",
 "th_TH":"Thai","he":"Hebrew","sr":"Serbian","lt":"Lithuanian","hy":"Armenian","tt":"Tatar",
}

SYS = (
 "You localize UI strings for MPC-HC, a Windows media player, into {lang}. Respond with ONLY a JSON "
 'object {{"translation":"..."}} and nothing else -- no markdown fence, no commentary.\n'
 "RULES:\n"
 "(1) The 'Meaning' line, when present, is AUTHORITATIVE for sense; siblings are grounding for "
 "terminology/style only.\n"
 "(2) Be MAXIMALLY CONCISE -- the shortest natural {lang} a native speaker accepts in a cramped UI "
 "control. Do not add words the English omits.\n"
 "(3) Match the English source's punctuation style exactly (ASCII/half-width : , . ( ) % - and the "
 "same trailing colon/period); never use full-width CJK punctuation.\n"
 "(4) Preserve every printf specifier (%s %d %u %ld %.2f %% %1 ...) exactly and in the same order.\n"
 "(5) Preserve the '&' accelerator: exactly one '&' before a letter of the TRANSLATED text (it need "
 "not match the English letter); a literal '&&' stays '&&'. For CJK/Indic where the house convention "
 "appends a Latin key in parentheses (e.g. text(&F)), follow that convention.\n"
 "(6) Preserve leading/trailing whitespace, embedded tabs, and line breaks exactly.\n"
 "(7) Do not translate command-line switch tokens before a tab in \"/switch\\tdescription\".\n"
 "(8) Honor the hint: reproduce every keep_verbatim token exactly; apply agreement (gender/number); "
 "heed notes; match the siblings' terminology."
)

def places(s):
    return sorted(m.group(0) for m in re.finditer(r"%(?:%|[-+#0-9.]*(?:l?[sduxXf]|ld|lf|lu))", s or "") if m.group(0) != "%%")
def amp(s): return len(re.findall(r"(?<!&)&(?!&)", s or ""))

def load_po_map(lang):
    d = {}
    for f in glob.glob(f"{PO}/mpc-hc.{lang}.*.po"):
        try: po = polib.pofile(f)
        except Exception: continue
        for e in po:
            if e.obsolete: continue
            d[(e.msgctxt or "", e.msgid)] = e.msgstr
    return d

def completeness(d):
    tot = len(d); tr = sum(1 for v in d.values() if v and v.strip())
    return (100.0 * tr / tot) if tot else 0.0

def build_user(msgid, enr, hint, sibs):
    lines = [f"English: {msgid}"]
    if enr.get("semantic_purpose"): lines.append(f"Meaning: {enr['semantic_purpose']}")
    if enr.get("functional_purpose"): lines.append(f"Function: {enr['functional_purpose']}")
    if hint:
        h = {k: hint[k] for k in ("role", "keep_verbatim", "referent", "agreement", "notes") if hint.get(k)}
        if h: lines.append("Hint: " + json.dumps(h, ensure_ascii=False))
    if sibs:
        lines.append("Siblings:")
        for rl, v in sibs.items(): lines.append(f"  {rl}: {v}")
    return "\n".join(lines)

def parse_translation(text):
    m = re.search(r"\{.*\}", text, re.S)
    if not m: return None
    try: j = json.loads(m.group(0))
    except Exception: return None
    t = j.get("translation")
    return t if isinstance(t, str) else None

_API = {}
def _api_client():
    if "c" not in _API:
        import anthropic
        _API["c"] = anthropic.Anthropic()
    return _API["c"]

def call_model(backend, system, user, model):
    """Return the model's raw text reply.
    backend 'claude-code' -> the Claude Code CLI in headless print mode (your subscription, NO api key);
    backend 'api'         -> the Anthropic SDK (ANTHROPIC_API_KEY)."""
    if backend == "claude-code":
        import subprocess, tempfile
        p = subprocess.run(
            ["claude", "-p", user, "--append-system-prompt", system, "--model", model,
             "--allowedTools", "", "--output-format", "text"],
            capture_output=True, text=True, encoding="utf-8", cwd=tempfile.gettempdir(), timeout=180)
        if p.returncode != 0:
            raise RuntimeError(f"claude -p exit {p.returncode}: {(p.stderr or '')[:200]}")
        return p.stdout or ""
    m = _api_client().messages.create(model=model, max_tokens=1024, system=system,
                                      messages=[{"role": "user", "content": user}])
    return "".join(b.text for b in m.content if b.type == "text")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threshold", type=float, default=85.0)
    ap.add_argument("--model", default="claude-sonnet-5")
    ap.add_argument("--backend", choices=["claude-code", "api"], default="claude-code",
                    help="claude-code = Claude Code CLI (your subscription, no key); api = Anthropic API (ANTHROPIC_API_KEY)")
    ap.add_argument("--langs", nargs="*", help="restrict to these langs (else all >= threshold)")
    ap.add_argument("--regenerate", action="store_true", help="also redo cells that already have a suggestion")
    ap.add_argument("--limit", type=int, default=0, help="cap cells per language (testing)")
    a = ap.parse_args()
    if a.backend == "api":
        try:
            import anthropic  # noqa: F401
        except ImportError:
            sys.exit("pip install anthropic  (or use --backend claude-code)")
        if not os.environ.get("ANTHROPIC_API_KEY"):
            sys.exit("ANTHROPIC_API_KEY not set  (or use --backend claude-code)")
    else:
        import shutil
        if not shutil.which("claude"):
            sys.exit("claude CLI not found -- install Claude Code and log in, or use --backend api")

    ce = sqlite3.connect(os.path.join(ROOT, "dist/core-enrichment.sqlite")); ce.row_factory = sqlite3.Row
    enr = {(r["msgctxt"] or "", r["msgid"]): dict(r) for r in ce.execute(
        "select s.msgctxt,s.msgid,s.id,s.semantic_purpose,s.functional_purpose from strings s")}
    hints = {r["string_id"]: dict(r) for r in ce.execute(
        "select string_id,role,keep_verbatim,referent,agreement,notes from string_hints")}
    sid = {(r["msgctxt"] or "", r["msgid"]): r["id"] for r in ce.execute("select id,msgctxt,msgid from strings")}

    all_langs = sorted({re.match(r"mpc-hc\.(.+?)\.(dialogs|menus|strings)\.po$", os.path.basename(f)).group(1)
                        for f in glob.glob(f"{PO}/*.po") if not os.path.basename(f).startswith("mpc-hc.installer.")})
    pos = {l: load_po_map(l) for l in set(all_langs) | set(REF)}
    targets = a.langs or [l for l in all_langs if completeness(pos[l]) >= a.threshold and l not in SKIP]

    grand = 0
    for l in targets:
        if l in SKIP: print(f"  {l}: skipped (override-only language)"); continue
        pack = os.path.join(ROOT, f"enrichment/lang/{l}.sqlite")
        cx = sqlite3.connect(pack)
        cx.execute("CREATE TABLE IF NOT EXISTS ai_suggestions (string_id INTEGER, msgstr TEXT, method TEXT)")
        cx.execute("CREATE TABLE IF NOT EXISTS reference_matches (string_id INTEGER, source TEXT, msgstr TEXT)")
        have = {r[0] for r in cx.execute("select string_id from ai_suggestions")}
        empty = [(c, m) for (c, m), v in pos[l].items() if not (v and v.strip()) and (c, m) in sid]
        todo = [(c, m) for (c, m) in empty if a.regenerate or sid[(c, m)] not in have]
        if a.limit: todo = todo[:a.limit]
        if not todo: print(f"  {l}: up to date (0 new empty cells)"); cx.close(); continue
        name = LANG_NAME.get(l, l)
        sysmsg = SYS.format(lang=name)
        done = fail = 0
        for (c, m) in todo:
            i = sid[(c, m)]
            e = enr.get((c, m), {}); h = hints.get(i)
            sibs = {rl: pos[rl][(c, m)] for rl in REF if rl != l and pos.get(rl, {}).get((c, m))}
            usr = build_user(m, e, h, sibs)
            tr = None
            for attempt in range(3):
                try:
                    tr = parse_translation(call_model(a.backend, sysmsg, usr, a.model))
                    break
                except Exception as ex:
                    if attempt == 2: print(f"    ! {l} {m[:30]!r}: {ex}")
                    else: time.sleep(2 * (attempt + 1))
            if not tr or not tr.strip():
                fail += 1; continue
            if places(tr) != places(m) or (amp(m) <= 1 and amp(tr) > 1):   # QA: placeholder + mnemonic parity
                fail += 1; print(f"    QA-drop {l} {m[:30]!r} -> {tr!r}"); continue
            cx.execute("INSERT INTO ai_suggestions(string_id,msgstr,method) VALUES(?,?,?)",
                       (i, tr, f"ai:{a.model}"))
            done += 1
            if done % 25 == 0: cx.commit()
        cx.commit(); cx.close()
        grand += done
        print(f"  {l} ({name}): filled {done}, QA/failed {fail}, of {len(todo)} new empty cells")
    print(f"\nTOTAL new suggestions: {grand}")

if __name__ == "__main__":
    main()
