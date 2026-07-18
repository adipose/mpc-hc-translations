// SPDX-License-Identifier: GPL-3.0-or-later
// THE RC-RECONSTRUCTION GATE: prove RcParser rebuilds each IDD_* DIALOG(EX) from
// mpc-hc.rc + resource.h field-for-field identical to what rc.exe compiled into
// dist/mpcresources.neutral.dll (the oracle). Only RT_DIALOG templates.
//
// Pass/fail (per dialog, in template order): control count; per control id, class
// (case-insensitive), text (normalized), rect x/y/cx/cy; plus dialog caption
// (normalized), font face (case-insensitive) + point size, dialog rect. Style is
// REPORT-ONLY. SKIPs cleanly (exit 0) when the DLL or RC is missing.
//   Usage: rc_conformance_test [DLL] [RC] [RESOURCE_H]
#include "mpctrans/rc_dialogs.h"
#include "mpctrans/control_index.h"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using mpctrans::RcControl;
using mpctrans::RcDialog;
using mpctrans::RcParser;
using mpctrans::rc_text_normalize;

// ---- oracle (compiled DLGTEMPLATEEX) model ----------------------------------
struct OCtl { long long id; std::string cls; std::string text; int x, y, cx, cy; unsigned long style; };
struct ODlg { std::string caption; std::string font_face; int font_pt = 0;
              int x = 0, y = 0, cx = 0, cy = 0; unsigned long style = 0; std::vector<OCtl> controls; };

static std::string narrow(const wchar_t* w, size_t n) {
    if (!n) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, s.data(), len, nullptr, nullptr);
    return s;
}

static std::string atom_to_class(unsigned short atom) {
    switch (atom) {
        case 0x0080: return "Button";
        case 0x0081: return "Edit";
        case 0x0082: return "Static";
        case 0x0083: return "ListBox";
        case 0x0084: return "ScrollBar";
        case 0x0085: return "ComboBox";
        default: return "#atom" + std::to_string(atom);
    }
}

// Walk one DLGTEMPLATEEX: dialog caption/font/rect/style + per-control id, class,
// text, signed rect, style. Returns false on malformed data.
static bool parse_dlgex(const unsigned char* p, size_t size, ODlg& out) {
    size_t off = 0;
    auto need = [&](size_t n) { return off + n <= size; };
    auto rd_u16 = [&] { unsigned short v; memcpy(&v, p + off, 2); off += 2; return v; };
    auto rd_u32 = [&] { unsigned v; memcpy(&v, p + off, 4); off += 4; return v; };
    auto rd_i16 = [&] { return (int)(int16_t)rd_u16(); };
    enum Kind { None, Ord, Str };
    auto rd_sz_or_ord = [&](std::string* s, unsigned short* ord) -> Kind {
        if (!need(2)) return None;
        unsigned short first = rd_u16();
        if (first == 0x0000) return None;
        if (first == 0xFFFF) {
            if (!need(2)) return None;
            if (ord) *ord = rd_u16(); else off += 2;
            return Ord;
        }
        std::wstring w; w.push_back((wchar_t)first);
        for (;;) {
            if (!need(2)) return Str;
            unsigned short c = rd_u16();
            if (!c) break;
            w.push_back((wchar_t)c);
        }
        if (s) *s = narrow(w.data(), w.size());
        return Str;
    };

    if (!need(2 * 2 + 4 * 3 + 2 + 2 * 4)) return false;
    unsigned short dlgVer = rd_u16(), signature = rd_u16();
    if (dlgVer != 1 || signature != 0xFFFF) return false;   // not a DLGTEMPLATEEX
    rd_u32();                       // helpID
    rd_u32();                       // exStyle
    out.style = rd_u32();
    unsigned short cItems = rd_u16();
    out.x = rd_i16(); out.y = rd_i16(); out.cx = rd_i16(); out.cy = rd_i16();
    rd_sz_or_ord(nullptr, nullptr); // menu
    rd_sz_or_ord(nullptr, nullptr); // windowClass (dialog class)
    rd_sz_or_ord(&out.caption, nullptr);
    if (out.style & 0x40 /*DS_SETFONT*/) {
        if (!need(2 + 2 + 1 + 1)) return false;
        out.font_pt = rd_u16();    // pointsize
        rd_u16();                  // weight
        off += 1 + 1;              // italic, charset
        rd_sz_or_ord(&out.font_face, nullptr);
    }
    for (unsigned i = 0; i < cItems; ++i) {
        off = (off + 3) & ~size_t(3);   // DWORD-aligned
        if (!need(4 * 3 + 2 * 4 + 4)) return false;
        rd_u32();                     // helpID
        rd_u32();                     // exStyle
        unsigned cstyle = rd_u32();
        int cx = rd_i16(), cy = rd_i16(), ccx = rd_i16(), ccy = rd_i16();
        long long id = (int)rd_u32();  // signed: IDC_STATIC == -1
        OCtl c; c.id = id; c.style = cstyle; c.x = cx; c.y = cy; c.cx = ccx; c.cy = ccy;
        std::string classbuf; unsigned short atom = 0;
        Kind wck = rd_sz_or_ord(&classbuf, &atom);  // windowClass (atom or string)
        c.cls = (wck == Ord) ? atom_to_class(atom) : (wck == Str ? classbuf : "");
        std::string title;
        rd_sz_or_ord(&title, nullptr);              // title (ordinal -> "")
        c.text = title;
        if (!need(2)) return false;
        unsigned short extra = rd_u16();
        if (!need(extra)) return false;
        off += extra;
        out.controls.push_back(std::move(c));
    }
    return true;
}

static BOOL CALLBACK enum_cb(HMODULE mod, LPCWSTR, LPWSTR name, LONG_PTR param) {
    auto* dlgs = reinterpret_cast<std::map<long long, ODlg>*>(param);
    if (!IS_INTRESOURCE(name)) return TRUE;   // mpc-hc dialog ids are all numeric
    HRSRC hr = FindResourceW(mod, name, MAKEINTRESOURCEW(5) /*RT_DIALOG*/);
    HGLOBAL hg = hr ? LoadResource(mod, hr) : nullptr;
    const unsigned char* p = hg ? (const unsigned char*)LockResource(hg) : nullptr;
    if (!p) return TRUE;
    ODlg d;
    if (parse_dlgex(p, SizeofResource(mod, hr), d))
        (*dlgs)[(long long)(USHORT)(ULONG_PTR)name] = std::move(d);
    return TRUE;
}

// ---- helpers ----------------------------------------------------------------
static std::string read_file(const std::string& path, bool binary) {
    std::ifstream f(path, binary ? std::ios::binary : std::ios::in);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}
static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

// First failing field (control index, or -1 for dialog-level), or "" on full match.
static std::string first_diff(const ODlg& o, const RcDialog& r, int style_diffs[]) {
    auto norm = [](const std::string& s) { return rc_text_normalize(s); };
    if (o.controls.size() != r.controls.size())
        return "control_count oracle=" + std::to_string(o.controls.size()) +
               " rc=" + std::to_string(r.controls.size());
    for (size_t i = 0; i < o.controls.size(); ++i) {
        const OCtl& a = o.controls[i];
        const RcControl& b = r.controls[i];
        if (a.id != b.id)
            return "ctl#" + std::to_string(i) + " id oracle=" + std::to_string(a.id) +
                   " rc=" + std::to_string(b.id);
        if (!iequals(a.cls, b.cls))
            return "ctl#" + std::to_string(i) + " class oracle='" + a.cls + "' rc='" + b.cls + "'";
        if (norm(a.text) != norm(b.text))
            return "ctl#" + std::to_string(i) + " text oracle='" + norm(a.text) +
                   "' rc='" + norm(b.text) + "'";
        if (a.x != b.x || a.y != b.y || a.cx != b.cx || a.cy != b.cy)
            return "ctl#" + std::to_string(i) + " rect oracle=(" +
                   std::to_string(a.x) + "," + std::to_string(a.y) + "," +
                   std::to_string(a.cx) + "," + std::to_string(a.cy) + ") rc=(" +
                   std::to_string(b.x) + "," + std::to_string(b.y) + "," +
                   std::to_string(b.cx) + "," + std::to_string(b.cy) + ")";
        if (a.style != b.style) {
            ++style_diffs[0];
            char buf[128];
            std::snprintf(buf, sizeof(buf), "ctl#%zu style oracle=0x%08lX rc=0x%08lX",
                          i, (unsigned long)a.style, (unsigned long)b.style);
            return buf;
        }
    }
    if (norm(o.caption) != norm(r.caption))
        return "caption oracle='" + norm(o.caption) + "' rc='" + norm(r.caption) + "'";
    if (!iequals(o.font_face, r.font_face))
        return "font_face oracle='" + o.font_face + "' rc='" + r.font_face + "'";
    if (o.font_pt != r.font_pt)
        return "font_pt oracle=" + std::to_string(o.font_pt) + " rc=" + std::to_string(r.font_pt);
    if (o.x != r.x || o.y != r.y || o.cx != r.cx || o.cy != r.cy)
        return "dialog_rect oracle=(" + std::to_string(o.x) + "," + std::to_string(o.y) + "," +
               std::to_string(o.cx) + "," + std::to_string(o.cy) + ") rc=(" +
               std::to_string(r.x) + "," + std::to_string(r.y) + "," +
               std::to_string(r.cx) + "," + std::to_string(r.cy) + ")";
    return "";
}

int main(int argc, char** argv) {
    std::string dll = argc > 1 ? argv[1] : "dist/mpcresources.neutral.dll";
    std::string rc  = argc > 2 ? argv[2] : "upstream/src/mpc-hc/mpc-hc.rc";
    std::string rh  = argc > 3 ? argv[3] : "upstream/src/mpc-hc/resource.h";
    for (const auto& p : {dll, rc})
        if (!std::filesystem::exists(p)) { std::printf("SKIP: %s not found\n", p.c_str()); return 0; }

    std::vector<RcDialog> parsed = RcParser::parse(read_file(rc, true), read_file(rh, false));

    HMODULE mod = LoadLibraryExA(dll.c_str(), nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) { std::printf("FAIL: cannot load %s\n", dll.c_str()); return 1; }
    std::map<long long, ODlg> oracle;
    EnumResourceNamesW(mod, MAKEINTRESOURCEW(5) /*RT_DIALOG*/, enum_cb, (LONG_PTR)&oracle);
    FreeLibrary(mod);

    std::map<long long, const RcDialog*> by_id;
    for (const auto& d : parsed) by_id[d.id] = &d;

    int total = 0, passed = 0, failed = 0, detailed = 0, style_diffs[1] = {0};
    std::vector<long long> extra_rc;
    for (const auto& d : parsed)
        if (!oracle.count(d.id)) extra_rc.push_back(d.id);

    for (const auto& [id, od] : oracle) {
        ++total;
        auto it = by_id.find(id);
        if (it == by_id.end()) {
            ++failed;
            std::printf("FAIL dialog %lld: no RC match\n", id);
            continue;
        }
        std::string diff = first_diff(od, *it->second, style_diffs);
        if (diff.empty()) {
            ++passed;
            std::printf("PASS dialog %lld (%zu controls)\n", id, od.controls.size());
        } else {
            ++failed;
            // Name the first differing control + field, but only for the first few.
            if (detailed < 8) { ++detailed; std::printf("FAIL dialog %lld: %s\n", id, diff.c_str()); }
            else              { std::printf("FAIL dialog %lld\n", id); }
        }
    }
    for (long long id : extra_rc) {
        ++failed;
        std::printf("FAIL dialog %lld: in RC but not in DLL\n", id);
    }

    std::printf("\n%d/%d dialogs match (id/class/text/rect) | style-diff controls: %d"
                " | rc-only: %zu\n",
                passed, total, style_diffs[0], extra_rc.size());
    std::printf("%s\n", failed ? "FAIL" : "RC-RECONSTRUCTION GATE GREEN");
    return failed ? 1 : 0;
}
