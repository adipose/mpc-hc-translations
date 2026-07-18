// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <string>
#include <vector>

// In-app "data refresh": update the committed SQLite/JSON data artifacts (dist/core-enrichment.sqlite,
// dist/control-index.json, enrichment/lang/<code>.sqlite) from the STUDIO_* repo's committed
// dist/data-manifest.json (see bundle-build/make_manifest.py), WITHOUT re-downloading the whole app.
// Portable core (no MFC) — Windows-only implementation (mpctrans::github's WinHTTP client + Windows
// CNG for sha256), same #ifdef _WIN32 split as github.cpp/ai_client.cpp so non-Windows gate builds
// still link.

namespace mpctrans::data_update {

// One manifest entry that differs locally.
struct RemoteFile { std::string repo_path, sha256; long long size = 0; };

struct UpdatePlan {
    std::string remote_version;          // manifest's "version" (a git short SHA, or a timestamp fallback)
    std::string local_version;           // the version last successfully applied, "" if never applied/unknown
    std::vector<RemoteFile> changed;      // files whose local sha256 differs (or the local file is missing)
};

// Fetch dist/data-manifest.json from STUDIO_OWNER/STUDIO_REPO@STUDIO_BRANCH (a raw.githubusercontent.com
// CDN fetch — no token needed, same as github::fetch_latest's other callers) and diff it against the
// local files. `local_path_for` maps a manifest repo_path ("dist/core-enrichment.sqlite",
// "enrichment/lang/fr.sqlite", ...) to the actual on-disk path the app reads (caller derives this from
// the resolved Bundle); an empty return means "this Studio build doesn't know where that artifact lives"
// and the entry is skipped. Throws std::runtime_error if the manifest itself can't be fetched or parsed.
UpdatePlan check(const std::function<std::string(const std::string& repo_path)>& local_path_for);

struct ApplyResult { int updated = 0, failed = 0; std::vector<std::string> errors; };

// Downloads each `plan.changed` file over a single keep-alive session, verifies its sha256 against the
// manifest, and only then atomically replaces the local file at local_path_for(repo_path). NEVER
// overwrites a good local file with unverified bytes: download+verify to a temp file in the SAME
// directory first — on a sha256 mismatch, a download failure, or a write failure, that ONE file is
// recorded in `errors` and the existing local file (if any) is left untouched; the rest of the plan
// still proceeds. `progress(repo_path, i, n)` fires just before file i (1-based) of n starts.
ApplyResult apply(const UpdatePlan& plan,
                  const std::function<std::string(const std::string& repo_path)>& local_path_for,
                  const std::function<void(const std::string& repo_path, int i, int n)>& progress);

} // namespace mpctrans::data_update
