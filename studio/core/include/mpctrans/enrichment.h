// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Read-only enrichment reader. CORE (language-independent) ships in the bundle; a small
// PER-LANGUAGE pack downloads on checkout. Backed by the sqlite3.c amalgamation (no external dep).
// Both are committed data artifacts: dist/core-enrichment.sqlite and enrichment/lang/*.sqlite.

namespace mpctrans {

struct StringInfo {            // from CORE enrichment (strings table)
    long long id = 0;
    std::string msgctxt, msgid;
    std::string ui_location, semantic_purpose, functional_purpose, category, ui_type, source_pot_file;
};
struct AiSuggestion   { long long string_id; std::string msgstr, method; };   // per-language pack
struct ReferenceMatch { long long string_id; std::string source, msgstr; };   // per-language pack

// Fidelity-ranked sibling grounding (M3): how "fresh" a language's translations are, by % of current
// translations set 2024+ (see data/README.md; sourced from a `language_fidelity` VIEW, copied into
// the committed core-enrichment.sqlite as a plain TABLE).
// `language` uses the source database's own code convention (e.g. "pt_BR", "zh_CN" -- underscore,
// mixed case), which does NOT match the Studio's own language codes (e.g. "pt-br", "zh-cn" --
// hyphen, lowercase, see MainFrame.cpp's LanguageEnglishName table); callers normalize before
// comparing.
struct LanguageFidelity { std::string language; double pct; };

// Translator hint layer (M4): per-string authoring guidance mined from the source strings, copied
// into the committed core-enrichment.sqlite as a `string_hints` table by
// bundle-build/generate_hints.py. Older bundles won't have this table -- see hint_for().
// `role` in {command,label,option,title,status,format-name,unit,value}; keep_verbatim = tokens the
// translation MUST reproduce exactly (placeholders, numerics); soft_verbatim = acronyms that may be
// kept or expanded; agreement = grammatical gender/number guidance; referent = the noun modified /
// what a placeholder denotes; parallel_group = a numbered-sibling family key or NULL.
struct StringHint {
    std::string role, form, referent, agreement, parallel_group, notes, source;
    std::vector<std::string> keep_verbatim, soft_verbatim;   // parsed from the JSON arrays
};

class CoreEnrichment {     // opens core-enrichment.sqlite (bundled), read-only; move-only
public:
    static CoreEnrichment open(const std::string& path);   // throws std::runtime_error
    CoreEnrichment() = default;
    CoreEnrichment(CoreEnrichment&& o) noexcept : db_(o.db_) { o.db_ = nullptr; }
    CoreEnrichment& operator=(CoreEnrichment&& o) noexcept { std::swap(db_, o.db_); return *this; }
    CoreEnrichment(const CoreEnrichment&) = delete;
    CoreEnrichment& operator=(const CoreEnrichment&) = delete;
    ~CoreEnrichment();
    std::optional<StringInfo> by_id(long long id) const;
    std::optional<StringInfo> by_key(const std::string& msgctxt, const std::string& msgid) const;
    std::vector<StringInfo>   all() const;                 // full catalog ("All strings" list)
    // Sorted desc by pct (freshest first). Empty vector if the `language_fidelity` table doesn't exist
    // in this bundle (an older export, or the lab snapshot's view was missing when it was built) --
    // this is a normal, non-error condition; callers must treat empty as "no ranking available", never
    // throw.
    std::vector<LanguageFidelity> language_fidelity() const;
    // Translator hint for one string, if the `string_hints` table exists in this bundle and has a
    // matching row. Empty (nullopt), never throws, if the table is missing (older bundle) or there's
    // no row for this string_id -- same graceful-degrade contract as language_fidelity() above.
    std::optional<StringHint> hint_for(long long string_id) const;
    // dialog_mapping helpers as needed…
private:
    void* db_ = nullptr;   // sqlite3*
};

class LangPack {           // opens enrichment/lang/<code>.sqlite (downloaded on checkout); move-only
public:
    static LangPack open(const std::string& path);         // throws std::runtime_error
    LangPack() = default;
    LangPack(LangPack&& o) noexcept : db_(o.db_) { o.db_ = nullptr; }
    LangPack& operator=(LangPack&& o) noexcept { std::swap(db_, o.db_); return *this; }
    LangPack(const LangPack&) = delete;
    LangPack& operator=(const LangPack&) = delete;
    ~LangPack();
    std::vector<AiSuggestion>   ai_for(long long string_id) const;   // suggestion/default
    std::vector<ReferenceMatch> refs_for(long long string_id) const; // reference matches
private:
    void* db_ = nullptr;
};

} // namespace mpctrans
