// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/txsync.h"
#include "mpctrans/config.h"
#include "mpctrans/po.h"
#include "mpctrans/validate.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace mpctrans::txsync {

namespace cfg = mpctrans::config;

namespace {
// True if `message` (a blame commit's first line) matches any of config::TX_SYNC_COMMIT_MARKERS,
// case-insensitively -- i.e. the blamed line was landed by a Transifex-sync commit, not authored by
// a translator, so its date is not evidence of "upstream is newer" (see tx_reverse_push_plan).
bool is_tx_sync_commit(const std::string& message) {
    std::string lower = message;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    for (const auto& marker : cfg::TX_SYNC_COMMIT_MARKERS) {
        std::string m = marker;
        for (auto& c : m) c = (char)std::tolower((unsigned char)c);
        if (!m.empty() && lower.find(m) != std::string::npos) return true;
    }
    return false;
}
} // namespace

// Credits the merged msgstr blocks to a synthetic "translator" (there's no single human author for a
// bulk sync) — PoFile::splice only bumps PO-Revision-Date/Last-Translator/# Translators: when at least
// one entry actually changed, same as every other splice() call site (MainFrame::SubmitPr).
static const char kTranslatorCredit[] = "Transifex sync (Studio)";

TxSyncResult tx_compute(const std::function<std::optional<std::string>(const std::string&)>& fetchUpstream,
                        const std::function<std::optional<std::string>(const std::string&)>& fetchTx,
                        const std::vector<std::string>& languages,
                        std::function<void(int done, int total)> progress) {
    TxSyncResult result;
    result.languages = languages;
    const int NRES = (int)(sizeof(cfg::RESOURCES) / sizeof(cfg::RESOURCES[0]));
    int total = (int)languages.size() * NRES;

    // Flatten the (lang,res) grid into a work list. Each item is TWO network fetches (upstream + tx).
    struct Item { std::string lang; int res; std::string path; };
    std::vector<Item> items;
    items.reserve(total);
    for (const auto& lang : languages)
        for (int res = 0; res < NRES; ++res)
            items.push_back({ lang, res, std::string(cfg::PO_DIR) + "/mpc-hc." + lang + "." +
                                          cfg::RESOURCES[res] + ".po" });

    // Prefetch every file CONCURRENTLY. The old code fetched all 264 files one-at-a-time, each opening
    // a fresh TLS connection -- so wall-clock was ~264 x (handshake + round-trip) even though every
    // fetch is independent (raw.githubusercontent.com is a CDN, no ordering, no server state). A bounded
    // pool overlaps that latency: N in flight at once turns a ~40s serial crawl into a few seconds. The
    // merge below stays a pure-CPU pass over the fetched bytes. (Not git-fast -- git ships one delta
    // pack over one connection -- but the same order of magnitude for this many small files.)
    struct Fetched { std::optional<std::string> up; std::string tx; bool haveUp = false; };
    std::vector<Fetched> fetched(items.size());
    std::atomic<size_t> next{ 0 };
    std::atomic<int> done{ 0 };
    std::mutex progressMx;
    unsigned hw = std::thread::hardware_concurrency();
    int workers = (int)std::min<size_t>(items.size(), hw ? std::min(hw * 2u, 16u) : 12u);
    auto worker = [&]() {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= items.size()) break;
            auto up = fetchUpstream(items[i].path);
            if (up) { fetched[i].up = std::move(up); fetched[i].haveUp = true;
                      fetched[i].tx = fetchTx(items[i].path).value_or(std::string()); }
            int d = ++done;
            if (progress) { std::lock_guard<std::mutex> lk(progressMx); progress(d, total); }
        }
    };
    std::vector<std::thread> pool;
    for (int w = 0; w < workers; ++w) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    // Merge pass -- deterministic (grid order), reads only the prefetched bytes, no network.
    for (size_t i = 0; i < items.size(); ++i) {
        const std::string& lang = items[i].lang;
        const int res = items[i].res;
        const std::string& path = items[i].path;
        if (!fetched[i].haveUp) continue;   // file absent upstream -> skipped, like before

        result.upstreamPoBytes[path] = *fetched[i].up;
        PoFile upPo = PoFile::parse_bytes(*fetched[i].up);
        PoFile txPo = PoFile::parse_bytes(fetched[i].tx);

        {
            for (const auto& e : upPo.entries) {
                const PoEntry* txEntry = txPo.find(e.msgctxt, e.msgid);
                const std::string& up = e.msgstr;
                std::string tx = txEntry ? txEntry->msgstr : std::string();

                if (!tx.empty()) {
                    bool badPlaceholders = false;
                    for (const auto& f : validate::rule_format(e.msgctxt, e.msgid, tx))
                        if (f.sev == validate::Severity::Error) { badPlaceholders = true; break; }
                    TxDecision d;
                    d.lang = lang; d.res = res; d.msgctxt = e.msgctxt; d.msgid = e.msgid;
                    d.upstreamStr = up; d.txStr = tx;
                    if (badPlaceholders) { d.kind = TxDecision::Discarded; d.include = false; result.decisions.push_back(d); }
                    else if (!up.empty() && up != tx) { d.kind = TxDecision::TxWins; d.include = true; result.decisions.push_back(d); }
                    else if (up.empty()) { d.kind = TxDecision::TxNew; d.include = true; result.decisions.push_back(d); }
                    else ++result.unchanged;
                } else {
                    if (!up.empty()) {
                        TxDecision d;
                        d.lang = lang; d.res = res; d.msgctxt = e.msgctxt; d.msgid = e.msgid;
                        d.upstreamStr = up; d.txStr = std::string();
                        d.kind = TxDecision::Protected; d.include = false;
                        result.decisions.push_back(d);
                    } else ++result.untranslated;
                }
            }
        }
    }
    return result;
}

std::vector<github::FileEdit> tx_build_edits(const std::map<std::string, std::string>& upstreamPoBytes,
                                             const TxSyncResult& result) {
    // group applied decisions by the file they belong to (lang + res -> repo path)
    std::map<std::string, std::vector<PoEntry>> editsByPath;
    for (const auto& d : result.decisions) {
        if ((d.kind != TxDecision::TxNew && d.kind != TxDecision::TxWins) || !d.include) continue;
        std::string path = std::string(cfg::PO_DIR) + "/mpc-hc." + d.lang + "." +
                           cfg::RESOURCES[d.res] + ".po";
        editsByPath[path].push_back(PoEntry{ d.msgctxt, d.msgid, d.txStr, {} });
    }

    std::vector<github::FileEdit> out;
    for (const auto& [path, edits] : editsByPath) {
        auto it = upstreamPoBytes.find(path);
        if (it == upstreamPoBytes.end()) continue;   // shouldn't happen: every decision came from a fetched file
        std::string spliced = PoFile::splice(it->second, edits, kTranslatorCredit);
        if (spliced != it->second) out.push_back({ path, spliced });
    }
    return out;
}

std::vector<CategoryMismatch> find_category_mismatches(const TxSyncResult& result) {
    std::vector<CategoryMismatch> out;

    // group non-Protected STRINGS (res==2) decisions by lang, preserving first-seen language order.
    std::map<std::string, std::vector<const TxDecision*>> byLang;
    std::vector<std::string> langOrder;
    for (const auto& d : result.decisions) {
        if (d.res != 2 || d.kind == TxDecision::Protected) continue;
        auto& v = byLang[d.lang];
        if (v.empty() && std::find(langOrder.begin(), langOrder.end(), d.lang) == langOrder.end())
            langOrder.push_back(d.lang);
        v.push_back(&d);
    }

    for (const auto& lang : langOrder) {
        std::string path = std::string(cfg::PO_DIR) + "/mpc-hc." + lang + "." + cfg::RESOURCES[2] + ".po";
        auto it = result.upstreamPoBytes.find(path);
        if (it == result.upstreamPoBytes.end()) continue;

        PoFile po = PoFile::parse_bytes(it->second);
        for (const auto* d : byLang[lang]) {
            const std::string& merged = (d->kind == TxDecision::Discarded) ? d->upstreamStr : d->txStr;
            for (auto& e : po.entries) {
                if (e.msgctxt == d->msgctxt && e.msgid == d->msgid) { e.msgstr = merged; break; }
            }
        }
        for (auto& f : validate::analyze_category_tree(po))
            out.push_back({ lang, f });
    }

    return out;
}

std::vector<github::FileEdit> tx_build_edits_validated(const TxSyncResult& result,
                                                       std::vector<HeldTranslation>& held) {
    auto edits = tx_build_edits(result.upstreamPoBytes, result);
    std::vector<github::FileEdit> out;
    for (auto& e : edits) {
        const std::string suf = ".strings.po";
        bool isStrings = e.repo_path.size() >= suf.size() &&
                         e.repo_path.compare(e.repo_path.size()-suf.size(), suf.size(), suf) == 0;
        if (!isStrings) { out.push_back(e); continue; }
        PoFile po = PoFile::parse_bytes(e.content);
        auto merged = validate::analyze_category_tree(po);
        if (merged.empty()) { out.push_back(e); continue; }   // clean merge -> ship as-is

        // lang between "mpc-hc." and ".strings.po"
        size_t a = e.repo_path.rfind("mpc-hc."); size_t b = e.repo_path.rfind(".strings.po");
        std::string lang = (a==std::string::npos) ? "" : e.repo_path.substr(a+7, b-(a+7));

        auto baseIt = result.upstreamPoBytes.find(e.repo_path);
        if (baseIt == result.upstreamPoBytes.end()) { out.push_back(e); continue; }   // can't classify -> ship (NEVER drop)
        PoFile base = PoFile::parse_bytes(baseIt->second);

        // INVARIANT: a refresh must never REMOVE translations. Dropping a file drops it back to the
        // develop tree (the commit's base_tree), silently wiping every translation the PR carried --
        // the bug tsubasanouta caught. So we only ever revert the SPECIFIC strings whose NEW value
        // breaks a tree that develop had CLEAN; a defect that already exists on develop (base) is
        // develop's problem, shipped as-is with its translation intact, never a reason to hold.
        std::set<std::pair<std::string,std::string>> baseFlagged;
        for (auto& f : validate::analyze_category_tree(base)) baseFlagged.insert({ f.msgctxt, f.msgid });
        std::set<std::pair<std::string,std::string>> newlyFlagged;
        for (auto& f : merged)
            if (!baseFlagged.count({ f.msgctxt, f.msgid })) newlyFlagged.insert({ f.msgctxt, f.msgid });

        if (newlyFlagged.empty()) { out.push_back(e); continue; }   // only pre-existing defects -> ship full merge

        // Revert ONLY the newly-breaking strings to base; keep every other translation.
        std::vector<PoEntry> keep;
        for (auto& ent : po.entries) {
            if (ent.msgid.empty()) continue;   // header
            const PoEntry* be = base.find(ent.msgctxt, ent.msgid);
            std::string baseStr = be ? be->msgstr : std::string();
            if (ent.msgstr == baseStr) continue;                  // unchanged vs base
            if (newlyFlagged.count({ ent.msgctxt, ent.msgid })) { // the NEW breaker -> hold just this string
                held.push_back({ lang, ent.msgctxt, ent.msgid, "held: would newly break the Options tree" });
                continue;
            }
            keep.push_back(PoEntry{ ent.msgctxt, ent.msgid, ent.msgstr, {} });
        }
        std::string repaired = PoFile::splice(baseIt->second, keep, "Transifex sync refresh (Studio)");
        out.push_back(github::FileEdit{ e.repo_path, repaired });   // always ship -- keeps all non-breaking translations
    }
    return out;
}

// ---- reverse push: Studio -> Transifex ----

ReversePushPlan tx_reverse_push_plan(TxSyncResult& result,
    const std::function<std::vector<txapi::Translation>(int res, const std::string& lang)>& listTx,
    const std::function<std::map<int, github::LineBlame>(const std::string& repo_path)>& blameDates) {
    ReversePushPlan plan;

    // Group decision INDICES by (lang,res) -- listTx is consulted once per group, and we mutate
    // result.decisions[i].include in place (can't do that through a copied TxDecision).
    std::map<std::pair<std::string, int>, std::vector<size_t>> groups;
    for (size_t i = 0; i < result.decisions.size(); ++i) {
        const auto& d = result.decisions[i];
        if (d.kind != TxDecision::Protected && d.kind != TxDecision::TxWins) continue;   // TxNew/Discarded: nothing
        groups[{ d.lang, d.res }].push_back(i);
    }

    std::map<std::string, std::map<int, github::LineBlame>> blameCache;   // repo_path -> line->blame (lazy, once each)
    auto getBlame = [&](const std::string& path) -> const std::map<int, github::LineBlame>& {
        auto it = blameCache.find(path);
        if (it == blameCache.end()) it = blameCache.emplace(path, blameDates(path)).first;
        return it->second;
    };

    for (auto& [key, idxs] : groups) {
        const std::string& lang = key.first;
        int res = key.second;
        std::vector<txapi::Translation> txList = listTx(res, lang);
        if (txList.empty()) {
            plan.notes.push_back("language " + lang + " not in Transifex");
            continue;
        }
        // Keyed on context+"\x1f"+source (== .po msgctxt+msgid), NOT context alone -- msgctxt is not
        // globally unique (e.g. sk's dialogs .po has two different strings both under
        // IDD_GOTO_DLG_IDC_STATIC), so a context-only join silently collapses distinct rows onto
        // whichever happened to be inserted last.
        auto joinKey = [](const std::string& ctx, const std::string& msgid) { return ctx + "\x1f" + msgid; };
        std::map<std::string, const txapi::Translation*> byKey;
        for (auto& t : txList) byKey[joinKey(t.context, t.source)] = &t;

        std::string path = std::string(cfg::PO_DIR) + "/mpc-hc." + lang + "." + cfg::RESOURCES[res] + ".po";

        // Both non-empty and differing (TxWins as computed, or a Protected row Transifex has since
        // filled in with something other than upstream's value): resolve via blame-line evidence.
        auto resolveConflict = [&](TxDecision& d, const txapi::Translation& t) {
            if (t.value == d.upstreamStr) { d.include = false; return; }   // DB already agrees w/ upstream

            const auto& blame = getBlame(path);
            int line = -1;
            auto pb = result.upstreamPoBytes.find(path);
            if (pb != result.upstreamPoBytes.end()) line = msgstr_line(pb->second, d.msgctxt, d.msgid);
            const github::LineBlame* lb = nullptr;
            if (line > 0) { auto it = blame.find(line); if (it != blame.end()) lb = &it->second; }

            // A blame commit whose message names a Transifex sync landed this line by MERGING/
            // REBASING Transifex's state onto develop -- committedDate is when that sync ran, not
            // when the translation was authored, and the VALUE it carried was Transifex's state AT
            // that time (which may be older than the live DB). Neither the date nor the fact upstream
            // "has" a value at that line is evidence upstream should win here -- keep Transifex's.
            if (lb && !lb->date.empty() && is_tx_sync_commit(lb->message)) {
                std::string sha7 = lb->sha.substr(0, std::min<size_t>(7, lb->sha.size()));
                plan.keepTransifex.push_back({ d.lang, d.res, d.msgctxt, d.msgid, d.upstreamStr, t.value, t.id,
                    "upstream line from a Transifex-sync commit " + sha7 + " '" + lb->message + "'" });
                return;   // include NOT flipped -- this isn't "upstream is newer" evidence
            }

            std::string upDate = lb ? lb->date : std::string();
            const std::string& txDate = t.datetime_translated;

            if (!upDate.empty() && !txDate.empty() && upDate > txDate) {   // ISO-8601 UTC: lexicographic == chronological
                std::string sha7 = lb ? lb->sha.substr(0, std::min<size_t>(7, lb->sha.size())) : std::string();
                std::string audit = lb ? (" (" + sha7 + " '" + lb->message + "')") : std::string();
                plan.push.push_back({ d.lang, d.res, d.msgctxt, d.msgid, d.upstreamStr, t.value, t.id,
                                      "upstream newer: " + upDate + " > " + txDate + audit });
                d.include = false;
            } else {
                std::string reason = (!upDate.empty() && !txDate.empty())
                                    ? "transifex newer" : "no date -- transifex wins (conservative)";
                plan.keepTransifex.push_back({ d.lang, d.res, d.msgctxt, d.msgid, d.upstreamStr, t.value, t.id, reason });
            }
        };

        for (size_t i : idxs) {
            TxDecision& d = result.decisions[i];
            auto it = byKey.find(joinKey(d.msgctxt, d.msgid));
            if (it == byKey.end()) {
                plan.notes.push_back("no Transifex row for " + lang + " " + d.msgctxt);
                continue;
            }
            const txapi::Translation& t = *it->second;

            if (d.kind == TxDecision::Protected) {
                if (t.value.empty()) {
                    plan.push.push_back({ d.lang, d.res, d.msgctxt, d.msgid, d.upstreamStr, t.value, t.id, "transifex empty" });
                    continue;
                }
                if (t.value == d.upstreamStr) continue;   // DB caught up since tx_compute ran -- nothing to do
                resolveConflict(d, t);                    // translated to something ELSE since -- treat as a conflict
                continue;
            }
            // TxWins
            resolveConflict(d, t);
        }
    }
    return plan;
}

void tx_apply_reverse_push(const txapi::Token& tok, const ReversePushPlan& plan,
    const std::function<void(const ReversePushItem&, bool ok, const std::string& msg)>& report) {
    // list_translations is expensive -- cache it per (res,lang), not per item.
    std::map<std::pair<int, std::string>, std::vector<txapi::Translation>> cache;

    for (const auto& item : plan.push) {
        auto key = std::make_pair(item.res, item.lang);
        auto it = cache.find(key);
        if (it == cache.end())
            it = cache.emplace(key, txapi::list_translations(tok, item.res, item.lang)).first;

        // Same (context, source msgid) join as tx_reverse_push_plan -- context alone can collide.
        const txapi::Translation* cur = nullptr;
        for (auto& t : it->second)
            if (t.context == item.msgctxt && t.source == item.msgid) { cur = &t; break; }
        if (!cur) { if (report) report(item, false, "no longer present in Transifex"); continue; }
        if (cur->value != item.txStr) {
            if (report) report(item, false, "DB changed to '" + cur->value + "'");
            continue;
        }
        try {
            txapi::patch_translation(tok, item.txId, item.upstreamStr);
            if (report) report(item, true, "pushed");
        } catch (const std::exception& e) {
            if (report) report(item, false, e.what());
        }
    }
}

} // namespace mpctrans::txsync
