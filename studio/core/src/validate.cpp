// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/validate.h"
#include "mpctrans/po.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>

// C++ mirror of potool/potool.py. Keep in lock-step with potool/tests/test_potool.py.
// (Entry rules are implemented; file-level checks are TODO — see markers.)

namespace mpctrans::validate {

// Two deliberate exclusions, identical to potool (verified across all 177 committed .po):
//  - no space flag ("100% spyware" must not tokenize as "% s")
//  - no bare %<digits> ("%25","%100" are percentages in tr/eu/…); only typed "%1!s!" is positional.
static const std::regex SPEC_RE(
    R"(%%|%\d+!\w+!|%[-+0#]*\d*(?:\.\d+)?(?:hh|ll|h|l|L|z|j|t|w)?[diouxXeEfFgGaAcsp@])");

std::vector<std::string> format_specs(const std::string& s) {
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), SPEC_RE);
         it != std::sregex_iterator(); ++it)
        out.push_back(it->str());
    return out;
}
static bool positional(const std::string& t) {
    return t.size() >= 2 && t[0] == '%' && std::isdigit((unsigned char)t[1]);
}

std::vector<Finding> rule_format(const std::string& ctx, const std::string& id, const std::string& str) {
    if (str.empty()) return {};
    auto a = format_specs(id);
    if (a.empty()) return {};   // source has no specifier -> its '%' is literal; skip (see potool)
    auto b = format_specs(str);
    std::multiset<std::string> ma(a.begin(), a.end()), mb(b.begin(), b.end());
    if (ma != mb)
        return {{Severity::Error, "format-specifiers", ctx, "source vs translation specifier set differs"}};
    std::vector<std::string> na, nb;   // non-positional order must be preserved
    for (auto& t : a) if (!positional(t) && t != "%%") na.push_back(t);
    for (auto& t : b) if (!positional(t) && t != "%%") nb.push_back(t);
    if (na != nb)
        return {{Severity::Error, "format-order", ctx, "reordered non-positional specifiers"}};
    return {};
}

int accelerator_count(const std::string& s) {   // count '&' not part of '&&'
    std::string t;
    for (size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '&' && s[i + 1] == '&') i += 2;
        else t += s[i++];
    }
    return (int)std::count(t.begin(), t.end(), '&');
}

std::optional<char> accelerator_letter(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') continue;
        if (i + 1 < s.size() && s[i + 1] == '&') { ++i; continue; }   // escaped literal "&&" -- skip both
        if (i + 1 >= s.size()) return std::nullopt;                  // trailing '&', no letter follows
        char c = s[i + 1];
        if (c >= 'a' && c <= 'z') c = (char)std::toupper((unsigned char)c);
        return c;
    }
    return std::nullopt;
}

std::vector<Finding> rule_ampersand(const std::string& ctx, const std::string& id, const std::string& str) {
    if (str.empty()) return {};
    if (accelerator_count(id) != accelerator_count(str))   // SOFT: translators vary accelerators per language
        return {{Severity::Warning, "ampersand", ctx, "mnemonic '&' count differs"}};
    return {};
}

// Dialog-scoped: two DIFFERENT controls whose translated captions claim the same accelerator letter.
std::vector<Finding> rule_duplicate_accelerator(const std::vector<AcceleratorEntry>& controls) {
    std::vector<Finding> out;
    for (size_t i = 0; i < controls.size(); ++i) {
        if (controls[i].msgstr.empty()) continue;
        auto li = accelerator_letter(controls[i].msgstr);
        if (!li) continue;
        for (size_t j = 0; j < controls.size(); ++j) {
            if (i == j || controls[j].msgstr.empty()) continue;
            auto lj = accelerator_letter(controls[j].msgstr);
            if (!lj || *lj != *li) continue;
            std::string msg = std::string("duplicate accelerator '&") + std::string(1, *li) +
                               "' also used by " + controls[j].msgctxt;
            out.push_back({Severity::Warning, "duplicate-accelerator", controls[i].msgctxt, msg});
        }
    }
    return out;
}

std::vector<Finding> rule_newline(const std::string& ctx, const std::string& id, const std::string& str) {
    if (str.empty()) return {};
    if (std::count(id.begin(), id.end(), '\n') != std::count(str.begin(), str.end(), '\n'))
        return {{Severity::Warning, "newline", ctx, "\\n count differs"}};   // SOFT: reflow allowed
    return {};
}

static size_t lead(const std::string& s){ size_t i=0; while(i<s.size()&&std::isspace((unsigned char)s[i]))++i; return i; }
static size_t trail(const std::string& s){ size_t i=0; while(i<s.size()&&std::isspace((unsigned char)s[s.size()-1-i]))++i; return i; }

std::vector<Finding> rule_whitespace(const std::string& ctx, const std::string& id, const std::string& str) {
    if (str.empty()) return {};
    std::vector<Finding> out;
    if (lead(id)  != lead(str))  out.push_back({Severity::Warning, "leading-space",  ctx, "leading-whitespace mismatch"});
    if (trail(id) != trail(str)) out.push_back({Severity::Warning, "trailing-space", ctx, "trailing-whitespace mismatch"});
    return out;
}

std::vector<Finding> rule_length(const std::string& ctx, const std::string& id, const std::string& str) {
    // NOTE: byte length (potool uses codepoint length). Soft/advisory rule — acceptable divergence.
    if (!str.empty() && id.size() >= 8 && str.size() > 2 * id.size())
        return {{Severity::Warning, "length-ratio", ctx, "translation > 2x source length"}};
    return {};
}

std::vector<Finding> rule_empty(const std::string& ctx, const std::string&, const std::string& str) {
    if (str.empty()) return {{Severity::Info, "empty", ctx, "untranslated (falls back to English)"}};
    return {};
}

std::vector<Finding> check_entry(const std::string& ctx, const std::string& id, const std::string& str) {
    std::vector<Finding> f;
    for (auto* r : {rule_format, rule_ampersand, rule_newline, rule_whitespace, rule_length, rule_empty}) {
        auto v = r(ctx, id, str);
        f.insert(f.end(), v.begin(), v.end());
    }
    return f;
}

bool has_error(const std::vector<Finding>& f) {
    return std::any_of(f.begin(), f.end(), [](const Finding& x){ return x.sev == Severity::Error; });
}

// ---- file-level (TODO — port from potool: encoding/BOM, msgfmt -c, structural integrity) ----
std::vector<Finding> check_encoding(const std::string&) { return {}; /* TODO: BOM + UTF-8 decode */ }
std::vector<Finding> check_msgfmt(const std::string&)   { return {}; /* TODO: run msgfmt -c if present */ }
std::vector<Finding> check_structural(const PoFile&, const PoFile&) { return {}; /* TODO: (ctx,id) key set + header */ }
std::vector<Finding> check_po(const std::string&, const std::string*) { return {}; /* TODO: encoding+parse+entries[+structural] */ }

} // namespace mpctrans::validate
