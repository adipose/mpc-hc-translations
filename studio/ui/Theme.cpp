// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "Theme.h"
#include "resource.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace Theme {

// Opt the process into dark mode via the private uxtheme entry (ordinal 135, stable since Win10
// 1809) so DarkMode_Explorer takes on the scrolling common controls. Guarded — a no-op pre-1809.
enum PreferredAppMode { APPMODE_DEFAULT, APPMODE_ALLOWDARK, APPMODE_FORCEDARK, APPMODE_FORCELIGHT, APPMODE_MAX };
static void enableDarkMode() {
    static bool done = false; if (done) return; done = true;
    HMODULE ux = ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppMode_t = PreferredAppMode(WINAPI*)(PreferredAppMode);
    if (auto p = (SetPreferredAppMode_t)::GetProcAddress(ux, MAKEINTRESOURCEA(135))) p(APPMODE_ALLOWDARK);
    // ordinal 136 = FlushMenuThemes, refresh cached theme state
    if (auto f = (void(WINAPI*)())::GetProcAddress(ux, MAKEINTRESOURCEA(136))) f();
}
void SetDarkTitleBar(HWND top, bool dark) {
    BOOL v = dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(top, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &v, sizeof(v));
}
static bool systemUsesDarkMode() {   // uxtheme ordinal 132 = ShouldAppsUseDarkMode (Win10 1809+)
    HMODULE ux = ::GetModuleHandleW(L"uxtheme.dll");
    if (ux) if (auto p = (bool(WINAPI*)())::GetProcAddress(ux, MAKEINTRESOURCEA(132))) return p();
    return true;
}
void ApplyTitleBar(HWND top) {
    SetDarkTitleBar(top, IsDark());   // effective palette already resolves System -> dark/light
}
void PaintMenuBarBottomLine(HWND top) {
    MENUBARINFO mbi{ sizeof(mbi) };
    if (!::GetMenuBarInfo(top, OBJID_MENU, 0, &mbi) || mbi.rcBar.right <= mbi.rcBar.left) return;
    RECT wr; ::GetWindowRect(top, &wr);
    ::OffsetRect(&mbi.rcBar, -wr.left, -wr.top);   // screen -> window-DC coords
    int y = mbi.rcBar.bottom;
    HDC dc = ::GetWindowDC(top);
    // Windows draws a ~2px light 3D edge just below the bar; erase it with the content colour, then
    // lay down a single 1px themed border at the seam (the thin CMPCTheme MainMenuBorderColor line).
    RECT band   = { mbi.rcBar.left, y, mbi.rcBar.right, y + 3 };
    RECT border = { mbi.rcBar.left, y, mbi.rcBar.right, y + 1 };
    HBRUSH cb = ::CreateSolidBrush(WINDOW_BG);  ::FillRect(dc, &band, cb);   ::DeleteObject(cb);
    HBRUSH bb = ::CreateSolidBrush(MENU_BORDER); ::FillRect(dc, &border, bb); ::DeleteObject(bb);
    ::ReleaseDC(top, dc);
}
void InitTopWindow(HWND top) {
    enableDarkMode();
    ApplyTitleBar(top);
}

// --- palette (dark defaults; SetMode overwrites for Light) ---
// Values are transcribed 1:1 from CMPCTheme::InitializeColors() (upstream/src/mpc-hc/CMPCTheme.cpp).
// A few fields upstream marks "not implemented for light theme, default windows controls used" (it
// leaves a RGB(255,0,0) sentinel there because the owner-draw path is gated off entirely in light mode
// for those widgets) — for those we keep the Studio's own previously-chosen light value, noted below.
COLORREF WINDOW_BG = RGB(25,25,25), CONTENT_BG = RGB(32,32,32), CONTENT_SEL = RGB(119,119,119),
         CONTENT_DISABLED = RGB(40,40,40), GRID_LINE = RGB(43,43,43), FRAME_BORDER = RGB(99,99,99),
         TAB_INACTIVE = RGB(40,40,40), TAB_BORDER = RGB(99,99,99),
         TEXT = RGB(255,255,255), PROPPAGE_CAPTION_FG = RGB(255,255,255),
         TEXT_DIM = RGB(200,200,200), TEXT_DISABLED = RGB(109,109,109),
         GROUP_BORDER = RGB(118,118,118), GROUP_DISABLED = RGB(109,109,109),
         CTRL_BORDER = RGB(106,106,106),
         BTN_FILL = RGB(51,51,51), BTN_OUTER = RGB(240,240,240), BTN_INNER = RGB(155,155,155),
         BTN_INNER_FOCUS = RGB(255,255,255), BTN_FILL_HOVER = RGB(69,69,69), BTN_FILL_SEL = RGB(102,102,102),
         BTN_DOT = RGB(195,195,195), BTN_DOT_SEL = RGB(150,150,150), BTN_DOT_HOVER = RGB(181,181,181),
         CHK_BG = RGB(0,0,0), CHK_BORDER = RGB(137,137,137),
         CHK_BG_HOVER = RGB(8,8,8), CHK_BORDER_HOVER = RGB(121,121,121), CHK_MARK = RGB(222,222,222),
         SLIDER_CHANNEL = RGB(109,109,109), SLIDER_BORDER = RGB(0,0,0),
         SLIDER_THUMB = RGB(77,77,77), SLIDER_THUMB_HOVER = RGB(144,144,144), SLIDER_THUMB_DRAG = RGB(183,183,183),
         COMBO_ARROW = RGB(200,200,200), COMBO_ARROW_DISABLED = RGB(100,100,100),
         HEADER_HOT = RGB(67,67,67), HEADER_GRID = RGB(99,99,99), HEADER_SORT_ARROW = RGB(200,200,200),
         MENU_BG = RGB(43,43,43), MENU_SEL = RGB(65,65,65), MENUBAR_BG = RGB(43,43,43), MENUBAR_SEL = RGB(65,65,65),
         MENU_SEP = RGB(128,128,128),
         MENU_DISABLED = RGB(109,109,109), MENU_BORDER = RGB(32,32,32), MENU_ARROW = RGB(191,191,191);

static Mode g_pref = Mode::Dark;
static bool g_dark = true;
Mode Preference() { return g_pref; }
bool IsDark()     { return g_dark; }

void SetMode(Mode pref) {
    g_pref = pref;
    g_dark = pref == Mode::Dark || (pref == Mode::System && systemUsesDarkMode());
    if (!g_dark) {            // MPC-HC light modern palette (CMPCTheme::InitializeColors, LIGHT branch)
        WINDOW_BG = RGB(255,255,255); CONTENT_BG = RGB(255,255,255); CONTENT_SEL = RGB(0,120,215);
        CONTENT_DISABLED = RGB(255,255,255);  // sentinel upstream (light ListCtrlDisabledBGColor unused) -> = CONTENT_BG
        GRID_LINE = RGB(130,135,144);         // sentinel upstream (light ListCtrlGridColor unused) -> Studio's own choice
        FRAME_BORDER = RGB(130,135,144);
        TAB_INACTIVE = RGB(246,246,246); TAB_BORDER = RGB(227,227,227);
        TEXT = RGB(0,0,0); PROPPAGE_CAPTION_FG = RGB(245,245,245);   // caption text stays near-white on the blue gradient
        TEXT_DIM = RGB(109,109,109); TEXT_DISABLED = RGB(204,204,204);
        GROUP_BORDER = RGB(130,135,144); GROUP_DISABLED = RGB(176,176,176);
        CTRL_BORDER = RGB(106,106,106);
        BTN_FILL = RGB(225,225,225); BTN_OUTER = RGB(173,173,173); BTN_INNER = RGB(173,173,173);
        BTN_INNER_FOCUS = RGB(0,120,215); BTN_FILL_HOVER = RGB(229,241,251); BTN_FILL_SEL = RGB(204,228,247);
        BTN_DOT = RGB(17,17,17); BTN_DOT_SEL = RGB(60,20,7); BTN_DOT_HOVER = RGB(21,1,11);
        CHK_BG = RGB(255,255,255); CHK_BORDER = RGB(97,121,160);
        CHK_BG_HOVER = RGB(255,255,255); CHK_BORDER_HOVER = RGB(38,160,218); CHK_MARK = RGB(0,0,0);
        SLIDER_CHANNEL = RGB(128,128,128); SLIDER_BORDER = RGB(0,0,0);
        SLIDER_THUMB = RGB(205,205,205); SLIDER_THUMB_HOVER = RGB(166,166,166); SLIDER_THUMB_DRAG = RGB(119,119,119);
        HEADER_HOT = RGB(217,235,239); HEADER_GRID = RGB(130,135,144);  // grid sentinel -> GROUP_BORDER's value
        MENU_BG = RGB(238,238,238); MENU_SEL = RGB(255,255,255); MENUBAR_BG = RGB(255,255,255); MENUBAR_SEL = RGB(238,238,238);
        MENU_SEP = RGB(145,145,145);
        MENU_DISABLED = RGB(109,109,109); MENU_BORDER = RGB(255,255,255); MENU_ARROW = RGB(0,0,0);
    } else {                 // MPC-HC dark modern palette (CMPCTheme::InitializeColors, DARK branch)
        WINDOW_BG = RGB(25,25,25); CONTENT_BG = RGB(32,32,32); CONTENT_SEL = RGB(119,119,119);
        CONTENT_DISABLED = RGB(40,40,40); GRID_LINE = RGB(43,43,43); FRAME_BORDER = RGB(99,99,99);
        TAB_INACTIVE = RGB(40,40,40); TAB_BORDER = RGB(99,99,99);
        TEXT = RGB(255,255,255); PROPPAGE_CAPTION_FG = RGB(255,255,255);
        TEXT_DIM = RGB(200,200,200); TEXT_DISABLED = RGB(109,109,109);
        GROUP_BORDER = RGB(118,118,118); GROUP_DISABLED = RGB(109,109,109);
        CTRL_BORDER = RGB(106,106,106);
        BTN_FILL = RGB(51,51,51); BTN_OUTER = RGB(240,240,240); BTN_INNER = RGB(155,155,155);
        BTN_INNER_FOCUS = RGB(255,255,255); BTN_FILL_HOVER = RGB(69,69,69); BTN_FILL_SEL = RGB(102,102,102);
        BTN_DOT = RGB(195,195,195); BTN_DOT_SEL = RGB(150,150,150); BTN_DOT_HOVER = RGB(181,181,181);
        CHK_BG = RGB(0,0,0); CHK_BORDER = RGB(137,137,137);
        CHK_BG_HOVER = RGB(8,8,8); CHK_BORDER_HOVER = RGB(121,121,121); CHK_MARK = RGB(222,222,222);
        SLIDER_CHANNEL = RGB(109,109,109); SLIDER_BORDER = RGB(0,0,0);
        SLIDER_THUMB = RGB(77,77,77); SLIDER_THUMB_HOVER = RGB(144,144,144); SLIDER_THUMB_DRAG = RGB(183,183,183);
        HEADER_HOT = RGB(67,67,67); HEADER_GRID = RGB(99,99,99);
        MENU_BG = RGB(43,43,43); MENU_SEL = RGB(65,65,65); MENUBAR_BG = RGB(43,43,43); MENUBAR_SEL = RGB(65,65,65);
        MENU_SEP = RGB(128,128,128);
        MENU_DISABLED = RGB(109,109,109); MENU_BORDER = RGB(32,32,32); MENU_ARROW = RGB(191,191,191);
    }
}
// Theme-invariant (declared `const` outside InitializeColors() upstream — same value in both modes).
// COMBO_ARROW / COMBO_ARROW_DISABLED / HEADER_SORT_ARROW are initialized above and never change with SetMode.

// Solid brushes cached by color and never deleted, so class-registered / in-flight HBRUSHes never
// dangle across a mode switch (each palette color simply gets its own cached brush).
static HBRUSH solidCached(COLORREF c) {
    static std::vector<std::pair<COLORREF, HBRUSH>> cache;
    for (auto& e : cache) if (e.first == c) return e.second;
    HBRUSH b = ::CreateSolidBrush(c); cache.emplace_back(c, b); return b;
}
HBRUSH windowBrush()  { return solidCached(WINDOW_BG); }
HBRUSH contentBrush() { return solidCached(CONTENT_BG); }

int S(HDC dc, int v) { return ::MulDiv(v, ::GetDeviceCaps(dc, LOGPIXELSX), 96); }

HBRUSH windowCtl(HDC dc)  { ::SetTextColor(dc, TEXT); ::SetBkColor(dc, WINDOW_BG);  return windowBrush(); }
HBRUSH contentCtl(HDC dc) { ::SetTextColor(dc, TEXT); ::SetBkColor(dc, CONTENT_BG); return contentBrush(); }

// ============================================================================================
// DPI + per-window transient state (hover / pressed-half / hot-header-column). MPC-HC's classes
// carry this as CWnd member fields (isHover, downPos, hotItem); the studio's shared subclass proc
// has no per-instance storage, so it's kept in small hash maps keyed by HWND instead.
// ============================================================================================
int DpiOf(HWND h) {          // CMPCThemeUtil call sites all go through DpiHelper -> GetDpiForWindow
    static auto pGetDpiForWindow = reinterpret_cast<UINT(WINAPI*)(HWND)>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (pGetDpiForWindow) { UINT d = pGetDpiForWindow(h); if (d) return (int)d; }
    HDC dc = ::GetDC(h); int dpi = dc ? ::GetDeviceCaps(dc, LOGPIXELSX) : 96; if (dc) ::ReleaseDC(h, dc);
    return dpi ? dpi : 96;
}
// DpiHelper::GetSystemMetricsDPI (Win8+) resolves to GetSystemMetricsForDpi on builds that have it;
// MulDiv-scale the 96-dpi metric as a fallback, matching what DpiHelper does pre-Win10 1607.
static int MenuCheckMetricForDpi(int metric, int dpi) {
    static auto p = reinterpret_cast<int(WINAPI*)(int, UINT)>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    if (p) return p(metric, (UINT)dpi);
    return ::MulDiv(::GetSystemMetrics(metric), dpi, 96);
}
static int dpiBucket(int dpi) { return dpi < 120 ? 0 : dpi < 144 ? 1 : dpi < 168 ? 2 : dpi < 192 ? 3 : 4; }
// CMPCThemeComboBox::drawComboArrow / CMPCThemeHeaderCtrl::drawSortArrow share this table.
static float chevronSteps(int dpi) {
    static const float t[5] = { 3.5f, 4.f, 5.f, 5.f, 6.f };
    return t[dpiBucket(dpi)];
}
// CMPCThemeSpinButtonCtrl::drawSpinArrow's table.
static float spinnerSteps(int dpi) {
    static const float t[5] = { 2.f, 3.f, 4.f, 4.f, 4.5f };
    return t[dpiBucket(dpi)];
}

static std::unordered_map<HWND, bool>& hoverMap() { static std::unordered_map<HWND, bool> m; return m; }
static bool isHover(HWND h) { auto it = hoverMap().find(h); return it != hoverMap().end() && it->second; }
static void setHoverState(HWND h, bool v) {
    bool& cur = hoverMap()[h];
    if (cur != v) { cur = v; ::InvalidateRect(h, nullptr, TRUE); }
}
static std::unordered_map<HWND, POINT>& downPosMap() { static std::unordered_map<HWND, POINT> m; return m; }
static POINT downPosOf(HWND h) { auto it = downPosMap().find(h); return it != downPosMap().end() ? it->second : POINT{ -1,-1 }; }
static std::unordered_map<HWND, int>& headerHotMap() { static std::unordered_map<HWND, int> m; return m; }

// MFC's CDC::GetHalftoneBrush(): an 8x8 1bpp 50%-checkerboard pattern brush. FrameRect with this
// brush (after SetTextColor/SetBkColor) is how CMPCThemeButton/ComboBox/RadioOrCheck draw their
// dotted keyboard-focus rectangle.
static HBRUSH halftoneBrush() {
    static HBRUSH br = nullptr;
    if (!br) {
        static const WORD bits[8] = { 0x5555, 0xaaaa, 0x5555, 0xaaaa, 0x5555, 0xaaaa, 0x5555, 0xaaaa };
        HBITMAP bmp = ::CreateBitmap(8, 8, 1, 1, bits);
        br = ::CreatePatternBrush(bmp);
        ::DeleteObject(bmp);
    }
    return br;
}

// ---- GDI+ (chevron arrows: combo/header/spinner glyphs are drawn as anti-aliased strokes, not
// filled polygons -- see CMPCThemeComboBox::drawComboArrow / CMPCThemeHeaderCtrl::drawSortArrow /
// CMPCThemeSpinButtonCtrl::drawSpinArrow) and PNG-strip decoding (checkbox/radio glyphs). ----
static void ensureGdiplus() {
    static bool done = false; if (done) return; done = true;
    static ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
}
// CMPCThemeComboBox::drawComboArrow / CMPCThemeHeaderCtrl::drawSortArrow: two overlapping down-
// chevron strokes (not a filled triangle) — this is what gives MPC-HC's arrow its slightly bold,
// "double-struck" look versus a plain filled glyph.
static void drawChevron(HDC dc, COLORREF clr, RECT r, int dpi) {
    ensureGdiplus();
    float steps = chevronSteps(dpi);
    float xPos = r.left + ((r.right - r.left) - (steps * 2 + 1)) / 2.f;
    float yPos = r.top + ((r.bottom - r.top) - (steps + 1)) / 2.f;
    Gdiplus::Graphics gfx(dc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Color gc; gc.SetFromCOLORREF(clr);
    Gdiplus::Pen pen(gc, 1.0f);
    for (int i = 0; i < 2; ++i) {
        Gdiplus::PointF v[3] = { {xPos, yPos}, {steps + xPos, yPos + steps}, {steps * 2 + xPos, yPos} };
        gfx.DrawLines(&pen, v, 3);
    }
}
static void drawSortArrow(HDC dc, COLORREF clr, RECT r, bool ascending, int dpi) {
    ensureGdiplus();
    float steps = chevronSteps(dpi);
    float xPos = r.left + ((r.right - r.left) - (steps * 2 + 1)) / 2.f;
    float yPos = (float)r.top;
    Gdiplus::Graphics gfx(dc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Color gc; gc.SetFromCOLORREF(clr);
    Gdiplus::Pen pen(gc, 1.0f);
    for (int i = 0; i < 2; ++i) {
        Gdiplus::PointF v[3];
        if (ascending) { v[0] = {xPos,yPos}; v[1] = {steps+xPos,yPos+steps}; v[2] = {steps*2+xPos,yPos}; }
        else           { v[0] = {xPos,yPos+steps}; v[1] = {steps+xPos,yPos}; v[2] = {steps*2+xPos,yPos+steps}; }
        gfx.DrawLines(&pen, v, 3);
    }
}
// CMPCThemeSpinButtonCtrl::drawSpinArrow: a small filled triangle, orientation 0=left 1=right 2=top 3=bottom.
static void drawSpinArrow(HDC dc, COLORREF clr, RECT r, int orientation, int dpi) {
    ensureGdiplus();
    float steps = spinnerSteps(dpi);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    // INTEGER positions, exactly as CMPCThemeSpinButtonCtrl::drawSpinArrow computes them (its
    // xPos/yPos are ints): float centering put vertices on half-pixels, which with SmoothingModeNone
    // (the integral-steps path) rendered chopped, asymmetric triangles -- the 'bar with a stem'
    // up-arrows on IDD_PPAGESUBSTYLE's small spinners.
    int xPos, yPos; int xsign, ysign;
    switch (orientation) {
        case 0: xPos = r.right - (w - (int)steps) / 2;            yPos = r.top + (hgt - ((int)steps * 2 + 1)) / 2; xsign = -1; ysign =  1; break; // left
        case 1: xPos = r.left + (w - ((int)steps + 1)) / 2;       yPos = r.top + (hgt - ((int)steps * 2 + 1)) / 2; xsign =  1; ysign =  1; break; // right
        case 2: xPos = r.left + (w - ((int)steps * 2 + 1)) / 2;   yPos = r.bottom - (hgt - (int)steps) / 2;        xsign =  1; ysign = -1; break; // top
        default:xPos = r.left + (w - ((int)steps * 2 + 1)) / 2;   yPos = r.top + (hgt - ((int)steps + 1)) / 2;     xsign =  1; ysign =  1; break; // bottom
    }
    // PointF construction matches upstream: int anchors, float step offsets (implicit widening there;
    // explicit casts here because brace-init rejects the narrowing int -> REAL path).
    float fx = (float)xPos, fy = (float)yPos;
    Gdiplus::PointF v[3];
    if (orientation == 0 || orientation == 1) {
        v[0] = {fx, fy}; v[1] = {fx + steps * xsign, fy + steps * ysign}; v[2] = {fx, fy + steps * 2 * ysign};
    } else {
        v[0] = {fx, fy}; v[1] = {fx + steps * xsign, fy + steps * ysign}; v[2] = {fx + steps * 2 * xsign, fy};
    }
    Gdiplus::Graphics gfx(dc);
    bool frac = (steps != std::floor(steps));
    gfx.SetSmoothingMode(frac ? Gdiplus::SmoothingModeAntiAlias : Gdiplus::SmoothingModeNone);
    Gdiplus::Color gc; gc.SetFromCOLORREF(clr);
    Gdiplus::Pen pen(gc, 1.0f);
    gfx.DrawPolygon(&pen, v, 3);
    Gdiplus::SolidBrush br(gc);
    gfx.FillPolygon(&br, v, 3);
}

// ---- checkbox/radio sprite strips (CMPCThemeUtil::drawCheckBox / getResourceByDPI). PNGs decoded
// via GDI+ (Bitmap::FromStream on the resource bytes) rather than upstream's ATL CImage -- same
// bits, minimal loader per the task brief. Cached per resource id, never freed (app lifetime). ----
namespace {
struct GlyphStrip { HBITMAP bmp = nullptr; int size = 0; };
GlyphStrip loadGlyphStrip(UINT resId) {
    GlyphStrip gs;
    HINSTANCE inst = AfxGetInstanceHandle();
    HRSRC hRsrc = ::FindResourceW(inst, MAKEINTRESOURCEW(resId), L"PNG");
    if (!hRsrc) return gs;
    DWORD size = ::SizeofResource(inst, hRsrc);
    HGLOBAL hRes = ::LoadResource(inst, hRsrc);
    void* src = hRes ? ::LockResource(hRes) : nullptr;
    if (!src || !size) return gs;
    HGLOBAL hBuf = ::GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hBuf) return gs;
    void* dst = ::GlobalLock(hBuf);
    memcpy(dst, src, size);
    ::GlobalUnlock(hBuf);
    IStream* stream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(hBuf, TRUE, &stream))) { ::GlobalFree(hBuf); return gs; }
    ensureGdiplus();
    {
        Gdiplus::Bitmap bmp(stream);
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            HBITMAP hbm = nullptr;
            if (bmp.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hbm) == Gdiplus::Ok) {
                gs.bmp = hbm;
                gs.size = (int)bmp.GetHeight();   // strip is `size` tall; each cell is `size` wide
            }
        }
    }
    stream->Release();
    return gs;
}
GlyphStrip& glyphStripCached(UINT resId) {
    static std::vector<std::pair<UINT, GlyphStrip>> cache;
    for (auto& e : cache) if (e.first == resId) return e.second;
    cache.emplace_back(resId, loadGlyphStrip(resId));
    return cache.back().second;
}
// CMPCTheme::ThemeCheckBoxes / ThemeRadios: index 3 (168dpi bucket) maps to the same 144 strip as
// index 2 -- there is no 168 checkbox/radio PNG upstream.
UINT checkboxResourceForDpi(int dpi) {
    static const UINT ids[5] = { IDB_DT_CB_96, IDB_DT_CB_120, IDB_DT_CB_144, IDB_DT_CB_144, IDB_DT_CB_192 };
    return ids[dpiBucket(dpi)];
}
UINT radioResourceForDpi(int dpi) {
    static const UINT ids[5] = { IDB_DT_RADIO_96, IDB_DT_RADIO_120, IDB_DT_RADIO_144, IDB_DT_RADIO_144, IDB_DT_RADIO_192 };
    return ids[dpiBucket(dpi)];
}
} // namespace

// ---- push button base (CMPCThemeButton::drawButtonBase) — shared by ordinary push buttons and by
// BS_PUSHLIKE checkboxes/radios (CMPCThemeRadioOrCheck::OnPaint delegates to this when BS_PUSHLIKE
// is set, with `thin=false` — the only caller that draws the full outer+inner double border). ----
static void drawButtonBase(HDC dc, RECT rect, const wchar_t* text, int textLen,
                            bool selected, bool highlighted, bool focused, bool disabledLook, bool thin) {
    if (!thin) {   // the near-white outer border only appears for BS_PUSHLIKE (thin=true everywhere else)
        HBRUSH ob = ::CreateSolidBrush(BTN_OUTER); ::FrameRect(dc, &rect, ob); ::DeleteObject(ob);
        ::InflateRect(&rect, -1, -1);
    }
    COLORREF bg = BTN_FILL, dottedClr = BTN_DOT, innerClr;
    if (selected)          { innerClr = BTN_INNER;       bg = BTN_FILL_SEL;   dottedClr = BTN_DOT_SEL; }
    else if (highlighted)  { innerClr = BTN_INNER;       bg = BTN_FILL_HOVER; dottedClr = BTN_DOT_HOVER; }
    else if (focused)      { innerClr = BTN_INNER_FOCUS; }
    else                   { innerClr = BTN_INNER; }
    HBRUSH ib = ::CreateSolidBrush(innerClr); ::FrameRect(dc, &rect, ib); ::DeleteObject(ib);
    ::InflateRect(&rect, -1, -1);
    HBRUSH fb = ::CreateSolidBrush(bg); ::FillRect(dc, &rect, fb); ::DeleteObject(fb);
    if (focused) {
        RECT fr = rect; ::InflateRect(&fr, -1, -1);
        ::SetTextColor(dc, dottedClr); ::SetBkColor(dc, bg);
        ::FrameRect(dc, &fr, halftoneBrush());
    }
    if (textLen > 0) {
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, disabledLook ? TEXT_DISABLED : TEXT);
        ::DrawTextW(dc, text, textLen, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// ---- group box (CMPCThemeGroupBox::OnPaint) ----
static void draw_group(HWND h, HDC dc) {
    RECT rc; ::GetClientRect(h, &rc);
    ::FillRect(dc, &rc, windowBrush());
    HFONT f = (HFONT)::SendMessage(h, WM_GETFONT, 0, 0);
    HGDIOBJ of = f ? ::SelectObject(dc, f) : nullptr;
    RECT wCalc{ 0, 0, 0, 0 };
    ::DrawTextW(dc, L"W", 1, &wCalc, DT_SINGLELINE | DT_CALCRECT);   // CMPCThemeUtil::GetTextSize("W")
    RECT rb = rc; rb.top += wCalc.bottom / 2;
    HBRUSH bd = ::CreateSolidBrush(GROUP_BORDER); ::FrameRect(dc, &rb, bd); ::DeleteObject(bd);
    wchar_t txt[256] = {}; int n = ::GetWindowTextW(h, txt, 256);
    if (n) {
        CStringW text(txt); text += L" ";   // upstream appends a trailing space ("seems to be the default behavior")
        RECT tr = rc; tr.left += S(dc, 8);   // CMPCTheme::GroupBoxTextIndent
        ::SetBkMode(dc, OPAQUE); ::SetBkColor(dc, WINDOW_BG);   // opaque text paints straight over the border line
        ::SetTextColor(dc, ::IsWindowEnabled(h) ? TEXT : GROUP_DISABLED);
        ::DrawTextW(dc, text, text.GetLength(), &tr, DT_TOP | DT_LEFT | DT_SINGLELINE | DT_EDITCONTROL);
    }
    if (of) ::SelectObject(dc, of);
}

// ---- checkbox / radio (CMPCThemeRadioOrCheck::OnPaint + CMPCThemeUtil::drawCheckBoxInternal) ----
// Glyphs come from the checkboxes-*/radios-*.png sprite strips (one cell per state), not a vector
// drawing — matching upstream exactly instead of approximating with Ellipse/Polyline.
static void draw_check(HWND h, HDC dc, bool radio) {
    RECT rc; ::GetClientRect(h, &rc);
    LONG style = (LONG)::GetWindowLongPtr(h, GWL_STYLE);
    LRESULT checkState = ::SendMessageW(h, BM_GETCHECK, 0, 0);
    wchar_t txt[512] = {}; int n = ::GetWindowTextW(h, txt, 512);
    HFONT wf = (HFONT)::SendMessage(h, WM_GETFONT, 0, 0);

    if (style & BS_PUSHLIKE) {   // push-like check/radio -> CMPCThemeButton::drawButtonBase, thin=false
        bool selected = checkState != BST_UNCHECKED;
        bool highlighted = isHover(h);
        bool focused = ::GetFocus() == h;
        bool indeterminate = checkState == BST_INDETERMINATE;
        HGDIOBJ of = wf ? ::SelectObject(dc, wf) : nullptr;
        drawButtonBase(dc, rc, txt, n, selected, highlighted, focused, indeterminate, /*thin=*/false);
        if (of) ::SelectObject(dc, of);
        return;
    }

    ::FillRect(dc, &rc, windowBrush());
    int dpi = DpiOf(h);
    int cbW = MenuCheckMetricForDpi(SM_CXMENUCHECK, dpi), cbH = MenuCheckMetricForDpi(SM_CYMENUCHECK, dpi);

    // CMPCThemeRadioOrCheck::OnPaint (~L80-104) computes rectCheck.top = (rectItem.Height() - cbHeight)/2
    // unconditionally, BEFORE any text metrics exist: at that point rectItem is still the untouched
    // client rect (only .left/.right get shifted in for the check column just above; the DT_CALCRECT
    // text-measuring block runs later, at L114-144, and never feeds back into rectCheck). So upstream
    // itself always vcenters the glyph on the FULL control height, even for BS_MULTILINE|BS_TOP -- it
    // does NOT anchor to the first text line there. That full-height centering is what this function
    // already replicated below (unconditionally, prior to this change).
    //
    // But upstream's OWN full-height centering doesn't match NATIVE (light, non-owner-drawn) multiline
    // checkboxes, which keep the glyph pinned to the first text line while the rest of the wrapped text
    // flows below it -- a real dark/light pixel mismatch for BS_MULTILINE|BS_TOP controls (see task
    // brief). Since upstream has no BS_TOP-specific branch to port verbatim, anchor to the first line's
    // font metrics ourselves here so dark matches native light behavior; single-line and non-TOP
    // multiline (BS_VCENTER/BS_BOTTOM) controls keep the original full-height centering unchanged.
    HGDIOBJ of = wf ? ::SelectObject(dc, wf) : nullptr;   // select font now: needed for GetTextMetricsW below
    LONG vert = style & BS_VCENTER;                        // BS_VCENTER == BS_TOP|BS_BOTTOM
    bool topMultiline = (style & BS_MULTILINE) && vert == BS_TOP;

    RECT g;   // CMPCThemeRadioOrCheck::OnPaint: cbWidth x cbHeight box, left-aligned
    g.left = rc.left; g.right = g.left + cbW;
    if (topMultiline) {
        TEXTMETRICW tm{}; ::GetTextMetricsW(dc, &tm);       // one text line's height, for the first-line anchor
        g.top = rc.top + (tm.tmHeight - cbH) / 2;
    } else {
        g.top = rc.top + ((rc.bottom - rc.top) - cbH) / 2;
    }
    g.bottom = g.top + cbH;
    RECT textR = rc; textR.left = g.right + 2;

    bool hover = isHover(h);
    COLORREF borderClr = hover ? CHK_BORDER_HOVER : CHK_BORDER;
    COLORREF bgClr = hover ? CHK_BG_HOVER : CHK_BG;
    UINT resId = radio ? radioResourceForDpi(dpi) : checkboxResourceForDpi(dpi);
    GlyphStrip& strip = glyphStripCached(resId);

    if (!radio && checkState != BST_CHECKED) {
        // unchecked/indeterminate checkbox: hand-drawn frame+fill, never from the PNG (the PNG's
        // 2 cells are only "checked, regular" / "checked, hover" -- see drawCheckBoxInternal).
        int sz = strip.size ? strip.size : min(cbW, cbH);
        RECT dr{ 0, 0, sz, sz };
        ::OffsetRect(&dr, g.left, g.top + ((g.bottom - g.top) - sz) / 2);
        HBRUSH bd = ::CreateSolidBrush(borderClr); ::FrameRect(dc, &dr, bd); ::DeleteObject(bd);
        ::InflateRect(&dr, -1, -1);
        HBRUSH bgb = ::CreateSolidBrush(bgClr); ::FillRect(dc, &dr, bgb); ::DeleteObject(bgb);
        if (checkState == BST_INDETERMINATE) {
            ::InflateRect(&dr, -2, -2);
            HBRUSH ck = ::CreateSolidBrush(CHK_MARK); ::FillRect(dc, &dr, ck); ::DeleteObject(ck);
        }
    } else if (strip.bmp) {   // checked checkbox, or any-state radio: blit the sprite cell
        int index = radio ? ((checkState ? 1 : 0) + (hover ? 2 : 0)) : (hover ? 1 : 0);
        RECT dr{ 0, 0, strip.size, strip.size };
        ::OffsetRect(&dr, g.left, g.top + ((g.bottom - g.top) - strip.size) / 2);
        HDC mdc = ::CreateCompatibleDC(dc); HGDIOBJ old = ::SelectObject(mdc, strip.bmp);
        ::BitBlt(dc, dr.left, dr.top, strip.size, strip.size, mdc, index * strip.size, 0, SRCCOPY);
        ::SelectObject(mdc, old); ::DeleteDC(mdc);
    }

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, ::IsWindowEnabled(h) ? TEXT : TEXT_DISABLED);
    // Honor BS_MULTILINE: several MPC-HC radios/checks are multiline and sized for 2+ lines (e.g.
    // IDD_PPAGEPLAYER's IDC_RADIO1/2 are BS_MULTILINE|BS_TOP at 126x27 DLU). Drawing them
    // DT_SINGLELINE clipped long (translated) captions instead of wrapping like the real control.
    // DT_VCENTER/DT_BOTTOM are DT_SINGLELINE-only, so for wrapped text we measure and offset.
    // (`vert`/font selection computed above, alongside the glyph rect -- topMultiline needed it early.)
    UINT fmt = DT_LEFT;
    if (style & BS_MULTILINE) {
        fmt |= DT_WORDBREAK | DT_TOP;
        if (vert != BS_TOP) {                    // bottom- or centre-aligned: place the block ourselves
            RECT calc = textR;
            ::DrawTextW(dc, txt, n, &calc, fmt | DT_CALCRECT);
            int extra = (textR.bottom - textR.top) - (calc.bottom - calc.top);
            if (extra > 0) textR.top += (vert == BS_BOTTOM) ? extra : extra / 2;
        }
    } else {
        fmt |= DT_VCENTER | DT_SINGLELINE;
    }
    ::DrawTextW(dc, txt, n, &textR, fmt);
    if (of) ::SelectObject(dc, of);
}

// ---- push button (CMPCThemeButton::drawButton -> drawButtonBase, thin=true) ----
static void draw_button(HWND h, HDC dc) {
    RECT rc; ::GetClientRect(h, &rc);
    wchar_t txt[256] = {}; int n = ::GetWindowTextW(h, txt, 256);
    bool selected = (::SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;   // native-tracked mouse-down
    bool highlighted = isHover(h);
    bool focused = ::GetFocus() == h;
    bool disabled = !::IsWindowEnabled(h);
    HFONT f = (HFONT)::SendMessage(h, WM_GETFONT, 0, 0); HGDIOBJ of = f ? ::SelectObject(dc, f) : nullptr;
    drawButtonBase(dc, rc, txt, n, selected, highlighted, focused, disabled, /*thin=*/true);
    if (of) ::SelectObject(dc, of);
}

// ---- combo box closed face (CMPCThemeComboBox::OnPaint) ----
static void draw_combo(HWND h, HDC dc) {
    RECT r; ::GetClientRect(h, &r);
    int dpi = DpiOf(h);
    COMBOBOXINFO info{ sizeof(info) };
    ::GetComboBoxInfo(h, &info);
    // CBS_DROPDOWN/CBS_SIMPLE have a real child Edit (dlg-item 1001, as upstream's GetDlgItem(1001)
    // checks); CBS_DROPDOWNLIST does NOT -- its COMBOBOXINFO.hwndItem is the combo itself, so testing
    // hwndItem != null wrongly picked the edit branch for every settings combo (divider + button cell
    // instead of MPC-HC's single flat button-fill face).
    HWND editChild = ::GetDlgItem(h, 1001);
    bool hasEdit = editChild != nullptr;
    bool listOpen = info.hwndList && ::IsWindowVisible(info.hwndList);
    bool pressed = info.stateButton == STATE_SYSTEM_PRESSED;
    bool hover = isHover(h);
    bool enabled = ::IsWindowEnabled(h);
    bool focused, drawDotted = false;
    COLORREF borderClr;
    if (hasEdit) {
        focused = ::GetFocus() == info.hwndItem;
        borderClr = focused ? BTN_INNER_FOCUS : BTN_INNER;
    } else {
        focused = ::GetFocus() == h;
        drawDotted = focused;   // dropdown-list style: no edit border-color change, a dotted rect around the text instead
        borderClr = BTN_INNER;
    }
    COLORREF bkColor, fgColor = TEXT, arrowColor = COMBO_ARROW;
    if (listOpen || pressed)     { bkColor = BTN_FILL_SEL; drawDotted = false; }
    else if (!pressed && hover)  { bkColor = BTN_FILL_HOVER; }
    else if (!enabled)           { bkColor = BTN_FILL; fgColor = TEXT_DISABLED; arrowColor = COMBO_ARROW_DISABLED; }
    else                         { bkColor = BTN_FILL; }

    RECT rBG = r; ::InflateRect(&rBG, -1, -1);
    RECT rButton = info.rcButton;
    if (hasEdit) {   // the real Edit paints its own text; we own the button cell + divider + border only
        HBRUSH bb = ::CreateSolidBrush(bkColor); ::FillRect(dc, &rButton, bb); ::DeleteObject(bb);
        RECT rFill = rBG; rFill.right = rButton.left - 1;
        ::FillRect(dc, &rFill, windowBrush());   // parent dialog bg shows through (drawParentDialogBGClr)
        RECT rDivider = { rFill.right, rFill.top, rFill.right + 1, rFill.bottom };
        HBRUSH db = ::CreateSolidBrush(BTN_INNER); ::FillRect(dc, &rDivider, db); ::DeleteObject(db);
    } else {
        HBRUSH bb = ::CreateSolidBrush(bkColor); ::FillRect(dc, &rBG, bb); ::DeleteObject(bb);
        wchar_t txt[256] = {}; int n = ::GetWindowTextW(h, txt, 256);
        RECT rText = r; rText.right = info.rcItem.right; ::InflateRect(&rText, -3, -3);
        HFONT f = (HFONT)::SendMessage(h, WM_GETFONT, 0, 0); HGDIOBJ of = f ? ::SelectObject(dc, f) : nullptr;
        ::SetBkColor(dc, bkColor); ::SetTextColor(dc, fgColor);
        ::DrawTextW(dc, txt, n, &rText, DT_VCENTER | DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        if (drawDotted) {
            ::SetTextColor(dc, bkColor ^ 0x00FFFFFF);
            ::FrameRect(dc, &rText, halftoneBrush());
        }
        if (of) ::SelectObject(dc, of);
    }
    drawChevron(dc, arrowColor, rButton, dpi);
    HBRUSH fb = ::CreateSolidBrush(borderClr); ::FrameRect(dc, &r, fb); ::DeleteObject(fb);
}

// ---- up/down spinner (CMPCThemeSpinButtonCtrl::OnPaint, vertical) ----
// Buddied with UDS_ALIGNRIGHT (see LivePreview::IntegrateSpinners): frame the control in the edit's
// border color but skip the left edge it shares with the buddy edit, then draw the two arrow buttons.
static void draw_updown(HWND h, HDC dc) {
    RECT rc; ::GetClientRect(h, &rc);
    ::FillRect(dc, &rc, contentBrush());                       // ContentBGColor
    bool hasBuddy = ::SendMessageW(h, UDM_GETBUDDY, 0, 0) != 0;
    // UDS_HORZ spinners (e.g. IDD_PPAGELOGO's logo selector) split LEFT/RIGHT with left/right arrows;
    // vertical ones split TOP/BOTTOM -- CMPCThemeSpinButtonCtrl::OnPaint's horz branch (.cpp:131-195),
    // ported 1:1 (buddy border excludes the shared TOP edge when horz, LEFT edge when vertical).
    bool horz = (::GetWindowLongPtr(h, GWL_STYLE) & UDS_HORZ) != 0;
    if (hasBuddy) {
        int save = ::SaveDC(dc);
        if (horz) ::ExcludeClipRect(dc, rc.left + 1, rc.top, rc.right - 1, rc.top + 1);
        else      ::ExcludeClipRect(dc, rc.left, rc.top + 1, rc.left + 1, rc.bottom - 1);
        HBRUSH eb = ::CreateSolidBrush(CTRL_BORDER); ::FrameRect(dc, &rc, eb); ::DeleteObject(eb);
        ::RestoreDC(dc, save);
    }
    int dpi = DpiOf(h);
    POINT dp = downPosOf(h);          // last WM_LBUTTONDOWN/mouse-move-with-button point (see ThemeProc)
    int buddySpacing = hasBuddy ? 1 : 0;
    bool enabled = ::IsWindowEnabled(h);
    COLORREF arrowClr = enabled ? TEXT : TEXT_DISABLED;
    for (int i = 0; i < 2; ++i) {                              // 0 = up/left, 1 = down/right
        RECT b = rc;
        if (horz) {
            b.left += 1; b.top += 1; b.right -= 1; b.bottom -= 1 + buddySpacing;   // DeflateRect(1,1,1,1+bs)
            int ww = b.right - b.left;
            if (i == 0) b.right -= ww / 2; else b.left += ww / 2;
            b.left += 1; b.right -= 1;                         // DeflateRect(1,0)
        } else {
            b.left += 1; b.top += 1; b.right -= 1 + buddySpacing; b.bottom -= 1;   // DeflateRect(1,1,1+bs,1)
            int hh = b.bottom - b.top;
            if (i == 0) b.bottom -= hh / 2; else b.top += hh / 2;
            b.top += 1; b.bottom -= 1;                         // DeflateRect(0,1)
        }
        bool pressed = ::PtInRect(&b, dp) != FALSE;
        HBRUSH bf = ::CreateSolidBrush(pressed ? BTN_FILL_SEL : BTN_FILL); ::FillRect(dc, &b, bf); ::DeleteObject(bf);
        HBRUSH bi = ::CreateSolidBrush(BTN_INNER); ::FrameRect(dc, &b, bi); ::DeleteObject(bi);
        int orient = horz ? (i == 0 ? 0 /*left*/ : 1 /*right*/) : (i == 0 ? 2 /*top*/ : 3 /*bottom*/);
        drawSpinArrow(dc, arrowClr, b, orient, dpi);
    }
}

// ---- list-view column header (CMPCThemeHeaderCtrl::drawItem, applied per-column via DrawAllItems) ----
static void draw_header(HWND h, HDC dc) {
    RECT rc; ::GetClientRect(h, &rc);
    HFONT f = (HFONT)::SendMessage(h, WM_GETFONT, 0, 0); HGDIOBJ of = f ? ::SelectObject(dc, f) : nullptr;
    ::SetBkMode(dc, OPAQUE);
    int dpi = DpiOf(h);
    int cnt = Header_GetItemCount(h);
    int hot = headerHotMap().count(h) ? headerHotMap()[h] : -2;
    HPEN gridPen = ::CreatePen(PS_SOLID, 1, HEADER_GRID);
    HGDIOBJ oldPen = ::SelectObject(dc, gridPen);

    auto drawCell = [&](int i, RECT ir) {
        RECT rGrid = ir; rGrid.top -= 1; rGrid.bottom -= 1;
        COLORREF bg = (i == hot) ? HEADER_HOT : CONTENT_BG;   // header band = ContentBGColor, not a separate color
        HBRUSH bgb = ::CreateSolidBrush(bg); ::FillRect(dc, &rGrid, bgb); ::DeleteObject(bgb);
        if (i > 0) { ::MoveToEx(dc, rGrid.left, rGrid.top, nullptr); ::LineTo(dc, rGrid.left, rGrid.bottom); }
        else       { ::MoveToEx(dc, rGrid.left, rGrid.bottom, nullptr); }
        ::LineTo(dc, rGrid.right, rGrid.bottom);
        if (i >= 0) {
            wchar_t txt[128] = {}; HDITEMW it{}; it.mask = HDI_TEXT | HDI_FORMAT; it.pszText = txt; it.cchTextMax = 128;
            Header_GetItem(h, i, &it);
            UINT align = it.fmt & HDF_JUSTIFYMASK;
            RECT tr = ir;
            UINT fmt = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
            if (align == HDF_CENTER) fmt |= DT_CENTER;
            else if (align == HDF_LEFT) { fmt |= DT_LEFT; tr.left += S(dc, 6); }
            else { fmt |= DT_RIGHT; tr.right -= S(dc, 6); }
            ::SetTextColor(dc, TEXT); ::SetBkColor(dc, bg);
            ::DrawTextW(dc, txt, -1, &tr, fmt);
            if (it.fmt & (HDF_SORTUP | HDF_SORTDOWN))
                drawSortArrow(dc, HEADER_SORT_ARROW, tr, (it.fmt & HDF_SORTUP) != 0, dpi);
        }
    };
    int xMax = rc.left;
    for (int i = 0; i < cnt; ++i) { RECT ir; if (!Header_GetItemRect(h, i, &ir)) continue; xMax = max(xMax, (int)ir.right); drawCell(i, ir); }
    RECT tail = rc; tail.left = xMax; tail.right = rc.right + 1;
    drawCell(-1, tail);   // "tail border" past the last column

    ::SelectObject(dc, oldPen); ::DeleteObject(gridPen);
    if (of) ::SelectObject(dc, of);
}

// ---- tab strip (CMPCThemeTabCtrl style): outlined tabs, inactive ones tinted, the active one filled
// with the content color and merged through the baseline. All tabs share the same top (no pop-up, so
// the top outline is never clipped by the control's client edge). ----
static void draw_tabs(HWND h, HDC dc) {
    RECT rc; ::GetClientRect(h, &rc);
    ::FillRect(dc, &rc, windowBrush());
    HFONT f = (HFONT)::SendMessage(h, WM_GETFONT, 0, 0); HGDIOBJ of = f ? ::SelectObject(dc, f) : nullptr;
    ::SetBkMode(dc, TRANSPARENT);
    int cnt = TabCtrl_GetItemCount(h), sel = TabCtrl_GetCurSel(h);
    if (cnt <= 0) { if (of) ::SelectObject(dc, of); return; }

    HPEN pen = ::CreatePen(PS_SOLID, 1, TAB_BORDER);
    RECT r0; TabCtrl_GetItemRect(h, 0, &r0);
    int base = r0.bottom;                       // shared bottom of the tab row = content baseline
    { HGDIOBJ op = ::SelectObject(dc, pen);
      ::MoveToEx(dc, rc.left, base, nullptr); ::LineTo(dc, rc.right, base);   // separator under the tabs
      ::SelectObject(dc, op); }

    auto drawTab = [&](int i, bool on) {
        RECT ir; if (!TabCtrl_GetItemRect(h, i, &ir)) return;
        int bottom = on ? base + 1 : ir.bottom;         // the active tab reaches through the baseline
        RECT fill = { ir.left, ir.top, ir.right, bottom };
        HBRUSH bb = ::CreateSolidBrush(on ? WINDOW_BG : TAB_INACTIVE);
        ::FillRect(dc, &fill, bb); ::DeleteObject(bb);
        HGDIOBJ op = ::SelectObject(dc, pen);           // outline: left edge up, across the top, right edge down
        ::MoveToEx(dc, ir.left, bottom, nullptr);
        ::LineTo(dc, ir.left, ir.top);
        ::LineTo(dc, ir.right, ir.top);
        ::LineTo(dc, ir.right, bottom);
        ::SelectObject(dc, op);
        wchar_t txt[128] = {}; TCITEMW it{}; it.mask = TCIF_TEXT; it.pszText = txt; it.cchTextMax = 128;
        TabCtrl_GetItem(h, i, &it);
        // CMPCThemeTabCtrl::doDrawItem always uses TextFGColor -- inactive tab labels are NOT dimmed.
        ::SetTextColor(dc, TEXT);       // center in the native item rect (no inset -> no ellipsis)
        ::DrawTextW(dc, txt, -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    for (int i = 0; i < cnt; ++i) if (i != sel) drawTab(i, false);   // inactive first
    if (sel >= 0) drawTab(sel, true);                                // active on top
    ::DeleteObject(pen);
    if (of) ::SelectObject(dc, of);
}

// Edit border (port of CMPCThemeEdit::OnNcPaint): the DarkMode theme draws a too-bright edit border
// that clashes with the (owner-drawn) spinner. Redraw it in EditBorderColor so a spin field reads as
// one continuous box. Not merged into ThemeProc: an edit paints its own text/bg, so it must NOT get
// the owner-draw WM_ERASEBKGND/WM_PAINT handling.
static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_NCDESTROY) { ::RemoveWindowSubclass(h, EditProc, id); return ::DefSubclassProc(h, msg, w, l); }
    if (msg == WM_NCPAINT) {
        LRESULT r = ::DefSubclassProc(h, msg, w, l);       // themed non-client first
        LONG st = (LONG)::GetWindowLongPtr(h, GWL_STYLE), ex = (LONG)::GetWindowLongPtr(h, GWL_EXSTYLE);
        if ((st & WS_BORDER) || (ex & WS_EX_CLIENTEDGE)) {
            HDC dc = ::GetWindowDC(h);
            RECT rc; ::GetWindowRect(h, &rc); ::OffsetRect(&rc, -rc.left, -rc.top);
            HBRUSH b = ::CreateSolidBrush(CTRL_BORDER); ::FrameRect(dc, &rc, b); ::DeleteObject(b);
            ::ReleaseDC(h, dc);
        }
        return r;
    }
    return ::DefSubclassProc(h, msg, w, l);
}

// ---- report-mode list view rows (CMPCThemePlayerListCtrl::OnPaint / drawItem / DrawAllItems) ----
// Scoped port: report-style rows/columns with selection + disabled-row shading + optional grid lines,
// matching the exact colors/metrics/text-alignment of upstream's drawItem. NOT ported: the memory-DC
// double-buffer pipeline (upstream buffers for scroll-perf on large playlists; these preview lists are
// tiny), state-image/checkbox columns, per-row bold "flagged" font, and custom grid-color callbacks —
// none of which the translator-facing dialogs' option/setting lists use. Scrollbars stay native (see
// SetWindowTheme in ApplyToChildren) per the task brief.
static void draw_list(HWND h, HDC dc) {
    RECT rc; ::GetClientRect(h, &rc);
    ::FillRect(dc, &rc, contentBrush());
    LONG style = (LONG)::GetWindowLongPtr(h, GWL_STYLE);
    if ((style & LVS_TYPEMASK) != LVS_REPORT) return;   // icon/list modes: leave to native paint (rare in these dialogs)

    HWND hdr = ListView_GetHeader(h);
    int cols = hdr ? Header_GetItemCount(hdr) : 1; if (cols < 1) cols = 1;
    bool showSelAlways = (style & LVS_SHOWSELALWAYS) != 0;
    bool listHasFocus = ::GetFocus() == h;
    bool enabled = ::IsWindowEnabled(h);
    DWORD ext = ListView_GetExtendedListViewStyle(h);
    bool gridLines = (ext & LVS_EX_GRIDLINES) != 0;
    // LVS_EX_CHECKBOXES (TxSyncDlg's decision list): comctl32 normally draws the checkbox glyph
    // itself as part of native item painting, which this function entirely replaces (own
    // BeginPaint/EndPaint in ThemeProc, id 7) -- so it must be drawn here too, or checked items would
    // show no glyph at all. Click-to-toggle keeps working regardless (LVM_HITTEST's
    // LVHT_ONITEMSTATEICON region is comctl32-internal geometry, unaffected by how we paint over it).
    bool checkboxes = (ext & LVS_EX_CHECKBOXES) != 0;
    int checkBox = S(dc, 13), checkPad = S(dc, 4);
    HFONT f = (HFONT)::SendMessage(h, WM_GETFONT, 0, 0); HGDIOBJ of = f ? ::SelectObject(dc, f) : nullptr;
    ::SetBkMode(dc, OPAQUE);
    HPEN gridPen = gridLines ? ::CreatePen(PS_SOLID, 1, GRID_LINE) : nullptr;

    int top = ListView_GetTopIndex(h), per = ListView_GetCountPerPage(h);
    int total = ListView_GetItemCount(h);
    int first = max(0, top - 1), last = min(total - 1, top + per + 1);
    for (int item = first; item <= last; ++item) {
        RECT rowRect{}; if (!ListView_GetItemRect(h, item, &rowRect, LVIR_BOUNDS)) continue;
        if (rowRect.bottom < rc.top || rowRect.top > rc.bottom) continue;
        bool selected = (ListView_GetItemState(h, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        selected = selected && (showSelAlways || listHasFocus);
        if (!enabled) {
            HBRUSH b = ::CreateSolidBrush(CONTENT_DISABLED); ::FillRect(dc, &rowRect, b); ::DeleteObject(b);
        }
        for (int sub = 0; sub < cols; ++sub) {
            RECT rBounds{};
            if (!ListView_GetSubItemRect(h, item, sub, LVIR_BOUNDS, &rBounds)) continue;
            RECT rText = rBounds;
            wchar_t txt[256] = {}; ListView_GetItemText(h, item, sub, txt, 256);
            UINT hdrAlign = HDF_LEFT;
            if (hdr) { HDITEMW hi{}; hi.mask = HDI_FORMAT; Header_GetItem(hdr, sub, &hi); hdrAlign = hi.fmt & HDF_JUSTIFYMASK; }
            UINT fmt = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
            if (hdrAlign == HDF_CENTER)     fmt |= DT_CENTER;
            else if (hdrAlign == HDF_RIGHT) { fmt |= DT_RIGHT; rText.right -= S(dc, 6); }
            else                            { fmt |= DT_LEFT;  rText.left += S(dc, sub == 0 ? 2 : 6); }
            COLORREF bg = !enabled ? CONTENT_DISABLED : (selected ? CONTENT_SEL : CONTENT_BG);
            if (enabled) { HBRUSH b = ::CreateSolidBrush(bg); ::FillRect(dc, &rBounds, b); ::DeleteObject(b); }
            if (sub == 0 && checkboxes) {
                RECT cb{ rBounds.left + checkPad, rBounds.top + ((rBounds.bottom - rBounds.top) - checkBox) / 2, 0, 0 };
                cb.right = cb.left + checkBox; cb.bottom = cb.top + checkBox;
                HBRUSH cbg = ::CreateSolidBrush(CHK_BG); ::FillRect(dc, &cb, cbg); ::DeleteObject(cbg);
                HPEN pen = ::CreatePen(PS_SOLID, 1, CHK_BORDER);
                HGDIOBJ opPen = ::SelectObject(dc, pen), opBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
                ::Rectangle(dc, cb.left, cb.top, cb.right, cb.bottom);
                ::SelectObject(dc, opBrush); ::SelectObject(dc, opPen); ::DeleteObject(pen);
                if (ListView_GetCheckState(h, item)) {
                    RECT mk{ cb.left + S(dc, 3), cb.top + S(dc, 3), cb.right - S(dc, 3), cb.bottom - S(dc, 3) };
                    HBRUSH mkb = ::CreateSolidBrush(CHK_MARK); ::FillRect(dc, &mk, mkb); ::DeleteObject(mkb);
                }
                rText.left = cb.right + checkPad;
            }
            ::SetTextColor(dc, TEXT); ::SetBkColor(dc, bg);
            ::DrawTextW(dc, txt, -1, &rText, fmt);
            if (gridLines) {
                HGDIOBJ op = ::SelectObject(dc, gridPen);
                if (sub != 0) { ::MoveToEx(dc, rBounds.left, rBounds.top, nullptr); ::LineTo(dc, rBounds.left, rBounds.bottom - 1); }
                ::MoveToEx(dc, rBounds.left, rBounds.bottom - 1, nullptr); ::LineTo(dc, rBounds.right, rBounds.bottom - 1);
                ::SelectObject(dc, op);
            }
        }
    }
    if (gridPen) ::DeleteObject(gridPen);
    if (of) ::SelectObject(dc, of);
}

// ---- trackbar / slider channel + thumb (port of CMPCThemeSliderCtrl::OnNMCustomdraw) ----
// Upstream's CWnd-derived CMPCThemeSliderCtrl reflects NM_CUSTOMDRAW back to itself via
// ON_NOTIFY_REFLECT (src/mpc-hc/CMPCThemeSliderCtrl.cpp:31). A raw HWND child has no equivalent
// auto-reflection, so this is called directly from the owning dialog's WM_NOTIFY instead (see
// LivePreview::PreviewDlgProc) -- the drawing logic itself, quoted below, is unaffected by where
// it runs from; only the trigger differs. Verified against upstream (CMPCThemeSliderCtrl.cpp:39-116):
//   if (AppIsThemeLoaded()) {
//       switch (pNMCD->dwDrawStage) {
//           case CDDS_PREPAINT: lr = CDRF_NOTIFYITEMDRAW; break;
//           case CDDS_ITEMPREPAINT:
//               if (pNMCD->dwItemSpec == TBCD_CHANNEL) { ... FillSolidRect(WindowBGColor); ... }
//               else if (pNMCD->dwItemSpec == TBCD_THUMB) { ... FillSolidRect(Scroll* per state); ... }
// gated there on AppIsThemeLoaded() (the player's "is a theme active" flag); here on IsDark(), since
// light mode never gets an owner-drawn look upstream either -- CMPCThemeUtil::fulfillThemeReqs's
// TRACKBAR_CLASS branch (CMPCThemeUtil.cpp:141-146) subclasses CMPCThemeSliderCtrl unconditionally,
// and with AppIsThemeLoaded() false every stage just falls through to CDRF_DODEFAULT (native).
//
// Also checked and NOT ported: the task brief expected CMPCThemeSliderCtrl::PreSubclassWindow to call
// SetWindowTheme(L"", L"") to stop DarkMode_Explorer fighting the custom draw. It doesn't --
// PreSubclassWindow (CMPCThemeSliderCtrl.cpp:18-26) only subclasses the tooltip; makeThemed()
// (CMPCThemeUtil.cpp:180-184) is a bare SubclassWindow with no theme call either; compare
// CMPCThemeUtil.cpp:92, which DOES call it for group boxes -- trackbars get no such call anywhere in
// the codebase. So this doesn't call SetWindowTheme on the trackbar either (see ApplyToChildren).
bool TrackbarCustomDraw(NMHDR* hdr, LRESULT* result) {
    if (!hdr || hdr->code != NM_CUSTOMDRAW || !IsDark()) return false;
    wchar_t cls[32] = {}; ::GetClassNameW(hdr->hwndFrom, cls, 32);
    if (_wcsicmp(cls, L"msctls_trackbar32") != 0) return false;

    HWND h = hdr->hwndFrom;
    LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)hdr;
    LRESULT lr = CDRF_DODEFAULT;

    switch (cd->dwDrawStage) {
        case CDDS_PREPAINT:
            lr = CDRF_NOTIFYITEMDRAW;
            break;
        case CDDS_ITEMPREPAINT:
            if (cd->dwItemSpec == TBCD_CHANNEL) {
                HDC dc = cd->hdc;
                RECT rc; ::GetClientRect(h, &rc);
                HBRUSH wb = ::CreateSolidBrush(WINDOW_BG); ::FillRect(dc, &rc, wb); ::DeleteObject(wb);

                RECT channelRect{}; ::SendMessageW(h, TBM_GETCHANNELRECT, 0, (LPARAM)&channelRect);
                RECT thumbRect{};   ::SendMessageW(h, TBM_GETTHUMBRECT,   0, (LPARAM)&thumbRect);
                LONG style = (LONG)::GetWindowLongPtr(h, GWL_STYLE);

                RECT r;
                if (style & TBS_VERT) {   // GetChannelRect returns 90deg-rotated dims for a vertical slider
                    RECT rot{ channelRect.top, channelRect.left, channelRect.bottom, channelRect.right };
                    if (rot.left > rot.right)  std::swap(rot.left, rot.right);      // CRect::NormalizeRect
                    if (rot.top  > rot.bottom) std::swap(rot.top,  rot.bottom);
                    cd->rc = RECT{ thumbRect.left + 2, rot.top, thumbRect.right - 3, rot.bottom - 2 };
                    r = cd->rc; ::InflateRect(&r, -6, 0);                           // CRect::DeflateRect(6,0,6,0)
                } else {
                    cd->rc = RECT{ channelRect.left, thumbRect.top + 2, channelRect.right - 2, thumbRect.bottom - 3 };
                    r = cd->rc; ::InflateRect(&r, 0, -6);                           // CRect::DeflateRect(0,6,0,6)
                }

                HBRUSH cb = ::CreateSolidBrush(SLIDER_CHANNEL); ::FillRect(dc, &r, cb); ::DeleteObject(cb);
                HBRUSH bd = ::CreateSolidBrush(SLIDER_BORDER);  ::FrameRect(dc, &r, bd); ::DeleteObject(bd);
                lr = CDRF_SKIPDEFAULT;
            } else if (cd->dwItemSpec == TBCD_THUMB) {
                HDC dc = cd->hdc;
                cd->rc.bottom--;
                RECT r = cd->rc; r.right -= 1;                                     // CRect::DeflateRect(0,0,1,0)

                // m_bDrag is declared and read upstream (CMPCThemeSliderCtrl.h:15, .cpp:96) but never
                // set true anywhere in the class -- no OnLButtonDown override, no setter, and
                // OnLButtonUp only ever clears it. That branch is dead code in upstream today; there is
                // no "drag" state to reproduce, so it's simply never true here either.
                bool drag = false;
                bool hover = isHover(h);
                COLORREF fill = drag ? SLIDER_THUMB_DRAG : hover ? SLIDER_THUMB_HOVER : SLIDER_THUMB;
                HBRUSH fb = ::CreateSolidBrush(fill);          ::FillRect(dc, &r, fb); ::DeleteObject(fb);
                HBRUSH bd = ::CreateSolidBrush(SLIDER_BORDER); ::FrameRect(dc, &r, bd); ::DeleteObject(bd);
                lr = CDRF_SKIPDEFAULT;
            }
            break;
    }
    *result = lr;
    return true;
}

// One subclass proc handles every owner-drawn class; it dispatches on window class + button style.
static LRESULT CALLBACK ThemeProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_NCDESTROY) {
        ::RemoveWindowSubclass(h, ThemeProc, id);
        hoverMap().erase(h); downPosMap().erase(h); headerHotMap().erase(h);
        return ::DefSubclassProc(h, msg, w, l);
    }
    // LIGHT theme: buttons/groups, combos, spinners and tabs paint NATIVELY -- the player's
    // light-consistency overhaul gates ALL custom control painting on drawThemedControls, which is
    // true only in dark (upstream ef3e901c0 / 631b46f48 / 0dfe2f18a / 8233693e6 / 0b61498ea). Bail
    // before any custom erase/color/paint. Headers (4) and list views (7) keep the custom painter in
    // both modes: they are Studio's own review-list chrome, not part of the preview's player fidelity.
    if (!IsDark() && (id == 1 || id == 2 || id == 3 || id == 5) &&
        (msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_CTLCOLORLISTBOX))
        return ::DefSubclassProc(h, msg, w, l);
    if (msg == WM_CTLCOLORLISTBOX) { return (LRESULT)contentCtl((HDC)w); }   // a combo's open dropdown
    // id 8 (trackbar, see ApplyToChildren) is excluded from the two checks below: it isn't painted by
    // this subclass at all (Theme::TrackbarCustomDraw runs at the PARENT, off WM_NOTIFY) -- it's only
    // subclassed here for hover-rect tracking, so its own WM_ERASEBKGND/WM_PAINT must stay 100% native
    // (upstream's CMPCThemeSliderCtrl overrides neither), the same way the trackbar is untouched by
    // this file entirely in light mode.
    if (msg == WM_ERASEBKGND && id != 8) return 1;   // every OTHER branch fully repaints in WM_PAINT -> no erase flicker
    // The owner-drawn look depends on enabled/checked/focus state; the control class doesn't always
    // repaint the whole rect when those change (e.g. EnableWindow on a push button), so force it.
    if (msg == WM_ENABLE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
        msg == BM_SETCHECK || msg == BM_SETSTATE || msg == BM_SETSTYLE) {
        LRESULT r = ::DefSubclassProc(h, msg, w, l);
        ::InvalidateRect(h, nullptr, TRUE);
        return r;
    }
    // Hover tracking (CMPCThemeButton/RadioOrCheck/ComboBox::checkHover — no per-instance CWnd here,
    // so state lives in hoverMap()). Buttons/checks/radios hover over their whole client rect; a combo
    // with a real Edit child only hovers its dropdown-button cell (checkHover's special case).
    if ((id == 1 || id == 2) && (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE)) {
        bool inside = false;
        if (msg == WM_MOUSEMOVE) {
            POINT pt{ (short)LOWORD(l), (short)HIWORD(l) };
            if (id == 2) {
                COMBOBOXINFO info{ sizeof(info) };
                ::GetComboBoxInfo(h, &info);
                RECT zone; ::GetClientRect(h, &zone);
                if (info.hwndItem) zone = info.rcButton;
                inside = ::PtInRect(&zone, pt) != FALSE;
            } else {
                inside = true;
            }
            if (!isHover(h)) { TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; ::TrackMouseEvent(&tme); }
        }
        setHoverState(h, inside);
    }
    // Pressed-half tracking for the spinner (CMPCThemeSpinButtonCtrl::downPos).
    if (id == 3) {
        if (msg == WM_LBUTTONDOWN || (msg == WM_MOUSEMOVE && (w & MK_LBUTTON))) {
            downPosMap()[h] = POINT{ (short)LOWORD(l), (short)HIWORD(l) };
            ::InvalidateRect(h, nullptr, TRUE);
        } else if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) {
            downPosMap()[h] = POINT{ -1, -1 };
            ::InvalidateRect(h, nullptr, TRUE);
        }
    }
    // Hot column tracking for the header (CMPCThemeHeaderCtrl::checkHot).
    if (id == 4 && (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE)) {
        int hot = -2;
        if (msg == WM_MOUSEMOVE) {
            HDHITTESTINFO hti{}; hti.pt = POINT{ (short)LOWORD(l), (short)HIWORD(l) };
            hot = (int)::SendMessageW(h, HDM_HITTEST, 0, (LPARAM)&hti);
            if (!(hti.flags & HHT_ONHEADER)) hot = -2;
            if (!headerHotMap().count(h)) { TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; ::TrackMouseEvent(&tme); }
        }
        int& cur = headerHotMap()[h];
        if (cur != hot) { cur = hot; ::InvalidateRect(h, nullptr, TRUE); }
    }
    // Hover tracking for the trackbar THUMB (CMPCThemeSliderCtrl::checkHover, CMPCThemeSliderCtrl.cpp
    // ~L125-138: hover is true only over the thumb rect, not the whole control, unlike Button/Combo
    // above). The actual painting happens in Theme::TrackbarCustomDraw at the PARENT's WM_NOTIFY; this
    // subclass exists solely to keep hoverMap() current for it.
    if (id == 8 && (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE)) {
        bool inside = false;
        if (msg == WM_MOUSEMOVE) {
            POINT pt{ (short)LOWORD(l), (short)HIWORD(l) };
            RECT thumb{}; ::SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
            inside = ::PtInRect(&thumb, pt) != FALSE;
            if (!isHover(h)) { TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; ::TrackMouseEvent(&tme); }
        }
        setHoverState(h, inside);
    }
    if (msg == WM_PAINT && id != 8) {   // id 8: native WM_PAINT drives the NM_CUSTOMDRAW this file paints from
        PAINTSTRUCT ps; HDC dc = ::BeginPaint(h, &ps);
        switch (id) {
            case 1: {   // BUTTON — dispatch on BS_ style
                DWORD bt = (DWORD)(::GetWindowLongPtr(h, GWL_STYLE) & BS_TYPEMASK);
                if (bt == BS_GROUPBOX) draw_group(h, dc);
                else if (bt == BS_RADIOBUTTON || bt == BS_AUTORADIOBUTTON) draw_check(h, dc, true);
                else if (bt == BS_CHECKBOX || bt == BS_AUTOCHECKBOX || bt == BS_3STATE || bt == BS_AUTO3STATE)
                    draw_check(h, dc, false);
                else draw_button(h, dc);
                break;
            }
            case 2: draw_combo(h, dc);  break;
            case 3: draw_updown(h, dc); break;
            case 4: draw_header(h, dc); break;
            case 5: draw_tabs(h, dc);   break;
            case 7: draw_list(h, dc);   break;
        }
        ::EndPaint(h, &ps);
        return 0;
    }
    return ::DefSubclassProc(h, msg, w, l);
}

void ApplyToChildren(HWND parent) {
    // Dark uses the DarkMode common-control theme (dark scrollbars/selection); Light uses the normal
    // Explorer theme. The owner-draw subclasses read the live palette, so a switch just repaints.
    ::EnumChildWindows(parent, [](HWND c, LPARAM) -> BOOL {
        const wchar_t* ccTheme = IsDark() ? L"DarkMode_Explorer" : L"Explorer";
        wchar_t cls[40]; ::GetClassNameW(c, cls, 40);
        if      (_wcsicmp(cls, L"Button") == 0)          ::SetWindowSubclass(c, ThemeProc, 1, 0);
        else if (_wcsicmp(cls, L"ComboBox") == 0) {
            ::SetWindowSubclass(c, ThemeProc, 2, 0);
            // The drop-down list is a popup OWNED by the combo, not a child of `parent`, so
            // EnumChildWindows never reaches it -- theme it directly or its scrollbar stays light
            // against the (already dark) list. Same trap as the preview host's own scrollbars.
            COMBOBOXINFO cbi{ sizeof(cbi) };
            if (::GetComboBoxInfo(c, &cbi) && cbi.hwndList) ::SetWindowTheme(cbi.hwndList, ccTheme, nullptr);
        }
        else if (_wcsicmp(cls, L"msctls_updown32") == 0) ::SetWindowSubclass(c, ThemeProc, 3, 0);
        else if (_wcsicmp(cls, L"SysHeader32") == 0)     ::SetWindowSubclass(c, ThemeProc, 4, 0);
        else if (_wcsicmp(cls, L"SysTabControl32") == 0) ::SetWindowSubclass(c, ThemeProc, 5, 0);
        else if (_wcsicmp(cls, L"SysListView32") == 0) {
            // Scrollbars stay native/dark-themed (SetWindowTheme on the control colors the NC scrollbar
            // chrome); the report-mode row/header content is fully owner-painted by draw_list (id 7)
            // instead of relying on DarkMode_Explorer's own (mismatched) row padding.
            ::SetWindowTheme(c, ccTheme, nullptr);
            ListView_SetBkColor(c, CONTENT_BG); ListView_SetTextBkColor(c, CLR_NONE); ListView_SetTextColor(c, TEXT);
            ::SetWindowSubclass(c, ThemeProc, 7, 0);
        } else if (_wcsicmp(cls, L"SysTreeView32") == 0) {
            ::SetWindowTheme(c, ccTheme, nullptr);
            TreeView_SetBkColor(c, CONTENT_BG); TreeView_SetTextColor(c, TEXT);
        } else if (_wcsicmp(cls, L"Edit") == 0) {
            ::SetWindowTheme(c, ccTheme, nullptr);   // scrollbars follow the theme; colors via WM_CTLCOLOR
            LONG_PTR st = ::GetWindowLongPtr(c, GWL_STYLE), ex = ::GetWindowLongPtr(c, GWL_EXSTYLE);
            if (IsDark()) {
                // Swap the 2px sunken client edge for a flat 1px border, then paint it EditBorderColor —
                // MPC-HC's flat field look (a 1px FrameRect can't fully recolor a 2px client edge).
                // Remember the template chrome (window prop, integer payload) so a later light pass can
                // restore it: upstream CMPCThemeEdit is 100% native in light (ef724f668).
                if (!::GetPropW(c, L"StudioEditOrig"))
                    ::SetPropW(c, L"StudioEditOrig",
                               (HANDLE)(LONG_PTR)(4 | ((st & WS_BORDER) ? 2 : 0) | ((ex & WS_EX_CLIENTEDGE) ? 1 : 0)));
                ::SetWindowLongPtr(c, GWL_STYLE, st | WS_BORDER);
                ::SetWindowLongPtr(c, GWL_EXSTYLE, ex & ~WS_EX_CLIENTEDGE);
                ::SetWindowPos(c, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                RECT er; ::GetClientRect(c, &er); ::InflateRect(&er, -2, -2);
                ::SendMessageW(c, EM_SETRECT, 0, (LPARAM)&er);   // CMPCThemeEdit::PreSubclassWindow's 2px text inset
                ::SetWindowSubclass(c, EditProc, 6, 0);  // redraw the border in EditBorderColor
            } else {
                // LIGHT: fully native edit chrome. Undo a previous dark restyle (main-UI edits persist
                // across theme switches; preview-dialog edits are recreated and never carry the prop).
                if (LONG_PTR bits = (LONG_PTR)::GetPropW(c, L"StudioEditOrig")) {
                    ::SetWindowLongPtr(c, GWL_STYLE, (bits & 2) ? (st | WS_BORDER) : (st & ~WS_BORDER));
                    ::SetWindowLongPtr(c, GWL_EXSTYLE, (bits & 1) ? (ex | WS_EX_CLIENTEDGE) : (ex & ~WS_EX_CLIENTEDGE));
                    ::SetWindowPos(c, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                    ::SendMessageW(c, EM_SETRECT, 0, 0);   // NULL rect: back to the default formatting rect
                    ::RemovePropW(c, L"StudioEditOrig");
                }
                ::RemoveWindowSubclass(c, EditProc, 6);    // no flat-border repaint in light
            }
        } else if (_wcsicmp(cls, L"ListBox") == 0) {
            ::SetWindowTheme(c, ccTheme, nullptr);
        } else if (_wcsicmp(cls, L"msctls_trackbar32") == 0 && IsDark()) {
            // Light: leave the trackbar completely untouched (no subclass at all) -- upstream gives it
            // no light-mode owner-draw path either, and native trackbars need their own WM_PAINT/
            // WM_ERASEBKGND unmolested. Dark: subclass only for hover-rect tracking (see ThemeProc id 8
            // above); the actual paint is Theme::TrackbarCustomDraw, off the PARENT dialog's WM_NOTIFY.
            ::SetWindowSubclass(c, ThemeProc, 8, 0);
        }
        return TRUE;
    }, 0);
}

// ---- native menu theming (port of CMPCThemeMenu: owner-draw items + dark background) ----
namespace {
struct MenuItemData { CStringW caption; bool isMenubar = false, isSeparator = false, isFirst = false, isRadioCheck = false; };
std::vector<std::unique_ptr<MenuItemData>> g_menuItems;   // owns the dwItemData blobs (app lifetime)
HFONT g_menuFont = nullptr;
// CMPCThemeMenu::symbolFont/bulletFont/checkFont: the submenu arrow, checkmark, and radio-check
// bullet are drawn as text glyphs in "MS UI Gothic" (CMPCTheme::uiSymbolFont), not vector shapes.
HFONT g_menuSymbolFont = nullptr, g_menuBulletFont = nullptr, g_menuCheckFont = nullptr;
struct { int iconSpace, iconPad, rowPad, subPad, sepPad, sepH, postText, accelSpace, textH; } g_md{};
bool g_menuInit = false;

void initMenu() {
    if (g_menuInit) return; g_menuInit = true;
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_menuFont = ::CreateFontIndirectW(&ncm.lfMenuFont);
    HDC dc = ::GetDC(nullptr);
    int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    auto SC = [&](int v) { return ::MulDiv(v, dpi, 96); };
    g_md = { SC(22), SC(10), SC(4) + 3, SC(20), SC(8), SC(7), SC(20), SC(30), 0 };
    HGDIOBJ of = ::SelectObject(dc, g_menuFont);
    SIZE sz{}; ::GetTextExtentPoint32W(dc, L"W", 1, &sz); g_md.textH = sz.cy;
    ::SelectObject(dc, of); ::ReleaseDC(nullptr, dc);

    // CMPCThemeUtil::getFontByFace(..., uiSymbolFont, size, weight): lfHeight = -MulDiv(size, dpi, 72).
    auto makeSymbolFont = [&](int pointSize, LONG weight) {
        LOGFONTW lf{}; lf.lfHeight = -::MulDiv(pointSize, dpi, 72); lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET; lf.lfQuality = CLEARTYPE_QUALITY;
        wcsncpy_s(lf.lfFaceName, L"MS UI Gothic", LF_FACESIZE);
        return ::CreateFontIndirectW(&lf);
    };
    g_menuSymbolFont = makeSymbolFont(14, FW_BOLD);
    g_menuBulletFont = makeSymbolFont(6, FW_REGULAR);
    g_menuCheckFont  = makeSymbolFont(10, FW_REGULAR);
}
void splitAccel(const CStringW& cap, CStringW& left, CStringW& right) {
    int t = cap.Find(L'\t');
    if (t >= 0) { left = cap.Left(t); right = cap.Mid(t + 1); } else { left = cap; right = L""; }
}
SIZE textSize(HDC dc, const CStringW& s) {
    SIZE sz{}; ::GetTextExtentPoint32W(dc, s, s.GetLength(), &sz); return sz;
}
}

void ThemeMenu(HMENU menu, bool isMenubar) {
    initMenu();
    MENUINFO mi{ sizeof(mi) };
    // CMPCThemeMenu::fulfillThemeReqs: the top menu bar uses MenubarBGColor, every popup/submenu uses
    // MenuBGColor — the two coincide in dark mode but differ in light (white bar vs. light-gray popup).
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS; mi.hbrBack = solidCached(isMenubar ? MENUBAR_BG : MENU_BG);
    ::SetMenuInfo(menu, &mi);
    int n = ::GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW ti{ sizeof(ti) }; ti.fMask = MIIM_FTYPE | MIIM_DATA;
        ::GetMenuItemInfoW(menu, i, TRUE, &ti);
        if (!ti.dwItemData) {                                     // not yet themed
            auto* obj = new MenuItemData();
            obj->isMenubar = isMenubar; obj->isFirst = (i == 0);
            obj->isSeparator = (ti.fType & MFT_SEPARATOR) != 0;
            obj->isRadioCheck = (ti.fType & MFT_RADIOCHECK) != 0;
            wchar_t buf[256] = L""; ::GetMenuStringW(menu, i, buf, 256, MF_BYPOSITION);
            obj->caption = buf;
            g_menuItems.emplace_back(obj);
            MENUITEMINFOW mo{ sizeof(mo) }; mo.fMask = MIIM_FTYPE | MIIM_DATA;
            mo.fType = MFT_OWNERDRAW | ti.fType; mo.dwItemData = (ULONG_PTR)obj;
            ::SetMenuItemInfoW(menu, i, TRUE, &mo);
        }
        if (HMENU sub = ::GetSubMenu(menu, i)) ThemeMenu(sub, false);
    }
}

bool MenuMeasureItem(MEASUREITEMSTRUCT* mis) {
    if (!mis || mis->CtlType != ODT_MENU) return false;
    auto* mo = (MenuItemData*)mis->itemData; if (!mo) return false;
    initMenu();
    if (mo->isSeparator) { mis->itemWidth = 0; mis->itemHeight = g_md.sepH; return true; }
    HDC dc = ::GetDC(nullptr); HGDIOBJ of = ::SelectObject(dc, g_menuFont);
    if (mo->isMenubar) {
        mis->itemWidth = textSize(dc, mo->caption).cx;
        mis->itemHeight = g_md.textH + g_md.rowPad;
    } else {
        CStringW left, right; splitAccel(mo->caption, left, right);
        mis->itemHeight = g_md.textH + g_md.rowPad;
        mis->itemWidth = g_md.iconSpace + g_md.postText + g_md.subPad + textSize(dc, left).cx;
        if (right.GetLength() > 0) mis->itemWidth += g_md.accelSpace + textSize(dc, right).cx;
    }
    ::SelectObject(dc, of); ::ReleaseDC(nullptr, dc);
    return true;
}

bool MenuDrawItem(DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_MENU) return false;
    auto* mo = (MenuItemData*)dis->itemData; if (!mo) return false;
    initMenu();
    HDC dc = dis->hDC;
    HMENU menu = (HMENU)dis->hwndItem;                 // for ODT_MENU, hwndItem is the HMENU
    bool hasSub = false, checked = (dis->itemState & ODS_CHECKED) != 0;
    for (int a = 0, cnt = ::GetMenuItemCount(menu); a < cnt; ++a) {   // find this item by its data ptr
        MENUITEMINFOW q{ sizeof(q) }; q.fMask = MIIM_DATA | MIIM_SUBMENU;
        ::GetMenuItemInfoW(menu, a, TRUE, &q);
        if (q.dwItemData == (ULONG_PTR)mo) { hasSub = (q.hSubMenu != nullptr); break; }
    }
    CRect rcFull(dis->rcItem), rcM(rcFull);
    CRect rcIcon(rcM.left, rcM.top, rcM.left + g_md.iconSpace, rcM.bottom);
    CRect rcText(rcM.left + g_md.iconSpace + g_md.iconPad, rcM.top, rcM.right - g_md.subPad, rcM.bottom);
    CRect rcArrow(rcM.right - g_md.subPad, rcM.top, rcM.right, rcM.bottom);
    UINT align = DT_LEFT;
    COLORREF fg = (dis->itemState & ODS_DISABLED) ? MENU_DISABLED : TEXT;
    COLORREF arrowClr = (dis->itemState & ODS_DISABLED) ? MENU_DISABLED : MENU_ARROW;
    // CMPCThemeMenu::DrawItem: the bar uses Menubar* colors, popups use Menu* — differ in light mode.
    COLORREF bgColor = mo->isMenubar ? MENUBAR_BG : MENU_BG;
    COLORREF selColor = mo->isMenubar ? MENUBAR_SEL : MENU_SEL;

    ::SetBkMode(dc, TRANSPARENT);
    { HBRUSH b = ::CreateSolidBrush(bgColor); ::FillRect(dc, &rcM, b); ::DeleteObject(b); }
    if (mo->isMenubar) { rcM = rcFull; rcText = rcFull; align = DT_CENTER; }
    // (the light 1px line Windows paints under the whole menu bar is covered in the frame's WM_NCPAINT)
    if (mo->isSeparator) {
        int off = (g_md.sepH - 1) / 2;
        CRect s(rcM.left + g_md.sepPad, rcM.top + off, rcM.right - g_md.sepPad, rcM.top + off + 1);
        HBRUSH b = ::CreateSolidBrush(MENU_SEP); ::FillRect(dc, &s, b); ::DeleteObject(b);
    } else {
        HGDIOBJ of = ::SelectObject(dc, g_menuFont);
        if ((dis->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) && (dis->itemAction & (ODA_SELECT | ODA_DRAWENTIRE))) {
            HBRUSH b = ::CreateSolidBrush(selColor); ::FillRect(dc, &rcM, b); ::DeleteObject(b);
        }
        CStringW left, right; splitAccel(mo->caption, left, right);
        UINT acc = (dis->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0;
        ::SetTextColor(dc, fg);
        ::DrawTextW(dc, left, left.GetLength(), &rcText, DT_VCENTER | align | DT_SINGLELINE | acc);
        if (!mo->isMenubar) {
            if (right.GetLength() > 0)
                ::DrawTextW(dc, right, right.GetLength(), &rcText, DT_VCENTER | DT_RIGHT | DT_SINGLELINE | acc);
            if (hasSub) {   // ">" glyph in the symbol font (CMPCThemeMenu::DrawItem), not a drawn triangle
                HGDIOBJ osf = ::SelectObject(dc, g_menuSymbolFont);
                ::SetTextColor(dc, arrowClr);
                ::DrawTextW(dc, L">", 1, &rcArrow, DT_VCENTER | DT_CENTER | DT_SINGLELINE);
                ::SelectObject(dc, osf);
            }
            if (checked) {   // checkmark (u2714) or, for MFT_RADIOCHECK items, a bullet (u25CF) -- each its own symbol font size
                HGDIOBJ osf = ::SelectObject(dc, mo->isRadioCheck ? g_menuBulletFont : g_menuCheckFont);
                ::SetTextColor(dc, fg);
                const wchar_t* glyph = mo->isRadioCheck ? L"\u25CF" : L"\u2714";   // bullet / checkmark
                ::DrawTextW(dc, glyph, 1, &rcIcon, DT_VCENTER | DT_CENTER | DT_SINGLELINE);
                ::SelectObject(dc, osf);
            }
        }
        ::SelectObject(dc, of);
    }
    ::ExcludeClipRect(dc, rcFull.left, rcFull.top, rcFull.right, rcFull.bottom);
    return true;
}

} // namespace Theme
