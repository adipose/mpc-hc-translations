#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Record AI-suggestion coverage per language into enrichment/ai-coverage.json.

For each language it reports: translatable strings, human-translated count/%, empty (untranslated)
cells, how many of those empty cells have an AI suggestion in the pack, and the resulting AI-fill
coverage %. This is the running record of "what we've generated AI strings for" -- it makes gaps
visible and lets a later pass target only the languages/cells that still need suggestions (or whose
English changed upstream, so their suggestion is stale and should be regenerated).

Run after an AI-fill pass (and after an upstream re-pin, since completeness is read from the .po).
"""
import sqlite3, glob, os, re, json, io, time
import polib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PO   = os.path.join(ROOT, "upstream/src/mpc-hc/mpcresources/PO")

def main():
    ce = sqlite3.connect(os.path.join(ROOT, "dist/core-enrichment.sqlite"))
    sid = {(c or "", m): i for i, c, m in ce.execute("select id,msgctxt,msgid from strings")}

    langs = sorted({re.match(r"mpc-hc\.(.+?)\.(dialogs|menus|strings)\.po$", os.path.basename(f)).group(1)
                    for f in glob.glob(f"{PO}/*.po")
                    if not os.path.basename(f).startswith("mpc-hc.installer.")})

    out = {}
    tot_empty = tot_ai = 0
    for l in langs:
        total = trans = 0
        empty_ids = []
        for f in glob.glob(f"{PO}/mpc-hc.{l}.*.po"):
            for e in polib.pofile(f):
                if e.obsolete: continue
                total += 1
                if e.msgstr and e.msgstr.strip():
                    trans += 1
                else:
                    k = (e.msgctxt or "", e.msgid)
                    if k in sid: empty_ids.append(sid[k])
        # AI suggestions present in this language's pack
        ai_ids, method = set(), None
        p = os.path.join(ROOT, f"enrichment/lang/{l}.sqlite")
        if os.path.exists(p):
            c = sqlite3.connect(p)
            try:
                for said, meth in c.execute("select string_id, method from ai_suggestions"):
                    ai_ids.add(said); method = method or meth
            except sqlite3.OperationalError:
                pass
            c.close()
        empty = total - trans
        covered = sum(1 for i in empty_ids if i in ai_ids)
        tot_empty += empty; tot_ai += covered
        out[l] = {
            "total": total, "human_translated": trans,
            "human_pct": round(100.0 * trans / total, 1) if total else 0.0,
            "empty": empty, "ai_suggested": covered,
            "ai_coverage_pct": round(100.0 * covered / empty, 1) if empty else 100.0,
            "method": method,
        }
    report = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "summary": {
            "languages": len(langs),
            "languages_with_ai": sum(1 for v in out.values() if v["ai_suggested"] > 0),
            "empty_cells_total": tot_empty,
            "ai_suggested_total": tot_ai,
            "overall_gap_coverage_pct": round(100.0 * tot_ai / tot_empty, 1) if tot_empty else 100.0,
        },
        "languages": dict(sorted(out.items(), key=lambda kv: -kv[1]["ai_coverage_pct"])),
    }
    dst = os.path.join(ROOT, "enrichment/ai-coverage.json")
    with io.open(dst, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, ensure_ascii=False)
    s = report["summary"]
    print(f"wrote enrichment/ai-coverage.json")
    print(f"  {s['languages']} langs | {s['languages_with_ai']} with AI suggestions")
    print(f"  gap coverage: {s['ai_suggested_total']}/{s['empty_cells_total']} empty cells = {s['overall_gap_coverage_pct']}%")

if __name__ == "__main__":
    main()
