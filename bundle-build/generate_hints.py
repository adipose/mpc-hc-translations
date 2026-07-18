#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Generate the translator "hint layer" for core-enrichment.sqlite.

Two halves (see the analysis in memory/semantic-hint-layer.md):
  * DETERMINISTIC (no model): keep_verbatim tokens (placeholders, numeric/ratio tokens),
    soft_verbatim acronyms, and numbered-sibling parallel groups. Computed from the source
    string alone, so it is exact and free.
  * LLM (--llm): role, grammatical-agreement/referent, and a short translator note. Runs via the
    Claude Code CLI by default (your subscription -- NO api key); --backend api uses ANTHROPIC_API_KEY.

Hints are written to a `string_hints` table keyed by strings.id. Both halves are idempotent.
Use --only-missing for the incremental build step (fills hints for NEW strings only).

Examples:
  python bundle-build/generate_hints.py --db dist/core-enrichment.sqlite            # deterministic, all
  python bundle-build/generate_hints.py --db dist/core-enrichment.sqlite --llm      # + LLM, all
  python bundle-build/generate_hints.py --db dist/core-enrichment.sqlite --llm --only-missing
"""
import argparse, json, os, re, sqlite3, sys, time

# ---- deterministic placeholder tokenizer (potool rule: NO space flag, so "100% spyware"
#      does NOT parse as "% s"; %% is an escaped literal, not a specifier) ----
_PH = re.compile(r"%(?:%|[-+#0-9.]*(?:l[sduxXf]|ld|lf|lu|[sduxXf]))")
_NUMRANGE = re.compile(r"\b\d+(?:\.\d+)?\s*(?:[-:xX]\s*\d+(?:\.\d+)?)+\b")   # 16 - 235, 5:4, 1920x1080
_NUM = re.compile(r"(?<![%\w])\d+(?:\.\d+)?%?\b")                            # 50%, 4, 2.0
_ACRO = re.compile(r"\b[A-Z][A-Z0-9][A-Z0-9\-]{0,5}\b")                     # DVD, VSync? no (mixed) -> ALLCAPS only
_PLUSFMT = re.compile(r"%\w\+|\bTitle\b")  # unused; placeholder for future shapes

def placeholders(s):
    return [m.group(0) for m in _PH.finditer(s) if m.group(0) != "%%"]

def mandatory_verbatim(msgid):
    """Tokens a translation MUST reproduce exactly: printf specifiers, numeric/ratio tokens,
    and the '+' rating shape."""
    toks = []
    toks += placeholders(msgid)
    for m in _NUMRANGE.finditer(msgid):
        toks.append(re.sub(r"\s+", "", m.group(0)))
    for m in _NUM.finditer(msgid):
        toks.append(m.group(0))
    if re.search(r"%d\+", msgid):
        toks.append("%d+")
    # dedupe, keep order
    seen=set(); out=[]
    for t in toks:
        if t not in seen: seen.add(t); out.append(t)
    return out

def soft_verbatim(msgid):
    """Acronyms/format tokens that should survive but MAY be expanded to a valid localized form
    (D3D->Direct3D, VSync stays or 'vertical sync'). Advisory, not mandatory."""
    out=[]
    for m in _ACRO.finditer(msgid):
        tok=m.group(0)
        if tok not in ("OK","I","A","AB") and len(tok)>=2:
            out.append(tok)
    return out

def compute_parallel_groups(rows):
    """rows: list of (id, msgid). Returns {id: group_key} for numbered siblings (differ only by digits)."""
    fam={}
    for sid, msgid in rows:
        key=re.sub(r"\d+", "#", msgid)
        if key!=msgid:
            fam.setdefault(key, []).append(sid)
    out={}
    for key, ids in fam.items():
        if len(ids)>=2:
            for sid in ids: out[sid]=key
    return out

SCHEMA = """
CREATE TABLE IF NOT EXISTS string_hints (
  string_id      INTEGER PRIMARY KEY,
  role           TEXT,           -- command|label|option|title|status|format-name|unit|value
  form           TEXT,           -- imperative|noun|noun-phrase|adjective|numeric-token|proper-noun
  keep_verbatim  TEXT,           -- JSON array: MUST reproduce exactly (placeholders, numerics)
  soft_verbatim  TEXT,           -- JSON array: acronyms/format tokens (keep or expand)
  referent       TEXT,           -- what the string modifies / what each placeholder denotes
  agreement      TEXT,           -- gender/number agreement note, or 'n/a'
  parallel_group TEXT,           -- numbered-sibling family key, or NULL
  notes          TEXT,           -- short translator note
  source         TEXT            -- 'deterministic' | 'llm:<model>' | 'app:<model>'
);
"""

def run_deterministic(con, only_missing):
    con.executescript(SCHEMA)
    rows=[(r[0], r[1]) for r in con.execute("select id, msgid from strings")]
    pgroups=compute_parallel_groups(rows)
    have={r[0] for r in con.execute("select string_id from string_hints")}
    n=0
    for sid, msgid in rows:
        if only_missing and sid in have:
            continue
        mv=mandatory_verbatim(msgid); sv=soft_verbatim(msgid)
        pg=pgroups.get(sid)
        # upsert deterministic fields (preserve any existing LLM fields)
        con.execute("""
          INSERT INTO string_hints(string_id, keep_verbatim, soft_verbatim, parallel_group, source)
          VALUES(?,?,?,?, 'deterministic')
          ON CONFLICT(string_id) DO UPDATE SET
            keep_verbatim=excluded.keep_verbatim,
            soft_verbatim=excluded.soft_verbatim,
            parallel_group=excluded.parallel_group
        """, (sid, json.dumps(mv), json.dumps(sv), pg))
        n+=1
    con.commit()
    return n

# ---------------- LLM half ----------------
LLM_SYSTEM = (
 "You add a compact TRANSLATOR HINT for a short Windows-media-player UI string, to guide "
 "localization. Respond with ONLY a JSON object: "
 '{"role":"command|label|option|title|status|format-name|unit|value",'
 '"form":"imperative|noun|noun-phrase|adjective|numeric-token|proper-noun",'
 '"referent":"<the noun this modifies, or what each placeholder denotes; \'\' if none>",'
 '"agreement":"<gender/number agreement guidance for gendered languages, or n/a>",'
 '"notes":"<=140 chars, only non-obvious guidance (keep-untranslated, command-not-label, etc.)"}. '
 "Base it on the English string and the provided Meaning; be terse; no prose outside the JSON."
)

_API={}
def _api_client():
    if "c" not in _API:
        import anthropic
        _API["c"]=anthropic.Anthropic()
    return _API["c"]

def _call(backend, system, user, model):
    if backend=="claude-code":   # Claude Code CLI, headless -- your subscription, NO api key
        import subprocess, tempfile
        p=subprocess.run(["claude","-p",user,"--append-system-prompt",system,"--model",model,
                          "--allowedTools","","--output-format","text"],
                         capture_output=True, text=True, encoding="utf-8", cwd=tempfile.gettempdir(), timeout=180)
        if p.returncode!=0: raise RuntimeError(f"claude -p exit {p.returncode}: {(p.stderr or '')[:200]}")
        return p.stdout or ""
    m=_api_client().messages.create(model=model, max_tokens=300, system=system,
                                    messages=[{"role":"user","content":user}])
    return "".join(b.text for b in m.content if b.type=="text")

def llm_hint(backend, model, msgid, semantic, functional, ui_location):
    user=(f"String: {msgid!r}\nMeaning: {semantic}\n"
          f"Function: {functional}\nLocation: {ui_location}")
    text=_call(backend, LLM_SYSTEM, user, model)
    m=re.search(r"\{.*\}", text, re.S)
    return json.loads(m.group(0)) if m else {}

def run_llm(con, model, only_missing, limit, backend):
    if backend=="api":
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
    con.executescript(SCHEMA)
    q=("select s.id,s.msgid,s.semantic_purpose,s.functional_purpose,s.ui_location "
       "from strings s left join string_hints h on h.string_id=s.id "
       "where s.semantic_purpose is not null and length(trim(s.semantic_purpose))>0")
    if only_missing:
        q+=" and (h.role is null)"
    todo=list(con.execute(q))
    if limit: todo=todo[:limit]
    done=0
    for sid,msgid,sem,fun,loc in todo:
        for attempt in range(3):
            try:
                h=llm_hint(backend, model, msgid, sem or "", fun or "", loc or "")
                break
            except Exception as e:
                if attempt==2: print(f"  skip {sid} {msgid!r}: {e}"); h=None
                else: time.sleep(2*(attempt+1))
        if not h: continue
        con.execute("""
          INSERT INTO string_hints(string_id, role, form, referent, agreement, notes, source)
          VALUES(?,?,?,?,?,?,?)
          ON CONFLICT(string_id) DO UPDATE SET
            role=excluded.role, form=excluded.form, referent=excluded.referent,
            agreement=excluded.agreement, notes=excluded.notes, source=excluded.source
        """, (sid, h.get("role"), h.get("form"), h.get("referent"),
              h.get("agreement"), h.get("notes"), f"llm:{model}"))
        done+=1
        if done % 25 == 0:
            con.commit(); print(f"  ...{done}/{len(todo)}")
    con.commit()
    return done

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--db", default="dist/core-enrichment.sqlite")
    ap.add_argument("--llm", action="store_true", help="also run the LLM half")
    ap.add_argument("--model", default="claude-haiku-4-5-20251001")
    ap.add_argument("--only-missing", action="store_true", help="build-step mode: only fill NEW strings")
    ap.add_argument("--limit", type=int, default=0, help="cap LLM calls (testing)")
    ap.add_argument("--backend", choices=["claude-code","api"], default="claude-code", help="claude-code = Claude Code CLI (subscription, no key); api = Anthropic API")
    a=ap.parse_args()
    con=sqlite3.connect(a.db)
    n=run_deterministic(con, a.only_missing)
    print(f"deterministic hints written: {n}")
    if a.llm:
        d=run_llm(con, a.model, a.only_missing, a.limit or 0, a.backend)
        print(f"LLM hints written: {d}  (model={a.model})")
    con.close()

if __name__=="__main__":
    main()
