// SPDX-License-Identifier: GPL-3.0-or-later
// C++ side of the validation CONTRACT — mirrors potool/tests/test_potool.py. If this and the
// Python suite ever disagree, one of the two implementations has drifted. Build & run standalone.
#include "mpctrans/validate.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace mpctrans::validate;
static int fails = 0;

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

    if (fails) std::printf("%d case(s) FAILED\n", fails);
    else       std::printf("all C++ conformance cases passed\n");
    return fails ? 1 : 0;
}
