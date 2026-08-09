// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

// GitHub client — device-flow OAuth + the atomic Git Data commit/PR flow.
// Native impl: WinHTTP (HTTPS) + nlohmann/json. Token stored via Windows Credential Manager/DPAPI.
// See plan-translation-studio "Device-flow auth & GitHub API commit — DE-RISKED".

namespace mpctrans::github {

struct DeviceCode { std::string device_code, user_code, verification_uri; int interval = 5, expires_in = 900; };
struct Token      { std::string access_token; };

// ---- device flow (no client secret) ----
DeviceCode request_device_code(const std::string& scope);
// Poll GH_ACCESS_TOKEN_URL, honoring interval / slow_down / expired_token. `on_prompt` shows the
// user_code + verification_uri (Studio opens the browser via ShellExecute). Blocks until done.
Token      poll_for_token(const DeviceCode&, std::function<void(const DeviceCode&)> on_prompt);

// ---- token persistence (Windows Credential Manager; DPAPI-backed) ----
// cred_target is parameterized so tests can roundtrip on a scratch entry.
inline constexpr wchar_t CRED_TARGET[] = L"mpc-hc-translation-studio/github";
void                 store_token(const Token&, const wchar_t* cred_target = CRED_TARGET);
std::optional<Token> load_token(const wchar_t* cred_target = CRED_TARGET);
void                 clear_token(const wchar_t* cred_target = CRED_TARGET);

// Display identity of the authorized user, "Name <email>" when public, else the login.
// Used for the .po Last-Translator header at splice time.
std::string whoami(const Token&);

// ---- PR flow (apply onto CURRENT upstream, not the bundle SHA) ----
struct FileEdit { std::string repo_path; std::string content; };  // e.g. mpc-hc.fr.strings.po + new bytes

// Fetch the latest upstream bytes of a repo-relative path from raw.githubusercontent.com @ the
// default branch HEAD (a CDN — not under the REST API's 60/hr unauthenticated cap; no token needed).
std::string fetch_latest(const Token&, const std::string& repo_path);

// Generalized fetch — any owner/repo/branch, not just UPSTREAM_* (used by mpctrans::corrections to
// pull data/research-corrections.jsonl from the STUDIO_* repo). Returns nullopt when the path is
// missing there (HTTP 404 — e.g. the overlay file hasn't been created upstream yet: start from empty
// content); throws on other request failures, same as fetch_latest.
std::optional<std::string> fetch_latest(const Token&, const std::string& owner, const std::string& repo,
                                        const std::string& branch, const std::string& repo_path);

// ---- change detection (disk-cache invalidation; see mpctrans::fetch_cache) ----

// Bare 40-hex SHA of UPSTREAM_BRANCH's HEAD: GET /repos/<owner>/<repo>/commits/<branch> with
// Accept: application/vnd.github.sha (the body IS the SHA, not JSON). Unauthenticated is fine — one
// call per launch, well under the 60/hr cap; the token's Authorization header is sent when available.
std::string head_sha(const Token&);

// Repo-relative paths that differ between `base` and `head`: GET /repos/<owner>/<repo>/compare/
// <base>...<head>, reading only the first page (GitHub returns at most 300 files per compare).
// Sets `complete = false` — meaning the caller should treat EVERY path as changed — when `base` is
// empty, the request/parse fails, or the file list looks truncated (>= 300 entries).
std::vector<std::string> changed_paths(const Token&, const std::string& base, const std::string& head,
                                       bool& complete);

// Keep-alive HTTPS session for many fetches to raw.githubusercontent.com — reuse one instance so
// the TLS handshake is paid once, not per request (WinHTTP pools the connection). Used by the bulk
// prefetch: a few sessions, many files each. Non-copyable; Windows-only.
class RawSession {
public:
    RawSession();   // GH_RAW/<UPSTREAM_OWNER>/<UPSTREAM_REPO>/<UPSTREAM_BRANCH> (the bulk-prefetch default)
    // Generalized, same rationale as fetch_latest's owner/repo/branch overload above — used by
    // mpctrans::data_update to pull many files from STUDIO_* instead of UPSTREAM_*.
    RawSession(std::string owner, std::string repo, std::string branch);
    ~RawSession();
    RawSession(const RawSession&) = delete;
    RawSession& operator=(const RawSession&) = delete;
    std::string fetch(const std::string& repo_path);   // GET <raw>/<owner>/<repo>/<branch>/<repo_path>
private:
    void* m_session = nullptr;   // HINTERNET
    std::string m_owner, m_repo, m_branch;
};

// Full flow, generalized: ensure the caller's fork of owner/repo -> merge-upstream (fork's default
// branch == owner/repo's `base_branch` HEAD) -> create blobs/tree/commit (parent = that HEAD) on
// <branch_prefix>-<date> on the fork. Does NOT create the PR: returns GitHub's cross-fork "Open a
// pull request" COMPARE URL (title/body pre-filled) so the user reviews the diff on GitHub and clicks
// "Create pull request" -- creation is the user's explicit action there; the Studio PRs nothing itself.
// Used directly by mpctrans::corrections (owner/repo = STUDIO_*, branch_prefix = "research-fix")
// and via the open_translation_pr wrapper below (owner/repo = UPSTREAM_*, branch_prefix = "trans/<lang>").
std::string open_pr(const Token&, const std::string& owner, const std::string& repo,
                    const std::string& base_branch, const std::string& branch_prefix,
                    const std::vector<FileEdit>& edits, const std::string& title,
                    const std::string& body = "");

// Translation PRs: a thin wrapper over open_pr targeting UPSTREAM_OWNER/UPSTREAM_REPO@UPSTREAM_BRANCH
// with branch_prefix "trans/<lang>". Caller supplies edits already spliced onto the latest .po.
// Returns the compare URL (see open_pr) -- the user creates the PR on GitHub after reviewing.
std::string open_translation_pr(const Token&, const std::string& lang,
                                const std::vector<FileEdit>& edits, const std::string& title,
                                const std::string& body = "");

// ---- Transifex sync (mpctrans::txsync) ----

// Fast-forwards TRANSIFEX_OWNER/TRANSIFEX_REPO@TRANSIFEX_BRANCH with `edits`, as a real merge commit
// of upstream's CURRENT develop into it (parents = [current transifex HEAD, upstream develop HEAD] —
// first parent is the transifex branch, so this reads as "merge develop into transifex" the way `git
// merge` would record it). Unlike open_pr, this pushes directly (no fork, no PR): the user owns
// TRANSIFEX_OWNER/TRANSIFEX_REPO, so a straight PATCH of the branch ref is the equivalent of the old
// `uptransifex.sh` flow's local push, just done via the Git Data API. Blobs are created directly in
// that repo (edits are already the FULL merged file content — see txsync::tx_build_edits). The ref
// update is NOT forced: if the branch moved since the caller last read it, this throws (HTTP 422/409
// from the PATCH) rather than silently discarding whatever landed there. Returns the new commit sha.
std::string update_transifex_branch(const Token&, const std::vector<FileEdit>& edits,
                                    const std::string& message);

} // namespace mpctrans::github
