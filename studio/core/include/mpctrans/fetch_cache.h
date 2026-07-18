// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>

// Disk cache for the GitHub downloads (mpc-hc.rc, resource.h, every language's .po), keyed by the
// upstream HEAD commit SHA they were fetched at. Lets a relaunch or "Get latest" skip re-downloading
// ~135 files when nothing changed upstream (see MainFrame::StartPrefetch's dumb-check-first flow:
// github::head_sha + github::changed_paths decide what's dirty; this module just stores/loads bytes).
//
// Layout: %LOCALAPPDATA%\MPC-HC Translation Studio\fetch-cache\<repo-relative path>, plus a
// manifest.txt at the cache root holding the SHA the CURRENT contents correspond to.
//
// No MFC dependency (usable from the prefetch worker thread); the LOCALAPPDATA lookup is
// Windows-only, matching the rest of this Windows-only Studio app (non-Windows builds — the portable
// CMake test gates — get a no-op stub that always misses).

namespace mpctrans::fetch_cache {

// The SHA the on-disk cache currently corresponds to, or nullopt if there's no cache yet (or its
// manifest is missing/unreadable).
std::optional<std::string> load_manifest_sha();

// Store `bytes` under `repo_path` (e.g. "src/mpc-hc/mpc-hc.rc" or a PO_DIR path). Subdirectories are
// created as needed.
void store(const std::string& repo_path, const std::string& bytes);

// Load previously-stored bytes for `repo_path`, or nullopt on a cache miss.
std::optional<std::string> load(const std::string& repo_path);

// Pre-create the on-disk directory for `repo_dir` (a repo-relative directory, e.g. config::PO_DIR).
// Call once, before fanning concurrent store() calls out across worker threads that all write into
// the same directory, so the threads don't race creating it.
void ensure_dir(const std::string& repo_dir);

// Record that the cache now corresponds to `sha`. Call LAST, only after every store() the caller
// needed has succeeded — writes to a temp file and renames over manifest.txt so a crash mid-write
// can't leave the manifest pointing at a half-written cache.
void write_manifest_sha(const std::string& sha);

} // namespace mpctrans::fetch_cache
