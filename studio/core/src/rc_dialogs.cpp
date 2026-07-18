// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime RC dialog parser: reconstructs each IDD_* DIALOG(EX) block from the raw
// .rc (UTF-16LE) + resource.h into RcDialog/RcControl, mirroring what rc.exe emits.
// The compiled template in dist/mpcresources.neutral.dll is the oracle; see
// tests/rc_conformance_test.cpp. Portable — no Windows headers used here.
#include "mpctrans/rc_dialogs.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mpctrans {

namespace {

// ---------------------------------------------------------------------------
// UTF-16LE (raw file bytes, optional 0xFF 0xFE BOM) -> UTF-8 std::string.
// ---------------------------------------------------------------------------
void append_utf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string decode_utf16le(const std::string& b) {
    std::string out;
    out.reserve(b.size());
    size_t i = 0;
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) i = 2;
    for (; i + 1 < b.size(); i += 2) {
        unsigned u = (unsigned char)b[i] | ((unsigned char)b[i + 1] << 8);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < b.size()) {
            unsigned lo = (unsigned char)b[i + 2] | ((unsigned char)b[i + 3] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                unsigned cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                append_utf8(out, cp);
                i += 2;  // loop adds 2 more
                continue;
            }
        }
        append_utf8(out, u);
    }
    return out;
}

// Decode RC bytes to UTF-8, auto-detecting the source encoding: a UTF-16LE BOM (FF FE) or an
// ASCII byte followed by NUL (interleaved-null pattern) means UTF-16LE (the on-disk working-tree
// file); otherwise the bytes are already UTF-8 (what GitHub's contents API returns — the repo
// stores the blob UTF-8 via a working-tree-encoding filter). A UTF-8 BOM is stripped.
std::string decode_rc(const std::string& b) {
    bool utf16 = (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE)
              || (b.size() >= 2 && b[0] != 0 && b[1] == 0);   // e.g. '/' 0x00 ...
    if (utf16) return decode_utf16le(b);
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB &&
        (unsigned char)b[2] == 0xBF) return b.substr(3);
    return b;   // already UTF-8
}

// ---------------------------------------------------------------------------
// Symbol table: standard Win32 control ids (afxres/winres, not in resource.h) seeded
// first, then resource.h #defines add/override. Unknown symbols resolve to -1.
// ---------------------------------------------------------------------------
std::unordered_map<std::string, long long> build_symbols(const std::string& h) {
    static const std::pair<const char*, long long> seeds[] = {
        {"IDOK", 1}, {"IDCANCEL", 2}, {"IDABORT", 3}, {"IDRETRY", 4}, {"IDIGNORE", 5},
        {"IDYES", 6}, {"IDNO", 7}, {"IDCLOSE", 8}, {"IDHELP", 9}, {"IDTRYAGAIN", 10},
        {"IDCONTINUE", 11}, {"IDC_STATIC", -1}, {"ID_APPLY_NOW", 0x3021},
    };
    std::unordered_map<std::string, long long> syms;
    for (auto& s : seeds) syms.emplace(s.first, s.second);

    // #define NAME (0xHEX | -?DEC)
    auto is_id = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    size_t i = 0, n = h.size();
    while (i < n) {
        // find "#define"
        if (h[i] == '#' && i + 7 <= n && h.compare(i, 7, "#define") == 0) {
            size_t j = i + 7;
            while (j < n && (h[j] == ' ' || h[j] == '\t')) j++;
            size_t name0 = j;
            while (j < n && is_id(h[j])) j++;
            if (j == name0) { i++; continue; }
            std::string name = h.substr(name0, j - name0);
            while (j < n && (h[j] == ' ' || h[j] == '\t')) j++;
            size_t v0 = j;
            if (j < n && h[j] == '-') j++;
            bool hex = false;
            if (j + 2 <= n && h[j] == '0' && (h[j + 1] == 'x' || h[j + 1] == 'X')) {
                hex = true; j += 2;
            }
            size_t d0 = j;
            while (j < n) {
                char c = h[j];
                if (std::isdigit((unsigned char)c) || (hex && std::isxdigit((unsigned char)c))) j++;
                else break;
            }
            if (j == d0) { i++; continue; }  // value not a plain integer (e.g. expression) -> skip
            // boundary: must be end-of-token
            if (j < n && is_id(h[j])) { i++; continue; }
            std::string vs = h.substr(v0, j - v0);
            long long val = 0;
            int sign = 1; size_t k = 0;
            if (k < vs.size() && vs[k] == '-') { sign = -1; k++; }
            for (; k < vs.size(); ++k) {
                char c = vs[k];
                int dv = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                        : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
                val = val * (hex ? 16 : 10) + dv;
            }
            syms[name] = sign * val;
            i = j;
        } else {
            i++;
        }
    }
    return syms;
}

// ---------------------------------------------------------------------------
// Win32 style constant table (window/class/button/static/edit/combo/list/scroll/
// updown/track/tree/list-view/progress/anim). Values per winuser.h — verified.
// ---------------------------------------------------------------------------
const std::unordered_map<std::string, unsigned long>& style_table() {
    static const std::unordered_map<std::string, unsigned long> m = {
        // Window (WS_*)
        {"WS_OVERLAPPED",0x00000000},{"WS_POPUP",0x80000000},{"WS_CHILD",0x40000000},
        {"WS_MINIMIZE",0x20000000},{"WS_VISIBLE",0x10000000},{"WS_DISABLED",0x08000000},
        {"WS_CLIPSIBLINGS",0x04000000},{"WS_CLIPCHILDREN",0x02000000},{"WS_MAXIMIZE",0x01000000},
        {"WS_CAPTION",0x00C00000},{"WS_BORDER",0x00800000},{"WS_DLGFRAME",0x00400000},
        {"WS_VSCROLL",0x00200000},{"WS_HSCROLL",0x00100000},{"WS_SYSMENU",0x00080000},
        {"WS_THICKFRAME",0x00040000},{"WS_GROUP",0x00020000},{"WS_TABSTOP",0x00010000},
        {"WS_MINIMIZEBOX",0x00020000},{"WS_MAXIMIZEBOX",0x00010000},
        // Extended window (WS_EX_*)
        {"WS_EX_DLGMODALFRAME",0x00000001},{"WS_EX_TOPMOST",0x00000008},{"WS_EX_ACCEPTFILES",0x00000010},
        {"WS_EX_TRANSPARENT",0x00000020},{"WS_EX_TOOLWINDOW",0x00000080},{"WS_EX_WINDOWEDGE",0x00000100},
        {"WS_EX_CLIENTEDGE",0x00000200},{"WS_EX_CONTEXTHELP",0x00000400},{"WS_EX_RIGHT",0x00001000},
        {"WS_EX_STATICEDGE",0x00020000},{"WS_EX_APPWINDOW",0x00040000},{"WS_EX_LAYERED",0x00080000},
        // Dialog (DS_*)
        {"DS_SETFONT",0x40},{"DS_MODALFRAME",0x80},{"DS_NOFAILCREATE",0x0010},{"DS_CENTER",0x0800},
        {"DS_3DLOOK",0x0004},{"DS_FIXEDSYS",0x0008},{"DS_CONTROL",0x0400},
        // Button (BS_*)
        {"BS_PUSHBUTTON",0x00},{"BS_DEFPUSHBUTTON",0x01},{"BS_CHECKBOX",0x02},{"BS_AUTOCHECKBOX",0x03},
        {"BS_RADIOBUTTON",0x04},{"BS_3STATE",0x05},{"BS_AUTO3STATE",0x06},{"BS_GROUPBOX",0x07},
        {"BS_USERBUTTON",0x08},{"BS_AUTORADIOBUTTON",0x09},{"BS_PUSHBOX",0x0A},{"BS_OWNERDRAW",0x0B},
        {"BS_LEFTTEXT",0x20},{"BS_ICON",0x40},{"BS_BITMAP",0x80},{"BS_LEFT",0x0100},{"BS_RIGHT",0x0200},
        {"BS_CENTER",0x0300},{"BS_TOP",0x0400},{"BS_BOTTOM",0x0800},{"BS_VCENTER",0x0C00},
        {"BS_PUSHLIKE",0x1000},{"BS_MULTILINE",0x2000},{"BS_NOTIFY",0x4000},{"BS_FLAT",0x8000},
        // Static (SS_*)
        {"SS_LEFT",0x00},{"SS_CENTER",0x01},{"SS_RIGHT",0x02},{"SS_ICON",0x03},{"SS_BLACKRECT",0x04},
        {"SS_GRAYRECT",0x05},{"SS_WHITERECT",0x06},{"SS_BLACKFRAME",0x07},{"SS_GRAYFRAME",0x08},
        {"SS_WHITEFRAME",0x09},{"SS_SIMPLE",0x0B},{"SS_LEFTNOWORDWRAP",0x0C},{"SS_OWNERDRAW",0x0D},
        {"SS_BITMAP",0x0E},{"SS_ENHMETAFILE",0x0F},{"SS_ETCHEDHORZ",0x10},{"SS_ETCHEDVERT",0x11},
        {"SS_ETCHEDFRAME",0x12},{"SS_REALSIZECONTROL",0x040},{"SS_NOPREFIX",0x0080},{"SS_NOTIFY",0x0100},
        {"SS_CENTERIMAGE",0x0200},{"SS_RIGHTJUST",0x0400},{"SS_REALSIZEIMAGE",0x0800},{"SS_SUNKEN",0x1000},
        {"SS_EDITCONTROL",0x2000},{"SS_ENDELLIPSIS",0x4000},{"SS_PATHELLIPSIS",0x8000},{"SS_WORDELLIPSIS",0xC000},
        // Edit (ES_*)
        {"ES_LEFT",0x00},{"ES_CENTER",0x01},{"ES_RIGHT",0x02},{"ES_MULTILINE",0x04},{"ES_UPPERCASE",0x08},
        {"ES_LOWERCASE",0x10},{"ES_PASSWORD",0x20},{"ES_AUTOVSCROLL",0x40},{"ES_AUTOHSCROLL",0x80},
        {"ES_NOHIDESEL",0x0100},{"ES_OEMCONVERT",0x0400},{"ES_READONLY",0x0800},{"ES_WANTRETURN",0x1000},
        {"ES_NUMBER",0x2000},
        // Combo (CBS_*)
        {"CBS_SIMPLE",0x01},{"CBS_DROPDOWN",0x02},{"CBS_DROPDOWNLIST",0x03},{"CBS_OWNERDRAWFIXED",0x10},
        {"CBS_OWNERDRAWVARIABLE",0x20},{"CBS_AUTOHSCROLL",0x40},{"CBS_OEMCONVERT",0x80},{"CBS_SORT",0x0100},
        {"CBS_HASSTRINGS",0x0200},{"CBS_NOINTEGRALHEIGHT",0x0400},{"CBS_DISABLENOSCROLL",0x0800},
        {"CBS_UPPERCASE",0x2000},{"CBS_LOWERCASE",0x4000},
        // List (LBS_*)
        {"LBS_NOTIFY",0x01},{"LBS_SORT",0x02},{"LBS_NOREDRAW",0x04},{"LBS_MULTIPLESEL",0x08},
        {"LBS_OWNERDRAWFIXED",0x10},{"LBS_OWNERDRAWVARIABLE",0x20},{"LBS_HASSTRINGS",0x40},
        {"LBS_USETABSTOPS",0x80},{"LBS_NOINTEGRALHEIGHT",0x0100},{"LBS_MULTICOLUMN",0x0200},
        {"LBS_WANTKEYBOARDINPUT",0x0400},{"LBS_EXTENDEDSEL",0x0800},{"LBS_DISABLENOSCROLL",0x1000},
        {"LBS_NODATA",0x2000},{"LBS_NOSEL",0x4000},
        // Scrollbar (SBS_*)
        {"SBS_HORZ",0x00},{"SBS_VERT",0x01},{"SBS_TOPALIGN",0x02},{"SBS_LEFTALIGN",0x02},
        {"SBS_BOTTOMALIGN",0x04},{"SBS_RIGHTALIGN",0x04},{"SBS_SIZEBOX",0x08},{"SBS_SIZEGRIP",0x10},
        // Up-down (UDS_*)
        {"UDS_WRAP",0x0001},{"UDS_SETBUDDYINT",0x0002},{"UDS_ALIGNRIGHT",0x0004},{"UDS_ALIGNLEFT",0x0008},
        {"UDS_AUTOBUDDY",0x0010},{"UDS_ARROWKEYS",0x0020},{"UDS_HORZ",0x0040},{"UDS_NOTHOUSANDS",0x0080},
        {"UDS_HOTTRACK",0x0100},
        // Trackbar (TBS_*)
        {"TBS_AUTOTICKS",0x0001},{"TBS_VERT",0x0002},{"TBS_TOP",0x0004},{"TBS_BOTH",0x0008},
        {"TBS_NOTICKS",0x0010},{"TBS_ENABLESELRANGE",0x0020},{"TBS_FIXEDLENGTH",0x0040},
        {"TBS_NOTHUMB",0x0080},{"TBS_TOOLTIPS",0x0100},{"TBS_DOWNISLEFT",0x0400},
        // Tree view (TVS_*)
        {"TVS_HASBUTTONS",0x0001},{"TVS_HASLINES",0x0002},{"TVS_LINESATROOT",0x0004},
        {"TVS_EDITLABELS",0x0008},{"TVS_DISABLEDRAGDROP",0x0010},{"TVS_SHOWSELALWAYS",0x0020},
        // List view (LVS_*)
        {"LVS_REPORT",0x0001},{"LVS_SMALLICON",0x0002},{"LVS_LIST",0x0003},{"LVS_SINGLESEL",0x0004},
        {"LVS_SHOWSELALWAYS",0x0008},{"LVS_SORTASCENDING",0x0010},{"LVS_SORTDESCENDING",0x0020},
        {"LVS_SHAREIMAGELISTS",0x0040},{"LVS_NOLABELWRAP",0x0080},{"LVS_AUTOARRANGE",0x0100},
        {"LVS_EDITLABELS",0x0200},{"LVS_OWNERDRAWFIXED",0x0400},{"LVS_NOSCROLL",0x2000},
        {"LVS_ALIGNLEFT",0x0800},{"LVS_NOCOLUMNHEADER",0x4000},{"LVS_NOSORTHEADER",0x8000},
        // Progress (PBS_*)
        {"PBS_SMOOTH",0x01},{"PBS_VERTICAL",0x04},
        // Animation (ACS_*)
        {"ACS_CENTER",0x0001},{"ACS_TRANSPARENT",0x0002},{"ACS_AUTOPLAY",0x0004},
    };
    return m;
}

// ---------------------------------------------------------------------------
// Tokenizer. A token is a quoted string literal (body kept verbatim, "" escapes
// preserved), a comma, a pipe, or a "word" (identifier / number / { / }). Line
// and block comments are stripped, line continuations joined, and # preprocessor
// directives dropped — all while never looking inside a string literal.
// ---------------------------------------------------------------------------
enum class TT { WORD, STR, COMMA, PIPE };
struct Tok { TT type; std::string val; };

bool is_word_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '+' || c == '-';
}

std::vector<Tok> tokenize(const std::string& s) {
    std::vector<Tok> toks;
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '#') { while (i < n && s[i] != '\n') i++; continue; }            // #directive
        if (c == '\\' && i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) {   // \<newline>
            i += 2; while (i < n && (s[i] == '\n' || s[i] == '\r')) i++; continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {                          // line comment
            i += 2; while (i < n && s[i] != '\n') i++; continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {                          // block comment
            i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++; i += 2; continue;
        }
        if (c == '"') {                                                          // string literal
            i++;
            std::string v;
            while (i < n) {
                if (s[i] == '"') {
                    if (i + 1 < n && s[i + 1] == '"') { v += "\"\""; i += 2; continue; } // "" escape (kept)
                    i++; break;
                }
                v += s[i]; i++;
            }
            toks.push_back({TT::STR, std::move(v)});
            continue;
        }
        if (c == ',') { toks.push_back({TT::COMMA, ","}); i++; continue; }
        if (c == '|') { toks.push_back({TT::PIPE, "|"}); i++; continue; }
        if (c == '{' || c == '}') { toks.push_back({TT::WORD, std::string(1, c)}); i++; continue; }
        if (c == '(' || c == ')') { i++; continue; }                             // ignore parens
        if (is_word_char(c)) {
            std::string v;
            while (i < n && is_word_char(s[i])) { v += s[i]; i++; }
            toks.push_back({TT::WORD, std::move(v)});
            continue;
        }
        i++;  // any other punctuation: drop
    }
    return toks;
}

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool is_number(const std::string& s) {
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    if (i >= s.size()) return false;
    if (s[i] == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        if (i >= s.size()) return false;
        for (; i < s.size(); ++i) if (!std::isxdigit((unsigned char)s[i])) return false;
        return true;
    }
    for (; i < s.size(); ++i) if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}

long long parse_int(const std::string& s) {
    bool neg = false; size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    bool hex = false;
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { hex = true; i += 2; }
    long long v = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        v = v * (hex ? 16 : 10) + d;
    }
    return neg ? -v : v;
}

// Recognizers for statement/structural keywords (so a style expr or field scan
// knows where a statement or header attribute ends).
bool is_control_kw(const std::string& kw) {
    static const std::unordered_map<std::string, char> k = {
        {"CONTROL",1},{"LTEXT",1},{"RTEXT",1},{"CTEXT",1},{"ICON",1},{"PUSHBUTTON",1},
        {"DEFPUSHBUTTON",1},{"CHECKBOX",1},{"AUTOCHECKBOX",1},{"STATE3",1},{"AUTO3STATE",1},
        {"RADIOBUTTON",1},{"AUTORADIOBUTTON",1},{"GROUPBOX",1},{"PUSHBOX",1},{"EDITTEXT",1},
        {"COMBOBOX",1},{"LISTBOX",1},{"SCROLLBAR",1},
    };
    return k.count(kw) != 0;
}
bool is_struct_kw(const std::string& kw) {
    return kw == "STYLE" || kw == "EXSTYLE" || kw == "CAPTION" || kw == "FONT" ||
           kw == "BEGIN" || kw == "END" || kw == "MENU" || kw == "CLASS" ||
           kw == "LANGUAGE" || kw == "CHARACTERISTICS" || kw == "VERSION" ||
           kw == "DIALOGEX" || kw == "DIALOG" || kw == "{" || kw == "}";
}

// ---------------------------------------------------------------------------
// Style expression evaluation. A field is the PIPE-separated tokens between commas
// (or the tokens of a header STYLE/EXSTYLE expr). Terms: optional NOT + a numeric
// literal or a symbolic constant (style table, then resource.h, else 0). A term
// prefixed with NOT accumulates into a not-mask that is cleared from the result.
// ---------------------------------------------------------------------------
struct StyleParts { unsigned long pos = 0, nots = 0; };
StyleParts eval_parts(const std::vector<Tok>& field,
                      const std::unordered_map<std::string, long long>& syms) {
    const auto& st = style_table();
    StyleParts sp;
    size_t k = 0;
    while (k < field.size()) {
        if (field[k].type != TT::WORD) { k++; continue; }
        bool negate = false;
        if (upper(field[k].val) == "NOT") { negate = true; k++; }
        unsigned long v = 0;
        if (k < field.size() && field[k].type == TT::WORD) {
            const std::string& name = field[k].val;
            if (is_number(name)) v = (unsigned long)parse_int(name);
            else {
                auto it = st.find(upper(name));
                if (it == st.end()) {
                    auto sit = syms.find(name);
                    if (sit == syms.end()) {
                        auto sit2 = syms.find(upper(name));
                        if (sit2 != syms.end()) v = (unsigned long)sit2->second;
                    } else v = (unsigned long)sit->second;
                } else v = it->second;
            }
            k++;
        }
        if (negate) sp.nots |= v; else sp.pos |= v;
    }
    return sp;
}
unsigned long eval_field(const std::vector<Tok>& field,
                         const std::unordered_map<std::string, long long>& syms) {
    StyleParts sp = eval_parts(field, syms);
    return sp.pos & ~sp.nots;
}

// Read a STYLE/EXSTYLE expression starting at index i, consuming PIPE-separated
// terms until a comma or a structural/control keyword. Returns the value.
unsigned long read_header_style(const std::vector<Tok>& toks, size_t& i,
                                const std::unordered_map<std::string, long long>& syms) {
    std::vector<Tok> field;
    while (i < toks.size()) {
        const Tok& t = toks[i];
        if (t.type == TT::COMMA) break;
        if (t.type == TT::PIPE) { field.push_back(t); i++; continue; }
        if (t.type == TT::WORD) {
            std::string kw = upper(t.val);
            if (is_struct_kw(kw) || is_control_kw(kw)) break;
            field.push_back(t); i++; continue;
        }
        break;  // stray STRING
    }
    return eval_field(field, syms);
}

// Field accessors over a comma-split statement.
const Tok* first_word(const std::vector<Tok>& f) {
    for (const auto& t : f) if (t.type == TT::WORD) return &t;
    return nullptr;
}
std::string field_str(const std::vector<Tok>& f) {
    for (const auto& t : f) if (t.type == TT::STR) return t.val;
    return "";
}
long long field_id(const std::vector<Tok>& f, const std::unordered_map<std::string, long long>& syms) {
    const Tok* w = first_word(f);
    if (!w) return -1;
    if (is_number(w->val)) return parse_int(w->val);
    auto it = syms.find(w->val);
    return it == syms.end() ? -1 : it->second;
}
// The control id token as written — the first WORD of the id field (a symbol like
// "IDC_STATIC"/"IDOK", or a raw-number id's text). Mirrors the symbol build_index.py's
// TranslationDataRC captures as the msgctxt control segment.
std::string field_sym(const std::vector<Tok>& f) {
    const Tok* w = first_word(f);
    return w ? w->val : "";
}
int field_coord(const std::vector<Tok>& f, const std::unordered_map<std::string, long long>& syms) {
    const Tok* w = first_word(f);
    if (!w) return 0;
    if (is_number(w->val)) return (int)parse_int(w->val);
    auto it = syms.find(w->val);
    return it == syms.end() ? 0 : (int)it->second;
}

const unsigned long WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000;

// Shorthand default-style bit aliases (suffixed/local to avoid clashing with any
// Win32 macro a future Windows-including TU might see).
const unsigned long SS_LEFT = 0x00, SS_CENTER = 0x01, SS_RIGHT = 0x02, SS_ICON = 0x03;
const unsigned long BS_PUSHBUTTON = 0x00, BS_DEFPUSHBUTTON = 0x01, BS_CHECKBOX = 0x02,
      BS_AUTOCHECKBOX = 0x03, BS_3STATE = 0x05, BS_AUTO3STATE = 0x06, BS_RADIOBUTTON = 0x04,
      BS_AUTORADIOBUTTON = 0x09, BS_GROUPBOX = 0x07, BS_PUSHBOX = 0x0A;
const unsigned long WS_TABSTOP_v = 0x00010000, WS_BORDER_v = 0x00800000, WS_VSCROLL_v = 0x00200000;
const unsigned long WS_GROUP_v = 0x00020000;
const unsigned long ES_LEFT = 0x00, LBS_NOTIFY = 0x01, SBS_HORZ = 0x00;

// Parse one control statement. `stmt` is the keyword; `fields` are its comma-split
// argument fields (each field is the PIPE-separated tokens between commas). The final
// style is computed once, after the type-default base and the explicit style field are
// known, so that the implicit WS_CHILD|WS_VISIBLE rc.exe adds is OR'd in BEFORE the
// explicit not-mask is applied (i.e. an explicit "NOT WS_VISIBLE" is honoured).
RcControl parse_control(const std::string& stmt,
                        const std::vector<std::vector<Tok>>& fields,
                        const std::unordered_map<std::string, long long>& syms) {
    RcControl c;
    unsigned long base = 0;          // synthesized type-default bits (shorthand only)
    int style_idx = -1;              // explicit style field index, -1 when absent
    auto str_at = [&](size_t i) { return fields.size() > i ? field_str(fields[i]) : ""; };
    auto id_at  = [&](size_t i) { return fields.size() > i ? field_id(fields[i], syms) : -1; };
    auto sym_at = [&](size_t i) { return fields.size() > i ? field_sym(fields[i]) : ""; };
    auto coord  = [&](size_t i) { return fields.size() > i ? field_coord(fields[i], syms) : 0; };
    auto ex_at  = [&](size_t i) { return fields.size() > i ? eval_field(fields[i], syms) : 0; };
    // Field layouts: GEN = CONTROL; TEXT = text,id,x,y,cx,cy[,style[,ex]]
    //                NOTEXT = id,x,y,cx,cy[,style[,ex]] (EDITTEXT/COMBOBOX/LISTBOX/SCROLLBAR)
    enum Form { GEN, TEXT, NOTEXT } form = GEN;

    if (stmt == "CONTROL")            { form = GEN;    c.cls = str_at(2); }
    else if (stmt == "LTEXT")         { form = TEXT;   c.cls = "Static";  base = SS_LEFT   | WS_GROUP_v; }
    else if (stmt == "CTEXT")         { form = TEXT;   c.cls = "Static";  base = SS_CENTER | WS_GROUP_v; }
    else if (stmt == "RTEXT")         { form = TEXT;   c.cls = "Static";  base = SS_RIGHT  | WS_GROUP_v; }
    else if (stmt == "ICON")          { form = TEXT;   c.cls = "Static";  base = SS_ICON; }
    else if (stmt == "PUSHBUTTON")    { form = TEXT;   c.cls = "Button";  base = BS_PUSHBUTTON    | WS_TABSTOP_v; }
    else if (stmt == "DEFPUSHBUTTON") { form = TEXT;   c.cls = "Button";  base = BS_DEFPUSHBUTTON | WS_TABSTOP_v; }
    else if (stmt == "CHECKBOX")      { form = TEXT;   c.cls = "Button";  base = BS_CHECKBOX      | WS_TABSTOP_v; }
    else if (stmt == "AUTOCHECKBOX")  { form = TEXT;   c.cls = "Button";  base = BS_AUTOCHECKBOX  | WS_TABSTOP_v; }
    else if (stmt == "STATE3")        { form = TEXT;   c.cls = "Button";  base = BS_3STATE        | WS_TABSTOP_v; }
    else if (stmt == "AUTO3STATE")    { form = TEXT;   c.cls = "Button";  base = BS_AUTO3STATE    | WS_TABSTOP_v; }
    else if (stmt == "RADIOBUTTON")   { form = TEXT;   c.cls = "Button";  base = BS_RADIOBUTTON   | WS_TABSTOP_v; }
    else if (stmt == "AUTORADIOBUTTON"){ form = TEXT;  c.cls = "Button";  base = BS_AUTORADIOBUTTON | WS_TABSTOP_v; }
    else if (stmt == "GROUPBOX")      { form = TEXT;   c.cls = "Button";  base = BS_GROUPBOX; }
    else if (stmt == "PUSHBOX")       { form = TEXT;   c.cls = "Button";  base = BS_PUSHBOX; }
    else if (stmt == "EDITTEXT")      { form = NOTEXT; c.cls = "Edit";     base = ES_LEFT | WS_BORDER_v | WS_TABSTOP_v; }
    else if (stmt == "COMBOBOX")      { form = NOTEXT; c.cls = "ComboBox"; base = WS_TABSTOP_v; }
    else if (stmt == "LISTBOX")       { form = NOTEXT; c.cls = "ListBox";  base = LBS_NOTIFY | WS_BORDER_v | WS_VSCROLL_v; }
    else if (stmt == "SCROLLBAR")     { form = NOTEXT; c.cls = "ScrollBar";base = SBS_HORZ; }

    if (form == GEN) {
        c.text = str_at(0); c.id = id_at(1); style_idx = 3;
        c.sym = sym_at(1);
        c.x = coord(4); c.y = coord(5); c.cx = coord(6); c.cy = coord(7);
        c.ex_style = ex_at(8);
    } else if (form == TEXT) {
        c.text = str_at(0); c.id = id_at(1); style_idx = 6;
        c.sym = sym_at(1);
        c.x = coord(2); c.y = coord(3); c.cx = coord(4); c.cy = coord(5);
        c.ex_style = ex_at(7);
    } else {  // NOTEXT
        c.text = ""; c.id = id_at(0); style_idx = 5;
        c.sym = sym_at(0);
        c.x = coord(1); c.y = coord(2); c.cx = coord(3); c.cy = coord(4);
        c.ex_style = ex_at(6);
    }

    StyleParts sp{0, 0};
    if (style_idx >= 0 && fields.size() > (size_t)style_idx) sp = eval_parts(fields[style_idx], syms);
    c.style = (base | sp.pos | WS_CHILD | WS_VISIBLE) & ~sp.nots;
    return c;
}

} // namespace

std::vector<RcDialog> RcParser::parse(const std::string& rc_utf16_bytes,
                                      const std::string& resource_h_text) {
    std::string rc = decode_rc(rc_utf16_bytes);   // UTF-16LE (on-disk) or UTF-8 (GitHub API)
    auto syms = build_symbols(resource_h_text);
    auto toks = tokenize(rc);
    std::vector<RcDialog> dialogs;
    size_t i = 0, n = toks.size();

    auto skip_seps = [&]() { while (i < n && toks[i].type == TT::COMMA) i++; };
    auto read_rect_num = [&]() -> int {  // skip commas, read one numeric word
        skip_seps();
        if (i < n && toks[i].type == TT::WORD) {
            int v = is_number(toks[i].val) ? (int)parse_int(toks[i].val)
                                           : (int)field_coord({toks[i]}, syms);
            i++;
            return v;
        }
        return 0;
    };

    while (i < n) {
        // dialog start: WORD(name) WORD(DIALOGEX|DIALOG)
        if (i + 1 < n && toks[i].type == TT::WORD && toks[i+1].type == TT::WORD) {
            std::string kw = upper(toks[i + 1].val);
            if (kw == "DIALOGEX" || kw == "DIALOG") {
                RcDialog d;
                d.sym = toks[i].val;               // IDD_* name token as written
                d.id = field_id({toks[i]}, syms);
                i += 2;
                d.x = read_rect_num(); d.y = read_rect_num();
                d.cx = read_rect_num(); d.cy = read_rect_num();

                // header attributes until BEGIN/{
                while (i < n) {
                    if (toks[i].type == TT::WORD) {
                        std::string hk = upper(toks[i].val);
                        if (hk == "BEGIN" || hk == "{") { i++; break; }
                        if (hk == "STYLE")   { i++; d.style = read_header_style(toks, i, syms); continue; }
                        if (hk == "EXSTYLE") { i++; d.ex_style = read_header_style(toks, i, syms); continue; }
                        if (hk == "CAPTION") {
                            i++;
                            if (i < n && toks[i].type == TT::STR) { d.caption = toks[i].val; i++; }
                            continue;
                        }
                        if (hk == "FONT") {
                            i++;
                            d.font_pt = read_rect_num();
                            skip_seps();
                            if (i < n && toks[i].type == TT::STR) { d.font_face = toks[i].val; i++; }
                            // optional weight, italic, charset (always numeric after the face)
                            while (i < n && toks[i].type == TT::COMMA && i + 1 < n &&
                                   toks[i+1].type == TT::WORD && is_number(toks[i+1].val)) {
                                i += 2;
                            }
                            continue;
                        }
                        if (hk == "MENU" || hk == "CLASS") {  // skip the single following value
                            i++;
                            if (i < n && (toks[i].type == TT::WORD || toks[i].type == TT::STR)) i++;
                            continue;
                        }
                        // unknown header token (number, etc.): skip
                        i++; continue;
                    }
                    i++;  // skip stray comma
                }

                // body: control statements until END/}
                while (i < n) {
                    if (toks[i].type != TT::WORD) { i++; continue; }
                    std::string bk = upper(toks[i].val);
                    if (bk == "END" || bk == "}") { i++; break; }
                    if (bk == "BEGIN" || bk == "{") { i++; continue; }  // stray nested opener
                    if (!is_control_kw(bk)) { i++; continue; }

                    std::string stmt = bk;
                    i++;
                    std::vector<Tok> stmt_tokens;
                    while (i < n) {
                        const Tok& t = toks[i];
                        if (t.type == TT::WORD) {
                            std::string wk = upper(t.val);
                            if (is_control_kw(wk) || wk == "END" || wk == "}" ||
                                wk == "BEGIN" || wk == "{" || is_struct_kw(wk)) break;
                        }
                        stmt_tokens.push_back(t);
                        i++;
                    }
                    // split into fields by comma
                    std::vector<std::vector<Tok>> fields(1);
                    for (const auto& t : stmt_tokens) {
                        if (t.type == TT::COMMA) fields.emplace_back();
                        else fields.back().push_back(t);
                    }
                    d.controls.push_back(parse_control(stmt, fields, syms));
                }
                dialogs.push_back(std::move(d));
                continue;
            }
        }
        i++;
    }
    return dialogs;
}

// Parse a MENU/MENUEX body (just past its BEGIN) into items, recursing into POPUPs. `i` stops after
// the matching END. Extra MENUEX type/state tokens after an id are skipped by the main loop below.
static void parse_menu_body(const std::vector<Tok>& toks, size_t& i, size_t n,
                            const std::unordered_map<std::string, long long>& syms,
                            std::vector<RcMenuItem>& out) {
    while (i < n) {
        if (toks[i].type != TT::WORD) { i++; continue; }
        std::string kw = upper(toks[i].val);
        if (kw == "END" || toks[i].val == "}") { i++; return; }
        if (kw == "POPUP") {
            i++;
            RcMenuItem item; item.sym = "POPUP";
            if (i < n && toks[i].type == TT::STR) { item.text = toks[i].val; i++; }
            while (i < n && !(toks[i].type == TT::WORD && (upper(toks[i].val) == "BEGIN" || toks[i].val == "{"))) i++;
            if (i < n) i++;                                   // consume BEGIN
            parse_menu_body(toks, i, n, syms, item.items);
            out.push_back(std::move(item));
        } else if (kw == "MENUITEM") {
            i++;
            if (i < n && toks[i].type == TT::WORD && upper(toks[i].val) == "SEPARATOR") {
                RcMenuItem item; item.separator = true; out.push_back(std::move(item)); i++;
            } else if (i < n && toks[i].type == TT::STR) {
                RcMenuItem item; item.text = toks[i].val; i++;
                if (i < n && toks[i].type == TT::COMMA) i++;
                if (i < n && toks[i].type == TT::WORD) {      // command id
                    item.sym = toks[i].val; item.command = field_id({ toks[i] }, syms); i++;
                }
                out.push_back(std::move(item));
            }
        } else {
            i++;
        }
    }
}

std::vector<RcMenu> rc_parse_menus(const std::string& rc_utf16_bytes, const std::string& resource_h_text) {
    std::string rc = decode_rc(rc_utf16_bytes);
    auto syms = build_symbols(resource_h_text);
    auto toks = tokenize(rc);
    std::vector<RcMenu> menus;
    size_t i = 0, n = toks.size();
    while (i < n) {
        if (i + 1 < n && toks[i].type == TT::WORD && toks[i + 1].type == TT::WORD) {
            std::string kw = upper(toks[i + 1].val);          // "<name> MENU|MENUEX"
            if (kw == "MENU" || kw == "MENUEX") {
                RcMenu m; m.sym = toks[i].val; m.id = field_id({ toks[i] }, syms);
                i += 2;
                while (i < n && !(toks[i].type == TT::WORD && (upper(toks[i].val) == "BEGIN" || toks[i].val == "{"))) i++;
                if (i < n) i++;                               // consume BEGIN
                parse_menu_body(toks, i, n, syms, m.items);
                menus.push_back(std::move(m));
                continue;
            }
        }
        i++;
    }
    return menus;
}

// ---------------------------------------------------------------------------
// DLGTEMPLATEEX byte emitter — the inverse of the test-side walker (parse_dlgex in
// tests/rc_conformance_test.cpp / tests/rc_emit_test.cpp). Produces a little-endian byte
// stream structurally identical to what rc.exe writes into RT_DIALOG: header, an optional
// DS_SETFONT block, then DWORD-aligned per-control records. Standard control classes are
// encoded as 0xFFFF + ordinal atom; everything else as a UTF-16Z class string. The emitted
// buffer round-trips through parse_dlgex (the rc_emit gate).
// ---------------------------------------------------------------------------
namespace {

void push_u16(std::vector<unsigned char>& v, unsigned short w) {
    v.push_back((unsigned char)(w & 0xFF));
    v.push_back((unsigned char)((w >> 8) & 0xFF));
}
void push_u32(std::vector<unsigned char>& v, unsigned u) {
    v.push_back((unsigned char)(u & 0xFF));
    v.push_back((unsigned char)((u >> 8) & 0xFF));
    v.push_back((unsigned char)((u >> 16) & 0xFF));
    v.push_back((unsigned char)((u >> 24) & 0xFF));
}
void push_coord16(std::vector<unsigned char>& v, int c) {
    push_u16(v, (unsigned short)(std::int16_t)c);  // signed 16-bit coordinate as a WORD
}

// Append a UTF-8 string as UTF-16LE code units + a terminating NUL — the reverse of the
// test-side narrow(). Minimal decoder: 1/2/3-byte BMP sequences + 4-byte -> surrogate pair.
void push_utf16z(std::vector<unsigned char>& v, const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char b0 = (unsigned char)s[i];
        if (b0 < 0x80) {                                       // 1 byte
            push_u16(v, b0);
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < n) {        // 2 bytes
            unsigned cp = ((unsigned)(b0 & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
            push_u16(v, (unsigned short)cp);
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < n) {        // 3 bytes
            unsigned cp = ((unsigned)(b0 & 0x0F) << 12) |
                          (((unsigned char)s[i + 1] & 0x3F) << 6) |
                          ((unsigned char)s[i + 2] & 0x3F);
            push_u16(v, (unsigned short)cp);
            i += 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < n) {        // 4 bytes -> surrogate pair
            unsigned cp = ((unsigned)(b0 & 0x07) << 18) |
                          (((unsigned char)s[i + 1] & 0x3F) << 12) |
                          (((unsigned char)s[i + 2] & 0x3F) << 6) |
                          ((unsigned char)s[i + 3] & 0x3F);
            cp -= 0x10000;
            push_u16(v, (unsigned short)(0xD800 + (cp >> 10)));
            push_u16(v, (unsigned short)(0xDC00 + (cp & 0x3FF)));
            i += 4;
        } else {                                               // stray byte: pass through
            push_u16(v, b0);
            i += 1;
        }
    }
    push_u16(v, 0x0000);
}

// Map a standard Win32 control class name (case-insensitive) to its ordinal atom, or 0
// when it is not a standard class (then the real name is written as a UTF-16Z string).
unsigned short class_atom(const std::string& cls) {
    auto ieq = [](const std::string& a, const char* b) {
        size_t n = a.size();
        for (size_t i = 0; i < n; ++i) {
            if (b[i] == 0) return false;
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        }
        return b[n] == 0;
    };
    if (ieq(cls, "Button"))    return 0x0080;
    if (ieq(cls, "Edit"))      return 0x0081;
    if (ieq(cls, "Static"))    return 0x0082;
    if (ieq(cls, "ListBox"))   return 0x0083;
    if (ieq(cls, "ScrollBar")) return 0x0084;
    if (ieq(cls, "ComboBox"))  return 0x0085;
    return 0;
}

} // namespace

std::vector<unsigned char> emit_dlgtemplate(const RcDialog& d) {
    std::vector<unsigned char> v;
    v.reserve(256);

    // ---- header -------------------------------------------------------------
    push_u16(v, 1);                       // dlgVer
    push_u16(v, 0xFFFF);                  // signature (marks a DLGTEMPLATEEX)
    push_u32(v, 0);                       // helpID
    push_u32(v, d.ex_style);              // exStyle
    push_u32(v, d.style);                 // style
    push_u16(v, (unsigned short)d.controls.size());  // cDlgItems
    push_coord16(v, d.x); push_coord16(v, d.y); push_coord16(v, d.cx); push_coord16(v, d.cy);
    push_u16(v, 0x0000);                  // menu: sz_or_ord none
    push_u16(v, 0x0000);                  // windowClass: sz_or_ord none
    if (d.caption.empty()) push_u16(v, 0x0000); else push_utf16z(v, d.caption);

    if (d.style & 0x40 /*DS_SETFONT*/) {
        push_u16(v, (unsigned short)d.font_pt);  // pointsize
        push_u16(v, 0);                          // weight
        v.push_back(0);                          // italic
        v.push_back(1);                          // charset = DEFAULT_CHARSET
        if (d.font_face.empty()) push_u16(v, 0x0000); else push_utf16z(v, d.font_face);
    }

    // ---- controls (each begins on a DWORD boundary) -------------------------
    for (const auto& c : d.controls) {
        while (v.size() % 4 != 0) v.push_back(0x00);   // pad to DWORD boundary FIRST
        push_u32(v, 0);                          // helpID
        push_u32(v, c.ex_style);                 // exStyle
        push_u32(v, c.style);                    // style
        push_coord16(v, c.x); push_coord16(v, c.y); push_coord16(v, c.cx); push_coord16(v, c.cy);
        push_u32(v, (unsigned)(int)c.id);        // id (IDC_STATIC -1 -> 0xFFFFFFFF)
        unsigned short atom = class_atom(c.cls);
        if (atom) { push_u16(v, 0xFFFF); push_u16(v, atom); }  // standard class atom
        else      { push_utf16z(v, c.cls); }                  // real class string
        // title verbatim — the parser keeps "" escapes; the gate normalizes both sides.
        if (c.text.empty()) push_u16(v, 0x0000); else push_utf16z(v, c.text);
        push_u16(v, 0x0000);                      // extraCount
    }
    return v;
}

// ---------------------------------------------------------------------------
// rc_dialog_records — reproduce bundle-build/build_index.py's dialog records from the
// parsed dialogs. build_index delegates to upstream's TranslationDataRC, which (a) emits
// no record for empty-text controls (its dialogEntry regex needs >=1 body char), (b) drops
// control texts its `excludedStrings` filter rejects, and (c) stores dialogs in an
// OrderedDict keyed by (msgctxt, msgid) — so multiple controls in one dialog that share the
// same (control symbol, text) collapse to a SINGLE record. We mirror all three so the
// rc_index gate's RC-derived multiset matches control-index.json exactly.
//   excludedStrings = re.compile(r'(?:http://|\.\.\.|\d+)$', UNICODE|IGNORECASE), used as
//   .match() (anchored at start) -> a text is dropped iff it is exactly "http://" (case-
//   insensitive), exactly "...", or entirely ASCII digits. Captions are NOT filtered
//   (TranslationDataRC applies the filter only to control/menu/stringtable entries).
// ---------------------------------------------------------------------------
static bool excluded_text(const std::string& s) {
    if (s == "...") return true;
    bool all_digits = !s.empty();
    for (char c : s) if (!std::isdigit((unsigned char)c)) { all_digits = false; break; }
    if (all_digits) return true;
    static const char url[] = "http://";           // size 7
    if (s.size() == sizeof(url) - 1) {
        bool eq = true;
        for (size_t k = 0; k < sizeof(url) - 1; ++k)
            if (std::tolower((unsigned char)s[k]) != (unsigned char)url[k]) { eq = false; break; }
        if (eq) return true;
    }
    return false;
}

std::vector<DialogRecord> rc_dialog_records(const std::vector<RcDialog>& dialogs) {
    std::vector<DialogRecord> out;
    // build_index iterates TranslationDataRC's OrderedDict -> one record per unique
    // (msgctxt, msgid). Dedup the same way (first occurrence wins, in RC order).
    std::set<std::pair<std::string, std::string>> seen;
    auto emit = [&](long long dialog, std::optional<long long> control,
                    const std::string& control_sym,
                    const std::string& msgctxt, const std::string& msgid) {
        if (!seen.emplace(msgctxt, msgid).second) return;   // OrderedDict collapses dupes
        DialogRecord r;
        r.dialog = dialog;
        r.control = control;
        r.control_sym = control_sym;
        r.msgctxt = msgctxt;
        // Collapse RC-doubled "" -> " so the record's msgid matches the gettext-unescaped .po msgid
        // (po->find is EXACT and does NOT normalize) and build_index.py's control-index.json. Without
        // this, live-RC dialog rows for strings with embedded quotes (e.g. ""Open DVD/BD"" behavior)
        // never match the .po and render blank. (bug #1; keeps the RC-INDEX GATE test in parity.)
        r.msgid = rc_text_normalize(msgid);
        out.push_back(std::move(r));
    };

    for (const auto& d : dialogs) {
        if (!d.caption.empty())
            emit(d.id, std::nullopt, "CAPTION", d.sym + "_CAPTION", d.caption);
        for (const auto& c : d.controls) {
            if (c.text.empty()) continue;          // no-text control: TranslationDataRC emits none
            if (excluded_text(c.text)) continue;   // excludedStrings: "..." / digits / "http://"
            emit(d.id, c.id, c.sym, d.sym + "_" + c.sym, c.text);   // c.id already resolved
        }                                                          // (IDC_STATIC -> -1, unknown -> -1)
    }
    return out;
}

} // namespace mpctrans
