// SPDX-License-Identifier: GPL-3.0-or-later
// THE DRY-RUN GATE (workstream H): the neutral resource DLL + the control-index must cohere.
// Loads dist/mpcresources.neutral.dll as a datafile, parses every RT_DIALOG DLGTEMPLATEEX,
// and checks each control-index DialogRecord against the real templates:
//   - the dialog id exists in the DLL
//   - CAPTION records match the template caption text
//   - control records match a template item with that control id + English text
// This is exactly the lookup LivePreview does at runtime (in reverse), so a pass proves
// substitution and click-to-edit can resolve real rendered controls.
// SKIPs (exit 0) when either gitignored artifact is missing.
//   Usage: neutral_dll_test [DLL] [CONTROL_INDEX_JSON]
#include "mpctrans/control_index.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace mpctrans;

struct DlgItem { long long id; std::string text; };
struct Dlg { std::string caption; std::vector<DlgItem> items; };

static std::string narrow(const wchar_t* w, size_t n) {
    if (!n) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, s.data(), len, nullptr, nullptr);
    return s;
}

// DLGTEMPLATEEX walker (mpc-hc dialogs are all DIALOGEX). Returns false on malformed data.
static bool parse_dlgex(const unsigned char* p, size_t size, Dlg& out) {
    size_t off = 0;
    auto need = [&](size_t n) { return off + n <= size; };
    auto rd_u16 = [&] { unsigned short v; memcpy(&v, p + off, 2); off += 2; return v; };
    auto rd_u32 = [&] { unsigned v; memcpy(&v, p + off, 4); off += 4; return v; };
    // sz_Or_Ord: 0x0000 none | 0xFFFF + ordinal WORD | null-terminated WCHARs
    auto rd_sz_or_ord = [&](std::string* text) -> bool {
        if (!need(2)) return false;
        unsigned short first = rd_u16();
        if (first == 0x0000) return true;
        if (first == 0xFFFF) { if (!need(2)) return false; rd_u16(); return true; }
        std::wstring w; w.push_back((wchar_t)first);
        for (;;) {
            if (!need(2)) return false;
            unsigned short c = rd_u16();
            if (!c) break;
            w.push_back((wchar_t)c);
        }
        if (text) *text = narrow(w.data(), w.size());
        return true;
    };

    if (!need(2 * 2 + 4 * 3 + 2 + 2 * 4)) return false;
    unsigned short dlgVer = rd_u16(), signature = rd_u16();
    if (dlgVer != 1 || signature != 0xFFFF) return false;   // not an EX template
    rd_u32();                       // helpID
    rd_u32();                       // exStyle
    unsigned style = rd_u32();
    unsigned short cItems = rd_u16();
    off += 2 * 4;                   // x y cx cy
    if (!rd_sz_or_ord(nullptr)) return false;   // menu
    if (!rd_sz_or_ord(nullptr)) return false;   // windowClass
    if (!rd_sz_or_ord(&out.caption)) return false;
    if (style & 0x40 /*DS_SETFONT*/) {
        if (!need(2 + 2 + 1 + 1)) return false;
        off += 2 + 2 + 1 + 1;       // pointsize weight italic charset
        if (!rd_sz_or_ord(nullptr)) return false;   // typeface
    }
    for (unsigned i = 0; i < cItems; ++i) {
        off = (off + 3) & ~size_t(3);   // items are DWORD-aligned
        if (!need(4 * 3 + 2 * 4 + 4)) return false;
        off += 4 * 3;                // helpID exStyle style
        off += 2 * 4;                // x y cx cy
        long long id = (int)rd_u32();   // signed: IDC_STATIC == -1
        DlgItem it; it.id = id;
        if (!rd_sz_or_ord(nullptr)) return false;   // windowClass
        if (!rd_sz_or_ord(&it.text)) return false;  // title (ordinal for icons -> empty)
        if (!need(2)) return false;
        unsigned short extra = rd_u16();
        if (!need(extra)) return false;
        off += extra;
        out.items.push_back(std::move(it));
    }
    return true;
}

static BOOL CALLBACK enum_cb(HMODULE mod, LPCWSTR, LPWSTR name, LONG_PTR param) {
    auto* dlgs = reinterpret_cast<std::map<long long, Dlg>*>(param);
    if (!IS_INTRESOURCE(name)) return TRUE;   // mpc-hc dialog ids are all numeric
    HRSRC hr = FindResourceW(mod, name, MAKEINTRESOURCEW(5) /*RT_DIALOG*/);
    HGLOBAL hg = hr ? LoadResource(mod, hr) : nullptr;
    const unsigned char* p = hg ? (const unsigned char*)LockResource(hg) : nullptr;
    if (!p) return TRUE;
    Dlg d;
    if (parse_dlgex(p, SizeofResource(mod, hr), d))
        (*dlgs)[(long long)(USHORT)(ULONG_PTR)name] = std::move(d);
    return TRUE;
}

int main(int argc, char** argv) {
    std::string dll  = argc > 1 ? argv[1] : "dist/mpcresources.neutral.dll";
    std::string idx  = argc > 2 ? argv[2] : "dist/control-index.json";
    for (const auto& p : {dll, idx})
        if (!std::filesystem::exists(p)) { std::printf("SKIP: %s not found\n", p.c_str()); return 0; }

    HMODULE mod = LoadLibraryExA(dll.c_str(), nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) { std::printf("FAIL: cannot load %s\n", dll.c_str()); return 1; }
    std::map<long long, Dlg> dlgs;
    EnumResourceNamesW(mod, MAKEINTRESOURCEW(5) /*RT_DIALOG*/, enum_cb, (LONG_PTR)&dlgs);

    ControlIndex ci = ControlIndex::load(idx);
    int fail = 0, caption_ok = 0, caption_bad = 0, ctrl_ok = 0, ctrl_bad = 0;
    std::set<long long> missing_dialogs;
    auto err = [&](const std::string& m) { if (++fail <= 10) std::printf("  FAIL %s\n", m.c_str()); };

    for (const auto& r : ci.dialogs()) {
        auto it = dlgs.find(r.dialog);
        if (it == dlgs.end()) { missing_dialogs.insert(r.dialog); continue; }
        const Dlg& d = it->second;
        // index msgid is the raw RC form ("" for a quote); template text is compiled
        std::string want = rc_text_normalize(r.msgid);
        if (!r.control) {
            if (d.caption == want) ++caption_ok;
            else { ++caption_bad; err("caption mismatch " + r.msgctxt +
                                       ": dll='" + d.caption + "' index='" + r.msgid + "'"); }
        } else {
            bool hit = false;
            for (const auto& item : d.items)
                if (item.id == *r.control && item.text == want) { hit = true; break; }
            if (hit) ++ctrl_ok;
            else { ++ctrl_bad; err("control not found " + r.msgctxt + " (id " +
                                   std::to_string(*r.control) + ", '" + r.msgid + "')"); }
        }
    }
    for (long long d : missing_dialogs) err("dialog id missing from DLL: " + std::to_string(d));

    std::printf("\ndll dialogs: %zu | captions %d ok / %d bad | controls %d ok / %d bad | "
                "missing dialog ids: %zu\n",
                dlgs.size(), caption_ok, caption_bad, ctrl_ok, ctrl_bad, missing_dialogs.size());
    if (caption_ok == 0 || ctrl_ok == 0) err("nothing resolved");
    FreeLibrary(mod);
    std::printf("%s\n", fail ? "FAIL" : "DRY-RUN GATE GREEN: neutral DLL and control-index cohere");
    return fail ? 1 : 0;
}
