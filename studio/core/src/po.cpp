// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/po.h"

#include <ctime>
#include <stdexcept>

// gettext-canonical PO I/O — a byte-exact C++ port of polib's serialization (_str_field +
// textwrap.wrap(escaped, 76, drop_whitespace=False, break_long_words=False)). All width math is
// in Unicode codepoints (matching Python len()). Gated by tests/po_roundtrip_test over all 177 .po.

namespace mpctrans {

// ---------- escaping (order-independent single pass == polib.escape) ----------
std::string escape_po(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) switch (c) {
        case '\\': o += "\\\\"; break;
        case '"':  o += "\\\""; break;
        case '\n': o += "\\n";  break;
        case '\t': o += "\\t";  break;
        case '\r': o += "\\r";  break;
        default:   o += c;
    }
    return o;
}
std::string unescape_po(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[++i]) {
                case 'n': o += '\n'; break;  case 't': o += '\t'; break;
                case 'r': o += '\r'; break;  case '"': o += '"';  break;
                case '\\': o += '\\'; break; default: o += '\\'; o += s[i];
            }
        } else o += s[i];
    }
    return o;
}

// ---------- helpers ----------
static size_t cp_len(const std::string& s) {                  // UTF-8 codepoint count
    size_t n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n; return n;
}
static std::vector<std::string> splitlines_keepends(const std::string& s) {  // \n, \r\n, \r
    std::vector<std::string> out; size_t start = 0, i = 0;
    while (i < s.size()) {
        if (s[i] == '\n') { out.push_back(s.substr(start, i - start + 1)); start = ++i; }
        else if (s[i] == '\r') {
            size_t len = (i + 1 < s.size() && s[i + 1] == '\n') ? 2 : 1;
            out.push_back(s.substr(start, i - start + len)); i += len; start = i;
        } else ++i;
    }
    if (start < s.size()) out.push_back(s.substr(start));
    return out;
}
// UTF-8 codepoint view: (byte offset, byte length, is-letter, ascii-char-or-0).
// "letter" ~= Python [^\d\W]: ASCII A-Za-z, plus (approx) any multibyte codepoint.
struct CP { size_t off, len; bool letter; char ascii; };
static std::vector<CP> codepoints(const std::string& s) {
    std::vector<CP> v; size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        size_t len = (c < 0x80) ? 1 : (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (i + len > s.size()) len = 1;
        bool letter; char ascii = 0;
        if (c < 0x80) { ascii = (char)c; letter = (c>='A'&&c<='Z')||(c>='a'&&c<='z'); }
        else letter = true;
        v.push_back({i, len, letter, ascii}); i += len;
    }
    return v;
}
// break_on_hyphens: split a word after a '-' when (letter,letter,-) or (letter,-,letter,-) precede
// and a letter follows — matching Python textwrap's wordsep_re main hyphen branch.
static std::vector<std::string> split_word_hyphens(const std::string& w) {
    auto cp = codepoints(w);
    std::vector<std::string> out; size_t seg = 0;
    for (size_t k = 0; k < cp.size(); ++k) {
        if (cp[k].ascii != '-') continue;
        bool pre = (k >= 2 && cp[k-1].letter && cp[k-2].letter) ||
                   (k >= 3 && cp[k-1].letter && cp[k-2].ascii=='-' && cp[k-3].letter);
        // lookahead (?=letter -? letter): needs TWO letters after the hyphen (opt. hyphen between)
        bool post = (k + 1 < cp.size() && cp[k+1].letter) &&
                    ( (k + 2 < cp.size() && cp[k+2].letter) ||
                      (k + 3 < cp.size() && cp[k+2].ascii == '-' && cp[k+3].letter) );
        if (pre && post) { size_t end = cp[k].off + cp[k].len; out.push_back(w.substr(seg, end - seg)); seg = end; }
    }
    out.push_back(w.substr(seg));
    return out;
}
// textwrap.wrap(text, W, drop_whitespace=False, break_long_words=False, break_on_hyphens=True).
static std::vector<std::string> tw_wrap(const std::string& text, size_t W) {
    std::vector<std::string> chunks; size_t i = 0;
    while (i < text.size()) {
        bool sp = text[i] == ' '; size_t j = i;
        while (j < text.size() && (text[j] == ' ') == sp) ++j;
        std::string run = text.substr(i, j - i);
        if (sp) chunks.push_back(run);
        else for (auto& sub : split_word_hyphens(run)) chunks.push_back(sub);
        i = j;
    }
    std::vector<std::string> lines; size_t k = 0, n = chunks.size();
    while (k < n) {
        std::string cur; size_t cur_len = 0;
        while (k < n) { size_t l = cp_len(chunks[k]);
            if (cur_len + l <= W) { cur += chunks[k]; cur_len += l; ++k; } else break; }
        if (k < n && cp_len(chunks[k]) > W && cur_len == 0) { cur = chunks[k]; ++k; }  // long word overflow
        if (!cur.empty()) lines.push_back(cur);
        else if (k < n) { lines.push_back(chunks[k]); ++k; }   // progress guard
        else break;
    }
    return lines;
}

// polib _BaseEntry._str_field: returns the emitted lines for one field.
static std::vector<std::string> str_field(const std::string& fieldname, const std::string& field) {
    std::vector<std::string> lines;
    auto segs = splitlines_keepends(field);
    if (segs.size() > 1) {
        lines.push_back("");
        for (auto& s : segs) lines.push_back(s);
    } else {
        size_t sc = 0;
        for (char c : field) if (c=='\\'||c=='\n'||c=='\r'||c=='\t'||c=='\v'||c=='\b'||c=='\f'||c=='"') ++sc;
        long real_wrap = 78 - (long)(fieldname.size() + 3) + (long)sc;
        if ((long)cp_len(field) > real_wrap) {
            lines.push_back("");
            for (auto& w : tw_wrap(escape_po(field), 76)) lines.push_back(unescape_po(w));
        } else {
            lines.push_back(field);
        }
    }
    std::vector<std::string> out;
    out.push_back(fieldname + " \"" + escape_po(lines[0]) + "\"");
    for (size_t k = 1; k < lines.size(); ++k) out.push_back("\"" + escape_po(lines[k]) + "\"");
    return out;
}

std::string PoFile::serialize_entry(const PoEntry& e) {
    std::vector<std::string> ret = e.comments;                 // '#…' lines verbatim
    if (!e.msgctxt.empty()) { auto f = str_field("msgctxt", e.msgctxt); ret.insert(ret.end(), f.begin(), f.end()); }
    { auto f = str_field("msgid",  e.msgid);  ret.insert(ret.end(), f.begin(), f.end()); }
    { auto f = str_field("msgstr", e.msgstr); ret.insert(ret.end(), f.begin(), f.end()); }
    ret.push_back("");                                          // polib appends '' -> trailing '\n'
    std::string out;
    for (size_t i = 0; i < ret.size(); ++i) { out += ret[i]; if (i + 1 < ret.size()) out += "\n"; }
    return out;
}

std::string wrap_po(const std::string& keyword, const std::string& value, int /*width*/) {
    auto f = str_field(keyword, value);
    std::string out; for (size_t i = 0; i < f.size(); ++i) { out += f[i]; if (i + 1 < f.size()) out += "\n"; }
    return out;
}

// ---------- parsing ----------
static std::string extract_quoted(const std::string& line) {  // raw (escaped) content between first/last '"'
    size_t a = line.find('"'); size_t b = line.rfind('"');
    if (a == std::string::npos || b <= a) return "";
    return line.substr(a + 1, b - a - 1);
}
static bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

static PoEntry parse_entry_block(const std::string& block) {
    PoEntry e;
    std::vector<std::string> lines; { size_t s = 0, i = 0;
        for (; i < block.size(); ++i) if (block[i] == '\n') { lines.push_back(block.substr(s, i - s)); s = i + 1; }
        if (s < block.size()) lines.push_back(block.substr(s)); }
    size_t i = 0;
    while (i < lines.size() && !lines[i].empty() && lines[i][0] == '#') e.comments.push_back(lines[i++]);
    auto read_field = [&](const char* kw, std::string& dest) {
        std::string raw = extract_quoted(lines[i]); ++i;
        while (i < lines.size() && !lines[i].empty() && lines[i][0] == '"') raw += extract_quoted(lines[i++]);
        dest = unescape_po(raw);
    };
    for (; i < lines.size();) {
        if      (starts_with(lines[i], "msgctxt")) read_field("msgctxt", e.msgctxt);
        else if (starts_with(lines[i], "msgid"))   read_field("msgid",   e.msgid);
        else if (starts_with(lines[i], "msgstr"))  read_field("msgstr",  e.msgstr);
        else ++i;
    }
    return e;
}

PoFile PoFile::parse_bytes(const std::string& bytes_in) {
    std::string s; s.reserve(bytes_in.size());               // normalize CRLF->LF
    for (size_t i = 0; i < bytes_in.size(); ++i) {
        if (bytes_in[i] == '\r' && i + 1 < bytes_in.size() && bytes_in[i + 1] == '\n') continue;
        s += bytes_in[i];
    }
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) s = s.substr(3);     // strip BOM

    PoFile po;
    std::vector<std::string> blocks; size_t pos = 0;
    while (true) { size_t nn = s.find("\n\n", pos);
        if (nn == std::string::npos) { blocks.push_back(s.substr(pos)); break; }
        blocks.push_back(s.substr(pos, nn - pos)); pos = nn + 2; }
    po.raw_header = blocks.empty() ? "" : blocks[0] + "\n\n";
    for (size_t bi = 1; bi < blocks.size(); ++bi)
        if (!blocks[bi].empty()) po.entries.push_back(parse_entry_block(blocks[bi]));
    return po;
}
PoFile PoFile::parse_file(const std::string&) { throw std::logic_error("parse_file: use parse_bytes(read(path))"); }

std::string PoFile::reconstruct() const {
    std::string out = raw_header;
    for (size_t i = 0; i < entries.size(); ++i) {
        out += serialize_entry(entries[i]);
        if (i + 1 < entries.size()) out += "\n";
    }
    return out;
}

const PoEntry* PoFile::find(const std::string& ctx, const std::string& id) const {
    for (auto& e : entries) if (e.msgctxt == ctx && e.msgid == id) return &e;
    return nullptr;
}
std::string PoFile::get_header(const std::string& k) const {
    for (auto& kv : metadata) if (kv.first == k) return kv.second;
    return {};
}

// ---------- splice (minimal-diff writer) ----------
// "2026-07-05 12:34+0000" (gettext/Transifex PO-Revision-Date format), UTC.
static std::string now_po_revision_utc(std::string* year_out) {
    std::time_t t = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char buf[32]; std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M+0000", &g);
    if (year_out) { char y[8]; std::strftime(y, sizeof y, "%Y", &g); *year_out = y; }
    return buf;
}
// Replace the value of `"Key: value\n"` inside the raw header block (the \n is the two
// literal chars backslash+n of the quoted header line, not a newline).
static bool replace_header_field(std::string& hdr, const std::string& key, const std::string& value) {
    const std::string prefix = "\"" + key + ": ";
    size_t p = hdr.find(prefix);
    if (p == std::string::npos) return false;
    size_t start = p + prefix.size();
    size_t end = hdr.find("\\n\"", start);
    if (end == std::string::npos) return false;
    hdr.replace(start, end - start, value);
    return true;
}
// Append '# {translator}, {year}' at the end of the '# Translators:' block (before its
// terminating bare '#'/'# ' line). No-op if the translator is already credited (any year).
static void add_translator_credit(std::string& hdr, const std::string& translator, const std::string& year) {
    const std::string marker = "# Translators:\n";
    size_t tpos = hdr.find(marker);
    if (tpos == std::string::npos) return;
    const std::string credit_prefix = "# " + translator + ",";
    size_t pos = tpos + marker.size(), insert_at = pos;
    while (pos < hdr.size() && hdr[pos] == '#') {
        size_t eol = hdr.find('\n', pos);
        if (eol == std::string::npos) eol = hdr.size();
        std::string line = hdr.substr(pos, eol - pos);
        if (line.rfind(credit_prefix, 0) == 0) return;      // already credited
        if (line == "#" || line == "# ") break;             // end-of-block terminator
        pos = eol + 1;
        insert_at = pos;
    }
    hdr.insert(insert_at, credit_prefix + " " + year + "\n");
}

std::string PoFile::splice(const std::string& original_bytes, const std::vector<PoEntry>& edits,
                           const std::string& translator) {
    PoFile po = parse_bytes(original_bytes);
    bool changed = false;
    for (const auto& ed : edits) {
        for (auto& e : po.entries)
            if (e.msgctxt == ed.msgctxt && e.msgid == ed.msgid) {
                if (e.msgstr != ed.msgstr) { e.msgstr = ed.msgstr; changed = true; }
                break;
            }
        // key absent in the original -> edit dropped (apply-onto-latest PR model)
    }
    if (changed) {
        std::string year;
        replace_header_field(po.raw_header, "PO-Revision-Date", now_po_revision_utc(&year));
        if (!translator.empty()) {
            replace_header_field(po.raw_header, "Last-Translator", escape_po(translator));
            add_translator_credit(po.raw_header, translator, year);
        }
        // POT-Creation-Date is never touched.
    }
    return po.reconstruct();   // byte-exact for unchanged entries => minimal diff
}

// ---------- msgstr_line ----------
// Re-scans the raw bytes line-by-line (rather than reusing parse_bytes's block splitter) so the
// returned line number is a real 1-based offset into `bytes` -- what GitHub's blame API indexes by.
// Matching is done on the ESCAPED (raw quoted-line) form via escape_po, the same representation
// parse_entry_block's read_field unescapes from -- so this stays consistent with the parser without
// re-deriving it. msgctxt is assumed never wrapped (true of every .po this tool touches); msgid
// wraps are handled by concatenating continuation lines exactly like read_field does.
int msgstr_line(const std::string& bytes, const std::string& msgctxt, const std::string& msgid) {
    std::vector<std::string> lines;
    { size_t s = 0;
      for (size_t i = 0; i < bytes.size(); ++i)
          if (bytes[i] == '\n') { lines.push_back(bytes.substr(s, i - s)); s = i + 1; }
      if (s < bytes.size()) lines.push_back(bytes.substr(s));
    }
    auto strip_cr = [](std::string s) { if (!s.empty() && s.back() == '\r') s.pop_back(); return s; };

    const std::string escCtx = escape_po(msgctxt);
    const std::string escId  = escape_po(msgid);

    size_t n = lines.size(), i = 0;
    while (i < n) {
        while (i < n && strip_cr(lines[i]).empty()) ++i;   // skip blank separator line(s)
        if (i >= n) break;

        bool haveCtx = false, haveId = false;
        std::string blockCtx, blockId;
        size_t msgstrLine = 0;
        size_t j = i;
        while (j < n) {
            std::string line = strip_cr(lines[j]);
            if (line.empty()) break;   // end of this entry's block
            if (!haveId && !haveCtx && line.rfind("msgctxt \"", 0) == 0) {
                blockCtx = extract_quoted(line);
                haveCtx = true;
                ++j; continue;
            }
            if (!haveId && line.rfind("msgid \"", 0) == 0) {
                blockId = extract_quoted(line);
                haveId = true;
                ++j;
                while (j < n) {   // wrapped continuation lines: bare quoted strings
                    std::string cl = strip_cr(lines[j]);
                    if (!cl.empty() && cl[0] == '"') { blockId += extract_quoted(cl); ++j; }
                    else break;
                }
                continue;
            }
            if (msgstrLine == 0 && line.rfind("msgstr", 0) == 0) msgstrLine = j + 1;   // 1-based
            ++j;
        }
        if (haveId && msgstrLine != 0) {
            std::string ctxCmp = haveCtx ? blockCtx : std::string();
            if (ctxCmp == escCtx && blockId == escId) return (int)msgstrLine;
        }
        i = (j > i) ? j : i + 1;   // advance past this block (guards zero-progress on odd input)
    }
    return -1;
}

} // namespace mpctrans
