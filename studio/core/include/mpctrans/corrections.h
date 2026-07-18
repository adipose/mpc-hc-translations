// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include "mpctrans/github.h"

// Research corrections: a Studio user who spots an error in a string's per-string research write-up
// (the Type/Category/Where/Meaning/Function panel, backed by mpctrans::StringInfo) can submit a fix
// as a PR against THIS project (STUDIO_OWNER/STUDIO_REPO), reusing github::open_pr — the same Git
// Data commit/PR flow the translation submit uses (see github::open_translation_pr).
//
// Corrections are appended to data/research-corrections.jsonl (append-only overlay; latest line per
// (msgctxt, msgid, field) wins). The committed dist/core-enrichment.sqlite applies it LAST, after
// the union + escaping-alias rows, so a fix reaches every alias row sharing the corrected msgctxt too.
// See data/README.md for the on-disk format.

namespace mpctrans::corrections {

// The only two correctable fields — StringInfo::semantic_purpose ("Meaning") and
// StringInfo::functional_purpose ("Function"). Category/Where/Type are not user-editable research.
enum class Field { SemanticPurpose, FunctionalPurpose };
const char* field_name(Field f);   // "semantic_purpose" / "functional_purpose" — the overlay's `field` value

struct Correction {
    std::string msgctxt, msgid;   // identifies the string, same key CoreEnrichment::by_key uses
    Field field;
    std::string old_text;         // the value being replaced — shown in the PR body, not written to the overlay
    std::string new_text;         // the corrected value — written to the overlay as `text`
    std::string note;             // optional "why" explanation
};

// Builds one data/research-corrections.jsonl line (no trailing newline; proper JSON escaping via
// nlohmann/json). `date` is generated here (UTC, ISO 8601, e.g. "2026-07-10T14:32:07Z").
std::string build_correction_line(const std::string& msgctxt, const std::string& msgid, Field field,
                                  const std::string& text, const std::string& note,
                                  const std::string& author);

// Full flow: fetch the current data/research-corrections.jsonl from the studio repo's HEAD (a
// missing file there is fine — start from empty content), append one line per correction, and open a
// PR against STUDIO_OWNER/STUDIO_REPO (branch prefix "research-fix") via github::open_pr. The PR
// title is "research: correct <msgctxt>" of the FIRST correction (corrections in one call are
// expected to be for the same string — e.g. Meaning + Function fixed together); the body lists each
// field's old/new text and note. Returns the compare URL (see github::open_pr) -- the branch is
// created on the fork and the user creates the PR on GitHub after reviewing. Throws on any failure
// (network, auth, empty `corrections`).
std::string submit_research_correction(const github::Token& t, const std::string& author,
                                       const std::vector<Correction>& corrections);

} // namespace mpctrans::corrections
