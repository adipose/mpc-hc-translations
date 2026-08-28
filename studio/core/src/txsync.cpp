// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/txsync.h"
#include "mpctrans/config.h"
#include "mpctrans/po.h"
#include "mpctrans/validate.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace mpctrans::txsync {

namespace cfg = mpctrans::config;

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

} // namespace mpctrans::txsync
