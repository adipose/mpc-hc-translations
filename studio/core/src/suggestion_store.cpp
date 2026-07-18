// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/suggestion_store.h"

#include <filesystem>
#include <mutex>

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>     // SHGetKnownFolderPath / FOLDERID_LocalAppData
#include <cstdio>
#include <sqlite3.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")   // CoTaskMemFree

namespace mpctrans::suggestion_store {

namespace fs = std::filesystem;

namespace {

// SQLITE_THREADSAFE=0 (studio/core/CMakeLists.txt ~line 33): the vendored sqlite3 build allows NO
// concurrent calls into ANY sqlite3 API from two threads, ever — even against different connections.
// This mutex is the ONLY thing making that safe here, since put()/get() are hit from the UI thread
// (selection point-lookups, status-line counts) and the M2 background generation worker (writes) at
// the same time. Every public function below takes it for its entire body. Do not remove/narrow this
// under the assumption sqlite "handles its own locking" — with THREADSAFE=0 it explicitly does not.
std::mutex g_mutex;
sqlite3* g_db = nullptr;
bool g_openAttempted = false;   // true once we've tried to open (success OR failure) -- open once

fs::path db_path() {
    fs::path root;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        root = fs::path(local) / L"MPC-HC Translation Studio";   // same root as fetch_cache's
                                                                  // "...\fetch-cache" sibling
        CoTaskMemFree(local);
    }
    if (!root.empty()) {
        std::error_code ec;
        fs::create_directories(root, ec);
    }
    return root / L"ai-suggestions.sqlite";
}

std::string now_iso8601_utc() {
    SYSTEMTIME st; ::GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

// RAII prepared statement -- same idiom as enrichment.cpp's Stmt, with one deliberate difference:
// this store is fail-soft (never throws) since it's a best-effort local cache hit from a background
// worker and the UI thread alike (matches fetch_cache.cpp's "a failed store just means a miss later"
// posture) -- a bad prepare just leaves `s` null and step()/exec() become no-ops.
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~Stmt() { sqlite3_finalize(s); }
    void bind(int i, long long v)          { if (s) sqlite3_bind_int64(s, i, v); }
    void bind(int i, double v)             { if (s) sqlite3_bind_double(s, i, v); }
    void bind(int i, const std::string& v) { if (s) sqlite3_bind_text(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT); }
    void bindNull(int i)                   { if (s) sqlite3_bind_null(s, i); }
    bool step() { return s && sqlite3_step(s) == SQLITE_ROW; }
    void exec() { if (s) sqlite3_step(s); }
    long long   i64(int c)  { return sqlite3_column_int64(s, c); }
    double      dbl(int c)  { return sqlite3_column_double(s, c); }
    bool        isNull(int c) { return sqlite3_column_type(s, c) == SQLITE_NULL; }
    std::string text(int c) {
        const unsigned char* t = sqlite3_column_text(s, c);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
};

// Must be called with g_mutex already held. Opens (creating the file + schema on first use) exactly
// once per process -- lazy, mirrors fetch_cache::cache_root()'s static-local pattern, but guarded by
// the mutex (not a function-local static) since this needs to also run the CREATE TABLE/INDEX once.
sqlite3* db_locked() {
    if (g_openAttempted) return g_db;
    g_openAttempted = true;

    std::string path = db_path().u8string();   // UTF-8, matching enrichment.cpp's sqlite3_open_v2 convention
    if (sqlite3_open_v2(path.c_str(), &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
        return nullptr;
    }
    sqlite3_busy_timeout(g_db, 5000);

    const char* schema =
        "CREATE TABLE IF NOT EXISTS ai_suggestions ("
        " id INTEGER PRIMARY KEY,"
        " lang TEXT, msgctxt TEXT, msgid TEXT, suggestion TEXT,"
        " model TEXT, prompt_version TEXT, fits_px INTEGER NULL, flags TEXT NULL, created_at TEXT);"
        "CREATE UNIQUE INDEX IF NOT EXISTS ai_suggestions_key "
        " ON ai_suggestions(lang, msgctxt, msgid, model);";
    char* errmsg = nullptr;
    if (sqlite3_exec(g_db, schema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        sqlite3_close(g_db); g_db = nullptr;
        return nullptr;
    }

    // M3 additive migration: a DB file created by the pre-M3 binary won't have these columns yet.
    // "duplicate column name" (already migrated) is expected and silently ignored -- unlike the
    // CREATE TABLE step above, a failure here is never fatal (existing columns are untouched either
    // way, so there's nothing to roll back or abort over).
    sqlite3_exec(g_db, "ALTER TABLE ai_suggestions ADD COLUMN agreement REAL", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "ALTER TABLE ai_suggestions ADD COLUMN back_translation_score REAL", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "ALTER TABLE ai_suggestions ADD COLUMN role TEXT", nullptr, nullptr, nullptr);

    // Lazy translator-hint cache (see suggestion_store.h). Keyed by the English (msgctxt,msgid).
    sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS hint_cache ("
        " msgctxt TEXT, msgid TEXT, role TEXT, form TEXT, referent TEXT, agreement TEXT, notes TEXT,"
        " model TEXT, created TEXT, PRIMARY KEY(msgctxt, msgid))",
        nullptr, nullptr, nullptr);

    return g_db;
}

} // namespace

void put(const Suggestion& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* db = db_locked();
    if (!db) return;

    // INSERT OR REPLACE conflicts on the (lang,msgctxt,msgid,model) unique index -- deletes any
    // existing row for that key and inserts a fresh one with a NEW (larger) id, so "newest wins" for
    // both get()'s ORDER BY id DESC and count() naturally stays a single logical row per key.
    Stmt q(db, "INSERT OR REPLACE INTO ai_suggestions "
              "(lang, msgctxt, msgid, suggestion, model, prompt_version, fits_px, flags, created_at, "
              " agreement, back_translation_score, role) "
              "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    q.bind(1, s.lang); q.bind(2, s.msgctxt); q.bind(3, s.msgid); q.bind(4, s.suggestion);
    q.bind(5, s.model); q.bind(6, s.prompt_version);
    if (s.fits_px) q.bind(7, (long long)*s.fits_px); else q.bindNull(7);
    q.bind(8, s.flags);
    q.bind(9, now_iso8601_utc());   // store's own clock -- see the header comment on Suggestion::created_at
    if (s.agreement) q.bind(10, *s.agreement); else q.bindNull(10);
    if (s.back_translation_score) q.bind(11, *s.back_translation_score); else q.bindNull(11);
    q.bind(12, s.role);
    q.exec();
}

namespace {
// SELECT list + row->Suggestion population shared by get()/get_all() (kept identical so the two
// never drift on column order).
const char* kSelectCols =
    "lang, msgctxt, msgid, suggestion, model, prompt_version, fits_px, flags, created_at, "
    "agreement, back_translation_score, role ";

Suggestion row_to_suggestion(Stmt& q) {
    Suggestion out;
    out.lang = q.text(0); out.msgctxt = q.text(1); out.msgid = q.text(2);
    out.suggestion = q.text(3); out.model = q.text(4); out.prompt_version = q.text(5);
    if (!q.isNull(6)) out.fits_px = (int)q.i64(6);
    out.flags = q.text(7); out.created_at = q.text(8);
    if (!q.isNull(9)) out.agreement = q.dbl(9);
    if (!q.isNull(10)) out.back_translation_score = q.dbl(10);
    out.role = q.text(11);
    return out;
}
} // namespace

// Newest row for (lang,msgctxt,msgid) across ANY model. In practice M2 only ever generates with one
// configured model at a time, so there's normally exactly one row; cross-model comparison across
// several simultaneous candidates is M3's job (not built here -- see suggestion_store.h).
std::optional<Suggestion> get(const std::string& lang, const std::string& msgctxt,
                              const std::string& msgid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* db = db_locked();
    if (!db) return std::nullopt;

    Stmt q(db, (std::string("SELECT ") + kSelectCols +
               "FROM ai_suggestions WHERE lang=? AND msgctxt=? AND msgid=? ORDER BY id DESC LIMIT 1").c_str());
    q.bind(1, lang); q.bind(2, msgctxt); q.bind(3, msgid);
    if (!q.step()) return std::nullopt;
    return row_to_suggestion(q);
}

// ALL rows (any model) for (lang,msgctxt,msgid), newest first -- see suggestion_store.h for the
// get() vs get_all() rationale (M3's cross-model vote leaves two current rows per cell).
std::vector<Suggestion> get_all(const std::string& lang, const std::string& msgctxt,
                                const std::string& msgid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* db = db_locked();
    std::vector<Suggestion> out;
    if (!db) return out;

    Stmt q(db, (std::string("SELECT ") + kSelectCols +
               "FROM ai_suggestions WHERE lang=? AND msgctxt=? AND msgid=? ORDER BY id DESC").c_str());
    q.bind(1, lang); q.bind(2, msgctxt); q.bind(3, msgid);
    while (q.step()) out.push_back(row_to_suggestion(q));
    return out;
}

void remove(const std::string& lang, const std::string& msgctxt, const std::string& msgid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* db = db_locked();
    if (!db) return;
    Stmt q(db, "DELETE FROM ai_suggestions WHERE lang=? AND msgctxt=? AND msgid=?");
    q.bind(1, lang); q.bind(2, msgctxt); q.bind(3, msgid);
    q.exec();
}

long long count(const std::string& lang) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* db = db_locked();
    if (!db) return 0;
    Stmt q(db, "SELECT COUNT(*) FROM ai_suggestions WHERE lang=?");
    q.bind(1, lang);
    if (!q.step()) return 0;
    return q.i64(0);
}

std::set<std::pair<std::string, std::string>> primary_keys(const std::string& lang,
                                                             const std::string& model,
                                                             const std::string& prompt_version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::set<std::pair<std::string, std::string>> out;
    sqlite3* db = db_locked();
    if (!db) return out;
    Stmt q(db, "SELECT msgctxt, msgid FROM ai_suggestions "
              "WHERE lang=? AND model=? AND prompt_version=? AND role='primary'");
    q.bind(1, lang); q.bind(2, model); q.bind(3, prompt_version);
    while (q.step()) out.emplace(q.text(0), q.text(1));
    return out;
}

std::optional<HintRow> get_cached_hint(const std::string& msgctxt, const std::string& msgid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* db = db_locked();
    if (!db) return std::nullopt;
    Stmt q(db, "SELECT role, form, referent, agreement, notes, model FROM hint_cache "
              "WHERE msgctxt=? AND msgid=?");
    q.bind(1, msgctxt); q.bind(2, msgid);
    if (!q.step()) return std::nullopt;
    HintRow h;
    h.role = q.text(0); h.form = q.text(1); h.referent = q.text(2);
    h.agreement = q.text(3); h.notes = q.text(4); h.model = q.text(5);
    return h;
}

void put_cached_hint(const std::string& msgctxt, const std::string& msgid, const HintRow& h) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* db = db_locked();
    if (!db) return;
    Stmt q(db, "INSERT OR REPLACE INTO hint_cache "
              "(msgctxt, msgid, role, form, referent, agreement, notes, model, created) "
              "VALUES (?,?,?,?,?,?,?,?,?)");
    q.bind(1, msgctxt); q.bind(2, msgid); q.bind(3, h.role); q.bind(4, h.form);
    q.bind(5, h.referent); q.bind(6, h.agreement); q.bind(7, h.notes); q.bind(8, h.model);
    q.bind(9, now_iso8601_utc());
    q.exec();
}

} // namespace mpctrans::suggestion_store

#else // !_WIN32 -- no LOCALAPPDATA concept; disable the store (get() always misses, put()/remove()
      // are no-ops, count() is always 0). Keeps the portable CMake test gates (g++/clang) linking;
      // the Studio app itself is Windows-only. Mirrors fetch_cache.cpp's stub half exactly.

namespace mpctrans::suggestion_store {
void put(const Suggestion&) {}
std::optional<Suggestion> get(const std::string&, const std::string&, const std::string&) { return std::nullopt; }
std::vector<Suggestion> get_all(const std::string&, const std::string&, const std::string&) { return {}; }
void remove(const std::string&, const std::string&, const std::string&) {}
long long count(const std::string&) { return 0; }
std::set<std::pair<std::string, std::string>> primary_keys(const std::string&, const std::string&,
                                                             const std::string&) { return {}; }
std::optional<HintRow> get_cached_hint(const std::string&, const std::string&) { return std::nullopt; }
void put_cached_hint(const std::string&, const std::string&, const HintRow&) {}
} // namespace mpctrans::suggestion_store

#endif
