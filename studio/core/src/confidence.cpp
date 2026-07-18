// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/confidence.h"
#include "mpctrans/validate.h"

#include <cctype>
#include <map>
#include <vector>

namespace mpctrans::confidence {

std::string normalize_for_agreement(const std::string& s) {
    // Strip every '&' byte first: a bare '&' before a letter is a mnemonic marker (drop it), an
    // escaped "&&" is a literal ampersand that ALSO collapses to nothing per spec -- removing every
    // '&' individually produces the right result for both cases ("&Play" -> "Play", "a&&b" -> "ab").
    std::string stripped;
    stripped.reserve(s.size());
    for (char c : s) if (c != '&') stripped.push_back(c);

    // Collapse whitespace runs to a single space, trim leading/trailing, ASCII-casefold.
    std::string out;
    out.reserve(stripped.size());
    bool in_ws = false;
    for (char c : stripped) {
        bool is_ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (is_ws) { in_ws = true; continue; }
        if (in_ws && !out.empty()) out.push_back(' ');
        in_ws = false;
        char lc = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        out.push_back(lc);
    }
    return out;
}

double trigram_similarity(const std::string& a, const std::string& b) {
    if (a == b) return 1.0;               // identical (including both empty)
    if (a.empty() || b.empty()) return 0.0;
    if (a.size() < 3 || b.size() < 3) return 0.0;   // too short to form a trigram, and not equal

    auto trigrams = [](const std::string& s) {
        std::vector<std::string> v;
        v.reserve(s.size() >= 2 ? s.size() - 2 : 0);
        for (size_t i = 0; i + 3 <= s.size(); ++i) v.push_back(s.substr(i, 3));
        return v;
    };
    std::vector<std::string> ta = trigrams(a), tb = trigrams(b);

    std::map<std::string, int> counts;
    for (const auto& t : tb) ++counts[t];

    size_t shared = 0;
    for (const auto& t : ta) {
        auto it = counts.find(t);
        if (it != counts.end() && it->second > 0) { ++shared; --it->second; }
    }
    return 2.0 * static_cast<double>(shared) / static_cast<double>(ta.size() + tb.size());
}

double agreement_score(const std::string& a, const std::string& b) {
    std::string na = normalize_for_agreement(a);
    std::string nb = normalize_for_agreement(b);
    if (na == nb) return 1.0;
    return trigram_similarity(na, nb);
}

std::string mask_placeholders(const std::string& s) {
    std::vector<std::string> specs = validate::format_specs(s);
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    static const std::string kNumeric = "diouxefga";   // diouxXeEfFgGaA, lowercased
    static const std::string kStringy = "csp";
    for (const auto& spec : specs) {
        size_t at = s.find(spec, pos);
        if (at == std::string::npos) continue;   // defensive: shouldn't happen, format_specs is over `s`
        out.append(s, pos, at - pos);             // unchanged text before this spec

        if (spec == "%%") {
            out += spec;   // literal percent, not a placeholder -- left untouched
        } else {
            // Last alphabetic character scanning from the end (skips trailing "!" in positional
            // wrappers like "%1!s!"); no alphabetic char found (e.g. a bare "%@") falls through to
            // the "%S" default below.
            char conv = 0;
            for (auto it = spec.rbegin(); it != spec.rend(); ++it) {
                if (std::isalpha(static_cast<unsigned char>(*it))) { conv = *it; break; }
            }
            char lc = (conv >= 'A' && conv <= 'Z') ? static_cast<char>(conv - 'A' + 'a') : conv;
            if (kNumeric.find(lc) != std::string::npos)
                out += "%D";
            else if (kStringy.find(lc) != std::string::npos)
                out += "%S";
            else
                out += "%S";   // default: unrecognized conversion (incl. bare '@')
        }
        pos = at + spec.size();
    }
    out.append(s, pos, std::string::npos);
    return out;
}

double back_translation_similarity(const std::string& original, const std::string& back_translation) {
    std::string mo = mask_placeholders(original);
    std::string mb = mask_placeholders(back_translation);
    std::string no = normalize_for_agreement(mo);
    std::string nb = normalize_for_agreement(mb);
    return trigram_similarity(no, nb);
}

Confidence classify(const ConfidenceInputs& in) {
    bool agreement_ok = !in.agreement.has_value() || *in.agreement >= 0.85;
    bool back_ok = !in.back_translation_score.has_value() || *in.back_translation_score >= kBackTranslationThreshold;
    if (agreement_ok && back_ok && in.fits && !in.has_gate_flags) return Confidence::High;
    return Confidence::Low;
}

} // namespace mpctrans::confidence
