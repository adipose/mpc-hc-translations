// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mpctrans/github.h"
#include "mpctrans/txapi.h"
#include "mpctrans/validate.h"

// Transifex -> upstream translation sync — C++ port of tools/transifex_sync.py (read that file's
// docstring first; the merge rules below MUST stay identical). Merges the fork's `transifex` branch
// translations onto upstream's CURRENT POs, per (msgctxt,msgid) entry:
//
//   transifex has a translation:
//     - placeholder check FAILS (validate::rule_format finds an Error)      -> Discarded, keep upstream
//     - upstream has a DIFFERENT non-empty translation                     -> TxWins (transifex wins)
//     - upstream is empty                                                  -> TxNew
//     - otherwise                                                          -> ++unchanged (no decision row)
//   transifex has NO translation (absent key, or empty msgstr):
//     - upstream has one                                                   -> Protected (kept, not overwritten)
//     - otherwise                                                          -> ++untranslated (no decision row)
//
// The merged output's entry set/order is upstream's (obsolete transifex-only keys drop out) — see
// PoFile::splice, which tx_build_edits reuses for this exact reason.

namespace mpctrans::txsync {

struct TxDecision {
    std::string lang;
    int res = 0;   // index into config::RESOURCES (0=dialogs, 1=menus, 2=strings)
    std::string msgctxt, msgid, upstreamStr, txStr;
    enum Kind { TxNew, TxWins, Discarded, Protected } kind = TxNew;
    // Meaningful only for TxNew/TxWins (whether the row's translation is applied when building
    // edits); Discarded/Protected rows are never applied regardless of this flag — see tx_build_edits.
    bool include = true;
};

struct TxSyncResult {
    std::vector<TxDecision> decisions;
    int unchanged = 0, untranslated = 0;
    std::vector<std::string> languages;
    // DEVIATION from the spec's literal struct list: the spec's own tx_build_edits note says "let
    // tx_compute also return a std::map<std::string,std::string> upstreamPoBytes ... and have
    // tx_build_edits take that map instead of a fetcher" -- carrying it as a TxSyncResult member
    // (rather than a separate out-param/pair) keeps both functions' signatures simple. Keyed by the
    // repo-relative path (config::PO_DIR + "/mpc-hc.<lang>.<res>.po"); one entry per file that had a
    // successful upstream fetch (i.e. wasn't skipped).
    std::map<std::string, std::string> upstreamPoBytes;
};

// Fetches (upstream, transifex) bytes for repo_path = PO_DIR + "/mpc-hc.<lang>.<res>.po", for every
// language x resource. A nullopt from fetchUpstream skips that file entirely (no decisions, doesn't
// count toward unchanged/untranslated either) -- mirrors the python prototype's "tx_po = None ->
// file doesn't exist" tolerance, just applied to the upstream side (the side that can't be skipped
// there). A nullopt from fetchTx is treated as an empty PO (transifex hasn't touched this file yet).
// `progress(done, total)` fires once per file attempted (total = languages.size() * 3), regardless of
// whether that file was skipped -- so a caller's progress bar always reaches total.
TxSyncResult tx_compute(const std::function<std::optional<std::string>(const std::string& repo_path)>& fetchUpstream,
                        const std::function<std::optional<std::string>(const std::string& repo_path)>& fetchTx,
                        const std::vector<std::string>& languages,
                        std::function<void(int done, int total)> progress);

// Builds one github::FileEdit per changed file: groups decisions by (lang,res), keeps only
// TxNew/TxWins rows with include==true, splices their txStr onto the CACHED upstream bytes (via
// PoFile::splice — the same minimal-diff writer MainFrame::SubmitPr uses), and includes the file only
// if splice() actually changed something (a row whose txStr already equals what's on disk, or whose
// key vanished from upstream since tx_compute ran, produces no edit).
std::vector<github::FileEdit> tx_build_edits(const std::map<std::string, std::string>& upstreamPoBytes,
                                             const TxSyncResult& result);

struct CategoryMismatch { std::string lang; validate::CategoryFinding finding; };
// Runs analyze_category_tree on every language that has >=1 STRINGS (res==2, non-Protected) decision,
// against that language's merged strings .po (upstream bytes from result.upstreamPoBytes, with each
// decision's merged value overlaid: txStr for TxNew/TxWins, upstreamStr for Discarded).
std::vector<CategoryMismatch> find_category_mismatches(const TxSyncResult& result);

struct HeldTranslation { std::string lang, msgctxt, msgid, reason; };
// Like tx_build_edits, but for refreshing an OPEN sync PR: any translation that would break the
// Options tree (analyze_category_tree finds a defect in the resulting strings.po) is HELD BACK --
// reverted to the develop-base value -- so one bad late translation never stalls the good ones.
// Placeholder-invalid translations are already Discarded (not applied) by tx_build_edits. Reports
// every held string in `held`. Returns the FULL-file edits safe to push to the PR branch.
std::vector<github::FileEdit> tx_build_edits_validated(const TxSyncResult& result,
                                                       std::vector<HeldTranslation>& held);

// ---- reverse push: Studio -> Transifex ----
//
// Transifex's GitHub integration is ONE-WAY (DB -> the fork's `transifex` branch), so a translation
// that reaches upstream develop via a translator's PR gets reverted by the NEXT sync ("Transifex
// wins" on a TxWins/Protected conflict) unless the live Transifex DB is updated too. This plans (and
// applies) exactly that: pushing upstream develop's value into Transifex via its REST API wherever
// upstream should win, and flipping the corresponding TxDecision's `include` off so the SAME sync
// pass's merge doesn't turn around and revert it back onto disk.

struct ReversePushItem {
    std::string lang; int res;
    std::string msgctxt, msgid, upstreamStr, txStr, txId, reason;
};
struct ReversePushPlan {
    std::vector<ReversePushItem> push;           // upstream should overwrite Transifex
    std::vector<ReversePushItem> keepTransifex;  // Transifex's value wins (or no evidence either way)
    std::vector<std::string> notes;              // e.g. "language <lang> not in Transifex"
};

// Decides what upstream develop should push INTO Transifex so the two agree, consulting the LIVE DB
// via `listTx` (called at most once per (res,lang) group present in result.decisions) and per-file
// blame line evidence via `blameDates` (repo_path -> line->github::LineBlame map; may come back empty
// -- treated as "no evidence", never as an error). A blame line whose commit message matches
// config::TX_SYNC_COMMIT_MARKERS is a Transifex-sync commit, not a translator's edit -- its date is
// ignored and the row is kept as Transifex's (see the .cpp for the full rule). MUTATES `result`: a
// TxWins decision that loses to a newer (non-sync) upstream value, or that Transifex's live DB
// already agrees with, gets `include = false` so tx_build_edits/tx_build_edits_validated's merge
// keeps upstream's value instead of reverting it.
ReversePushPlan tx_reverse_push_plan(TxSyncResult& result,
    const std::function<std::vector<txapi::Translation>(int res, const std::string& lang)>& listTx,
    const std::function<std::map<int, github::LineBlame>(const std::string& repo_path)>& blameDates);

// Applies `plan.push`: PATCHes each item's Transifex value to upstreamStr, exact-match-guarded on
// txStr (list_translations is re-checked once per (res,lang) group so concurrent DB edits are never
// clobbered -- an item whose live value no longer equals what the plan saw is skipped, not forced).
// `report(item, ok, msg)` fires once per item ("pushed", a skip reason, or the PATCH failure).
void tx_apply_reverse_push(const txapi::Token&, const ReversePushPlan&,
    const std::function<void(const ReversePushItem&, bool ok, const std::string& msg)>& report);

} // namespace mpctrans::txsync
