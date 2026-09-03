// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// PO model + gettext-canonical (polib-compatible) serialization + surgical splice.
// FIDELITY CONTRACT: serialize_entry() must byte-match polib output. This is gated by
// tests/po_roundtrip_test (load every one of the 177 committed .po, re-serialize, diff == 0).
// See plan-translation-studio "PO export-format fidelity — DE-RISKED".

namespace mpctrans {

struct PoEntry {
    std::string msgctxt;
    std::string msgid;
    std::string msgstr;
    std::vector<std::string> comments;   // '#…' lines, preserved verbatim
    bool is_header() const { return msgid.empty(); }
    std::pair<std::string, std::string> key() const { return {msgctxt, msgid}; }  // system-wide key
};

class PoFile {
public:
    // polib-compatible reader (UTF-8; handles wrapped continuations + escapes).
    static PoFile parse_file(const std::string& path);
    static PoFile parse_bytes(const std::string& bytes);

    std::vector<std::pair<std::string, std::string>> metadata;  // ordered header fields
    std::vector<PoEntry> entries;
    std::string raw_header;   // verbatim leading comments + header entry + trailing blank (LF)

    const PoEntry* find(const std::string& ctx, const std::string& id) const;
    std::string get_header(const std::string& key) const;

    // Reconstruct the whole file: raw_header (verbatim) + serialize_entry per entry.
    // The FIDELITY GATE (tests/po_roundtrip_test) asserts reconstruct() == the original LF bytes.
    std::string reconstruct() const;

    // Serialize ONE entry to gettext-canonical bytes (escaping + 78-col wrap, trailing blank line).
    // MUST reproduce polib byte-for-byte (tests/po_roundtrip_test is the gate).
    static std::string serialize_entry(const PoEntry&);

    // Minimal-diff writer: start from the ORIGINAL file bytes and replace only the msgstr blocks
    // of edited entries (matched by (msgctxt,msgid)), preserving every other byte. Edits whose key
    // is absent are dropped (matches the "apply onto latest .po" PR model). Header policy: bump
    // PO-Revision-Date + Last-Translator + append '# Translators:'; NEVER touch POT-Creation-Date.
    static std::string splice(const std::string& original_bytes,
                              const std::vector<PoEntry>& edits,
                              const std::string& translator /* "Name <email>" */);
};

// gettext-canonical helpers (public for the C++<->polib parity tests).
std::string escape_po(const std::string& s);    // \\  \"  \n  \t  \r
std::string unescape_po(const std::string& s);
std::string wrap_po(const std::string& keyword, const std::string& value, int width = 78);

// 1-based line number of the `msgstr` line for the (msgctxt,msgid) entry in raw PO `bytes`, or -1
// if that entry isn't found. Used by the reverse-push flow (mpctrans::txsync::tx_reverse_push_plan)
// to look up a GitHub blame date for a translation: the line a `git blame` would attribute to
// whoever last touched that entry's value. LF-based line counting; CRLF is tolerated (a trailing
// '\r' is stripped only for content comparison, never affecting the line count itself).
int msgstr_line(const std::string& bytes, const std::string& msgctxt, const std::string& msgid);

} // namespace mpctrans
