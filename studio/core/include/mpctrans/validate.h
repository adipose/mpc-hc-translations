// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <vector>

// Validation rules — the C++ mirror of potool/potool.py. These MUST reproduce
// potool/tests/test_potool.py 1:1 (the conformance contract). Any divergence is a bug.
// HARD (error): encoding/no-BOM, msgfmt-c (optional), format-specifier parity (only when the
//   SOURCE has a spec), structural integrity (PR mode). SOFT (warning/info): ampersand, newline,
//   whitespace, length-ratio, empty. Rationale + false-positive history: potool/README.md.

namespace mpctrans { class PoFile; }

namespace mpctrans::validate {

enum class Severity { Error, Warning, Info };
struct Finding { Severity sev; std::string rule; std::string ctx; std::string msg; };

// per-entry (pure — trivially unit-testable, must match potool)
std::vector<Finding> rule_format   (const std::string& ctx, const std::string& msgid, const std::string& msgstr);
std::vector<Finding> rule_ampersand(const std::string& ctx, const std::string& msgid, const std::string& msgstr);
std::vector<Finding> rule_newline  (const std::string& ctx, const std::string& msgid, const std::string& msgstr);
std::vector<Finding> rule_whitespace(const std::string& ctx, const std::string& msgid, const std::string& msgstr);
std::vector<Finding> rule_length   (const std::string& ctx, const std::string& msgid, const std::string& msgstr);
std::vector<Finding> rule_empty    (const std::string& ctx, const std::string& msgid, const std::string& msgstr);

// Extract the printf-style specifiers (%s, %d, %1!s!, %%, ...) from `s`, in order, using the SAME
// regex/rules as rule_format (percentages like "%25" are excluded; only typed positional specs
// like "%1!s!" count). Exposed for callers that need the exact tokens (e.g. the Review Queue's
// evidence text), not just parity.
std::vector<std::string> format_specs(const std::string& s);

// Count of '&' mnemonics in `s` (an "&&" is an escaped literal ampersand and does not count).
int accelerator_count(const std::string& s);
// The mnemonic LETTER (uppercased) immediately following the first real '&' in `s`, or nullopt if
// `s` has no '&' mnemonic (empty string, trailing '&', or only "&&"). Used to detect two controls
// in the same dialog claiming the same Alt+key.
//
// NOTE: this is a BYTE-level, not codepoint-level, comparison — for a non-Latin script the
// character following '&' may be the first byte of a multi-byte UTF-8 sequence, so this is only
// exact for single-byte (ASCII/Latin-1-ish) mnemonics. Acceptable approximation for M1.
std::optional<char> accelerator_letter(const std::string& s);

// Dialog-scoped: flags when two DIFFERENT controls in the same dialog have translated captions
// claiming the same accelerator letter (same Alt+key — only one control can actually be reached
// by it). `controls` is every translatable control's (msgctxt, msgstr) pair in ONE dialog; entries
// with an empty msgstr are skipped (an untranslated control keeps the English mnemonic — out of
// scope for this check). Returns one Warning Finding per (control, other-control) ordered pair for
// every colliding pair (i.e. two Findings per collision, one from each control's point of view),
// ctx = that control's msgctxt, msg names the letter and the OTHER control's msgctxt (e.g.
// "duplicate accelerator '&S' also used by IDC_TOOLBAR_ALIGN").
struct AcceleratorEntry { std::string msgctxt, msgstr; };
std::vector<Finding> rule_duplicate_accelerator(const std::vector<AcceleratorEntry>& controls);

// file-level
std::vector<Finding> check_encoding  (const std::string& path);
std::vector<Finding> check_msgfmt    (const std::string& path);                 // no-op if msgfmt absent
std::vector<Finding> check_structural(const PoFile& base, const PoFile& po);    // PR mode
std::vector<Finding> check_po        (const std::string& path, const std::string* base_path = nullptr);

bool has_error(const std::vector<Finding>&);

// Inline warnings for the edit panel: run per-entry rules on the string currently being edited.
std::vector<Finding> check_entry(const std::string& ctx, const std::string& msgid, const std::string& msgstr);

} // namespace mpctrans::validate
