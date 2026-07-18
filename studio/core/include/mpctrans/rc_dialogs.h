// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

#include "mpctrans/control_index.h"   // DialogRecord (for rc_dialog_records)

// Runtime RC dialog reconstruction (portable, no Windows/MFC types).
//
// Reconstructs each IDD_* DIALOGEX dialog from the raw .rc text + resource.h,
// reproducing the per-control structure the resource compiler emits. This is the
// parse side of rc_conformance_test, which proves the reconstruction matches the
// compiled RT_DIALOG templates in dist/mpcresources.neutral.dll field-for-field.
//
// Only dialogs (RT_DIALOG) are parsed — menus/stringtables/bitmaps are ignored.
namespace mpctrans {

// One control inside a dialog. `text` is the raw RC string-literal body (with
// `""` escapes intact, "" when none); callers normalize via rc_text_normalize.
// `cls` is the window-class name as it appears to Win32: "Button", "Edit",
// "Static", "ListBox", "ScrollBar", "ComboBox", or a real class such as
// "msctls_updown32". `id` is the resolved numeric id (IDC_STATIC == -1).
struct RcControl {
    long long id = -1;
    std::string sym;        // control id token as written ("IDC_TIMEONSEEKBAR", "IDOK",
                            //  "IDC_STATIC", "ID_APPLY_NOW"); a raw-number id yields its text
    std::string cls;        // verbatim class name (shorthand synthesizes "Button"/"Edit"/...)
    std::string text;       // raw RC literal body, "" if none
    int x = 0, y = 0, cx = 0, cy = 0;
    unsigned long style = 0, ex_style = 0;
};

// One reconstructed dialog. Controls are in RC source order (== compiled template
// order). `style`/`ex_style` are best-effort Win32 masks; `font_face`/`font_pt`
// are "" / 0 when no FONT record is present.
struct RcDialog {
    long long id = -1;
    std::string sym;        // IDD_* symbol token as written (e.g. "IDD_PPAGETHEME")
    std::string caption;
    std::string font_face;
    int font_pt = 0;
    int x = 0, y = 0, cx = 0, cy = 0;
    unsigned long style = 0, ex_style = 0;
    std::vector<RcControl> controls;
};

class RcParser {
public:
    // `rc_utf16_bytes` is the raw file content of the .rc (UTF-16LE, optional
    // 0xFF 0xFE BOM, CRLF). `resource_h_text` is resource.h as ASCII text.
    // Returns one RcDialog per IDD_* DIALOG(EX) block, in source order.
    static std::vector<RcDialog> parse(const std::string& rc_utf16_bytes,
                                       const std::string& resource_h_text);
};

// One MENU/MENUEX item: a command, a separator, or a POPUP with children.
struct RcMenuItem {
    std::string text;              // raw RC literal body (with '&' + optional "\t<accel>"); "" separator
    std::string sym;               // command symbol as written ("ID_..."); "POPUP" for a popup header
    long long   command = 0;       // resolved numeric command id (0 for popup/separator)
    bool        separator = false;
    std::vector<RcMenuItem> items; // children (POPUP); empty for a leaf
};
struct RcMenu { long long id = 0; std::string sym; std::vector<RcMenuItem> items; };

// Parse the MENU/MENUEX resources from the .rc into their real hierarchy (the RC parser above
// intentionally skips them). Same inputs as RcParser::parse.
std::vector<RcMenu> rc_parse_menus(const std::string& rc_utf16_bytes, const std::string& resource_h_text);

// Serialize an RcDialog into a DLGTEMPLATEEX byte buffer (little-endian), byte-layout
// identical to what rc.exe emits into RT_DIALOG. Round-trips through the same walker the
// conformance/emit gates use. Standard control classes are written as ordinal atoms.
std::vector<unsigned char> emit_dlgtemplate(const RcDialog& d);

// Reproduce bundle-build/build_index.py's dialog records (the translatable-control index)
// from the SAME parsed dialogs — one record per dialog CAPTION and per translatable control.
// Order: caption record first (when the dialog has a caption), then one record per control
// in source order. `msgid` is the raw RC literal body ("" escapes intact, NOT normalized).
// This is the rc_index gate's source of truth; it must stay byte-for-byte faithful to
// build_index (which delegates to upstream's TranslationDataRC extractor).
std::vector<DialogRecord> rc_dialog_records(const std::vector<RcDialog>& dialogs);

} // namespace mpctrans
