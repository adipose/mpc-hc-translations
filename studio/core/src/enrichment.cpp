// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/enrichment.h"

#include <stdexcept>

#include <json.hpp>
#include <sqlite3.h>

// Read-only readers over the enrichment DBs (vendored sqlite3 amalgamation). Schemas, both
// committed data artifacts:
// dist/core-enrichment.sqlite ->
//   strings(id, msgctxt, msgid, ui_location, semantic_purpose, functional_purpose,
//           category, ui_type, string_type, source_pot_file), dialog_mapping(...)
// enrichment/lang/*.sqlite ->
//   ai_suggestions(string_id, msgstr, method), reference_matches(string_id, source, msgstr)

namespace mpctrans {

using nlohmann::json;

namespace {

sqlite3* open_ro(const std::string& path, const char* what) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "out of memory";
        sqlite3_close(db);
        throw std::runtime_error(std::string(what) + ": cannot open " + path + " (" + err + ")");
    }
    return db;
}

// RAII prepared statement; column() maps NULL -> "".
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    void bind(int i, long long v)          { sqlite3_bind_int64(s, i, v); }
    void bind(int i, const std::string& v) { sqlite3_bind_text(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT); }
    bool step() { return sqlite3_step(s) == SQLITE_ROW; }
    long long   i64(int c)  { return sqlite3_column_int64(s, c); }
    std::string text(int c) {
        const unsigned char* t = sqlite3_column_text(s, c);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
};

// string_hints.keep_verbatim / .soft_verbatim are JSON arrays of strings (or NULL/empty). Tolerant
// of malformed or absent data -- just yields {} rather than throwing.
std::vector<std::string> parse_string_array(const std::string& text) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_array()) return out;
    for (const auto& e : j)
        if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}

StringInfo read_string_row(Stmt& q) {
    StringInfo si;
    si.id = q.i64(0);
    si.msgctxt = q.text(1);            si.msgid = q.text(2);
    si.ui_location = q.text(3);        si.semantic_purpose = q.text(4);
    si.functional_purpose = q.text(5); si.category = q.text(6);
    si.ui_type = q.text(7);            si.source_pot_file = q.text(8);
    return si;
}

} // namespace

// ---------- CoreEnrichment ----------
CoreEnrichment CoreEnrichment::open(const std::string& path) {
    CoreEnrichment ce;
    ce.db_ = open_ro(path, "CoreEnrichment::open");
    return ce;
}
CoreEnrichment::~CoreEnrichment() { sqlite3_close(static_cast<sqlite3*>(db_)); }

std::optional<StringInfo> CoreEnrichment::by_id(long long id) const {
    Stmt q(static_cast<sqlite3*>(db_),
           "select id, msgctxt, msgid, ui_location, semantic_purpose, functional_purpose, "
           "category, ui_type, source_pot_file from strings where id = ?");
    q.bind(1, id);
    if (!q.step()) return std::nullopt;
    return read_string_row(q);
}
std::optional<StringInfo> CoreEnrichment::by_key(const std::string& msgctxt,
                                                 const std::string& msgid) const {
    Stmt q(static_cast<sqlite3*>(db_),
           "select id, msgctxt, msgid, ui_location, semantic_purpose, functional_purpose, "
           "category, ui_type, source_pot_file from strings where msgctxt = ? and msgid = ?");
    q.bind(1, msgctxt); q.bind(2, msgid);
    if (!q.step()) return std::nullopt;
    return read_string_row(q);
}
std::vector<StringInfo> CoreEnrichment::all() const {
    Stmt q(static_cast<sqlite3*>(db_),
           "select id, msgctxt, msgid, ui_location, semantic_purpose, functional_purpose, "
           "category, ui_type, source_pot_file from strings order by id");
    std::vector<StringInfo> out;
    while (q.step()) out.push_back(read_string_row(q));
    return out;
}

std::vector<LanguageFidelity> CoreEnrichment::language_fidelity() const {
    // Deliberately NOT using the throwing Stmt helper above: a missing `language_fidelity` table
    // (older export, or the lab snapshot's view was absent when the export ran) is a normal, empty
    // result here -- not an exceptional condition -- so this uses raw sqlite3 calls directly and
    // just returns {} if prepare fails.
    sqlite3_stmt* stmt = nullptr;
    sqlite3* db = static_cast<sqlite3*>(db_);
    if (sqlite3_prepare_v2(db, "select language, pct from language_fidelity order by pct desc",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::vector<LanguageFidelity> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* lang = sqlite3_column_text(stmt, 0);
        LanguageFidelity lf;
        lf.language = lang ? reinterpret_cast<const char*>(lang) : "";
        lf.pct = sqlite3_column_double(stmt, 1);
        out.push_back(std::move(lf));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<StringHint> CoreEnrichment::hint_for(long long string_id) const {
    // Same non-throwing degrade as language_fidelity() above: a missing `string_hints` table (older
    // bundle, built before the hint layer existed) is normal here, not exceptional -- raw sqlite3
    // calls, nullopt on prepare failure or no matching row.
    sqlite3_stmt* stmt = nullptr;
    sqlite3* db = static_cast<sqlite3*>(db_);
    if (sqlite3_prepare_v2(db,
                           "select role, form, keep_verbatim, soft_verbatim, referent, agreement, "
                           "parallel_group, notes, source from string_hints where string_id = ?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt, 1, string_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    auto col_text = [&](int c) -> std::string {
        const unsigned char* t = sqlite3_column_text(stmt, c);
        return t ? reinterpret_cast<const char*>(t) : "";
    };
    StringHint h;
    h.role            = col_text(0);
    h.form            = col_text(1);
    h.keep_verbatim   = parse_string_array(col_text(2));
    h.soft_verbatim   = parse_string_array(col_text(3));
    h.referent        = col_text(4);
    h.agreement       = col_text(5);
    h.parallel_group  = col_text(6);
    h.notes           = col_text(7);
    h.source          = col_text(8);
    sqlite3_finalize(stmt);
    return h;
}

// ---------- LangPack ----------
LangPack LangPack::open(const std::string& path) {
    LangPack lp;
    lp.db_ = open_ro(path, "LangPack::open");
    return lp;
}
LangPack::~LangPack() { sqlite3_close(static_cast<sqlite3*>(db_)); }

std::vector<AiSuggestion> LangPack::ai_for(long long string_id) const {
    Stmt q(static_cast<sqlite3*>(db_),
           "select string_id, msgstr, method from ai_suggestions where string_id = ?");
    q.bind(1, string_id);
    std::vector<AiSuggestion> out;
    while (q.step()) out.push_back({q.i64(0), q.text(1), q.text(2)});
    return out;
}
std::vector<ReferenceMatch> LangPack::refs_for(long long string_id) const {
    Stmt q(static_cast<sqlite3*>(db_),
           "select string_id, source, msgstr from reference_matches where string_id = ?");
    q.bind(1, string_id);
    std::vector<ReferenceMatch> out;
    while (q.step()) out.push_back({q.i64(0), q.text(1), q.text(2)});
    return out;
}

} // namespace mpctrans
