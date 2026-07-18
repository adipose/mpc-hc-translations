// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/fetch_cache.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>     // SHGetKnownFolderPath / FOLDERID_LocalAppData
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")   // CoTaskMemFree

namespace mpctrans::fetch_cache {

namespace fs = std::filesystem;

namespace {

fs::path cache_root() {
    static fs::path root = [] {
        fs::path r;
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
            r = fs::path(local) / L"MPC-HC Translation Studio" / L"fetch-cache";
            CoTaskMemFree(local);
        }
        if (!r.empty()) {
            std::error_code ec;
            fs::create_directories(r, ec);
        }
        return r;
    }();
    return root;
}

fs::path manifest_path() { return cache_root() / L"manifest.txt"; }

} // namespace

std::optional<std::string> load_manifest_sha() {
    std::ifstream f(manifest_path(), std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.empty()) return std::nullopt;
    return s;
}

void store(const std::string& repo_path, const std::string& bytes) {
    fs::path p = cache_root() / fs::path(repo_path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return;                                    // best-effort: a failed store just means a miss later
    f.write(bytes.data(), (std::streamsize)bytes.size());
}

std::optional<std::string> load(const std::string& repo_path) {
    fs::path p = cache_root() / fs::path(repo_path);
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

void ensure_dir(const std::string& repo_dir) {
    std::error_code ec;
    fs::create_directories(cache_root() / fs::path(repo_dir), ec);
}

void write_manifest_sha(const std::string& sha) {
    fs::path tmp = cache_root() / L"manifest.txt.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(sha.data(), (std::streamsize)sha.size());
    }
    std::error_code ec;
    fs::rename(tmp, manifest_path(), ec);   // atomic-ish replace (MoveFileExW w/ REPLACE_EXISTING)
}

} // namespace mpctrans::fetch_cache

#else // !_WIN32 — no LOCALAPPDATA concept here; disable the cache (always miss, stores are no-ops).
      // Keeps the portable CMake test gates (g++/clang) linking; the Studio app is Windows-only.

namespace mpctrans::fetch_cache {
std::optional<std::string> load_manifest_sha() { return std::nullopt; }
void store(const std::string&, const std::string&) {}
std::optional<std::string> load(const std::string&) { return std::nullopt; }
void ensure_dir(const std::string&) {}
void write_manifest_sha(const std::string&) {}
} // namespace mpctrans::fetch_cache

#endif
