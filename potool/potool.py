#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""potool — PO validation core for MPC-HC translations.

Single source of truth for the rules, used by (a) the Studio edit-panel/pre-flight [v1 C++
mirrors this], and (b) the mpc-hc CI PR gate. The per-entry rule functions are intentionally
small and pure so the C++ core lib can reproduce them 1:1 (see tests/ conformance suite).

Rules
  HARD (error, blocks merge):
    - encoding: UTF-8, no BOM
    - msgfmt -c well-formedness (if gettext available)
    - format-specifier parity: multiset (+order for non-positional printf)
    - structural integrity (PR mode, --base): identical (msgctxt,msgid) key set;
      POT-Creation-Date / Plural-Forms header unchanged  (⇒ only msgstr may change)
  SOFT (warning/info, does not block) — verified against real upstream .po that these vary legitimately:
    - ampersand-mnemonic parity (translators add/move/drop accelerators per language)
    - newline parity (translators reflow multi-line message text)
    - leading/trailing whitespace mismatch
    - length ratio > 2.0
    - empty msgstr (info — partial translations are allowed)

Usage:  potool.py <file.po> [--base <base.po>] [--fail-on-warning]
Exit 1 if any error (or warning with --fail-on-warning).
"""
import argparse, os, re, shutil, subprocess, sys
from collections import Counter

# ---- finding model -------------------------------------------------------
def F(sev, rule, ctx, msg):  # severity, rule, msgctxt, message
    return {"sev": sev, "rule": rule, "ctx": ctx, "msg": msg}

# ---- format specifiers ---------------------------------------------------
# order matters: %% literal, then positional %1 / %1!s!, then standard printf
# Two deliberate exclusions (both verified against real upstream .po):
#  - NO space flag: it makes literal "100% spyware"/"100 % frei" tokenize as "% s"/"% f".
#  - NO bare %<digits>: tr/eu/hu/... write percentages as "%25","%100" (percent-first). MPC-HC
#    has one bare "%1" (FormatMessage) — not worth false-positiving every %NN percentage. Only the
#    typed FormatMessage form "%1!s!" is treated as positional.
SPEC_RE = re.compile(
    r"%%|%\d+!\w+!|%[-+0#]*\d*(?:\.\d+)?(?:hh|ll|h|l|L|z|j|t|w)?[diouxXeEfFgGaAcsp@]"
)
POSITIONAL = re.compile(r"^%\d")

def fmt_specs(s):
    return SPEC_RE.findall(s or "")

def rule_format(ctx, msgid, msgstr):
    if not msgstr:
        return []
    a = fmt_specs(msgid)
    if not a:
        # Source has NO specifier ⇒ its '%' is a literal percent ("% of the animation",
        # "Preview width (% of screen)"). Translations legitimately place '%' next to letters
        # (Malay "%s animasi", Hungarian "%-a"). Only enforce parity when the SOURCE has a spec
        # (the real risk: a source %d/%s dropped or changed). Verified across all 177 .po.
        return []
    b = fmt_specs(msgstr)
    if Counter(a) != Counter(b):
        return [F("error", "format-specifiers", ctx, f"msgid {a} vs msgstr {b}")]
    # same multiset — for non-positional printf, order must be preserved
    na = [t for t in a if not POSITIONAL.match(t) and t != "%%"]
    nb = [t for t in b if not POSITIONAL.match(t) and t != "%%"]
    if na != nb:
        return [F("error", "format-order", ctx, f"reordered non-positional specifiers {na} vs {nb}")]
    return []

# ---- ampersand mnemonics -------------------------------------------------
def mnemonics(s):
    return (s or "").replace("&&", "").count("&")

def rule_ampersand(ctx, msgid, msgstr):
    # WARNING not error: translators legitimately add/move/drop accelerator mnemonics per
    # language (verified against real upstream .po). Advisory for a reviewer, not a merge gate.
    if not msgstr:
        return []
    if mnemonics(msgid) != mnemonics(msgstr):
        return [F("warning", "ampersand", ctx,
                  f"mnemonic count {mnemonics(msgid)} (msgid) != {mnemonics(msgstr)} (msgstr)")]
    return []

# ---- newlines ------------------------------------------------------------
def rule_newline(ctx, msgid, msgstr):
    # WARNING not error: translators legitimately reflow multi-line message text (verified
    # against real upstream .po). Advisory, not a merge gate.
    if not msgstr:
        return []
    if msgid.count("\n") != msgstr.count("\n"):
        return [F("warning", "newline", ctx,
                  f"\\n count {msgid.count(chr(10))} != {msgstr.count(chr(10))}")]
    return []

# ---- soft rules ----------------------------------------------------------
def rule_whitespace(ctx, msgid, msgstr):
    if not msgstr:
        return []
    out = []
    lead = lambda s: len(s) - len(s.lstrip())
    trail = lambda s: len(s) - len(s.rstrip())
    if lead(msgid) != lead(msgstr):
        out.append(F("warning", "leading-space", ctx, "leading-whitespace mismatch"))
    if trail(msgid) != trail(msgstr):
        out.append(F("warning", "trailing-space", ctx, "trailing-whitespace mismatch"))
    return out

def rule_length(ctx, msgid, msgstr):
    if msgstr and len(msgid) >= 8 and len(msgstr) > 2 * len(msgid):
        return [F("warning", "length-ratio", ctx,
                  f"translation {len(msgstr)} > 2×{len(msgid)} source")]
    return []

def rule_empty(ctx, msgid, msgstr):
    if not msgstr:
        return [F("info", "empty", ctx, "untranslated (falls back to English)")]
    return []

ENTRY_RULES = [rule_format, rule_ampersand, rule_newline,
               rule_whitespace, rule_length, rule_empty]

# ---- file-level ----------------------------------------------------------
def check_encoding(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        return [F("error", "bom", "", "file has a UTF-8 BOM")]
    try:
        raw.decode("utf-8")
    except UnicodeDecodeError as e:
        return [F("error", "encoding", "", f"not valid UTF-8: {e}")]
    return []

def check_msgfmt(path):
    if not shutil.which("msgfmt"):
        return []
    r = subprocess.run(["msgfmt", "-c", "-o", os.devnull, path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return [F("error", "msgfmt", "", r.stderr.strip().splitlines()[-1] if r.stderr else "msgfmt -c failed")]
    return []

def check_structural(base, po):
    import polib
    b = {(e.msgctxt or "", e.msgid) for e in base if e.msgid}
    p = {(e.msgctxt or "", e.msgid) for e in po if e.msgid}
    out = []
    for k in sorted(p - b)[:20]:
        out.append(F("error", "structure-added", k[0], f"entry not in base: {k[1][:40]!r}"))
    for k in sorted(b - p)[:20]:
        out.append(F("error", "structure-removed", k[0], f"base entry removed: {k[1][:40]!r}"))
    for key in ("POT-Creation-Date", "Plural-Forms"):
        if base.metadata.get(key) != po.metadata.get(key):
            out.append(F("error", "header-changed", "", f"{key} must not change"))
    return out

def check_po(path, base_path=None):
    import polib
    findings = check_encoding(path)
    findings += check_msgfmt(path)
    try:
        po = polib.pofile(path)
    except Exception as e:
        return findings + [F("error", "parse", "", f"polib parse failed: {e}")]
    for e in po:
        if not e.msgid:      # header
            continue
        for rule in ENTRY_RULES:
            findings += rule(e.msgctxt or "", e.msgid, e.msgstr or "")
    if base_path:
        findings += check_structural(polib.pofile(base_path), po)
    return findings

# ---- CLI -----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("po")
    ap.add_argument("--base", help="base .po for structural integrity (PR mode)")
    ap.add_argument("--fail-on-warning", action="store_true")
    args = ap.parse_args()

    findings = check_po(args.po, args.base)
    order = {"error": 0, "warning": 1, "info": 2}
    for f in sorted(findings, key=lambda x: order.get(x["sev"], 9)):
        ctx = f" [{f['ctx']}]" if f["ctx"] else ""
        print(f"  {f['sev'].upper():7} {f['rule']}{ctx}: {f['msg']}")
    errors = sum(1 for f in findings if f["sev"] == "error")
    warns = sum(1 for f in findings if f["sev"] == "warning")
    print(f"{os.path.basename(args.po)}: {errors} error(s), {warns} warning(s)")
    sys.exit(1 if errors or (args.fail_on_warning and warns) else 0)


if __name__ == "__main__":
    main()
