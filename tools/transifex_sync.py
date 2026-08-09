# SPDX-License-Identifier: GPL-3.0-or-later
"""Transifex -> upstream translation sync (successor to the fork's uptransifex.sh flow).

Merges the fork's `transifex` branch translations onto upstream's current POs, per entry:

  transifex has a translation:
      - placeholder check FAILS (printf specifier set/order differs from msgid)  -> DISCARD it,
        keep upstream's value (clsid2's request on PR #3964: such strings crash format calls)
      - upstream has a DIFFERENT non-empty translation                            -> transifex wins
        (translator work is authoritative; counted + listed in the report)
      - otherwise                                                                 -> take transifex
  transifex has NO translation for the key (empty msgstr, or key absent entirely):
      -> KEEP upstream's translation.  This is the protection the old `git merge -X ours` +
         file-replacement flow lacked: translations added directly upstream (e.g. auto-generated /
         AI-filled strings for a new feature) were silently wiped because Transifex had not seen
         them yet.

The merged output's entry set/order is upstream's (obsolete transifex-only keys drop out).

Modes:
  dry-run (default): writes merged POs + report to --out, touches no branch.
  --commit:          creates PR branch `<pr-branch>` off <upstream-ref> with the merged POs, and
                     updates the local `transifex` branch (merge <upstream-ref> -X ours, then the
                     same merged POs) -- both via a temporary worktree.  NOTHING is ever pushed.

Usage:
  python tools/transifex_sync.py                       # dry run, report + merged POs in ./transifex-sync-out
  python tools/transifex_sync.py --commit              # also create branches/commits (no push)
"""
import argparse
import datetime
import os
import re
import subprocess
import sys
from collections import Counter

import polib

# potool-conformant printf specifier extraction -- byte-for-byte the same policy as Studio's
# validate.cpp SPEC_RE: no space flag ("100% spyware" must not tokenize as "% s"); no bare
# %<digits> ("%25" is a percentage in tr/eu/...); only typed "%1!s!" is positional.
SPEC_RE = re.compile(
    r"%%|%\d+!\w+!|%[-+0#]*\d*(?:\.\d+)?(?:hh|ll|h|l|L|z|j|t|w)?[diouxXeEfFgGaAcsp@]")


def format_specs(s):
    return SPEC_RE.findall(s)


def placeholder_ok(msgid, msgstr):
    """Studio's rule_format: specifier multiset must match; non-positional order preserved."""
    a = format_specs(msgid)
    if not a:                      # source has no specifier -> its '%' is literal
        return True
    b = format_specs(msgstr)
    if Counter(a) != Counter(b):
        return False
    def nonpos(toks):
        return [t for t in toks if t != "%%" and not (len(t) >= 2 and t[1].isdigit())]
    return nonpos(a) == nonpos(b)


def git(repo, *args, binary=False):
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed:\n{r.stderr.decode('utf-8', 'replace')}")
    return r.stdout if binary else r.stdout.decode("utf-8", "replace")


def load_po_from_ref(repo, ref, path):
    data = git(repo, "show", f"{ref}:{path}", binary=True)
    tmp = os.path.join(os.environ.get("TEMP", "/tmp"), "_txsync_tmp.po")
    with open(tmp, "wb") as f:
        f.write(data)
    return polib.pofile(tmp, wrapwidth=79)


def list_po_files(repo, ref, po_dir):
    out = git(repo, "ls-tree", "--name-only", ref, po_dir + "/")
    pat = re.compile(r"mpc-hc\.[^.]+\.(dialogs|menus|strings)\.po$")
    return sorted(p for p in out.splitlines() if pat.search(p))


def merge_file(up_po, tx_po, stats, detail, fname):
    """Merge tx translations into up_po IN PLACE per the header rules."""
    tx_map = {(e.msgctxt or "", e.msgid): e.msgstr for e in tx_po} if tx_po is not None else {}
    for e in up_po:
        key = (e.msgctxt or "", e.msgid)
        up, tx = e.msgstr, tx_map.get(key, "")
        if tx:
            if not placeholder_ok(e.msgid, tx):
                stats["discarded_invalid"] += 1
                detail.append(f"DISCARD {fname} :: {key[0]}: bad placeholders in {tx!r}")
                continue                      # keep upstream's value
            if up and up != tx:
                stats["conflict_tx_won"] += 1
                detail.append(f"TX-WINS {fname} :: {key[0]}: {up!r} -> {tx!r}")
                e.msgstr = tx
            elif not up:
                stats["tx_new"] += 1
                e.msgstr = tx
            else:
                stats["unchanged"] += 1
        else:
            if up:
                stats["protected"] += 1       # upstream-only translation kept (the new safeguard)
                detail.append(f"PROTECT {fname} :: {key[0]}: kept upstream {up!r}")
            else:
                stats["untranslated"] += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=r"C:\dev\mpc-hc")
    ap.add_argument("--upstream-ref", default="upstream/develop")
    ap.add_argument("--tx-ref", default="origin/transifex",
                    help="ref holding the Transifex translations (fetched first)")
    ap.add_argument("--po-dir", default="src/mpc-hc/mpcresources/PO")
    ap.add_argument("--out", default="transifex-sync-out")
    ap.add_argument("--commit", action="store_true",
                    help="create the PR branch + update the local transifex branch (no push)")
    ap.add_argument("--pr-branch", default=None,
                    help="branch name for the upstream PR (default transifex-sync-YYYYMMDD)")
    args = ap.parse_args()

    today = datetime.date.today().strftime("%Y%m%d")
    pr_branch = args.pr_branch or f"transifex-sync-{today}"

    print(f"== fetch ==")
    git(args.repo, "fetch", "upstream")
    git(args.repo, "fetch", "origin", "transifex")

    files = list_po_files(args.repo, args.upstream_ref, args.po_dir)
    print(f"== merging {len(files)} PO files: {args.tx_ref} -> {args.upstream_ref} ==")

    os.makedirs(args.out, exist_ok=True)
    grand = Counter()
    detail = []
    per_lang = {}
    merged_paths = []

    for path in files:
        fname = os.path.basename(path)
        lang = fname.split(".")[1]
        up_po = load_po_from_ref(args.repo, args.upstream_ref, path)
        try:
            tx_po = load_po_from_ref(args.repo, args.tx_ref, path)
        except RuntimeError:
            tx_po = None                       # file doesn't exist on the transifex branch
        stats = Counter()
        merge_file(up_po, tx_po, stats, detail, fname)
        per_lang.setdefault(lang, Counter()).update(stats)
        grand.update(stats)
        out_path = os.path.join(args.out, fname)
        up_po.save(out_path)
        merged_paths.append((path, out_path))

    print(f"\n== totals ==")
    for k in ("tx_new", "protected", "conflict_tx_won", "discarded_invalid", "unchanged", "untranslated"):
        print(f"  {k:18} {grand[k]}")
    interesting = {l: c for l, c in per_lang.items()
                   if c["tx_new"] or c["protected"] or c["conflict_tx_won"] or c["discarded_invalid"]}
    print(f"\n== languages with changes ({len(interesting)}) ==")
    for l in sorted(interesting):
        c = interesting[l]
        print(f"  {l:8} new={c['tx_new']:<5} protected={c['protected']:<5} "
              f"tx-wins={c['conflict_tx_won']:<4} discarded={c['discarded_invalid']}")

    report = os.path.join(args.out, "report.txt")
    with open(report, "w", encoding="utf-8") as f:
        f.write("\n".join(detail))
    print(f"\nreport: {report}  ({len(detail)} lines)")

    if not args.commit:
        print("\nDRY RUN complete -- no branches touched. Re-run with --commit to create:")
        print(f"  - {pr_branch} (off {args.upstream_ref}, for the upstream PR)")
        print(f"  - updated local transifex branch (merge {args.upstream_ref} -X ours + merged POs)")
        return 0

    wt = os.path.join(os.environ.get("TEMP", "/tmp"), f"txsync-wt-{today}")
    def apply_merged_and_commit(msg):
        for repo_path, out_path in merged_paths:
            dst = os.path.join(wt, repo_path)
            with open(out_path, "rb") as s, open(dst, "wb") as d:
                d.write(s.read())
        git(wt, "add", args.po_dir)
        if subprocess.run(["git", "-C", wt, "diff", "--cached", "--quiet"]).returncode != 0:
            git(wt, "commit", "-m", msg)
            print(f"  committed: {msg}")
        else:
            print("  nothing to commit")

    print(f"\n== branch {pr_branch} (for the upstream PR) ==")
    git(args.repo, "worktree", "add", "-f", "-b", pr_branch, wt, args.upstream_ref)
    try:
        apply_merged_and_commit(f"Transifex updates {datetime.date.today().isoformat()}")
    finally:
        git(args.repo, "worktree", "remove", "-f", wt)
        # branch (with its commit) survives worktree removal

    print(f"\n== update local transifex branch ==")
    git(args.repo, "worktree", "add", "-f", wt, "transifex")
    try:
        git(wt, "merge", args.upstream_ref, "-X", "ours", "--no-edit")
        apply_merged_and_commit(f"Sync merged translations {datetime.date.today().isoformat()}")
    finally:
        git(args.repo, "worktree", "remove", "-f", wt)

    print("\nDone. Nothing was pushed -- review, then push the branches yourself.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
