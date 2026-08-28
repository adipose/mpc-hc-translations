// SPDX-License-Identifier: GPL-3.0-or-later
// C++ side of the validation CONTRACT — mirrors potool/tests/test_potool.py. If this and the
// Python suite ever disagree, one of the two implementations has drifted. Build & run standalone.
#include "mpctrans/validate.h"
#include "mpctrans/po.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace mpctrans::validate;
using mpctrans::PoFile;
static int fails = 0;

// Minimal strings .po: a blank header block (so parse_bytes doesn't eat a real entry as the header),
// then one block per (msgctxt,msgid,msgstr) triple.
static PoFile buildPo(const std::vector<std::tuple<std::string, std::string, std::string>>& entries) {
    std::string bytes = "msgid \"\"\nmsgstr \"\"\n\n";
    for (auto& [ctx, id, str] : entries)
        bytes += "msgctxt \"" + ctx + "\"\nmsgid \"" + id + "\"\nmsgstr \"" + str + "\"\n\n";
    return PoFile::parse_bytes(bytes);
}
static bool hasDefect(const std::vector<CategoryFinding>& f, CatDefect d) {
    return std::any_of(f.begin(), f.end(), [&](const CategoryFinding& x) { return x.defect == d; });
}

// true iff the ERROR-severity rule names exactly equal `want`
static void expect(const char* name, const std::vector<Finding>& f, std::vector<std::string> want) {
    std::vector<std::string> got;
    for (auto& x : f) if (x.sev == Severity::Error) got.push_back(x.rule);
    std::sort(got.begin(), got.end()); std::sort(want.begin(), want.end());
    if (got != want) { std::printf("FAIL %s\n", name); ++fails; }
}
static void expect_sev(const char* name, const std::vector<Finding>& f, const std::string& rule, Severity sev) {
    for (auto& x : f) if (x.rule == rule && x.sev == sev) return;
    std::printf("FAIL %s (missing %s)\n", name, rule.c_str()); ++fails;
}

int main() {
    expect("fmt ok",       rule_format("c", "Chapter %d", "Kapitel %d"), {});
    expect("fmt type",     rule_format("c", "Chapter %d", "Kapitel %s"), {"format-specifiers"});
    expect("fmt count",    rule_format("c", "%s of %s", "%s"),           {"format-specifiers"});
    expect("fmt order",    rule_format("c", "%s=%d", "%d=%s"),           {"format-order"});
    expect("fmt percent",  rule_format("c", "100%% done", "100%% fertig"), {});
    expect("fmt zoom",     rule_format("c", "50%", "%50"),               {});   // tr percentage, not a spec
    expect("fmt literal%", rule_format("c", "% of the animation", "%s animasi"), {});  // source has no spec

    expect("amp dropped",  rule_ampersand("c", "&File", "Datei"),        {});   // SOFT
    expect_sev("amp warn", rule_ampersand("c", "&File", "Datei"), "ampersand", Severity::Warning);
    expect("amp literal",  rule_ampersand("c", "a && b", "x && y"),      {});

    expect("nl lost",      rule_newline("c", "a\nb", "xy"),             {});   // SOFT
    expect_sev("nl warn",  rule_newline("c", "a\nb", "xy"), "newline", Severity::Warning);
    expect_sev("empty",    rule_empty("c", "Hi", ""), "empty", Severity::Info);

    // -- Options-tree category consistency (analyze_category_tree) --
    {
        // 1. disagreement: same English parent "Internal Filters" translated two different ways.
        PoFile po = buildPo({
            { "IDD_PPAGEINTERNALFILTERS", "Internal Filters::General", "K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos" },
            { "IDD_PPAGEAUDIOSWITCHER",   "Internal Filters::Audio Switcher", "Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::Audi\xC3\xB3-v\xC3\xA1lt\xC3\xB3" },
        });
        auto f = analyze_category_tree(po);
        if (f.size() < 2) { std::printf("FAIL cat disagreement (expected >=2 findings, got %zu)\n", f.size()); ++fails; }
        if (!hasDefect(f, CatDefect::Disagreement)) { std::printf("FAIL cat disagreement (missing Disagreement)\n"); ++fails; }
    }
    {
        // 2. collision: "External Filters" and "Internal Filters" both translate their parent to the
        // same Hungarian text -- the two branches would merge in the Options tree.
        PoFile po = buildPo({
            { "IDD_PPAGEEXTERNALFILTERS", "External Filters", "K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k" },
            { "IDD_PPAGEINTERNALFILTERS", "Internal Filters::General", "K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos" },
        });
        auto f = analyze_category_tree(po);
        if (!hasDefect(f, CatDefect::Collision)) { std::printf("FAIL cat collision (missing Collision)\n"); ++fails; }
    }
    {
        // 3. missing separator: msgid has "::" but msgstr doesn't -- category can't be split off.
        PoFile po = buildPo({
            { "IDD_PPAGEPLAYER", "Player::General", "Lej\xC3\xA1tsz\xC3\xB3" },
        });
        auto f = analyze_category_tree(po);
        if (!hasDefect(f, CatDefect::MissingSeparator)) { std::printf("FAIL cat missing-separator\n"); ++fails; }
    }
    {
        // 4. clean: two pages under the same English category, translated identically -- no findings.
        PoFile po = buildPo({
            { "IDD_PPAGEINTERNALFILTERS", "Internal Filters::General", "Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos" },
            { "IDD_PPAGEAUDIOSWITCHER",   "Internal Filters::Audio Switcher", "Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::Audi\xC3\xB3-v\xC3\xA1lt\xC3\xB3" },
        });
        auto f = analyze_category_tree(po);
        if (!f.empty()) { std::printf("FAIL cat clean (expected 0 findings, got %zu)\n", f.size()); ++fails; }
    }

    if (fails) std::printf("%d case(s) FAILED\n", fails);
    else       std::printf("all C++ conformance cases passed\n");
    return fails ? 1 : 0;
}
