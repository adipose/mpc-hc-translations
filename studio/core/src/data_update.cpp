// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/data_update.h"
#include "mpctrans/config.h"
#include "mpctrans/github.h"
#include "mpctrans/http_internal.h"   // github::widen — reused for UTF-8 -> UTF-16 local paths

#include <algorithm>
#include <optional>
#include <stdexcept>

#include <json.hpp>

// dist/data-manifest.json diff+apply (see bundle-build/make_manifest.py for the generator and
// data_update.h for the full contract). Fetches go through mpctrans::github's existing WinHTTP
// plumbing (fetch_latest for the single manifest file, a RawSession for the — possibly many — changed
// artifacts); sha256 is Windows CNG (bcrypt.h), matching the manifest's own hashlib.sha256 hex digest.

#ifdef _WIN32

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>     // SHGetKnownFolderPath / FOLDERID_LocalAppData -- the last-applied version marker
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")   // CoTaskMemFree

#include <filesystem>
#include <fstream>
#include <sstream>

namespace mpctrans::data_update {

using nlohmann::json;
namespace cfg = mpctrans::config;
namespace fs = std::filesystem;

namespace {

// ---------- sha256 (Windows CNG) ----------

std::string bytes_to_hex(const std::vector<BYTE>& bytes) {
    static const char tbl[] = "0123456789abcdef";
    std::string out; out.reserve(bytes.size() * 2);
    for (BYTE b : bytes) { out += tbl[b >> 4]; out += tbl[b & 0xF]; }
    return out;
}

// Matches Python's hashlib.sha256(...).hexdigest() — the manifest generator's own hash (see
// bundle-build/make_manifest.py). Throws on any BCrypt failure (should never happen on a real Windows
// install; SHA-256 is a mandatory CNG provider).
std::string sha256_hex(const std::string& data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("data_update: BCryptOpenAlgorithmProvider failed");
    DWORD cbHashObject = 0, cbHash = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbHashObject, sizeof(cbHashObject), &cbData, 0);
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&cbHash, sizeof(cbHash), &cbData, 0);
    std::vector<BYTE> hashObject(cbHashObject), hash(cbHash);

    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS status = BCryptCreateHash(hAlg, &hHash, hashObject.data(), cbHashObject, nullptr, 0, 0);
    if (BCRYPT_SUCCESS(status))
        status = BCryptHashData(hHash, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    if (BCRYPT_SUCCESS(status))
        status = BCryptFinishHash(hHash, hash.data(), cbHash, 0);
    if (hHash) BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!BCRYPT_SUCCESS(status)) throw std::runtime_error("data_update: sha256 hashing failed");
    return bytes_to_hex(hash);
}

// ---------- local file I/O ----------

// Whole-file read via a WIDE path (github::widen), not the ANSI-codepage std::ifstream(std::string)
// overload -- these are real filesystem paths (a user's profile dir can be non-ASCII), unlike the
// UTF-8 .po/JSON *content* the rest of libmpctrans deals with in std::string form.
std::optional<std::string> read_local_file(const std::string& path_utf8) {
    if (path_utf8.empty()) return std::nullopt;
    std::ifstream f(github::widen(path_utf8), std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::optional<std::string> local_sha256(const std::string& path_utf8) {
    auto bytes = read_local_file(path_utf8);
    if (!bytes) return std::nullopt;
    return sha256_hex(*bytes);
}

// Write `bytes` to a NEW temp file in the same directory as `tmp`, fully — 1MB chunks so a single
// WriteFile call's DWORD length never has to represent more than that. Cleans up its own temp file on
// any failure so a half-written download never lingers next to the target.
bool write_temp_file(const std::wstring& tmp, const std::string& bytes) {
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (size_t off = 0; ok && off < bytes.size(); ) {
        DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - off, size_t(1) << 20);
        DWORD written = 0;
        ok = ::WriteFile(h, bytes.data() + off, chunk, &written, nullptr) && written == chunk;
        off += written;
    }
    ::CloseHandle(h);
    if (!ok) ::DeleteFileW(tmp.c_str());
    return ok;
}

// Atomically replace the local file at `target_utf8` with `bytes`: write a temp file in the SAME
// directory first, then ReplaceFileW — falling back to MoveFileExW(MOVEFILE_REPLACE_EXISTING) when the
// target doesn't exist yet (a language pack this install never had, or ReplaceFileW's own
// ERROR_FILE_NOT_FOUND case). The existing file is never touched until the new bytes are verified AND
// fully on disk — see sha256_hex's caller (apply, below), which only reaches here after a match.
bool atomic_replace(const std::string& target_utf8, const std::string& bytes) {
    std::wstring target = github::widen(target_utf8);
    std::error_code ec;
    fs::create_directories(fs::path(target).parent_path(), ec);   // e.g. a lang pack never bundled locally

    std::wstring tmp = target + L".tmp";
    if (!write_temp_file(tmp, bytes)) return false;

    if (::ReplaceFileW(target.c_str(), tmp.c_str(), nullptr, 0, nullptr, nullptr)) return true;
    if (::GetLastError() == ERROR_FILE_NOT_FOUND &&
        ::MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    ::DeleteFileW(tmp.c_str());
    return false;
}

// ---------- last-applied version marker ----------
// A tiny sidecar under %LOCALAPPDATA%, separate from mpctrans::fetch_cache (that module's manifest.txt
// is scoped to the RC/.po prefetch's upstream HEAD sha -- a different cache with different semantics;
// reusing it here would conflate the two). Informational only: `check()`'s changed-file list is always
// computed from real sha256 comparisons, never from this value.
fs::path version_file() {
    fs::path r;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        r = fs::path(local) / L"MPC-HC Translation Studio" / L"data-update";
        CoTaskMemFree(local);
    }
    if (r.empty()) return r;
    std::error_code ec;
    fs::create_directories(r, ec);
    return r / L"version.txt";
}

std::string load_local_version() {
    fs::path p = version_file();
    if (p.empty()) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

void store_local_version(const std::string& v) {
    fs::path p = version_file();
    if (p.empty()) return;
    fs::path tmp = p; tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(v.data(), (std::streamsize)v.size());
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);   // atomic-ish replace, same pattern as fetch_cache::write_manifest_sha
}

} // namespace

UpdatePlan check(const std::function<std::string(const std::string&)>& local_path_for) {
    UpdatePlan plan;
    github::Token tok;   // unauthenticated -- fetch_latest's owner/repo/branch overload ignores it
    auto manifest = github::fetch_latest(tok, cfg::STUDIO_OWNER, cfg::STUDIO_REPO, cfg::STUDIO_BRANCH,
                                         "dist/data-manifest.json");
    if (!manifest)
        throw std::runtime_error("data_update: dist/data-manifest.json not found on " +
                                 std::string(cfg::STUDIO_OWNER) + "/" + cfg::STUDIO_REPO + "@" +
                                 cfg::STUDIO_BRANCH);

    json j = json::parse(*manifest, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("files") || !j["files"].is_object())
        throw std::runtime_error("data_update: malformed dist/data-manifest.json");

    plan.remote_version = j.value("version", std::string());
    plan.local_version  = load_local_version();

    for (auto& [repo_path, meta] : j["files"].items()) {
        if (!meta.is_object() || !meta.contains("sha256") || !meta["sha256"].is_string()) continue;
        RemoteFile rf;
        rf.repo_path = repo_path;
        rf.sha256    = meta["sha256"].get<std::string>();
        rf.size      = meta.value("size", (long long)0);

        std::string local = local_path_for(repo_path);
        if (local.empty()) continue;   // this Studio build has no on-disk slot for this manifest entry
        auto localSha = local_sha256(local);
        if (!localSha || *localSha != rf.sha256) plan.changed.push_back(std::move(rf));
    }
    return plan;
}

ApplyResult apply(const UpdatePlan& plan,
                  const std::function<std::string(const std::string&)>& local_path_for,
                  const std::function<void(const std::string&, int, int)>& progress) {
    ApplyResult result;
    if (plan.changed.empty()) return result;

    github::RawSession sess(cfg::STUDIO_OWNER, cfg::STUDIO_REPO, cfg::STUDIO_BRANCH);   // one keep-alive session
    int n = (int)plan.changed.size();
    for (int i = 0; i < n; ++i) {
        const RemoteFile& rf = plan.changed[i];
        if (progress) progress(rf.repo_path, i + 1, n);

        std::string local = local_path_for(rf.repo_path);
        if (local.empty()) {
            result.failed++;
            result.errors.push_back(rf.repo_path + ": no local path for this artifact");
            continue;
        }
        try {
            std::string bytes = sess.fetch(rf.repo_path);
            std::string gotSha = sha256_hex(bytes);
            if (gotSha != rf.sha256) {
                result.failed++;
                result.errors.push_back(rf.repo_path + ": sha256 mismatch after download -- not applied");
                continue;
            }
            if (!atomic_replace(local, bytes)) {
                result.failed++;
                result.errors.push_back(rf.repo_path + ": failed to write the local file");
                continue;
            }
            result.updated++;
        } catch (const std::exception& ex) {
            result.failed++;
            result.errors.push_back(rf.repo_path + ": " + ex.what());
        }
    }
    // Only record the new version once EVERY changed file applied cleanly -- a partial apply should
    // re-offer the same (still-partially-outdated) update next time, not silently mark itself current.
    if (result.failed == 0 && !plan.remote_version.empty()) store_local_version(plan.remote_version);
    return result;
}

} // namespace mpctrans::data_update

#else // !_WIN32 — keep non-Windows builds (validators/gates) linking, same posture as github.cpp

namespace mpctrans::data_update {
UpdatePlan check(const std::function<std::string(const std::string&)>&) {
    throw std::logic_error("data_update: Windows only");
}
ApplyResult apply(const UpdatePlan&, const std::function<std::string(const std::string&)>&,
                  const std::function<void(const std::string&, int, int)>&) {
    throw std::logic_error("data_update: Windows only");
}
} // namespace mpctrans::data_update

#endif
