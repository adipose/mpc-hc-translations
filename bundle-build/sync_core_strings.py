#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Add any current upstream strings that core-enrichment.sqlite is missing (e.g. after the submodule
re-pins to a newer develop that added strings). Each new (msgctxt, msgid) is inserted into the
`strings` table with a fresh id and EMPTY semantic_purpose -- enough for ai_fill (English + siblings)
and the deterministic hints to cover it, and to store a suggestion (the pack keys by string_id).
The rich Meaning is filled in later when core-enrichment is regenerated from the private lab DB.

Source of the current string universe: the English .pot templates in the pinned submodule.
"""
import glob, os, sqlite3, sys
import polib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PO   = os.path.join(ROOT, "upstream/src/mpc-hc/mpcresources/PO")

def main():
    db = os.path.join(ROOT, "dist/core-enrichment.sqlite")
    con = sqlite3.connect(db)
    have = {(c or "", m) for c, m in con.execute("select msgctxt, msgid from strings")}
    nextid = (con.execute("select coalesce(max(id), 0) from strings").fetchone()[0]) + 1

    universe = {}
    # ONLY the three app resource templates -- NOT mpc-hc.installer.strings.pot (InnoSetup setup-wizard
    # strings, which are not part of the app UI the Studio translates).
    for res in ("strings", "dialogs", "menus"):
        pot = os.path.join(PO, f"mpc-hc.{res}.pot")
        if not os.path.exists(pot): continue
        try: p = polib.pofile(pot)
        except Exception: continue
        for e in p:
            if e.obsolete or not e.msgid: continue
            universe[(e.msgctxt or "", e.msgid)] = res

    added = 0
    for (ctx, mid), res in universe.items():
        if (ctx, mid) in have: continue
        con.execute("insert into strings(id, msgctxt, msgid, ui_location, semantic_purpose, "
                    "functional_purpose, category, ui_type, string_type, source_pot_file) "
                    "values (?,?,?,?,?,?,?,?,?,?)",
                    (nextid, ctx, mid, "", "", "", "", "", res, f"mpc-hc.{res}.pot"))
        nextid += 1; added += 1
    con.commit(); con.close()
    print(f"sync_core_strings: added {added} new strings (empty Meaning); core-enrichment now current with the .pot")
    return added

if __name__ == "__main__":
    sys.exit(0 if main() >= 0 else 1)
