// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the sqlite enrichment readers against the real artifacts:
//   CORE  dist/core-enrichment.sqlite (committed) — SKIPped when absent.
//   PACKS enrichment/lang/*.sqlite (committed) — every pack must open; across all packs the
//         core string ids must surface AI suggestions and reference matches.
//   Usage: enrichment_test [CORE_SQLITE] [LANG_DIR]
#include "mpctrans/enrichment.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using namespace mpctrans;

int main(int argc, char** argv) {
    std::string core_path = argc > 1 ? argv[1] : "dist/core-enrichment.sqlite";
    std::string lang_dir  = argc > 2 ? argv[2] : "enrichment/lang";
    int fail = 0;
    auto err = [&](const std::string& m) { if (++fail <= 10) std::printf("  FAIL %s\n", m.c_str()); };

    // ---- core catalog ----
    std::vector<StringInfo> catalog;
    if (!fs::exists(core_path)) {
        std::printf("SKIP core: %s not found\n", core_path.c_str());
    } else {
        CoreEnrichment core = CoreEnrichment::open(core_path);
        catalog = core.all();
        if (catalog.size() < 1000) err("suspiciously small strings catalog");
        for (const auto& si : catalog) {
            auto a = core.by_id(si.id);
            if (!a || a->msgctxt != si.msgctxt || a->msgid != si.msgid)
                { err("by_id mismatch for id " + std::to_string(si.id)); break; }
            auto b = core.by_key(si.msgctxt, si.msgid);
            if (!b || b->id != si.id)   // (msgctxt,msgid) is the system-wide key -> must be unique
                { err("by_key mismatch for " + si.msgctxt); break; }
        }
        if (core.by_id(-42)) err("bogus id resolved");
        if (core.by_key("~NOPE~", "~nope~")) err("bogus key resolved");
        std::printf("core: %zu strings, by_id/by_key roundtrip OK\n", catalog.size());

        // ---- M3: language_fidelity() -- must never throw/crash; empty is a valid, non-error result
        //      (older export, or the lab snapshot's view was missing when it was built) ----
        auto fidelity = core.language_fidelity();
        if (fidelity.empty()) {
            std::printf("language_fidelity: 0 rows (table absent from this export) -- OK\n");
        } else {
            bool sorted = true;
            for (size_t i = 1; i < fidelity.size(); ++i) {
                if (fidelity[i].pct > fidelity[i - 1].pct) { sorted = false; break; }
            }
            if (!sorted) err("language_fidelity: rows not sorted descending by pct");
            for (const auto& lf : fidelity) {
                if (lf.language.empty()) { err("language_fidelity: empty language string"); break; }
            }
            std::printf("language_fidelity: %zu rows, sorted desc: %s\n",
                        fidelity.size(), sorted ? "OK" : "FAIL");
        }
    }

    // ---- per-language packs ----
    if (!fs::exists(lang_dir)) { std::printf("SKIP packs: %s not found\n", lang_dir.c_str()); }
    else {
        int packs = 0; long long ai_total = 0, ref_total = 0;
        for (auto& p : fs::directory_iterator(lang_dir)) {
            if (p.path().extension() != ".sqlite") continue;
            ++packs;
            LangPack lp = LangPack::open(p.path().string());
            if (!catalog.empty()) {
                for (const auto& si : catalog) {
                    for (auto& s : lp.ai_for(si.id)) {
                        ++ai_total;
                        if (s.string_id != si.id || s.msgstr.empty()) { err("bad ai row"); break; }
                    }
                    for (auto& r : lp.refs_for(si.id)) {
                        ++ref_total;
                        if (r.string_id != si.id || r.source.empty()) { err("bad ref row"); break; }
                    }
                }
            }
            if (!lp.ai_for(-42).empty() || !lp.refs_for(-42).empty()) err("bogus string_id resolved");
        }
        if (packs < 40) err("suspiciously few lang packs");
        if (!catalog.empty() && ai_total == 0) err("no AI suggestions surfaced from any pack");
        if (!catalog.empty() && ref_total == 0) err("no reference matches surfaced from any pack");
        std::printf("packs: %d opened, %lld ai suggestions, %lld reference matches: %s\n",
                    packs, ai_total, ref_total, fail ? "FAIL" : "OK");
    }
    return fail ? 1 : 0;
}
