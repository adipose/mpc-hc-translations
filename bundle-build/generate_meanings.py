#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Generate the "Meaning" (semantic_purpose) for strings in core-enrichment.sqlite that don't have one
yet -- typically the new upstream strings just added by sync_core_strings.py. Runs via the Claude Code
CLI by default (your subscription, NO api key; --backend api uses ANTHROPIC_API_KEY).

For each empty-Meaning string it asks the model to describe the in-app sense from the English text, the
resource id (IDD_=dialog / IDC_=control / IDS_=string / ID_=command), and nearby strings in the same
dialog/family, then writes semantic_purpose + functional_purpose + ui_type back to the row. Once filled,
generate_hints --llm and ai_fill get the authoritative Meaning to work from -- so the build no longer
depends on the private lab DB for new strings (the lab DB just produces richer Meanings when re-run).
"""
import argparse, json, os, re, sqlite3, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SYS = (
 "You describe the in-app MEANING of a short MPC-HC (Windows media player) UI string, to help "
 "translators pick the right sense. Respond with ONLY a JSON object "
 '{"semantic_purpose":"<1-2 sentences: what this control/string is and does, and where it appears>",'
 '"functional_purpose":"<short: the concrete action or value>",'
 '"ui_type":"<label|button|menu|checkbox|radio|option|title|message|status|format-name|unit>"}. '
 "Infer from the resource id prefix (IDD_=dialog, IDC_=dialog control, IDS_=string-table entry, "
 "ID_=menu/command) and the nearby strings. Be specific and accurate; do NOT invent features that "
 "don't exist. No prose outside the JSON."
)

_API = {}
def _api_client():
    if "c" not in _API:
        import anthropic
        _API["c"] = anthropic.Anthropic()
    return _API["c"]

def _call(backend, system, user, model):
    if backend == "claude-code":
        import subprocess, tempfile
        p = subprocess.run(
            ["claude", "-p", user, "--append-system-prompt", system, "--model", model,
             "--allowedTools", "", "--output-format", "text"],
            capture_output=True, text=True, encoding="utf-8", cwd=tempfile.gettempdir(), timeout=180)
        if p.returncode != 0:
            raise RuntimeError(f"claude -p exit {p.returncode}: {(p.stderr or '')[:200]}")
        return p.stdout or ""
    m = _api_client().messages.create(model=model, max_tokens=400, system=system,
                                      messages=[{"role": "user", "content": user}])
    return "".join(b.text for b in m.content if b.type == "text")

def parse_obj(text):
    m = re.search(r"\{.*\}", text, re.S)
    if not m: return None
    try: return json.loads(m.group(0))
    except Exception: return None

def siblings(con, sid, ctx):
    """A few nearby strings (English + Meaning) for context: same dialog if mapped, else the msgctxt
    prefix family (up to the last underscore)."""
    rows = con.execute(
        "select s.msgid, s.semantic_purpose from dialog_mapping dm "
        "join dialog_mapping d2 on d2.dialog_id=dm.dialog_id "
        "join strings s on s.id=d2.string_id "
        "where dm.string_id=? and s.id!=? and s.msgid!='' limit 6", (sid, sid)).fetchall()
    if not rows and ctx:
        pref = ctx.rsplit("_", 1)[0]
        rows = con.execute("select msgid, semantic_purpose from strings "
                           "where msgctxt like ? and id!=? and msgid!='' limit 6",
                           (pref + "%", sid)).fetchall()
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=os.path.join(ROOT, "dist/core-enrichment.sqlite"))
    ap.add_argument("--model", default="claude-sonnet-5")
    ap.add_argument("--backend", choices=["claude-code", "api"], default="claude-code")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()
    if a.backend == "claude-code":
        import shutil
        if not shutil.which("claude"): sys.exit("claude CLI not found (or use --backend api)")
    elif not os.environ.get("ANTHROPIC_API_KEY"):
        sys.exit("ANTHROPIC_API_KEY not set (or use --backend claude-code)")

    con = sqlite3.connect(a.db)
    todo = con.execute("select id, msgctxt, msgid from strings "
                       "where (semantic_purpose is null or trim(semantic_purpose)='') and msgid!=''").fetchall()
    if a.limit: todo = todo[:a.limit]
    if not todo:
        print("generate_meanings: no empty-Meaning strings -- up to date."); return 0
    done = fail = 0
    for sid, ctx, mid in todo:
        sib = siblings(con, sid, ctx or "")
        lines = [f"String: {mid!r}", f"Resource id: {ctx or '(none)'}"]
        if sib:
            lines.append("Nearby strings (English -> meaning):")
            for smid, ssem in sib:
                lines.append(f"  {smid!r}" + (f" -> {ssem}" if ssem else ""))
        user = "\n".join(lines)
        obj = None
        for attempt in range(3):
            try: obj = parse_obj(_call(a.backend, SYS, user, a.model)); break
            except Exception as e:
                if attempt == 2: print(f"  ! {ctx} {mid[:30]!r}: {e}")
                else: time.sleep(2 * (attempt + 1))
        if not obj or not obj.get("semantic_purpose"):
            fail += 1; continue
        con.execute("update strings set semantic_purpose=?, functional_purpose=?, ui_type=? where id=?",
                    (obj.get("semantic_purpose", ""), obj.get("functional_purpose", ""),
                     obj.get("ui_type", ""), sid))
        done += 1
        if done % 20 == 0: con.commit()
    con.commit(); con.close()
    print(f"generate_meanings: filled {done} Meanings, {fail} failed, of {len(todo)} empty-Meaning strings")
    return 0

if __name__ == "__main__":
    sys.exit(main())
