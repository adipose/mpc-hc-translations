#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Conformance suite for potool. Runnable with plain `python3` (no pytest).

This is the CONTRACT the v1 C++ validation core must reproduce 1:1. Each case pins a
(msgid, msgstr) or a (base, pr) file pair to the exact findings potool must emit.
"""
import os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import polib
import potool as P

FAILS = []

def expect(name, findings, want_rules):
    """want_rules: set of rule names expected among ERROR findings (exact match)."""
    got = {f["rule"] for f in findings if f["sev"] == "error"}
    if got != set(want_rules):
        FAILS.append(f"{name}: errors={sorted(got)} want={sorted(want_rules)}")

def expect_sev(name, findings, rule, sev):
    if not any(f["rule"] == rule and f["sev"] == sev for f in findings):
        FAILS.append(f"{name}: expected {sev}:{rule}, got {[(f['sev'],f['rule']) for f in findings]}")

# ---- per-entry rule units ------------------------------------------------
expect("fmt ok",        P.rule_format("c", "Chapter %d", "Kapitel %d"), [])
expect("fmt type",      P.rule_format("c", "Chapter %d", "Kapitel %s"), ["format-specifiers"])
expect("fmt count",     P.rule_format("c", "%s of %s", "%s"),           ["format-specifiers"])
expect("fmt order",     P.rule_format("c", "%s=%d", "%d=%s"),           ["format-order"])
expect("fmt positional",P.rule_format("c", "%1 and %2", "%2 and %1"),   [])   # reorder allowed
expect("fmt percent",   P.rule_format("c", "100%% done", "100%% fertig"), [])
# Windows env vars are literal text, not %a/%A specifiers (upstream #3138)
expect("fmt envvar case",  P.rule_format("c", "Logs in %appdata%\\MPC-HC", "Jurnale in %AppData%\\MPC-HC"), [])
expect("fmt envvar+spec",  P.rule_format("c", "%s in %APPDATA%", "%s in %appdata%"), [])
expect("fmt envvar drop",  P.rule_format("c", "%s in %APPDATA%", "in %appdata%"), ["format-specifiers"])
# a %<letters>% run that is really specifiers must NOT be masked (regression found by the test suite)
expect("fmt ld%% kept",    P.rule_format("c", "Quality: %ld%%", "Qualität: %ld %%"), [])
expect("fmt ld%% dropped", P.rule_format("c", "Quality: %ld%%", "Qualität: %%"), ["format-specifiers"])
expect("fmt dx%d kept",    P.rule_format("c", "%dx%d", "%dx%d"), [])
expect("fmt dx%d dropped", P.rule_format("c", "%dx%d", "%dx"), ["format-specifiers"])

expect("amp ok",        P.rule_ampersand("c", "&File", "&Datei"),       [])   # no error
expect("amp dropped",   P.rule_ampersand("c", "&File", "Datei"),        [])   # SOFT: warning, not error
expect_sev("amp dropped warn", P.rule_ampersand("c", "&File", "Datei"), "ampersand", "warning")
expect("amp literal",   P.rule_ampersand("c", "Save && exit", "Speichern && beenden"), [])

expect("nl ok",         P.rule_newline("c", "a\nb", "x\ny"),            [])
expect("nl lost",       P.rule_newline("c", "a\nb", "xy"),             [])   # SOFT: warning, not error
expect_sev("nl lost warn", P.rule_newline("c", "a\nb", "xy"), "newline", "warning")

expect_sev("empty info", P.rule_empty("c", "Hello", ""),               "empty", "info")
expect_sev("len warn",   P.rule_length("c", "a longer source string", "x" * 60), "length-ratio", "warning")
expect("len guard short", [f for f in P.rule_length("c", "OK", "x" * 20) if f["sev"] == "error"], [])

# ---- file-level integration ---------------------------------------------
def make_po(entries, meta=None):
    po = polib.POFile()
    po.metadata = meta or {
        "Project-Id-Version": "MPC-HC", "MIME-Version": "1.0",
        "Content-Type": "text/plain; charset=UTF-8", "Content-Transfer-Encoding": "8bit",
        "POT-Creation-Date": "2026-01-01 00:00+0000", "Language": "de",
        "Plural-Forms": "nplurals=2; plural=(n != 1);",
    }
    for ctx, mid, mstr in entries:
        po.append(polib.POEntry(msgctxt=ctx, msgid=mid, msgstr=mstr))
    fd, p = tempfile.mkstemp(suffix=".po"); os.close(fd); po.save(p)
    return p

BASE = [("IDD_X_CAPTION", "Open", "Öffnen"), ("ID_FILE", "&File", "&Datei")]

good = make_po(BASE)
expect("file good", P.check_po(good), [])

# structural: PR removes an entry
pr_removed = make_po([BASE[0]])
expect("struct removed", P.check_po(pr_removed, base_path=good), ["structure-removed"])

# structural: PR changes a msgid (⇒ old key removed + new key added)
pr_changed = make_po([("IDD_X_CAPTION", "Open", "Öffnen"), ("ID_FILE", "&Files", "&Dateien")])
expect("struct msgid", P.check_po(pr_changed, base_path=good), ["structure-added", "structure-removed"])

# structural: header changed
pr_hdr = make_po(BASE, meta={**polib.pofile(good).metadata, "Plural-Forms": "nplurals=3; plural=0;"})
expect("struct header", P.check_po(pr_hdr, base_path=good), ["header-changed"])

# BOM rejected
fd, bom = tempfile.mkstemp(suffix=".po"); os.close(fd)
with open(bom, "wb") as f:
    f.write(b"\xef\xbb\xbf" + open(good, "rb").read())
expect("bom", [f for f in P.check_encoding(bom)], ["bom"])

for p in (good, pr_removed, pr_changed, pr_hdr, bom):
    os.remove(p)

# ---- report --------------------------------------------------------------
if FAILS:
    print("FAIL:")
    for x in FAILS:
        print("  -", x)
    sys.exit(1)
print("all conformance cases passed")
