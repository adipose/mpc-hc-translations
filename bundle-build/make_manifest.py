#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Emit dist/data-manifest.json — the data-version manifest the Studio uses to refresh only the
CHANGED data artifacts (core-enrichment.sqlite, control-index.json, per-language packs)
without re-downloading the whole app.

Each entry: repo-relative path -> {sha256, size}. `version` is the git short SHA (or a
timestamp fallback) so the app can show "data up to date / update available" cheaply.
The app fetches this manifest via raw.githubusercontent.com, compares sha256 per file,
and pulls only the ones that differ.
"""
import argparse, hashlib, json, glob, os, subprocess, sys, io, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARTIFACTS = ["dist/core-enrichment.sqlite", "dist/control-index.json"] + sorted(
    os.path.relpath(p, ROOT).replace("\\", "/") for p in glob.glob(os.path.join(ROOT, "enrichment/lang/*.sqlite")))

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def version():
    try:
        return subprocess.check_output(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                                       text=True).strip()
    except Exception:
        return time.strftime("%Y%m%d%H%M%S")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=None,
                    help="build version to stamp (e.g. 0.21); defaults to the git short SHA")
    a = ap.parse_args()
    files = {}
    for rel in ARTIFACTS:
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            files[rel] = {"sha256": sha256(p), "size": os.path.getsize(p)}
    manifest = {"schema": 1, "version": a.version or version(),
                "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "files": files}
    out = os.path.join(ROOT, "dist/data-manifest.json")
    with io.open(out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print(f"wrote {out}: {len(files)} artifacts, version {manifest['version']}")

if __name__ == "__main__":
    main()
