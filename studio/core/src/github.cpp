// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/github.h"
#include "mpctrans/config.h"
#include "mpctrans/http_internal.h"

#include <stdexcept>

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>
#include <wincred.h>

#include <chrono>
#include <ctime>
#include <thread>

#include <json.hpp>

// GitHub client: WinHTTP (TLS handled by the OS) + nlohmann/json. Device-flow OAuth (public
// client id, no secret), token in the Windows Credential Manager, and the atomic Git Data
// commit/PR flow (fork -> merge-upstream -> blobs/tree/commit/ref -> cross-fork PR).

namespace mpctrans::github {

using nlohmann::json;
namespace cfg = mpctrans::config;

// widen() and HttpResponse/https_request live in mpctrans/http_internal.h now (declared there, NOT
// in the anonymous namespace) so they're linkable: mpctrans::ai_client (a separate .cpp) reuses this
// same WinHTTP plumbing rather than duplicating it. Everything else below stays internal via the
// anonymous namespace as before.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ---------- small helpers ----------
namespace {

std::string base64(const std::string& in) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8 | (unsigned char)in[i+2];
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8;
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

} // namespace

// (declaration + doc comment in mpctrans/http_internal.h)
HttpResponse https_request(const wchar_t* method, const std::string& url,
                           const std::vector<std::string>& headers, const std::string& body,
                           HINTERNET reuseSession) {
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
        throw std::runtime_error("github: bad url " + url);

    HINTERNET ses = reuseSession;
    if (!ses) {
        ses = WinHttpOpen(L"mpc-hc-translation-studio",
                          WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) throw std::runtime_error("github: WinHttpOpen failed");
        WinHttpSetTimeouts(ses, 15000, 15000, 30000, 30000);
    }
    bool ownSession = (reuseSession == nullptr);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, method, path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    HttpResponse res; DWORD gle = 0; bool ok = false;
    if (req) {
        std::wstring hdr;
        for (const auto& h : headers) hdr += widen(h) + L"\r\n";
        ok = WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
                                (DWORD)-1, body.empty() ? nullptr : (LPVOID)body.data(),
                                (DWORD)body.size(), (DWORD)body.size(), 0)
             && WinHttpReceiveResponse(req, nullptr);
        if (ok) {
            DWORD status = 0, len = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            res.status = (int)status;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                size_t off = res.body.size();
                res.body.resize(off + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, res.body.data() + off, avail, &got)) { res.body.resize(off); break; }
                res.body.resize(off + got);
            }
        } else gle = GetLastError();
    } else gle = GetLastError();
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ownSession) WinHttpCloseHandle(ses);     // keep a reused session open for the next fetch
    if (!ok) throw std::runtime_error("github: request failed for " + url +
                                      " (WinHTTP error " + std::to_string(gle) + ")");
    return res;
}

// api() and oauth_post() are internal (anonymous namespace), same as before the refactor —
// only HttpResponse/https_request/widen needed to become linkable for ai_client.cpp.
namespace {

// Percent-encode a string for a URL query value (RFC 3986 unreserved kept; everything else %XX).
std::string url_query_encode(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        bool unreserved = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) out += (char)c;
        else { out += '%'; out += hx[c >> 4]; out += hx[c & 0xF]; }
    }
    return out;
}

// GitHub REST call. Body (if any) is JSON. Returns parsed body; throws on status >= 400
// unless allowed_error matches (e.g. 422 handled by the caller).
json api(const Token& t, const wchar_t* method, const std::string& path,
         const json* body = nullptr, int* status_out = nullptr, int allowed_error = 0) {
    std::vector<std::string> h = {"Accept: application/vnd.github+json",
                                  "X-GitHub-Api-Version: 2022-11-28"};
    if (!t.access_token.empty()) h.push_back("Authorization: Bearer " + t.access_token);
    std::string b;
    if (body) { b = body->dump(); h.push_back("Content-Type: application/json"); }
    HttpResponse r = https_request(method, std::string(cfg::GH_API) + path, h, b);
    if (status_out) *status_out = r.status;
    json j = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (r.status >= 400 && r.status != allowed_error) {
        std::string msg = j.is_object() && j.contains("message") ? j["message"].get<std::string>()
                                                                 : r.body.substr(0, 200);
        throw std::runtime_error("github: " + path + " -> HTTP " + std::to_string(r.status) +
                                 " (" + msg + ")");
    }
    return j;
}

// OAuth endpoints take form bodies and return JSON only with this Accept header.
json oauth_post(const std::string& url, const std::string& form_body) {
    HttpResponse r = https_request(L"POST", url,
        {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"}, form_body);
    json j = json::parse(r.body, nullptr, false);
    if (r.status >= 400 || !j.is_object())
        throw std::runtime_error("github: oauth " + url + " -> HTTP " + std::to_string(r.status));
    return j;
}

} // namespace

// ---------- device flow ----------
DeviceCode request_device_code(const std::string& scope) {
    json j = oauth_post(cfg::GH_DEVICE_CODE_URL,
                        "client_id=" + std::string(cfg::OAUTH_CLIENT_ID) + "&scope=" + scope);
    if (!j.contains("device_code"))
        throw std::runtime_error("github: device code response missing device_code: " + j.dump());
    DeviceCode dc;
    dc.device_code      = j["device_code"].get<std::string>();
    dc.user_code        = j["user_code"].get<std::string>();
    dc.verification_uri = j["verification_uri"].get<std::string>();
    dc.interval         = j.value("interval", 5);
    dc.expires_in       = j.value("expires_in", 900);
    return dc;
}

Token poll_for_token(const DeviceCode& dc, std::function<void(const DeviceCode&)> on_prompt) {
    if (on_prompt) on_prompt(dc);
    int interval = dc.interval > 0 ? dc.interval : 5;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(dc.expires_in);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("github: device code expired before authorization");
        json j = oauth_post(cfg::GH_ACCESS_TOKEN_URL,
                            "client_id=" + std::string(cfg::OAUTH_CLIENT_ID) +
                            "&device_code=" + dc.device_code +
                            "&grant_type=urn:ietf:params:oauth:grant-type:device_code");
        if (j.contains("access_token")) return Token{j["access_token"].get<std::string>()};
        std::string err = j.value("error", "");
        if (err == "authorization_pending") continue;
        if (err == "slow_down") { interval += 5; continue; }
        throw std::runtime_error("github: device flow failed (" + err + ")");
    }
}

// ---------- token persistence (Credential Manager) ----------
void store_token(const Token& t, const wchar_t* cred_target) {
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = const_cast<wchar_t*>(cred_target);
    c.CredentialBlob = (LPBYTE)t.access_token.data();
    c.CredentialBlobSize = (DWORD)t.access_token.size();
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&c, 0))
        throw std::runtime_error("github: CredWrite failed (" + std::to_string(GetLastError()) + ")");
}
std::optional<Token> load_token(const wchar_t* cred_target) {
    PCREDENTIALW c = nullptr;
    if (!CredReadW(cred_target, CRED_TYPE_GENERIC, 0, &c)) return std::nullopt;
    Token t{std::string((const char*)c->CredentialBlob, c->CredentialBlobSize)};
    CredFree(c);
    return t;
}
void clear_token(const wchar_t* cred_target) {
    CredDeleteW(cred_target, CRED_TYPE_GENERIC, 0);   // missing entry is fine
}

// ---------- PR flow ----------
std::string whoami(const Token& t) {
    json u = api(t, L"GET", "/user");
    std::string name = u.contains("name") && u["name"].is_string() ? u["name"].get<std::string>()
                                                                   : u.at("login").get<std::string>();
    if (u.contains("email") && u["email"].is_string())
        name += " <" + u["email"].get<std::string>() + ">";
    return name;
}

std::string fetch_latest(const Token& /*t*/, const std::string& repo_path) {
    // Fetch via raw.githubusercontent.com (a CDN), NOT the REST contents API. The API caps
    // unauthenticated use at 60 requests/hour; raw is not under that cap, so we can pull the RC
    // plus every language's .po without a token or a git clone. (~5 min CDN cache — fine for
    // translation work.) For a private fork this would need a token; clsid2/mpc-hc is public.
    std::string url = std::string(cfg::GH_RAW) + "/" + cfg::UPSTREAM_OWNER + "/" +
                      cfg::UPSTREAM_REPO + "/" + cfg::UPSTREAM_BRANCH + "/" + repo_path;
    HttpResponse r = https_request(L"GET", url, {}, "");
    if (r.status != 200)
        throw std::runtime_error("github: fetch " + repo_path + " -> HTTP " + std::to_string(r.status));
    return r.body;
}

std::optional<std::string> fetch_latest(const Token& /*t*/, const std::string& owner,
                                        const std::string& repo, const std::string& branch,
                                        const std::string& repo_path) {
    std::string url = std::string(cfg::GH_RAW) + "/" + owner + "/" + repo + "/" + branch + "/" + repo_path;
    HttpResponse r = https_request(L"GET", url, {}, "");
    if (r.status == 404) return std::nullopt;
    if (r.status != 200)
        throw std::runtime_error("github: fetch " + repo_path + " -> HTTP " + std::to_string(r.status));
    return r.body;
}

// ---------- change detection ----------
std::string head_sha(const Token& t) {
    std::vector<std::string> h = {"Accept: application/vnd.github.sha",
                                  "X-GitHub-Api-Version: 2022-11-28"};
    if (!t.access_token.empty()) h.push_back("Authorization: Bearer " + t.access_token);
    std::string path = "/repos/" + std::string(cfg::UPSTREAM_OWNER) + "/" + cfg::UPSTREAM_REPO +
                       "/commits/" + cfg::UPSTREAM_BRANCH;
    HttpResponse r = https_request(L"GET", std::string(cfg::GH_API) + path, h, "");
    if (r.status != 200)
        throw std::runtime_error("github: head_sha -> HTTP " + std::to_string(r.status));
    std::string sha = r.body;
    while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r' || sha.back() == ' '))
        sha.pop_back();
    if (sha.empty()) throw std::runtime_error("github: head_sha -> empty response");
    return sha;
}

std::vector<std::string> changed_paths(const Token& t, const std::string& base,
                                       const std::string& head, bool& complete) {
    complete = false;
    std::vector<std::string> paths;
    if (base.empty() || head.empty()) return paths;   // caller treats everything as changed
    json j;
    try {
        j = api(t, L"GET", "/repos/" + std::string(cfg::UPSTREAM_OWNER) + "/" + cfg::UPSTREAM_REPO +
                          "/compare/" + base + "..." + head);
    } catch (const std::exception&) {
        return paths;                                   // request/HTTP-status failure -> incomplete
    }
    if (!j.is_object() || !j.contains("files") || !j["files"].is_array())
        return paths;                                   // unexpected shape -> incomplete
    for (const auto& f : j["files"])
        if (f.is_object() && f.contains("filename") && f["filename"].is_string())
            paths.push_back(f["filename"].get<std::string>());
    if (paths.size() >= 300) return paths;               // looks truncated -> incomplete (dirty=all)
    complete = true;
    return paths;
}

RawSession::RawSession() : RawSession(cfg::UPSTREAM_OWNER, cfg::UPSTREAM_REPO, cfg::UPSTREAM_BRANCH) {}
RawSession::RawSession(std::string owner, std::string repo, std::string branch)
    : m_owner(std::move(owner)), m_repo(std::move(repo)), m_branch(std::move(branch)) {
    HINTERNET s = WinHttpOpen(L"mpc-hc-translation-studio", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 15000, 15000, 30000, 30000);
    m_session = s;
}
RawSession::~RawSession() { if (m_session) WinHttpCloseHandle((HINTERNET)m_session); }
std::string RawSession::fetch(const std::string& repo_path) {
    if (!m_session) throw std::runtime_error("github: session not open");
    std::string url = std::string(cfg::GH_RAW) + "/" + m_owner + "/" + m_repo + "/" + m_branch +
                      "/" + repo_path;
    HttpResponse r = https_request(L"GET", url, {}, "", (HINTERNET)m_session);   // reuse the connection
    if (r.status != 200)
        throw std::runtime_error("github: fetch " + repo_path + " -> HTTP " + std::to_string(r.status));
    return r.body;
}

std::string open_pr(const Token& t, const std::string& owner, const std::string& repo,
                    const std::string& base_branch, const std::string& branch_prefix,
                    const std::vector<FileEdit>& edits, const std::string& title,
                    const std::string& body) {
    if (edits.empty()) throw std::runtime_error("github: no edits to commit");
    const std::string up = owner + "/" + repo;

    std::string login = api(t, L"GET", "/user").at("login").get<std::string>();
    const std::string fork = login + "/" + repo;

    // ensure the user fork exists (POST is idempotent; creation is async -> poll)
    api(t, L"POST", "/repos/" + up + "/forks");
    for (int tries = 0;; ++tries) {
        int st = 0;
        api(t, L"GET", "/repos/" + fork, nullptr, &st, /*allowed_error=*/404);
        if (st == 200) break;
        if (tries >= 30) throw std::runtime_error("github: fork " + fork + " not ready");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    // sync the fork's default branch (non-fatal: parents come from upstream HEAD directly)
    {
        int st = 0;
        json b = {{"branch", base_branch}};
        api(t, L"POST", "/repos/" + fork + "/merge-upstream", &b, &st, /*allowed_error=*/409);
    }

    // upstream HEAD commit + tree (the PR applies onto CURRENT upstream, not the bundle SHA)
    json ref = api(t, L"GET", "/repos/" + up + "/git/ref/heads/" + base_branch);
    std::string head_sha = ref.at("object").at("sha").get<std::string>();
    std::string base_tree = api(t, L"GET", "/repos/" + up + "/git/commits/" + head_sha)
                                .at("tree").at("sha").get<std::string>();

    // blobs -> tree -> commit, created in the fork (same object network as upstream)
    json tree_items = json::array();
    for (const auto& e : edits) {
        json blob = {{"content", base64(e.content)}, {"encoding", "base64"}};
        std::string blob_sha = api(t, L"POST", "/repos/" + fork + "/git/blobs", &blob)
                                   .at("sha").get<std::string>();
        tree_items.push_back({{"path", e.repo_path}, {"mode", "100644"},
                              {"type", "blob"}, {"sha", blob_sha}});
    }
    json tree_req = {{"base_tree", base_tree}, {"tree", tree_items}};
    std::string tree_sha = api(t, L"POST", "/repos/" + fork + "/git/trees", &tree_req)
                               .at("sha").get<std::string>();
    json commit_req = {{"message", title + (body.empty() ? "" : "\n\n" + body)},
                       {"tree", tree_sha}, {"parents", json::array({head_sha})}};
    std::string commit_sha = api(t, L"POST", "/repos/" + fork + "/git/commits", &commit_req)
                                 .at("sha").get<std::string>();

    // branch <branch_prefix>-<yyyymmdd-hhmm> on the fork (force-update if it already exists)
    std::time_t now = std::time(nullptr);
    std::tm g{}; gmtime_s(&g, &now);
    char stamp[16]; std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M", &g);
    std::string branch = branch_prefix + "-" + stamp;
    {
        int st = 0;
        json b = {{"ref", "refs/heads/" + branch}, {"sha", commit_sha}};
        api(t, L"POST", "/repos/" + fork + "/git/refs", &b, &st, /*allowed_error=*/422);
        if (st == 422) {
            json u = {{"sha", commit_sha}, {"force", true}};
            api(t, L"PATCH", "/repos/" + fork + "/git/refs/heads/" + branch, &u);
        }
    }

    // Do NOT create the PR here. Return GitHub's cross-fork "Open a pull request" compare URL for the
    // branch just created on the fork; the user reviews the diff on GitHub and clicks "Create pull
    // request". title/body pre-fill the form. Nothing is PR'd until the user takes that action.
    return "https://github.com/" + up + "/compare/" + base_branch + "..." + login + ":" + branch +
           "?expand=1&title=" + url_query_encode(title) +
           (body.empty() ? "" : "&body=" + url_query_encode(body));
}

std::string open_translation_pr(const Token& t, const std::string& lang,
                                const std::vector<FileEdit>& edits, const std::string& title,
                                const std::string& body) {
    return open_pr(t, cfg::UPSTREAM_OWNER, cfg::UPSTREAM_REPO, cfg::UPSTREAM_BRANCH,
                   "trans/" + lang, edits, title, body);
}

std::string update_transifex_branch(const Token& t, const std::string& owner, const std::string& repo,
                                    const std::string& branch, const std::vector<FileEdit>& edits,
                                    const std::string& message) {
    if (edits.empty()) throw std::runtime_error("github: no edits to commit");
    const std::string up = std::string(cfg::UPSTREAM_OWNER) + "/" + cfg::UPSTREAM_REPO;
    const std::string tx = owner + "/" + repo;

    // upstream develop HEAD + its tree (base_tree for the new tree below — same "apply onto CURRENT
    // upstream" posture as open_pr)
    json upRef = api(t, L"GET", "/repos/" + up + "/git/ref/heads/" + cfg::UPSTREAM_BRANCH);
    std::string upHeadSha = upRef.at("object").at("sha").get<std::string>();
    std::string upTreeSha = api(t, L"GET", "/repos/" + up + "/git/commits/" + upHeadSha)
                                .at("tree").at("sha").get<std::string>();

    // target branch HEAD (first parent of the merge commit)
    json txRef = api(t, L"GET", "/repos/" + tx + "/git/ref/heads/" + branch);
    std::string txHeadSha = txRef.at("object").at("sha").get<std::string>();

    // blobs + tree created directly in the target repo (no PR object involved, unlike open_pr)
    json tree_items = json::array();
    for (const auto& e : edits) {
        json blob = {{"content", base64(e.content)}, {"encoding", "base64"}};
        std::string blob_sha = api(t, L"POST", "/repos/" + tx + "/git/blobs", &blob)
                                   .at("sha").get<std::string>();
        tree_items.push_back({{"path", e.repo_path}, {"mode", "100644"},
                              {"type", "blob"}, {"sha", blob_sha}});
    }
    json tree_req = {{"base_tree", upTreeSha}, {"tree", tree_items}};
    std::string treeSha = api(t, L"POST", "/repos/" + tx + "/git/trees", &tree_req)
                              .at("sha").get<std::string>();

    // parents = [target-branch HEAD, upstream HEAD]: first parent is the branch being updated, so
    // this reads as "merge upstream develop into <branch>" (the direction `git merge` would record).
    json commit_req = {{"message", message}, {"tree", treeSha},
                       {"parents", json::array({ txHeadSha, upHeadSha })}};
    std::string commitSha = api(t, L"POST", "/repos/" + tx + "/git/commits", &commit_req)
                                .at("sha").get<std::string>();

    // NOT forced -- if the branch moved since the caller last read it, this throws rather than
    // silently discarding whatever landed there in the meantime.
    json patch = {{"sha", commitSha}, {"force", false}};
    api(t, L"PATCH", "/repos/" + tx + "/git/refs/heads/" + branch, &patch);

    return commitSha;
}

std::string rebase_pr_branch(const Token& t, const std::string& owner, const std::string& repo,
                             const std::string& branch, const std::vector<FileEdit>& edits,
                             const std::string& message) {
    if (edits.empty()) throw std::runtime_error("github: no edits to commit");
    const std::string up = std::string(cfg::UPSTREAM_OWNER) + "/" + cfg::UPSTREAM_REPO;
    const std::string tx = owner + "/" + repo;

    // upstream develop HEAD + its tree -- the SINGLE parent + base_tree, so the rebased branch is
    // "current develop, plus these translation files" and nothing else.
    json upRef = api(t, L"GET", "/repos/" + up + "/git/ref/heads/" + cfg::UPSTREAM_BRANCH);
    std::string upHeadSha = upRef.at("object").at("sha").get<std::string>();
    std::string upTreeSha = api(t, L"GET", "/repos/" + up + "/git/commits/" + upHeadSha)
                                .at("tree").at("sha").get<std::string>();

    // blobs + tree in the target repo (the fork shares upstream's object network, so upHeadSha is a
    // valid parent even though the commit is created here -- same as update_transifex_branch).
    json tree_items = json::array();
    for (const auto& e : edits) {
        json blob = {{"content", base64(e.content)}, {"encoding", "base64"}};
        std::string blob_sha = api(t, L"POST", "/repos/" + tx + "/git/blobs", &blob)
                                   .at("sha").get<std::string>();
        tree_items.push_back({{"path", e.repo_path}, {"mode", "100644"},
                              {"type", "blob"}, {"sha", blob_sha}});
    }
    json tree_req = {{"base_tree", upTreeSha}, {"tree", tree_items}};
    std::string treeSha = api(t, L"POST", "/repos/" + tx + "/git/trees", &tree_req)
                              .at("sha").get<std::string>();

    // SINGLE parent = develop HEAD -> a rebase, not a merge (contrast update_transifex_branch's two
    // parents). The PR branch becomes one clean commit atop current develop.
    json commit_req = {{"message", message}, {"tree", treeSha},
                       {"parents", json::array({ upHeadSha })}};
    std::string commitSha = api(t, L"POST", "/repos/" + tx + "/git/commits", &commit_req)
                                .at("sha").get<std::string>();

    // FORCE: the previous branch HEAD is not an ancestor of the rebased commit. This is the user's own
    // fork's bot-maintained sync branch, so a force-update (GitHub shows the PR as "force-pushed") is
    // the intended, clean-history behaviour.
    json patch = {{"sha", commitSha}, {"force", true}};
    api(t, L"PATCH", "/repos/" + tx + "/git/refs/heads/" + branch, &patch);

    return commitSha;
}

// ---- fork/branch discovery ----

std::vector<std::string> list_forks(const Token& t) {
    std::vector<std::string> out;
    std::string def = std::string(cfg::TRANSIFEX_OWNER) + "/" + cfg::TRANSIFEX_REPO;
    out.push_back(def);   // the app's long-standing default is always first, even if the GET below fails
    try {
        json j = api(t, L"GET", "/repos/" + std::string(cfg::UPSTREAM_OWNER) + "/" +
                          cfg::UPSTREAM_REPO + "/forks?per_page=100");
        if (j.is_array())
            for (const auto& f : j)
                if (f.is_object() && f.contains("full_name") && f["full_name"].is_string()) {
                    std::string full = f["full_name"].get<std::string>();
                    if (full != def) out.push_back(full);
                }
    } catch (const std::exception&) {}   // network/rate-limit failure -> just the default entry
    return out;
}

std::vector<std::string> list_branches(const Token& t, const std::string& owner, const std::string& repo) {
    std::vector<std::string> out;
    try {
        json j = api(t, L"GET", "/repos/" + owner + "/" + repo + "/branches?per_page=100");
        if (j.is_array())
            for (const auto& b : j)
                if (b.is_object() && b.contains("name") && b["name"].is_string())
                    out.push_back(b["name"].get<std::string>());
    } catch (const std::exception&) {}
    return out;
}

// ---- reverse-push: GraphQL blame ----

std::map<int, LineBlame> blame_line_dates(const Token& t, const std::string& owner, const std::string& repo,
                                          const std::string& branch, const std::string& repo_path) {
    std::map<int, LineBlame> out;
    try {
        static const char* kQuery =
            "query($o:String!,$r:String!,$e:String!,$p:String!){ repository(owner:$o,name:$r){ "
            "object(expression:$e){ ... on Commit { blame(path:$p){ ranges{ startingLine endingLine "
            "commit{ committedDate oid message } } } } } } }";
        json body = { { "query", kQuery },
                     { "variables", { { "o", owner }, { "r", repo }, { "e", branch }, { "p", repo_path } } } };
        std::vector<std::string> h = { "Accept: application/vnd.github+json", "Content-Type: application/json" };
        if (!t.access_token.empty()) h.push_back("Authorization: Bearer " + t.access_token);
        HttpResponse r = https_request(L"POST", "https://api.github.com/graphql", h, body.dump());
        if (r.status != 200) return out;
        json j = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) return out;
        json ranges = j["data"]["repository"]["object"]["blame"]["ranges"];
        if (!ranges.is_array()) return out;
        for (const auto& rg : ranges) {
            if (!rg.is_object()) continue;
            int a = rg.value("startingLine", 0), b = rg.value("endingLine", 0);
            LineBlame lb;
            if (rg.contains("commit") && rg["commit"].is_object()) {
                const json& c = rg["commit"];
                lb.date = c.value("committedDate", std::string());
                lb.sha = c.value("oid", std::string());
                std::string msg = c.value("message", std::string());
                size_t nl = msg.find('\n');                 // first line only
                lb.message = (nl == std::string::npos) ? msg : msg.substr(0, nl);
            }
            if (a <= 0 || b < a || lb.date.empty()) continue;
            for (int line = a; line <= b; ++line) out[line] = lb;
        }
    } catch (const std::exception&) {
        return {};
    }
    return out;
}

} // namespace mpctrans::github

#else // !_WIN32 — keep non-Windows builds (validators/gates) linking

namespace mpctrans::github {
std::wstring widen(const std::string&) { throw std::logic_error("github: Windows only"); }
HttpResponse https_request(const wchar_t*, const std::string&, const std::vector<std::string>&, const std::string&, void*) { throw std::logic_error("github: Windows only"); }
DeviceCode request_device_code(const std::string&) { throw std::logic_error("github: Windows only"); }
Token poll_for_token(const DeviceCode&, std::function<void(const DeviceCode&)>) { throw std::logic_error("github: Windows only"); }
void store_token(const Token&, const wchar_t*) { throw std::logic_error("github: Windows only"); }
std::optional<Token> load_token(const wchar_t*) { return std::nullopt; }
void clear_token(const wchar_t*) {}
std::string whoami(const Token&) { throw std::logic_error("github: Windows only"); }
std::string fetch_latest(const Token&, const std::string&) { throw std::logic_error("github: Windows only"); }
std::optional<std::string> fetch_latest(const Token&, const std::string&, const std::string&, const std::string&, const std::string&) { throw std::logic_error("github: Windows only"); }
std::string head_sha(const Token&) { throw std::logic_error("github: Windows only"); }
std::vector<std::string> changed_paths(const Token&, const std::string&, const std::string&, bool& complete) { complete = false; return {}; }
RawSession::RawSession() {}
RawSession::RawSession(std::string, std::string, std::string) {}
RawSession::~RawSession() {}
std::string RawSession::fetch(const std::string&) { throw std::logic_error("github: Windows only"); }
std::string open_pr(const Token&, const std::string&, const std::string&, const std::string&, const std::string&, const std::vector<FileEdit>&, const std::string&, const std::string&) { throw std::logic_error("github: Windows only"); }
std::string open_translation_pr(const Token&, const std::string&, const std::vector<FileEdit>&, const std::string&, const std::string&) { throw std::logic_error("github: Windows only"); }
std::string update_transifex_branch(const Token&, const std::string&, const std::string&, const std::string&, const std::vector<FileEdit>&, const std::string&) { throw std::logic_error("github: Windows only"); }
std::string rebase_pr_branch(const Token&, const std::string&, const std::string&, const std::string&, const std::vector<FileEdit>&, const std::string&) { throw std::logic_error("github: Windows only"); }
std::vector<std::string> list_forks(const Token&) { return {}; }
std::vector<std::string> list_branches(const Token&, const std::string&, const std::string&) { return {}; }
std::map<int, LineBlame> blame_line_dates(const Token&, const std::string&, const std::string&, const std::string&, const std::string&) { return {}; }
} // namespace mpctrans::github

#endif
