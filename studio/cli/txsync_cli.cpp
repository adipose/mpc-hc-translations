// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless CLI mirroring TxSyncDlg::OnRefreshPrClicked (studio/ui/TxSyncDlg.cpp) -- refreshes the
// currently open Transifex-sync PR's branch without the GUI, so it can run from a scheduled job.
// Same primitives (libmpctrans), same decision logic (mpctrans::txsync), same GitHub calls
// (mpctrans::github) -- this file only reproduces the sequencing/reporting the dialog's button
// handler does on the UI thread, as plain synchronous calls.
//
// Progress/status goes to stderr; the machine-readable report goes to stdout.
#include <windows.h>

#include "mpctrans/config.h"
#include "mpctrans/github.h"
#include "mpctrans/txapi.h"
#include "mpctrans/txsync.h"
#include "mpctrans/po.h"
#include "mpctrans/validate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mpctrans;

namespace {

void print_usage() {
    std::fprintf(stderr,
        "usage: txsync_cli refresh            [--po-dir DIR] [--dry-run] [--tx-owner O] [--tx-repo R] [--tx-branch B]\n"
        "       txsync_cli update-branch      [--po-dir DIR] [--dry-run] [--tx-owner O] [--tx-repo R] [--tx-branch B] [--push-transifex]\n"
        "       txsync_cli push-to-transifex  [--po-dir DIR] [--dry-run] [--tx-owner O] [--tx-repo R] [--tx-branch B]\n"
        "       txsync_cli propose            [--po-dir DIR] [--dry-run] [--tx-owner O] [--tx-repo R] [--tx-branch B]\n"
        "                                     compute the merge, run the Options-tree gate, create a fresh\n"
        "                                     transifex-sync-<stamp> branch on your fork and print the compare\n"
        "                                     URL (the PR itself is opened from it, e.g. with gh pr create).\n"
        "\n"
        "refresh            updates the open transifex-sync-* PR branch (rebase onto develop, validated top-up).\n"
        "update-branch      pushes the merge to the Transifex staging branch itself: a merge commit whose tree is\n"
        "                   CURRENT develop + the merged .po files -- so the branch matches develop on everything\n"
        "                   except the translations (mirrors the GUI's \"Update Transifex branch\").\n"
        "                   --push-transifex reverse-pushes upstream-newer translations INTO Transifex first (see\n"
        "                   push-to-transifex below) so this merge doesn't turn around and revert them.\n"
        "push-to-transifex  reverse sync: pushes upstream develop's translations INTO the live Transifex DB\n"
        "                   wherever upstream should win over what Transifex currently has. Transifex's GitHub\n"
        "                   integration is one-way (DB -> the transifex branch), so this is what keeps a\n"
        "                   translator's PR merged into develop from being reverted by the next sync. Prints\n"
        "                   the plan (push / keep-transifex / notes, with reasons) to stdout; with --dry-run,\n"
        "                   stops before applying it. Transifex token: Credential Manager, else env\n"
        "                   TRANSIFEX_TOKEN, else env TRANSIFEX.\n");
}

// Mirrors MainFrame::PopulateLanguages's glob exactly (studio/ui/MainFrame.cpp): every
// mpc-hc.<lang>.dialogs.po file in the PO dir contributes <lang>. Sorted + de-duplicated (the GUI's
// combo box population happens to preserve directory order; sorting here just makes the CLI's output
// deterministic across filesystems -- the resulting language SET is identical either way).
std::vector<std::string> enumerate_languages(const fs::path& poDir) {
    std::vector<std::string> langs;
    const std::string pre = "mpc-hc.", suf = ".dialogs.po";
    std::error_code ec;
    if (!fs::exists(poDir, ec) || ec) return langs;
    for (auto& p : fs::directory_iterator(poDir, ec)) {
        if (ec) break;
        std::string fn = p.path().filename().string();
        if (fn.size() > pre.size() + suf.size() &&
            fn.compare(0, pre.size(), pre) == 0 &&
            fn.compare(fn.size() - suf.size(), suf.size(), suf) == 0)
            langs.push_back(fn.substr(pre.size(), fn.size() - pre.size() - suf.size()));
    }
    std::sort(langs.begin(), langs.end());
    langs.erase(std::unique(langs.begin(), langs.end()), langs.end());
    return langs;
}

// Transifex token: stored Credential Manager entry first, else TRANSIFEX_TOKEN, else TRANSIFEX --
// same layered fallback as the GitHub token above, so this can run unattended in a scheduled job.
std::optional<txapi::Token> load_transifex_token() {
    std::optional<txapi::Token> t = txapi::load_token();
    if (!t) { if (const char* env = std::getenv("TRANSIFEX_TOKEN")) t = txapi::Token{ env }; }
    if (!t) { if (const char* env = std::getenv("TRANSIFEX"))       t = txapi::Token{ env }; }
    return t;
}

void print_reverse_push_plan(const txsync::ReversePushPlan& plan) {
    std::printf("push: %zu\n", plan.push.size());
    for (auto& it : plan.push)
        std::printf("  %s  %s  %s  -- %s\n", it.lang.c_str(), config::RESOURCES[it.res],
                    it.msgctxt.c_str(), it.reason.c_str());
    std::printf("keep transifex: %zu\n", plan.keepTransifex.size());
    for (auto& it : plan.keepTransifex)
        std::printf("  %s  %s  %s  -- %s\n", it.lang.c_str(), config::RESOURCES[it.res],
                    it.msgctxt.c_str(), it.reason.c_str());
    std::printf("notes: %zu\n", plan.notes.size());
    for (auto& n : plan.notes) std::printf("  %s\n", n.c_str());
}

int cmd_refresh(int argc, char** argv) {
    std::string poDir = "po";
    bool dryRun = false;
    std::string txOwner = config::TRANSIFEX_OWNER;
    std::string txRepo  = config::TRANSIFEX_REPO;
    std::string txBranch = config::TRANSIFEX_BRANCH;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a.empty()) continue;   // an empty argv entry (e.g. an unset $env var passed positionally) is not an option
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " needs an argument");
            return argv[++i];
        };
        if (a == "--po-dir")          poDir = next("--po-dir");
        else if (a == "--dry-run")    dryRun = true;
        else if (a == "--tx-owner")   txOwner = next("--tx-owner");
        else if (a == "--tx-repo")    txRepo = next("--tx-repo");
        else if (a == "--tx-branch")  txBranch = next("--tx-branch");
        else if (a == "--help" || a == "-h") { print_usage(); std::exit(0); }
        else throw std::runtime_error("unknown option: " + a);
    }

    // Enumerate languages up front (mirrors MainFrame::LanguageList, snapshotted before the dialog's
    // background compute in TxSyncDlg::OnInitDialog).
    std::vector<std::string> languages = enumerate_languages(poDir);
    if (languages.empty()) {
        std::fprintf(stderr,
            "error: no languages found under po-dir '%s' (expected mpc-hc.<lang>.dialogs.po files)\n",
            poDir.c_str());
        return 2;
    }
    std::fprintf(stderr, "found %zu language(s) under %s\n", languages.size(), poDir.c_str());

    // 1. Token: stored Credential Manager entry first (same as every GUI path), else GITHUB_TOKEN so
    // this can run unattended in a scheduled job.
    std::optional<github::Token> tok = github::load_token();
    if (!tok) {
        if (const char* env = std::getenv("GITHUB_TOKEN"))
            tok = github::Token{ env };
    }
    if (!tok)
        throw std::runtime_error("no GitHub token: sign in once via the Studio GUI, or set GITHUB_TOKEN");

    // 2. Identity -- also the owner whose fork/branch we operate on (TxSyncDlg::OnRefreshPrClicked
    // always targets the SIGNED-IN user's fork of upstream, not the tx source).
    std::string login = github::whoami(*tok);
    std::fprintf(stderr, "signed in as %s\n", login.c_str());

    // 3/4. Recompute the merge exactly like RunComputeAndPost does: upstream = config::UPSTREAM_*,
    // transifex source = --tx-* (defaults to config::TRANSIFEX_*).
    auto fetchUpstream = [&](const std::string& path) {
        return github::fetch_latest(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                    config::UPSTREAM_BRANCH, path);
    };
    auto fetchTx = [&](const std::string& path) {
        return github::fetch_latest(*tok, txOwner, txRepo, txBranch, path);
    };
    auto progress = [](int done, int total) {
        std::fprintf(stderr, "  fetching %d/%d\r", done, total);
    };
    std::fprintf(stderr, "computing sync against %s/%s@%s ...\n", txOwner.c_str(), txRepo.c_str(), txBranch.c_str());
    txsync::TxSyncResult result = txsync::tx_compute(fetchUpstream, fetchTx, languages, progress);
    std::fprintf(stderr, "\n");

    // 5. Find the open sync PR branch on the signed-in user's fork of upstream.
    std::vector<std::string> branches = github::list_branches(*tok, login, config::UPSTREAM_REPO);
    std::vector<std::string> sync;
    for (auto& b : branches)
        if (b.rfind("transifex-sync-", 0) == 0) sync.push_back(b);
    if (sync.empty()) {
        std::fprintf(stderr, "error: no open transifex-sync-* branch on %s/%s\n",
                     login.c_str(), config::UPSTREAM_REPO);
        return 3;
    }
    std::sort(sync.begin(), sync.end());   // "<prefix>-YYYYMMDD-HHMM" sorts chronologically
    std::string branch = sync.back();      // newest
    // The newest branch may belong to an already-MERGED sync PR (branches outlive their PRs); topping
    // it up would land on a dead branch. Refuse unless upstream has an OPEN PR headed by it.
    int prNum = github::open_pr_number(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO, login, branch);
    if (prNum == 0) {
        std::fprintf(stderr, "error: no OPEN pull request on %s/%s for %s:%s (merged or closed?) -- "
                             "propose a new sync PR instead\n",
                     config::UPSTREAM_OWNER, config::UPSTREAM_REPO, login.c_str(), branch.c_str());
        return 3;
    }
    std::fprintf(stderr, "open PR #%d for %s\n", prNum, branch.c_str());

    // 6. Validated edits (bad-for-Options-tree translations held back rather than blocking the rest).
    std::vector<txsync::HeldTranslation> held;
    std::vector<github::FileEdit> edits = txsync::tx_build_edits_validated(result, held);

    // 7. Machine-readable report on stdout.
    std::printf("target: %s/%s@%s\n", login.c_str(), config::UPSTREAM_REPO, branch.c_str());
    std::printf("changed files: %zu\n", edits.size());
    for (auto& e : edits) std::printf("  %s\n", e.repo_path.c_str());
    std::printf("held back: %zu\n", held.size());
    for (auto& h : held)
        std::printf("  %s  %s  -- %s\n", h.lang.c_str(), h.msgctxt.c_str(), h.reason.c_str());

    if (edits.empty()) {
        std::printf("nothing new to add\n");
        return 0;
    }

    // 7b. Skip when the PR branch already carries exactly these translations (same file set, same
    // entries). edits is the full diff against develop, so without this an hourly run would
    // force-push an identical rebase every time. Entries, not bytes: the serializer stamps
    // PO-Revision-Date to the minute. Branch files are fetched by the PR head SHA (immutable URL),
    // because the raw CDN serves a branch name stale for minutes after a push.
    // Runs under --dry-run too, so a dry run reports what a real one would decide.
    {
        std::vector<std::string> prFiles = github::pr_changed_files(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO, prNum);
        std::string headSha = github::pr_head_sha(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO, prNum);
        std::vector<std::string> ours;
        for (auto& e : edits) ours.push_back(e.repo_path);
        std::sort(prFiles.begin(), prFiles.end());
        std::sort(ours.begin(), ours.end());
        bool same = !prFiles.empty() && !headSha.empty() && prFiles == ours;
        std::string why = same ? "" : (prFiles.empty() || headSha.empty() ? "could not read the PR" : "PR file set differs");
        auto sameEntries = [](const std::string& a, const std::string& b) {
            PoFile pa = PoFile::parse_bytes(a), pb = PoFile::parse_bytes(b);
            if (pa.entries.size() != pb.entries.size()) return false;
            for (size_t k = 0; k < pa.entries.size(); ++k) {
                const auto& x = pa.entries[k]; const auto& y = pb.entries[k];
                if (x.msgctxt != y.msgctxt || x.msgid != y.msgid || x.msgstr != y.msgstr) return false;
            }
            return true;
        };
        for (size_t i = 0; same && i < edits.size(); ++i) {
            auto cur = github::fetch_latest(*tok, login, config::UPSTREAM_REPO, headSha, edits[i].repo_path);
            same = cur && sameEntries(*cur, edits[i].content);
            if (!same) why = "translations differ: " + edits[i].repo_path + (cur ? "" : " (not on branch)");
        }
        std::fprintf(stderr, "branch check: PR lists %zu file(s), sync has %zu -- %s\n",
                     prFiles.size(), ours.size(), same ? "identical, skipping push" : why.c_str());
        if (same) {
            std::printf("branch already holds these translations -- nothing to push\n");
            return 0;
        }
    }
    if (dryRun) {
        std::fprintf(stderr, "dry-run: stopping before push\n");
        return 0;
    }

    // 8. The actual push -- the one outward GitHub write this tool performs, and only on this path
    // (never under --dry-run, never when edits is empty).
    std::string sha = github::rebase_pr_branch(*tok, login, config::UPSTREAM_REPO, branch, edits,
        "Refresh Transifex sync (Studio CLI)");
    std::printf("pushed: %s\n", sha.c_str());
    return 0;
}

// Mirrors TxSyncDlg::OnUpdateBranchClicked: push the merge straight to the Transifex STAGING branch
// (not a PR branch) as a merge commit of current develop -- tree = develop + merged POs, so the
// branch matches develop on everything except the translations. Uses the RAW tx_build_edits (no
// hold/validation): the staging branch mirrors Transifex verbatim; filtering happens at PR time.
int cmd_update_branch(int argc, char** argv) {
    std::string poDir = "po";
    bool dryRun = false;
    bool pushTransifex = false;
    std::string txOwner = config::TRANSIFEX_OWNER;
    std::string txRepo  = config::TRANSIFEX_REPO;
    std::string txBranch = config::TRANSIFEX_BRANCH;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a.empty()) continue;   // an empty argv entry (e.g. an unset $env var passed positionally) is not an option
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " needs an argument");
            return argv[++i];
        };
        if (a == "--po-dir")               poDir = next("--po-dir");
        else if (a == "--dry-run")         dryRun = true;
        else if (a == "--tx-owner")        txOwner = next("--tx-owner");
        else if (a == "--tx-repo")         txRepo = next("--tx-repo");
        else if (a == "--tx-branch")       txBranch = next("--tx-branch");
        else if (a == "--push-transifex")  pushTransifex = true;
        else if (a == "--help" || a == "-h") { print_usage(); std::exit(0); }
        else throw std::runtime_error("unknown option: " + a);
    }

    std::vector<std::string> languages = enumerate_languages(poDir);
    if (languages.empty()) {
        std::fprintf(stderr,
            "error: no languages found under po-dir '%s' (expected mpc-hc.<lang>.dialogs.po files)\n",
            poDir.c_str());
        return 2;
    }
    std::fprintf(stderr, "found %zu language(s) under %s\n", languages.size(), poDir.c_str());

    std::optional<github::Token> tok = github::load_token();
    if (!tok) {
        if (const char* env = std::getenv("GITHUB_TOKEN"))
            tok = github::Token{ env };
    }
    if (!tok)
        throw std::runtime_error("no GitHub token: sign in once via the Studio GUI, or set GITHUB_TOKEN");

    std::string login = github::whoami(*tok);
    std::fprintf(stderr, "signed in as %s\n", login.c_str());
    // Only the branch's owner may move it (mirrors the GUI's owner gate).
    if (_stricmp(login.c_str(), txOwner.c_str()) != 0)
        throw std::runtime_error("signed in as \"" + login + "\" -- only " + txOwner +
                                 " can update " + txOwner + "/" + txRepo + "@" + txBranch);

    auto fetchUpstream = [&](const std::string& path) {
        return github::fetch_latest(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                    config::UPSTREAM_BRANCH, path);
    };
    auto fetchTx = [&](const std::string& path) {
        return github::fetch_latest(*tok, txOwner, txRepo, txBranch, path);
    };
    auto progress = [](int done, int total) {
        std::fprintf(stderr, "  fetching %d/%d\r", done, total);
    };
    std::fprintf(stderr, "computing sync against %s/%s@%s ...\n", txOwner.c_str(), txRepo.c_str(), txBranch.c_str());
    txsync::TxSyncResult result = txsync::tx_compute(fetchUpstream, fetchTx, languages, progress);
    std::fprintf(stderr, "\n");

    // --push-transifex: reverse-push upstream-newer translations INTO Transifex FIRST, so the
    // include-flags it flips take effect in the merge built just below (tx_build_edits) instead of
    // this same pass reverting what was just pushed.
    if (pushTransifex) {
        std::optional<txapi::Token> txTok = load_transifex_token();
        if (!txTok)
            throw std::runtime_error("no Transifex token: sign in once via the Studio GUI, "
                                     "or set TRANSIFEX_TOKEN (or TRANSIFEX)");
        auto listTx = [&](int res, const std::string& lang) {
            return txapi::list_translations(*txTok, res, lang);
        };
        auto blameDates = [&](const std::string& path) {
            return github::blame_line_dates(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                            config::UPSTREAM_BRANCH, path);
        };
        std::fprintf(stderr, "planning reverse push to Transifex ...\n");
        txsync::ReversePushPlan plan = txsync::tx_reverse_push_plan(result, listTx, blameDates);
        print_reverse_push_plan(plan);
        if (!dryRun && !plan.push.empty()) {
            txsync::tx_apply_reverse_push(*txTok, plan,
                [](const txsync::ReversePushItem& it, bool ok, const std::string& msg) {
                    std::fprintf(stderr, "  %s %s  %s  %s -- %s\n", ok ? "OK" : "FAIL", it.lang.c_str(),
                                config::RESOURCES[it.res], it.msgctxt.c_str(), msg.c_str());
                });
        }
    }

    std::vector<github::FileEdit> edits = txsync::tx_build_edits(result.upstreamPoBytes, result);

    std::printf("target: %s/%s@%s\n", txOwner.c_str(), txRepo.c_str(), txBranch.c_str());
    std::printf("changed files: %zu\n", edits.size());
    for (auto& e : edits) std::printf("  %s\n", e.repo_path.c_str());

    if (dryRun) {
        std::fprintf(stderr, "dry-run: stopping before push\n");
        return 0;
    }
    if (edits.empty()) {
        std::printf("nothing to update\n");
        return 0;
    }

    std::string sha = github::update_transifex_branch(*tok, txOwner, txRepo, txBranch, edits,
        "Merge upstream develop + Transifex sync (Studio CLI)");
    std::printf("pushed: %s\n", sha.c_str());
    return 0;
}

// Reverse sync: pushes upstream develop's translations INTO the live Transifex DB wherever upstream
// should win over what Transifex currently has -- see mpctrans::txsync::tx_reverse_push_plan's
// doc comment for why (Transifex's GitHub integration is one-way DB->branch, so this is the only
// way a translator's PR merged into develop survives the next Transifex-DB sync).
int cmd_push_to_transifex(int argc, char** argv) {
    std::string poDir = "po";
    bool dryRun = false;
    std::string txOwner = config::TRANSIFEX_OWNER;
    std::string txRepo  = config::TRANSIFEX_REPO;
    std::string txBranch = config::TRANSIFEX_BRANCH;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a.empty()) continue;   // an empty argv entry (e.g. an unset $env var passed positionally) is not an option
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " needs an argument");
            return argv[++i];
        };
        if (a == "--po-dir")          poDir = next("--po-dir");
        else if (a == "--dry-run")    dryRun = true;
        else if (a == "--tx-owner")   txOwner = next("--tx-owner");
        else if (a == "--tx-repo")    txRepo = next("--tx-repo");
        else if (a == "--tx-branch")  txBranch = next("--tx-branch");
        else if (a == "--help" || a == "-h") { print_usage(); std::exit(0); }
        else throw std::runtime_error("unknown option: " + a);
    }

    std::vector<std::string> languages = enumerate_languages(poDir);
    if (languages.empty()) {
        std::fprintf(stderr,
            "error: no languages found under po-dir '%s' (expected mpc-hc.<lang>.dialogs.po files)\n",
            poDir.c_str());
        return 2;
    }
    std::fprintf(stderr, "found %zu language(s) under %s\n", languages.size(), poDir.c_str());

    std::optional<github::Token> tok = github::load_token();
    if (!tok) {
        if (const char* env = std::getenv("GITHUB_TOKEN"))
            tok = github::Token{ env };
    }
    if (!tok)
        throw std::runtime_error("no GitHub token: sign in once via the Studio GUI, or set GITHUB_TOKEN");

    std::optional<txapi::Token> txTok = load_transifex_token();
    if (!txTok)
        throw std::runtime_error("no Transifex token: sign in once via the Studio GUI, "
                                 "or set TRANSIFEX_TOKEN (or TRANSIFEX)");

    std::string login = github::whoami(*tok);
    std::fprintf(stderr, "signed in as %s\n", login.c_str());

    auto fetchUpstream = [&](const std::string& path) {
        return github::fetch_latest(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                    config::UPSTREAM_BRANCH, path);
    };
    auto fetchTx = [&](const std::string& path) {
        return github::fetch_latest(*tok, txOwner, txRepo, txBranch, path);
    };
    auto progress = [](int done, int total) {
        std::fprintf(stderr, "  fetching %d/%d\r", done, total);
    };
    std::fprintf(stderr, "computing sync against %s/%s@%s ...\n", txOwner.c_str(), txRepo.c_str(), txBranch.c_str());
    txsync::TxSyncResult result = txsync::tx_compute(fetchUpstream, fetchTx, languages, progress);
    std::fprintf(stderr, "\n");

    auto listTx = [&](int res, const std::string& lang) {
        return txapi::list_translations(*txTok, res, lang);
    };
    auto blameDates = [&](const std::string& path) {
        return github::blame_line_dates(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                        config::UPSTREAM_BRANCH, path);
    };
    std::fprintf(stderr, "planning reverse push to Transifex ...\n");
    txsync::ReversePushPlan plan = txsync::tx_reverse_push_plan(result, listTx, blameDates);
    print_reverse_push_plan(plan);

    if (dryRun) {
        std::fprintf(stderr, "dry-run: stopping before push\n");
        return 0;
    }
    if (plan.push.empty()) {
        std::printf("nothing to push\n");
        return 0;
    }

    int failCount = 0;
    txsync::tx_apply_reverse_push(*txTok, plan,
        [&](const txsync::ReversePushItem& it, bool ok, const std::string& msg) {
            std::printf("  %s %s  %s  %s -- %s\n", ok ? "OK" : "FAIL", it.lang.c_str(),
                        config::RESOURCES[it.res], it.msgctxt.c_str(), msg.c_str());
            if (!ok) ++failCount;
        });
    return failCount ? 1 : 0;
}

} // namespace

// Mirrors MainFrame::OnProposeTxSyncPr (minus the GUI-local drafts overlay): compute the merge, refuse
// to ship any strings.po whose Options tree is broken (the same hard gate as the GUI), then create the
// transifex-sync-<stamp> branch on the signed-in user's fork via github::open_pr. open_pr deliberately
// does NOT create the pull request -- it returns GitHub's compare URL; the caller (or a human) opens it.
int cmd_propose(int argc, char** argv) {
    std::string poDir = "po";
    bool dryRun = false;
    std::string txOwner = config::TRANSIFEX_OWNER;
    std::string txRepo  = config::TRANSIFEX_REPO;
    std::string txBranch = config::TRANSIFEX_BRANCH;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a.empty()) continue;
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " needs an argument");
            return argv[++i];
        };
        if (a == "--po-dir")               poDir = next("--po-dir");
        else if (a == "--dry-run")         dryRun = true;
        else if (a == "--tx-owner")        txOwner = next("--tx-owner");
        else if (a == "--tx-repo")         txRepo = next("--tx-repo");
        else if (a == "--tx-branch")       txBranch = next("--tx-branch");
        else if (a == "--help" || a == "-h") { print_usage(); std::exit(0); }
        else throw std::runtime_error("unknown option: " + a);
    }

    std::vector<std::string> languages = enumerate_languages(poDir);
    if (languages.empty()) {
        std::fprintf(stderr, "error: no languages found under po-dir '%s'\n", poDir.c_str());
        return 2;
    }
    std::fprintf(stderr, "found %zu language(s) under %s\n", languages.size(), poDir.c_str());

    std::optional<github::Token> tok = github::load_token();
    if (!tok) { if (const char* env = std::getenv("GITHUB_TOKEN")) tok = github::Token{ env }; }
    if (!tok) throw std::runtime_error("no GitHub token: sign in once via the Studio GUI, or set GITHUB_TOKEN");
    std::fprintf(stderr, "signed in as %s\n", github::whoami(*tok).c_str());

    auto fetchUpstream = [&](const std::string& path) {
        return github::fetch_latest(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO, config::UPSTREAM_BRANCH, path);
    };
    auto fetchTx = [&](const std::string& path) { return github::fetch_latest(*tok, txOwner, txRepo, txBranch, path); };
    auto progress = [](int done, int total) { std::fprintf(stderr, "  fetching %d/%d\r", done, total); };
    std::fprintf(stderr, "computing sync against %s/%s@%s ...\n", txOwner.c_str(), txRepo.c_str(), txBranch.c_str());
    txsync::TxSyncResult result = txsync::tx_compute(fetchUpstream, fetchTx, languages, progress);
    std::fprintf(stderr, "\n");

    std::vector<github::FileEdit> edits = txsync::tx_build_edits(result.upstreamPoBytes, result);

    // Hard gate (same as the GUI): never ship a strings.po whose Options tree would break.
    int gateFails = 0;
    for (const auto& e : edits) {
        const std::string suf = ".strings.po";
        if (e.repo_path.size() < suf.size() || e.repo_path.compare(e.repo_path.size() - suf.size(), suf.size(), suf) != 0) continue;
        for (const auto& f : validate::analyze_category_tree(PoFile::parse_bytes(e.content))) {
            if (!gateFails) std::printf("GATE: Options-tree defects in files that would ship:\n");
            std::printf("  %s  %s: %s\n", e.repo_path.c_str(), f.msgctxt.c_str(), f.message.c_str());
            ++gateFails;
        }
    }
    if (gateFails) {
        std::fprintf(stderr, "error: %d Options-tree defect(s) -- fix them at the source (Transifex) first\n", gateFails);
        return 4;
    }

    int nNew = 0, nWins = 0, nDiscarded = 0, nProtected = 0;
    for (const auto& d : result.decisions) {
        switch (d.kind) {
            case txsync::TxDecision::TxNew:     ++nNew; break;
            case txsync::TxDecision::TxWins:    ++nWins; break;
            case txsync::TxDecision::Discarded: ++nDiscarded; break;
            case txsync::TxDecision::Protected: ++nProtected; break;
        }
    }
    char body[512];
    std::snprintf(body, sizeof body,
        "Transifex sync: %d new, %d conflicts (Transifex wins), %d discarded (bad placeholders), "
        "%d protected upstream-only translations, %d unchanged.",
        nNew, nWins, nDiscarded, nProtected, result.unchanged);

    std::printf("changed files: %zu\n", edits.size());
    for (auto& e : edits) std::printf("  %s\n", e.repo_path.c_str());
    std::printf("summary: %s\n", body);

    if (dryRun) { std::fprintf(stderr, "dry-run: stopping before creating the branch\n"); return 0; }
    if (edits.empty()) { std::printf("nothing to propose\n"); return 0; }

    std::string url = github::open_pr(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO, config::UPSTREAM_BRANCH,
                                      "transifex-sync", edits, "Transifex updates", body);
    std::printf("compare: %s\n", url.c_str());   // branch is created; the PR itself is opened from this URL
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(); return 2; }
    std::string sub = argv[1];
    if (sub == "--help" || sub == "-h") { print_usage(); return 0; }

    try {
        if (sub == "refresh")            return cmd_refresh(argc - 2, argv + 2);
        if (sub == "update-branch")      return cmd_update_branch(argc - 2, argv + 2);
        if (sub == "push-to-transifex")  return cmd_push_to_transifex(argc - 2, argv + 2);
        if (sub == "propose")            return cmd_propose(argc - 2, argv + 2);
        print_usage(); return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
