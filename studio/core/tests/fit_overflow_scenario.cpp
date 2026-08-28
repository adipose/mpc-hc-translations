// SPDX-License-Identifier: GPL-3.0-or-later
// FIT-OVERFLOW SCENARIO -- demonstrates the "translation too long for the control" defect class
// using the EXACT rule LivePreview::DetectOverflow applies (studio/ui/LivePreview.cpp:903-928):
// render the control's displayed text (mnemonic '&' dropped) in the control's own font via
// GetTextExtentPoint32W, compare against the client width, and for a Button subtract 8px for
// borders/margins. No RC file or resource DLL involved -- we create two real Win32 child controls
// on a hidden host window and measure them directly.
//   Usage: fit_overflow_scenario   (SKIPs cleanly if window/control creation fails, e.g. session 0)
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <string>
#include <vector>

// Themed common controls manifest (same as rc_render_test.cpp) -- not strictly required for plain
// BUTTON/STATIC, but keeps this test consistent with the rest of the render-gate family.
#pragma comment(linker, "/manifestdependency:\"type='win32' "                       \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "                    \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static int fails = 0;

// ---- verbatim port of LivePreview.cpp's displayText()/prefixProcesses()/DetectOverflow() rule,
// specialized to a single already-known HWND instead of EnumChildWindows over a dialog. ----
static std::wstring displayText(const wchar_t* s, int n, bool prefixProcessed) {
    std::wstring o; o.reserve(n);
    for (int r = 0; r < n; ++r) {
        if (prefixProcessed && s[r] == L'&') {
            if (r + 1 < n && s[r + 1] == L'&') { o.push_back(L'&'); ++r; }   // '&&' -> literal '&'
            // else: a lone '&' is the mnemonic marker -- dropped
        } else o.push_back(s[r]);
    }
    return o;
}
static bool prefixProcesses(const wchar_t* cls, LONG style) {
    if (!_wcsicmp(cls, L"Button")) return true;
    if (!_wcsicmp(cls, L"Static")) return !(style & SS_NOPREFIX);
    return false;
}
// Returns whether `ctrl` overflows its client rect, and reports the measured/available pixels.
static bool overflows(HWND ctrl, int* outRendered = nullptr, int* outAvail = nullptr) {
    wchar_t cls[64]; ::GetClassNameW(ctrl, cls, 64);
    LONG st = (LONG)::GetWindowLongPtr(ctrl, GWL_STYLE);
    wchar_t txt[512]; int n = ::GetWindowTextW(ctrl, txt, 512);
    std::wstring dt = displayText(txt, n, prefixProcesses(cls, st));   // drop the mnemonic '&'
    HDC dc = ::GetDC(ctrl);
    HFONT f = (HFONT)::SendMessage(ctrl, WM_GETFONT, 0, 0);
    HGDIOBJ old = f ? ::SelectObject(dc, f) : nullptr;
    SIZE sz{}; ::GetTextExtentPoint32W(dc, dt.c_str(), (int)dt.size(), &sz);
    if (old) ::SelectObject(dc, old);
    ::ReleaseDC(ctrl, dc);
    RECT rc; ::GetClientRect(ctrl, &rc);
    int avail = rc.right - rc.left;
    if (!_wcsicmp(cls, L"Button")) avail -= 8;   // borders/margins
    if (outRendered) *outRendered = sz.cx;
    if (outAvail) *outAvail = avail;
    return sz.cx > avail && avail > 0;
}

static void banner(const char* title) { std::printf("\n== %s ==\n", title); }
static void check(const char* what, HWND ctrl, bool wantOverflow) {
    int rendered = 0, avail = 0;
    bool got = overflows(ctrl, &rendered, &avail);
    std::printf("  %-46s rendered %dpx vs avail %dpx -> %s\n", what, rendered, avail,
                got ? "OVERFLOW" : "FIT");
    if (got == wantOverflow) {
        std::printf("    PASS (expected %s)\n", wantOverflow ? "OVERFLOW" : "FIT");
    } else {
        std::printf("    FAIL (expected %s)\n", wantOverflow ? "OVERFLOW" : "FIT");
        ++fails;
    }
}

int main() {
    std::printf("FIT-OVERFLOW SCENARIO -- exercising LivePreview::DetectOverflow's fit rule\n");

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    if (!::InitCommonControlsEx(&icc)) {
        std::printf("SKIP: InitCommonControlsEx failed (err %lu)\n", ::GetLastError());
        return 0;
    }

    WNDCLASSW hc{}; hc.lpfnWndProc = ::DefWindowProcW; hc.hInstance = hInst;
    hc.lpszClassName = L"FitOverflowHost";
    if (!::RegisterClassW(&hc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::printf("SKIP: RegisterClassW failed (err %lu)\n", ::GetLastError());
        return 0;
    }
    // WS_POPUP, never shown -- a headless host purely to parent the child controls.
    HWND host = ::CreateWindowExW(0, L"FitOverflowHost", L"", WS_POPUP, 0, 0, 300, 200,
                                  nullptr, nullptr, hInst, nullptr);
    if (!host) {
        std::printf("SKIP: CreateWindowExW(host) failed (err %lu)\n", ::GetLastError());
        return 0;
    }

    // A realistic dialog font (8pt "MS Shell Dlg" -- falls back to DEFAULT_GUI_FONT if creation fails).
    HFONT font = ::CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Shell Dlg");
    if (!font) font = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);

    HWND btnShort = ::CreateWindowExW(0, L"BUTTON", L"&OK", WS_CHILD | BS_PUSHBUTTON,
                                       10, 10, 60, 14, host, nullptr, hInst, nullptr);
    HWND btnLong  = ::CreateWindowExW(0, L"BUTTON", L"Zur\xFC" L"cksetzen auf Standardeinstellungen",
                                       WS_CHILD | BS_PUSHBUTTON,
                                       10, 30, 60, 14, host, nullptr, hInst, nullptr);
    HWND stcShort = ::CreateWindowExW(0, L"STATIC", L"Common", WS_CHILD | SS_LEFT,
                                       10, 50, 90, 14, host, nullptr, hInst, nullptr);
    HWND stcLong  = ::CreateWindowExW(0, L"STATIC",
                                       L"Zus\xE4tzliche Renderer-Einstellungen sind im Wiedergabe-Men\xFC verf\xFCgbar",
                                       WS_CHILD | SS_LEFT,
                                       10, 70, 90, 14, host, nullptr, hInst, nullptr);
    if (!btnShort || !btnLong || !stcShort || !stcLong) {
        std::printf("SKIP: control creation failed (err %lu)\n", ::GetLastError());
        ::DestroyWindow(host);
        return 0;
    }
    for (HWND c : { btnShort, btnLong, stcShort, stcLong })
        ::SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);

    banner("Button (~60px wide, -8px border allowance)");
    check("short caption \"&OK\"", btnShort, /*wantOverflow=*/false);
    check("long caption \"Zur\xC3\xBC" "cksetzen auf Standardeinstellungen\"", btnLong, /*wantOverflow=*/true);

    banner("Static SS_LEFT (~90px wide, no border allowance)");
    check("short text \"Common\"", stcShort, /*wantOverflow=*/false);
    check("long text \"Zus\xC3\xA4tzliche Renderer-Einstellungen ...\"", stcLong, /*wantOverflow=*/true);

    std::printf("\n(In the app this is LivePreview::MeasureFit over every rendered dialog control;\n"
                " the Review tab's fit scan and the Sync-review 'doesn't fit' filter surface exactly these.)\n");

    ::DestroyWindow(host);

    std::printf("\n");
    if (fails) std::printf("%d scenario(s) FAILED\n", fails);
    else       std::printf("all fit-overflow scenarios matched DetectOverflow's verdict\n");
    return fails;
}
