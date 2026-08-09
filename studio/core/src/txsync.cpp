// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/txsync.h"
#include "mpctrans/config.h"
#include "mpctrans/po.h"
#include "mpctrans/validate.h"

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
    int total = (int)languages.size() * (int)(sizeof(cfg::RESOURCES) / sizeof(cfg::RESOURCES[0]));
    int done = 0;

    for (const auto& lang : languages) {
        for (int res = 0; res < (int)(sizeof(cfg::RESOURCES) / sizeof(cfg::RESOURCES[0])); ++res) {
            std::string path = std::string(cfg::PO_DIR) + "/mpc-hc." + lang + "." +
                               cfg::RESOURCES[res] + ".po";
            auto upBytes = fetchUpstream(path);
            if (!upBytes) { ++done; if (progress) progress(done, total); continue; }   // file skipped
            auto txBytesOpt = fetchTx(path);
            std::string txBytes = txBytesOpt.value_or(std::string());   // absent tx file -> empty PO

            result.upstreamPoBytes[path] = *upBytes;
            PoFile upPo = PoFile::parse_bytes(*upBytes);
            PoFile txPo = PoFile::parse_bytes(txBytes);

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
            ++done; if (progress) progress(done, total);
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

} // namespace mpctrans::txsync
