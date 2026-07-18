// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Persistent store for M2's bulk-generated AI translation suggestions — mirrors the shape of the
// `ai_suggestions` table described in data/README.md (minus internal ids). This is NOT
// data/lab-snapshot.sqlite (a separate, private artifact) — it's a sibling of
// mpctrans::fetch_cache's disk cache, same root dir convention:
//   %LOCALAPPDATA%\MPC-HC Translation Studio\ai-suggestions.sqlite
// (fetch_cache's own files live one level deeper, in a `fetch-cache` subfolder of that same root —
// this file sits directly in the root instead.)
//
// Suppression rule (read-time, NOT enforced here): a suggestion is only meant to be SHOWN if the
// cell is untranslated OR currently validation-flagged. This module is a dumb point-lookup/upsert —
// it has no idea what a .po file or a validation finding is. The CALLER (MainFrame) decides
// eligibility before calling get()/put(), by re-deriving the cell's flag state itself (see
// MainFrame::CellFlag, shared between the confirmation/worklist pass and the display path so the two
// never drift).
//
// Uniqueness is (lang, msgctxt, msgid, model) — putting again for the same 4-tuple replaces the row
// (newest wins). get() returns the newest row for (lang, msgctxt, msgid) across ANY model — M2 only
// ever generates with one configured model at a time, so in practice there's exactly one row per
// cell; cross-model comparison (multiple simultaneous candidates) is M3's job, not built here.
//
// THREADING: the vendored sqlite3.c is compiled SQLITE_THREADSAFE=0 (see studio/core/CMakeLists.txt)
// — single-thread mode, meaning NO two threads may call ANY sqlite3 API concurrently, ever, even on
// different connections/files. put()/get()/remove()/count() are hit from the UI thread (point
// lookups on selection, status-line counts) AND from the M2 background generation worker thread
// (writes) at the same time. Every public entry point below is therefore serialized behind ONE
// process-wide static std::mutex in the .cpp — a plain std::lock_guard per call (these are tiny
// point operations; no perf concern). Do NOT "simplify" that mutex away — it is load-bearing.
//
// Real impl behind _WIN32 (LOCALAPPDATA only exists there); a no-op/std::nullopt stub otherwise, so
// the portable CMake test gates (g++/clang, non-Windows) keep linking. Mirrors fetch_cache.cpp's
// #ifdef shape exactly.

namespace mpctrans::suggestion_store {

struct Suggestion {
    std::string lang, msgctxt, msgid, suggestion, model, prompt_version;
    std::optional<int> fits_px;
    std::string flags;       // "" = no gate findings (validate::check_entry) at generation time
    // ISO-8601 UTC timestamp. Callers may leave this blank — put() always stamps its OWN "now" at
    // insert time (single source of truth for ordering; avoids trusting a caller-supplied clock).
    // get() returns back whatever the store actually persisted.
    std::string created_at;

    // ---- M3 (cross-model agreement + back-translation) ----
    // Cross-lineage agreement [0,1] with the vote candidate; nullopt = no vote made (single
    // provider, or the vote call for this cell failed) -- set on BOTH the primary and vote row when
    // a vote succeeded (both rows record the same agreement value between them).
    std::optional<double> agreement;
    // similarity(back-translated text, original English msgid) in [0,1]; nullopt = back-translation
    // pass not run or failed; only ever set on the PRIMARY row (the vote candidate isn't
    // independently back-translated).
    std::optional<double> back_translation_score;
    // "primary" | "vote" | "" (empty = pre-M3 row, or an on-demand single-suggestion row that was
    // never part of a bulk-generation pair).
    std::string role;
};

// Upsert: replaces any existing row for the same (lang, msgctxt, msgid, model) — newest wins.
void put(const Suggestion& s);

// Newest row for (lang, msgctxt, msgid), across any model. nullopt if none stored.
std::optional<Suggestion> get(const std::string& lang, const std::string& msgctxt,
                              const std::string& msgid);

// ALL rows (any model) for (lang, msgctxt, msgid), newest first (ORDER BY id DESC) -- unlike get()
// (which returns only the single newest row across any model), this surfaces the FULL set: after M3's
// cross-model vote, a cell legitimately has TWO current rows (role="primary" + role="vote") the
// display needs simultaneously (agreement + the alternate candidate's own text). get() is UNCHANGED
// and kept for existing single-row callers/tests.
std::vector<Suggestion> get_all(const std::string& lang, const std::string& msgctxt,
                                const std::string& msgid);

// Delete every row (any model) for this cell — called on accept/manual-edit (the cell's translated
// now, any stored suggestion is obsolete) and available generally for cleanup.
void remove(const std::string& lang, const std::string& msgctxt, const std::string& msgid);

// Row count for `lang` — cheap, for the status-line ("N stored AI suggestions for this language").
long long count(const std::string& lang);

// The AI-generation skip/resume rule (both the per-language and ALL-LANGUAGES generation commands):
// every (msgctxt, msgid) that already has a role='primary' row for `lang` at EXACTLY the given
// model + prompt_version. One query per language (not one per cell) — callers filter their worklist
// against this set before dispatching a cell to the worker, so re-invoking a generation run never
// re-bills a cell already covered by the CURRENT model+prompt combination; an older prompt_version or
// a different model still needs (re-)generating and is therefore NOT in this set.
std::set<std::pair<std::string, std::string>> primary_keys(const std::string& lang,
                                                             const std::string& model,
                                                             const std::string& prompt_version);

// ---- lazy translator-hint cache (language-independent) ----
// The LLM half of a translator hint (role/form/referent/agreement/notes), generated on demand with
// the user's own key when a string has no SHIPPED hint (a new upstream string, or a build whose LLM
// hints weren't filled) -- see mpctrans::ai_client::generate_hint and CoreEnrichment::hint_for. Keyed
// by (msgctxt, msgid) only: hints describe the ENGLISH string, so they're the same across languages.
// Same fail-soft, mutex-serialized posture as the suggestion rows above.
struct HintRow { std::string role, form, referent, agreement, notes, model; };
std::optional<HintRow> get_cached_hint(const std::string& msgctxt, const std::string& msgid);
void                   put_cached_hint(const std::string& msgctxt, const std::string& msgid, const HintRow&);

} // namespace mpctrans::suggestion_store
