// SPDX-License-Identifier: GPL-3.0-or-later
// BAD-TRANSLATION SCENARIOS — feeds deliberately-wrong translations through the REAL detection
// functions (mpctrans::validate) and prints what each one catches. This is not a conformance
// contract like validate_conformance_test.cpp (which mirrors potool/tests/test_potool.py 1:1);
// it's a readable "here's a bad translation, here's what fires" demo suite that still hard-asserts
// the expected defect on every scenario (nonzero exit on any failure). Build & run standalone.
#include "mpctrans/validate.h"
#include "mpctrans/po.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace mpctrans::validate;
using mpctrans::PoFile;
static int fails = 0;

static void banner(const char* title) {
    std::printf("\n== %s ==\n", title);
}
static void ok(const char* what) {
    std::printf("  PASS: %s\n", what);
}
static void bad(const char* what) {
    std::printf("  FAIL: %s\n", what);
    ++fails;
}
static std::string join(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
    return s + "]";
}

// Minimal strings .po: a blank header block (so parse_bytes doesn't eat a real entry as the
// header), then one block per (msgctxt,msgid,msgstr) triple. Same helper as validate_conformance_test.
static PoFile buildPo(const std::vector<std::tuple<std::string, std::string, std::string>>& entries) {
    std::string bytes = "msgid \"\"\nmsgstr \"\"\n\n";
    for (auto& [ctx, id, str] : entries)
        bytes += "msgctxt \"" + ctx + "\"\nmsgid \"" + id + "\"\nmsgstr \"" + str + "\"\n\n";
    return PoFile::parse_bytes(bytes);
}
static const CategoryFinding* findDefect(const std::vector<CategoryFinding>& f, CatDefect d) {
    for (auto& x : f) if (x.defect == d) return &x;
    return nullptr;
}
static bool hasError(const std::vector<Finding>& f, const std::string& rule) {
    for (auto& x : f) if (x.sev == Severity::Error && x.rule == rule) return true;
    return false;
}
static bool hasAnyError(const std::vector<Finding>& f) {
    for (auto& x : f) if (x.sev == Severity::Error) return true;
    return false;
}

int main() {
    std::printf("BAD TRANSLATION SCENARIOS -- exercising the real mpctrans::validate detectors\n");

    // =====================================================================================
    banner("A. Bad prefixes (Options tree) -- analyze_category_tree");
    // =====================================================================================
    {
        // A1. Disagreement: the SAME English parent "Internal Filters" gets translated two
        // different ways ("Kulso szurok" == "External filters" leaked into one branch).
        std::printf("\n-- A1. Disagreement: \"Internal Filters\" translated two different ways --\n");
        PoFile po = buildPo({
            { "IDD_PPAGEINTERNALFILTERS", "Internal Filters::General",
              "K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos" },  // "External filters::General" (WRONG)
            { "IDD_PPAGEAUDIOSWITCHER",   "Internal Filters::Audio Switcher",
              "Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::Audio" },                          // "Internal filters::Audio" (correct-ish)
        });
        std::printf("  IDD_PPAGEINTERNALFILTERS  \"Internal Filters::General\" -> \"K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos\"\n");
        std::printf("  IDD_PPAGEAUDIOSWITCHER    \"Internal Filters::Audio Switcher\" -> \"Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::Audio\"\n");
        auto f = analyze_category_tree(po);
        auto* d = findDefect(f, CatDefect::Disagreement);
        if (d) { ok("Disagreement fired"); std::printf("    -> %s\n", d->message.c_str()); }
        else   bad("expected a Disagreement finding");
    }
    {
        // A2. Collision: "External Filters" and "Internal Filters" both translate their parent
        // to the SAME Hungarian text -- the two branches would merge in the Options tree.
        std::printf("\n-- A2. Collision: two different English parents translate to the same text --\n");
        PoFile po = buildPo({
            { "IDD_PPAGEEXTERNALFILTERS", "External Filters",
              "K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k" },
            { "IDD_PPAGEINTERNALFILTERS", "Internal Filters::General",
              "K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos" },   // same parent text as above (WRONG)
        });
        std::printf("  IDD_PPAGEEXTERNALFILTERS  \"External Filters\" -> \"K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k\"\n");
        std::printf("  IDD_PPAGEINTERNALFILTERS  \"Internal Filters::General\" -> \"K\xC3\xBCls\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos\"\n");
        auto f = analyze_category_tree(po);
        auto* d = findDefect(f, CatDefect::Collision);
        if (d) { ok("Collision fired"); std::printf("    -> %s\n", d->message.c_str()); }
        else   bad("expected a Collision finding");
    }
    {
        // A3. Missing separator: msgid has "::" but msgstr doesn't -- the category can't be
        // split off the page title, so the page would land at the tree ROOT instead of nested.
        std::printf("\n-- A3. Missing separator: msgid has \"::\", msgstr doesn't --\n");
        PoFile po = buildPo({
            { "IDD_PPAGEPLAYER", "Player::General", "Lej\xC3\xA1tsz\xC3\xB3" },   // no "::" at all
        });
        std::printf("  IDD_PPAGEPLAYER  \"Player::General\" -> \"Lej\xC3\xA1tsz\xC3\xB3\" (no \"::\")\n");
        auto f = analyze_category_tree(po);
        auto* d = findDefect(f, CatDefect::MissingSeparator);
        if (d) { ok("MissingSeparator fired"); std::printf("    -> %s\n", d->message.c_str()); }
        else   bad("expected a MissingSeparator finding");
    }
    {
        // A4. Clean control: two pages under the same English category, translated identically.
        std::printf("\n-- A4. Clean control: consistent translation of the same parent --\n");
        PoFile po = buildPo({
            { "IDD_PPAGEINTERNALFILTERS", "Internal Filters::General",
              "Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos" },
            { "IDD_PPAGEAUDIOSWITCHER",   "Internal Filters::Audio Switcher",
              "Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::Audio" },
        });
        std::printf("  IDD_PPAGEINTERNALFILTERS  \"Internal Filters::General\" -> \"Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::\xC3\x81ltal\xC3\xA1nos\"\n");
        std::printf("  IDD_PPAGEAUDIOSWITCHER    \"Internal Filters::Audio Switcher\" -> \"Bels\xC5\x91 sz\xC5\xB1r\xC5\x91k::Audio\"\n");
        auto f = analyze_category_tree(po);
        if (f.empty()) ok("zero findings, as expected");
        else { bad("expected zero findings"); for (auto& x : f) std::printf("    unexpected: %s\n", x.message.c_str()); }
    }
    std::printf("\n(The Propose/Update-branch gate runs analyze_category_tree on each shipped strings.po\n"
                " and refuses the merge when it returns any finding.)\n");

    // =====================================================================================
    banner("B. Placeholder mismatch -- rule_format / format_specs");
    // =====================================================================================
    {
        // B1. Dropped specifier: the translation drops one of two "%s" -- runtime substitution
        // would either crash or silently swallow an argument.
        std::printf("\n-- B1. Dropped specifier --\n");
        std::string id = "%s of %s", str = "%s";
        std::printf("  msgid  \"%s\"  specs = %s\n", id.c_str(), join(format_specs(id)).c_str());
        std::printf("  msgstr \"%s\"          specs = %s\n", str.c_str(), join(format_specs(str)).c_str());
        auto f = rule_format("c", id, str);
        if (hasError(f, "format-specifiers")) ok("format-specifiers Error fired");
        else bad("expected a format-specifiers Error");
    }
    {
        // B2. Wrong type: %d (integer) mistranslated as %s (string) -- undefined behaviour /
        // garbage output at format time.
        std::printf("\n-- B2. Wrong specifier type --\n");
        std::string id = "Chapter %d", str = "Kapitel %s";
        std::printf("  msgid  \"%s\"  specs = %s\n", id.c_str(), join(format_specs(id)).c_str());
        std::printf("  msgstr \"%s\" specs = %s\n", str.c_str(), join(format_specs(str)).c_str());
        auto f = rule_format("c", id, str);
        if (hasError(f, "format-specifiers")) ok("format-specifiers Error fired");
        else bad("expected a format-specifiers Error");
    }
    {
        // B3. Reorder: same specifier SET, but a translator swapped the argument order. Note a
        // POSITIONAL pair like "%1!s! = %2!d!" -> "%2!d! = %1!s!" does NOT trip format-order --
        // rule_format only checks ORDER for NON-positional specs (positional specs carry their
        // own index, so reordering them in the string is legitimate and expected across
        // languages). validate_conformance_test's "fmt order" case uses plain %s/%d for exactly
        // this reason, so we reuse that exact pair here.
        std::printf("\n-- B3. Reordered (non-positional) specifiers --\n");
        std::string id = "%s=%d", str = "%d=%s";
        std::printf("  msgid  \"%s\"  specs = %s\n", id.c_str(), join(format_specs(id)).c_str());
        std::printf("  msgstr \"%s\"  specs = %s  (same SET, different ORDER)\n", str.c_str(), join(format_specs(str)).c_str());
        auto f = rule_format("c", id, str);
        if (hasError(f, "format-order")) ok("format-order Error fired");
        else bad("expected a format-order Error");

        std::printf("  (aside: a POSITIONAL swap \"%%1!s! = %%2!d!\" -> \"%%2!d! = %%1!s!\" is legitimate\n"
                    "   and correctly produces NO finding -- positional index carries the order.)\n");
        auto fp = rule_format("c", "%1!s! = %2!d!", "%2!d! = %1!s!");
        if (!hasAnyError(fp)) ok("positional reorder correctly produces no Error");
        else bad("positional reorder unexpectedly flagged");
    }
    {
        // B4. Clean control: a literal "%%" (escaped percent) on both sides -- not a specifier.
        std::printf("\n-- B4. Clean control: literal %%%% on both sides --\n");
        std::string id = "100%% done", str = "100%% fertig";
        std::printf("  msgid  \"%s\"  specs = %s\n", id.c_str(), join(format_specs(id)).c_str());
        std::printf("  msgstr \"%s\" specs = %s\n", str.c_str(), join(format_specs(str)).c_str());
        auto f = rule_format("c", id, str);
        if (!hasAnyError(f)) ok("no Error, as expected");
        else bad("expected no Error");
    }

    // =====================================================================================
    banner("C. Accelerators -- accelerator_count / rule_duplicate_accelerator");
    // =====================================================================================
    {
        // C1. Missing mnemonic: the English caption has an Alt-key ("&Save"), the translation
        // dropped it entirely -- the button becomes unreachable by keyboard.
        std::printf("\n-- C1. Missing mnemonic --\n");
        std::string idc = "&Save", str = "Speichern";
        int in = accelerator_count(idc), sn = accelerator_count(str);
        std::printf("  msgid  \"%s\"  accelerator_count = %d\n", idc.c_str(), in);
        std::printf("  msgstr \"%s\" accelerator_count = %d\n", str.c_str(), sn);
        if (in == 1 && sn == 0 && in > sn) ok("English has '&', translation dropped it (idc > strc)");
        else bad("expected English count 1 > translation count 0");
    }
    {
        // C2. Duplicate accelerator: two DIFFERENT controls in the same dialog both claim Alt+S.
        std::printf("\n-- C2. Duplicate accelerator across two controls --\n");
        std::vector<AcceleratorEntry> controls = { {"IDC_A", "&Save"}, {"IDC_B", "&Stop"} };
        std::printf("  IDC_A -> \"&Save\"   IDC_B -> \"&Stop\"   (both claim Alt+S)\n");
        auto f = rule_duplicate_accelerator(controls);
        if (!f.empty()) {
            ok("duplicate-accelerator finding(s) fired");
            for (auto& x : f) std::printf("    -> [%s] %s\n", x.ctx.c_str(), x.msg.c_str());
        } else bad("expected at least one duplicate-accelerator finding");
    }
    {
        // C2b. Clean control: two controls with distinct accelerators.
        std::printf("\n-- C2b. Clean control: distinct accelerators --\n");
        std::vector<AcceleratorEntry> controls = { {"IDC_A", "&Save"}, {"IDC_B", "&Open"} };
        std::printf("  IDC_A -> \"&Save\"   IDC_B -> \"&Open\"   (Alt+S vs Alt+O)\n");
        auto f = rule_duplicate_accelerator(controls);
        if (f.empty()) ok("zero findings, as expected");
        else { bad("expected zero findings"); for (auto& x : f) std::printf("    unexpected: %s\n", x.msg.c_str()); }
    }

    std::printf("\n");
    if (fails) std::printf("%d scenario(s) FAILED to fire the expected defect\n", fails);
    else       std::printf("all bad-translation scenarios correctly detected\n");
    return fails;
}
