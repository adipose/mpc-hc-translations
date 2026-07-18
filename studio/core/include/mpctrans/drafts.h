// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <string>
#include <vector>

// Pending (unsubmitted) translation edits, persisted locally so they survive restarts. Keyed by
// language code; each edit records which PO resource it belongs to and the (msgctxt,msgid) it keys.
// Serialization only (JSON, via the core's bundled nlohmann/json) — the caller owns the file I/O.
namespace mpctrans {

// A translation is bound to the exact English it was made against: on reload it re-applies only on an
// exact (msgctxt,msgid) match, and is discarded if the English changed upstream (no carry-over).
struct DraftEdit { int res = 0; std::string msgctxt, msgid, msgstr; };
using Drafts = std::map<std::string, std::vector<DraftEdit>>;   // language code -> edits

std::string serialize_drafts(const Drafts& d);
Drafts      parse_drafts(const std::string& json);   // {} on empty/invalid input

}
