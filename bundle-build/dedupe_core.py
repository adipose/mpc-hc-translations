#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
De-duplicate dist/core-enrichment.sqlite (and re-point the enrichment/lang/*.sqlite packs).

The lab-DB extraction left the same string in `strings` several times under different escaping
conventions (gettext `\\"`, RC-doubled `""`, JSON `\\\\"`, `\\n`/`\\t` vs real newline/tab). The Studio
resolves enrichment with an EXACT by_key(msgctxt, msgid) against the *unescaped* text the .po parser
(po.cpp unescape_po) / control-index produces, so only one escaping variant is ever reachable and the
rest are dead clutter that also make ai_fill translate the same string multiple times.

This collapses every (msgctxt, normalized-msgid) group to a single survivor whose msgid is set to the
exact lookup key the Studio uses (the polib-unescaped .pot form, or the control-index form) so it stays
reachable; merges the richest Meaning + hint into the survivor; re-points dialog_mapping and each pack's
ai_suggestions/reference_matches from the dropped ids to the survivor; and drops the InnoSetup installer
strings (msgctxt Messages_/CustomMessages_), which are not part of the app UI.

Dry-run by default (prints the plan + a reachability check). Pass --apply to write.
"""
import argparse, glob, json, os, sqlite3, sys
import polib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB   = os.path.join(ROOT, "dist/core-enrichment.sqlite")
PO   = os.path.join(ROOT, "upstream/src/mpc-hc/mpcresources/PO")
CI   = os.path.join(ROOT, "dist/control-index.json")
PACKS = sorted(glob.glob(os.path.join(ROOT, "enrichment/lang/*.sqlite")))

_UNESC = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}

def unescape(s):
    """Replicate po.cpp unescape_po: \\n \\t \\r \\" \\\\ ; unknown escape keeps the backslash."""
    o, i, n = [], 0, len(s or "")
    while i < n:
        c = s[i]
        if c == '\\' and i + 1 < n:
            o.append(_UNESC.get(s[i + 1], '\\' + s[i + 1])); i += 2
        else:
            o.append(c); i += 1
    return ''.join(o)

def normkey(ctx, msgid):
    s = (msgid or "").replace('""', '"').replace('\\n', '\n').replace('\\t', '\t') \
                     .replace('\\"', '"').replace('\\\\', '\\').strip()
    return (ctx or "", s)

def empty(v):
    return v is None or str(v).strip() == "" or str(v).strip() == "[]"

def hint_richness(h):
    # h = (string_id, role, form, keep_verbatim, soft_verbatim, referent, agreement, parallel_group, notes, source)
    return sum(0 if empty(v) else 1 for v in h[1:])

def load_lookup():
    exact, keys = {}, set()
    for res in ("strings", "dialogs", "menus"):
        p = os.path.join(PO, f"mpc-hc.{res}.pot")
        if os.path.exists(p):
            for e in polib.pofile(p):
                if e.msgid:
                    k = (e.msgctxt or "", e.msgid); keys.add(k); exact.setdefault(normkey(*k), k)
    ci = json.load(open(CI, encoding="utf-8"))
    for sect in ("dialogs", "menus"):
        for r in ci.get(sect, []):
            k = (r.get("msgctxt") or "", r.get("msgid") or ""); keys.add(k); exact.setdefault(normkey(*k), k)
    return keys, exact

def plan():
    keys, exact = load_lookup()
    con = sqlite3.connect(DB)
    strs = con.execute("select id,msgctxt,msgid,ui_location,semantic_purpose,functional_purpose,"
                       "category,ui_type,string_type,source_pot_file from strings").fetchall()
    hints = {h[0]: h for h in con.execute(
        "select string_id,role,form,keep_verbatim,soft_verbatim,referent,agreement,"
        "parallel_group,notes,source from string_hints")}
    con.close()

    installer_ids = {r[0] for r in strs if (r[1] or "").startswith(("Messages_", "CustomMessages_"))}

    # The Studio looks a string up under its LIVE key: dialogs/menus grid rows use the control-index
    # msgid (RC-doubled ""); the "All strings" grid + string-table use the .po unescaped form. Both can
    # be live for the same dialog string, so we bucket by live key -- NOT by normalized group -- keeping
    # one survivor per distinct live key and folding only the dead escaping variants into it.
    def bucket_key(ctx, msgid):
        k = (ctx or "", msgid or "")
        if k in keys: return k                       # already a live form
        nk = normkey(ctx, msgid)
        if nk in exact: return exact[nk]             # canonical live form of this string
        return (ctx or "", unescape(msgid or ""))    # synthetic (string absent from .pot/control-index)

    buckets = {}
    for r in strs:
        if r[0] in installer_ids: continue
        buckets.setdefault(bucket_key(r[1], r[2]), []).append(r)

    id_map, msgid_rewrite, field_updates, hint_moves = {}, {}, {}, {}
    MEANING_COLS = {3: "ui_location", 4: "semantic_purpose", 5: "functional_purpose",
                    6: "category", 7: "ui_type"}
    collapsed = 0
    for bkey, members in buckets.items():
        if len(members) == 1: continue
        collapsed += 1
        # survivor: prefer a member already stored in the exact live form, then richest Meaning, then id
        survivor = sorted(members, key=lambda m: (0 if (m[1], m[2]) == bkey else 1,
                                                  -len((m[4] or "").strip()), m[0]))[0]
        sid = survivor[0]
        if survivor[2] != bkey[1]:
            msgid_rewrite[sid] = bkey[1]
        # merge Meaning fields the survivor is missing from the richest sibling value
        for idx, col in MEANING_COLS.items():
            if empty(survivor[idx]):
                cand = [m[idx] for m in members if not empty(m[idx])]
                if cand:
                    field_updates.setdefault(sid, {})[col] = max(cand, key=len)
        # keep the richest hint of the bucket on the survivor
        rich = max(members, key=lambda m: hint_richness(hints.get(m[0], (m[0],) + (None,) * 9)))
        if rich[0] != sid and hints.get(rich[0]):
            hint_moves[sid] = hints[rich[0]]
        for m in members:
            if m[0] != sid: id_map[m[0]] = sid

    # reachability check on the resulting survivors
    survivors = {}
    for r in strs:
        if r[0] in installer_ids or r[0] in id_map: continue
        mid = msgid_rewrite.get(r[0], r[2])
        survivors[r[0]] = (r[1], mid)
    reachable = sum(1 for (c, m) in survivors.values() if (c, m) in keys)
    return dict(keys=keys, exact=exact, installer_ids=installer_ids, id_map=id_map,
                msgid_rewrite=msgid_rewrite, field_updates=field_updates, hint_moves=hint_moves,
                collapsed=collapsed, total=len(strs), survivors=survivors, reachable=reachable)

def report(p):
    print(f"  strings now:                 {p['total']}")
    print(f"  installer strings to drop:   {len(p['installer_ids'])}")
    print(f"  dup groups to collapse:      {p['collapsed']}")
    print(f"  rows dropped (merged away):  {len(p['id_map'])}")
    print(f"  survivor msgid rewrites:     {len(p['msgid_rewrite'])}")
    print(f"  survivor Meaning back-fills: {sum(len(v) for v in p['field_updates'].values())}")
    print(f"  hint rows moved to survivor: {len(p['hint_moves'])}")
    final = p['total'] - len(p['installer_ids']) - len(p['id_map'])
    print(f"  strings after:               {final}")
    print(f"  of {len(p['survivors'])} survivors, reachable via a real lookup key: {p['reachable']}")

def apply(p):
    id_map, installer = p['id_map'], p['installer_ids']
    con = sqlite3.connect(DB)
    for sid, mid in p['msgid_rewrite'].items():
        con.execute("update strings set msgid=? where id=?", (mid, sid))
    for sid, cols in p['field_updates'].items():
        for col, val in cols.items():
            con.execute(f"update strings set {col}=? where id=?", (val, sid))
    for sid, h in p['hint_moves'].items():
        con.execute("update string_hints set role=?,form=?,keep_verbatim=?,soft_verbatim=?,referent=?,"
                    "agreement=?,parallel_group=?,notes=?,source=? where string_id=?",
                    (h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], sid))
    for d, s in id_map.items():
        con.execute("update dialog_mapping set string_id=? where string_id=?", (s, d))
    drop = list(installer) + list(id_map.keys())
    q  = ",".join("?" * len(drop))
    qi = ",".join("?" * len(installer))
    con.execute(f"delete from strings       where id in ({q})", drop)
    con.execute(f"delete from string_hints  where string_id in ({q})", drop)
    con.execute(f"delete from dialog_mapping where string_id in ({qi})", list(installer))
    # collapse dialog_mapping duplicates created by repointing
    con.execute("delete from dialog_mapping where rowid not in "
                "(select min(rowid) from dialog_mapping group by string_id,dialog_id,control_id)")
    con.commit(); con.close()

    for pack in PACKS:
        pc = sqlite3.connect(pack)
        for tbl, keycols in (("ai_suggestions", "string_id,method"),
                             ("reference_matches", "string_id,source")):
            if not pc.execute("select 1 from sqlite_master where type='table' and name=?", (tbl,)).fetchone():
                continue
            if drop:
                if installer:
                    pc.execute(f"delete from {tbl} where string_id in ({qi})", list(installer))
                for d, s in id_map.items():
                    pc.execute(f"update {tbl} set string_id=? where string_id=?", (s, d))
            # keep the longest msgstr per key (dedupe rows created by repointing)
            pc.execute(f"delete from {tbl} where rowid not in "
                       f"(select rowid from (select rowid, row_number() over "
                       f"(partition by {keycols} order by length(msgstr) desc, rowid) rn from {tbl}) where rn=1)")
        pc.commit(); pc.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()
    p = plan()
    print("== core-enrichment dedup plan ==")
    report(p)
    if a.apply:
        apply(p)
        print("\n== applied. re-planning to confirm idempotence ==")
        report(plan())
    else:
        print("\n(dry run -- pass --apply to write)")

if __name__ == "__main__":
    sys.exit(main())
