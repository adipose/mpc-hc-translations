// SPDX-License-Identifier: GPL-3.0-or-later
// THE RC-RENDER GATE (Approach C end-to-end): prove the emitted templates are not just
// well-formed on paper but actually LOAD in Windows. For every IDD_* dialog parsed from
// mpc-hc.rc we emit a DLGTEMPLATEEX and hand it to CreateDialogIndirect — the exact call
// LivePreview uses — with NO resource DLL involved. Success (a non-null HWND) means every
// control class was created; we then assert the immediate-child count equals the parsed
// control count. This is the runtime proof the data-only rc_emit gate can't give.
//   Usage: rc_render_test [RC] [RESOURCE_H]   (SKIPs if the RC is missing)
#include "mpctrans/rc_dialogs.h"

#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Themed common controls (SysLink etc. need v6) so every real MPC-HC dialog can create.
#pragma comment(linker, "/manifestdependency:\"type='win32' "                       \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "                    \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using mpctrans::RcDialog;
using mpctrans::RcParser;
using mpctrans::emit_dlgtemplate;

static std::string read_file(const std::string& p, bool bin) {
    std::ifstream f(p, bin ? std::ios::binary : std::ios::in);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static INT_PTR CALLBACK NullDlgProc(HWND, UINT, WPARAM, LPARAM) { return FALSE; }
static int child_count(HWND dlg) {
    int n = 0;
    for (HWND c = ::GetWindow(dlg, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT)) ++n;
    return n;
}

int main(int argc, char** argv) {
    std::string rc = argc > 1 ? argv[1] : "upstream/src/mpc-hc/mpc-hc.rc";
    std::string rh = argc > 2 ? argv[2] : "upstream/src/mpc-hc/resource.h";
    if (!std::filesystem::exists(rc)) { std::printf("SKIP: %s not found\n", rc.c_str()); return 0; }

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_LINK_CLASS |
        ICC_STANDARD_CLASSES | ICC_DATE_CLASSES | ICC_USEREX_CLASSES |
        ICC_COOL_CLASSES | ICC_INTERNET_CLASSES };
    ::InitCommonControlsEx(&icc);
    // The one custom class in the templates (a plain box is enough to let creation succeed).
    WNDCLASSW mk{}; mk.lpfnWndProc = ::DefWindowProcW; mk.hInstance = hInst;
    mk.lpszClassName = L"MfcMaskedEdit"; mk.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    mk.hCursor = ::LoadCursor(nullptr, IDC_IBEAM); ::RegisterClassW(&mk);
    // Hidden host to parent the preview dialogs.
    WNDCLASSW hc{}; hc.lpfnWndProc = ::DefWindowProcW; hc.hInstance = hInst;
    hc.lpszClassName = L"RcRenderHost"; ::RegisterClassW(&hc);
    HWND host = ::CreateWindowExW(0, L"RcRenderHost", L"", WS_OVERLAPPED, 0, 0, 100, 100,
                                  nullptr, nullptr, hInst, nullptr);

    std::vector<RcDialog> dialogs = RcParser::parse(read_file(rc, true), read_file(rh, false));
    int total = 0, ok = 0, fail = 0;
    for (const auto& d : dialogs) {
        ++total;
        std::vector<unsigned char> t = emit_dlgtemplate(d);
        // Embed as a child (menu bars/captions only draw on top-level windows).
        DWORD style; memcpy(&style, t.data() + 12, 4);   // DLGTEMPLATEEX style @ offset 12
        style = (style & ~(DWORD)(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | 0x0800 /*DS_CENTER*/))
                | WS_CHILD;
        memcpy(t.data() + 12, &style, 4);

        HWND dlg = ::CreateDialogIndirectParamW(hInst, (LPCDLGTEMPLATE)t.data(), host, NullDlgProc, 0);
        if (!dlg) {
            ++fail;
            if (fail <= 8) std::printf("FAIL dialog %lld: CreateDialogIndirect -> null (err %lu)\n",
                                       d.id, ::GetLastError());
            continue;
        }
        int kids = child_count(dlg);
        if (kids != (int)d.controls.size()) {
            ++fail;
            if (fail <= 8) std::printf("FAIL dialog %lld: created %d children, expected %zu\n",
                                       d.id, kids, d.controls.size());
        } else {
            ++ok;
        }
        ::DestroyWindow(dlg);
    }
    if (host) ::DestroyWindow(host);

    std::printf("\n%d/%d dialogs rendered from RC via CreateDialogIndirect (no DLL)\n", ok, total);
    std::printf("%s\n", fail ? "FAIL" : "RC-RENDER GATE GREEN");
    return fail ? 1 : 0;
}
