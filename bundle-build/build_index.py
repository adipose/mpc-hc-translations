#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the control->entry index from the pinned upstream RC + resource.h.

Reuses MPC-HC's own `TranslationDataRC` extractor (from the submodule) so the index can
never diverge from upstream. Joins the symbolic (msgctxt, msgid) keys with resource.h so
the runtime index is keyed by NUMERIC ids -> the Studio matches a rendered control by
(dialogNumericID, controlNumericID, EnglishText) with no on-device symbol parsing.
Text disambiguates the many IDC_STATIC(-1) duplicates. Deterministic.

Output (a build artifact -> dist/, gitignored):
  {"schema":1, "upstream_sha":..., "dialogs":[{dialog,control,control_sym,msgctxt,msgid}...],
   "menus":[{command,sym,msgctxt,msgid}...]}
"""
import argparse, json, os, re, subprocess, sys

# Standard Windows / afxres control ids not in resource.h
STD_IDS = {
    "IDOK": 1, "IDCANCEL": 2, "IDABORT": 3, "IDRETRY": 4, "IDIGNORE": 5,
    "IDYES": 6, "IDNO": 7, "IDCLOSE": 8, "IDHELP": 9, "IDTRYAGAIN": 10,
    "IDCONTINUE": 11, "IDC_STATIC": -1,
    # ID_APPLY_NOW: afxres.h standard Apply command. MFC ships built-in translations for the
    # Apply buttons IT creates (property sheets, from mfc###<lang>.dll) — those never appear in
    # the RC and stay out of this index. The one RC-DEFINED use (IDD_FAVORGANIZE's PUSHBUTTON)
    # is template text, so it flows through the .po pipeline like any other control.
    "ID_APPLY_NOW": 0x3021,
}
DEFINE_RE = re.compile(r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|-?\d+)\b")


def parse_resource_h(path):
    m = dict(STD_IDS)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            mo = DEFINE_RE.match(line)
            if mo:
                sym, val = mo.group(1), mo.group(2)
                m[sym] = int(val, 16) if val.lower().startswith("0x") else int(val)
    return m


def split_dialog_ctx(msgctxt, dialog_syms):
    """IDD_X_<ctrl> -> (dialog_sym, control_sym). Longest IDD_ prefix wins."""
    best = None
    for i, ch in enumerate(msgctxt):
        if ch == "_":
            left = msgctxt[:i]
            if left in dialog_syms:
                best = (left, msgctxt[i + 1:])
    return best  # None if unresolved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rc", default="upstream/src/mpc-hc/mpc-hc.rc")
    ap.add_argument("--resource-h", default="upstream/src/mpc-hc/resource.h")
    ap.add_argument("--mpcres", default="upstream/src/mpc-hc/mpcresources")
    ap.add_argument("--out", default="dist/control-index.json")
    args = ap.parse_args()

    for p in (args.rc, args.resource_h, args.mpcres):
        if not os.path.exists(p):
            sys.exit(f"not found: {p}")

    resmap = parse_resource_h(args.resource_h)
    dialog_syms = {s for s in resmap if s.startswith("IDD_")}

    sys.path.insert(0, args.mpcres)
    from TranslationDataRC import TranslationDataRC  # upstream extractor
    td = TranslationDataRC()
    td.loadFromRC(args.rc)

    dialogs, menus = [], []
    unresolved_dialogs, missing_ctrl = 0, 0

    def rc_undouble(s):
        # RC stores an embedded double-quote as "" (e.g. ""Skip back/forward""). The app's
        # rc_text_normalize collapses that for render-matching, but the raw msgid is also used
        # verbatim as the Dialogs/Menus grid Row -> po->find / enrichment by_key do EXACT compares
        # against the gettext-unescaped .po msgid ("Skip back/forward"), so the "" form never matches
        # and the translation/Meaning goes missing. Emit the natural single-" form to match the .po.
        return s.replace('""', '"')

    for (msgctxt, msgid) in td.dialogs:
        msgid = rc_undouble(msgid)
        if msgctxt.endswith("_CAPTION"):
            dsym = msgctxt[: -len("_CAPTION")]
            if dsym not in dialog_syms:
                unresolved_dialogs += 1; continue
            dialogs.append({"dialog": resmap[dsym], "control": None,
                            "control_sym": "CAPTION", "msgctxt": msgctxt, "msgid": msgid})
            continue
        sp = split_dialog_ctx(msgctxt, dialog_syms)
        if not sp:
            unresolved_dialogs += 1; continue
        dsym, csym = sp
        cnum = resmap.get(csym)
        if cnum is None:
            cnum = -1; missing_ctrl += 1   # unknown symbol -> treat like IDC_STATIC, text disambiguates
        dialogs.append({"dialog": resmap[dsym], "control": cnum,
                        "control_sym": csym, "msgctxt": msgctxt, "msgid": msgid})

    for (sym, msgid) in td.menus:
        msgid = rc_undouble(msgid)
        if sym == "POPUP":
            menus.append({"command": None, "sym": "POPUP", "msgctxt": sym, "msgid": msgid})
        else:
            menus.append({"command": resmap.get(sym), "sym": sym, "msgctxt": sym, "msgid": msgid})

    # pinned upstream sha (best-effort)
    try:
        sha = subprocess.run(["git", "-C", os.path.dirname(args.rc) or ".", "rev-parse", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or None
    except Exception:
        sha = None

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "upstream_sha": sha, "dialogs": dialogs, "menus": menus},
                  f, ensure_ascii=False, indent=1, sort_keys=True)

    print(f"  dialog records: {len(dialogs)}  (unresolved dialog-id: {unresolved_dialogs}, unknown control-sym->-1: {missing_ctrl})")
    print(f"  menu records:   {len(menus)}")
    print(f"  resource.h symbols: {len(resmap)}  dialog ids: {len(dialog_syms)}")
    print(f"wrote {args.out}  ({os.path.getsize(args.out):,} bytes)")


if __name__ == "__main__":
    main()
