// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "LivePreview.h"
#include "Theme.h"
#include <commctrl.h>
#include <windowsx.h>   // GET_X_LPARAM
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

using namespace mpctrans;

// The neutral DLL is a resource-only DLL built from upstream English mpc-hc.rc (bundle-build/neutral-dll).
bool LivePreview::LoadNeutralDll(const CString& dllPath) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        ICC_WIN95_CLASSES | ICC_LINK_CLASS | ICC_STANDARD_CLASSES | ICC_DATE_CLASSES |
        ICC_USEREX_CLASSES | ICC_COOL_CLASSES | ICC_INTERNET_CLASSES };
    InitCommonControlsEx(&icc);        // SysLink needs ICC_LINK_CLASS (not in ICC_WIN95_CLASSES)

    // Stub for the one custom class in upstream templates (CreateDialogIndirect fails outright
    // on any unregistered class). Renders as a plain themed box — enough for preview.
    WNDCLASSW wc{};
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = AfxGetInstanceHandle();
    wc.lpszClassName = L"MfcMaskedEdit";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = ::LoadCursor(nullptr, IDC_IBEAM);
    ::RegisterClassW(&wc);             // already-registered is fine

    m_neutral = ::LoadLibraryEx(dllPath, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    return m_neutral != nullptr;
}
void LivePreview::Unload() {
    DestroyPreview();
    if (m_dlgFont) { ::DeleteObject(m_dlgFont); m_dlgFont = nullptr; }
    if (m_neutral) { ::FreeLibrary(m_neutral); m_neutral = nullptr; }
}
void LivePreview::DestroyPreview() {
    if (m_frame) { ::DestroyWindow(m_frame); m_frame = nullptr; }
    if (m_tip) { ::DestroyWindow(m_tip); m_tip = nullptr; }
    if (m_combo) { ::DestroyWindow(m_combo); m_combo = nullptr; }
    if (m_dlg) { ::DestroyWindow(m_dlg); m_dlg = nullptr; }
    m_highlight = nullptr;
    m_english.clear();
}

// A themed tooltip bubble: a top-level popup (owned by the frame, so it floats above the dialog's
// controls) that paints its window text on a dark rounded field, like MPC-HC's tooltips.
static LRESULT CALLBACK TooltipProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT || m == WM_PRINTCLIENT || m == WM_PRINT) {
        PAINTSTRUCT ps; HDC dc = (m == WM_PAINT) ? ::BeginPaint(h, &ps) : (HDC)w;
        RECT rc; ::GetClientRect(h, &rc);
        int dpi = ::GetDeviceCaps(dc, LOGPIXELSY), pad = ::MulDiv(6, dpi, 96);
        HBRUSH bg = ::CreateSolidBrush(Theme::MENU_BG);   ::FillRect(dc, &rc, bg); ::DeleteObject(bg);
        HBRUSH bd = ::CreateSolidBrush(Theme::CTRL_BORDER); ::FrameRect(dc, &rc, bd); ::DeleteObject(bd);
        NONCLIENTMETRICSW ncm{ sizeof(ncm) }; ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HFONT f = ::CreateFontIndirectW(&ncm.lfMessageFont); HGDIOBJ of = ::SelectObject(dc, f);
        wchar_t buf[512] = {}; int n = ::GetWindowTextW(h, buf, 512);
        ::SetBkMode(dc, TRANSPARENT); ::SetTextColor(dc, Theme::TEXT);
        RECT tr = rc; ::InflateRect(&tr, -pad, -pad);
        ::DrawTextW(dc, buf, n, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        ::SelectObject(dc, of); ::DeleteObject(f);
        if (m == WM_PAINT) ::EndPaint(h, &ps);
        return 0;
    }
    return ::DefWindowProc(h, m, w, l);
}
void LivePreview::HideTooltip() { if (m_tip) ::ShowWindow(m_tip, SW_HIDE); }
void LivePreview::ShowTooltip(const std::string& english, const CString& text) {
    HWND ctl = nullptr;
    if (!english.empty() && !text.IsEmpty())
        for (const auto& [hwnd, en] : m_english) if (en == english) { ctl = hwnd; break; }
    tipBelow(ctl, text);
}
// Float the tooltip bubble just below `ctl`, wrapping `text`. Null control / empty text hides it.
void LivePreview::tipBelow(HWND ctl, const CString& text) {
    if (!ctl || !m_dlg || !::IsWindow(ctl) || text.IsEmpty()) { HideTooltip(); return; }
    HDC hdc = ::GetDC(m_dlg); int dpi = ::GetDeviceCaps(hdc, LOGPIXELSY); ::ReleaseDC(m_dlg, hdc);
    RECT cr; ::GetWindowRect(ctl, &cr);            // hover just below the control's left edge
    tipAt(cr.left + ::MulDiv(12, dpi, 96), cr.bottom + ::MulDiv(2, dpi, 96), text);
}
// Create/position the tooltip bubble at screen (x, y), wrapping `text`. `above` anchors its bottom at y.
void LivePreview::tipAt(int x, int y, const CString& text, bool above) {
    if (!m_dlg || text.IsEmpty()) { HideTooltip(); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = TooltipProc; wc.hInstance = AfxGetInstanceHandle();
        wc.lpszClassName = L"MpcTransTooltip"; wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        ::RegisterClassW(&wc); reg = true;
    }
    if (!m_tip)
        m_tip = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MpcTransTooltip", L"",
                                  WS_POPUP, 0, 0, 0, 0, ::GetAncestor(m_dlg, GA_ROOT), nullptr,
                                  AfxGetInstanceHandle(), nullptr);
    ::SetWindowTextW(m_tip, text);
    HDC dc = ::GetDC(m_tip);
    int dpi = ::GetDeviceCaps(dc, LOGPIXELSY), pad = ::MulDiv(6, dpi, 96);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) }; ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT f = ::CreateFontIndirectW(&ncm.lfMessageFont); HGDIOBJ of = ::SelectObject(dc, f);
    RECT mr{ 0, 0, ::MulDiv(340, dpi, 96), 0 };
    ::DrawTextW(dc, text, -1, &mr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    ::SelectObject(dc, of); ::DeleteObject(f); ::ReleaseDC(m_tip, dc);
    int w = mr.right + pad * 2, h = mr.bottom + pad * 2;
    ::SetWindowPos(m_tip, HWND_TOP, x, above ? y - h : y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(m_tip, nullptr, TRUE); ::UpdateWindow(m_tip);
}

// The red ring is its own top-of-Z child window, not painting on the dialog: a group box (or any
// child) covering the control would clip a frame drawn on the dialog's DC (WS_CLIPCHILDREN). Its
// window region is a 1px ring so only the border is opaque; the center shows the control through.
static void fillRed(HDC dc, HWND h) {
    RECT rc; ::GetClientRect(h, &rc);
    HBRUSH b = ::CreateSolidBrush(RGB(230, 40, 40)); ::FillRect(dc, &rc, b); ::DeleteObject(b);
}
static LRESULT CALLBACK HighlightWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;   // clicks fall through to the control underneath
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PRINTCLIENT || m == WM_PRINT) { fillRed((HDC)w, h); return 0; }
    if (m == WM_PAINT) { PAINTSTRUCT ps; HDC dc = ::BeginPaint(h, &ps); fillRed(dc, h); ::EndPaint(h, &ps); return 0; }
    return ::DefWindowProc(h, m, w, l);
}
// Ring the control matching `english` with a 1px red locator frame; empty/no-match hides it.
void LivePreview::HighlightControl(const std::string& english) {
    HWND next = nullptr;
    if (!english.empty())
        for (const auto& [hwnd, en] : m_english)
            if (en == english) { next = hwnd; break; }
    m_highlight = next;
    ringFrame(next);
}
// Ring the SCREEN rect `rc` with the 1px red locator frame; nullptr hides it. The reusable primitive
// behind both HighlightControl (dialog controls, via ringFrame) and free-form callers like the
// command-help usage list, whose selected entry isn't a dialog control at all.
void LivePreview::RingScreenRect(const RECT* rc) {
    if (!rc) { if (m_frame) ::ShowWindow(m_frame, SW_HIDE); return; }

    static bool clsReg = false;
    if (!clsReg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = HighlightWndProc; wc.hInstance = AfxGetInstanceHandle();
        wc.lpszClassName = L"MpcTransHighlightFrame"; wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        ::RegisterClassW(&wc); clsReg = true;
    }
    // A top-level popup owned by the APP's main window (not the preview dialog, which may not exist —
    // e.g. it's destroyed outright while the command-help usage list is shown). Floats above everything
    // in the app; positioned in SCREEN coordinates.
    if (!m_frame) {
        HWND owner = ::AfxGetMainWnd() ? ::AfxGetMainWnd()->GetSafeHwnd() : nullptr;
        m_frame = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MpcTransHighlightFrame", L"",
                                    WS_POPUP, 0, 0, 0, 0, owner, nullptr, AfxGetInstanceHandle(), nullptr);
    }
    RECT r = *rc;
    const int w = r.right - r.left, h = r.bottom - r.top, t = 1;   // t = 1px border
    HRGN outer = ::CreateRectRgn(0, 0, w, h), inner = ::CreateRectRgn(t, t, w - t, h - t);
    ::CombineRgn(outer, outer, inner, RGN_DIFF); ::DeleteObject(inner);
    ::SetWindowRgn(m_frame, outer, TRUE);   // window takes ownership of the ring region
    ::SetWindowPos(m_frame, HWND_TOP, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(m_frame, nullptr, TRUE);
    ::UpdateWindow(m_frame);
}
// (Re)position the hollow red ring around `next`; nullptr / dead window hides it.
void LivePreview::ringFrame(HWND next) {
    if (!next || !::IsWindow(next)) { RingScreenRect(nullptr); return; }
    RECT r; ::GetWindowRect(next, &r);        // screen coords (popup is top-level)
    ::InflateRect(&r, 2, 2);
    RingScreenRect(&r);
}

// An expanded combo dropdown: a top-level popup (owned by the frame, so it floats above the dialog)
// that owner-draws the combo's item list on the themed content surface, with the selected row painted
// in CONTENT_SEL -- reproducing how MPC-HC's open combo looks. Items/selection come from LivePreview.
void LivePreview::PaintComboList(HDC dc, HWND h) {
    RECT rc; ::GetClientRect(h, &rc);
    int dpi = ::GetDeviceCaps(dc, LOGPIXELSY), padX = ::MulDiv(6, dpi, 96), padY = ::MulDiv(3, dpi, 96);
    HBRUSH bg = ::CreateSolidBrush(Theme::CONTENT_BG); ::FillRect(dc, &rc, bg); ::DeleteObject(bg);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) }; ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT f = ::CreateFontIndirectW(&ncm.lfMessageFont); HGDIOBJ of = ::SelectObject(dc, f);
    ::SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW tm{}; ::GetTextMetricsW(dc, &tm);
    int rowH = tm.tmHeight + padY * 2;
    for (int i = 0; i < (int)m_comboItems.size(); ++i) {
        RECT row{ rc.left, rc.top + i * rowH, rc.right, rc.top + (i + 1) * rowH };
        if (i == m_comboSel) {
            HBRUSH sb = ::CreateSolidBrush(Theme::CONTENT_SEL); ::FillRect(dc, &row, sb); ::DeleteObject(sb);
        }
        ::SetTextColor(dc, Theme::TEXT);
        RECT tr = row; tr.left += padX; tr.right -= padX;
        ::DrawTextW(dc, m_comboItems[i], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    ::SelectObject(dc, of); ::DeleteObject(f);
    HBRUSH bd = ::CreateSolidBrush(Theme::CTRL_BORDER); ::FrameRect(dc, &rc, bd); ::DeleteObject(bd);
}
static LRESULT CALLBACK ComboListProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT || m == WM_PRINTCLIENT || m == WM_PRINT) {
        auto* self = (LivePreview*)::GetWindowLongPtrW(h, GWLP_USERDATA);
        PAINTSTRUCT ps; HDC dc = (m == WM_PAINT) ? ::BeginPaint(h, &ps) : (HDC)w;
        if (self) self->PaintComboList(dc, h);
        if (m == WM_PAINT) ::EndPaint(h, &ps);
        return 0;
    }
    return ::DefWindowProc(h, m, w, l);
}
void LivePreview::HideComboDropdown() { if (m_combo) ::ShowWindow(m_combo, SW_HIDE); }
void LivePreview::ShowComboDropdown(long long comboCtrlId, const std::vector<CString>& items, int selIndex) {
    HWND ctl = m_dlg ? ::GetDlgItem(m_dlg, (int)comboCtrlId) : nullptr;
    if (!ctl || !::IsWindow(ctl) || items.empty()) { HideComboDropdown(); return; }
    m_comboItems = items; m_comboSel = selIndex;
    ringFrame(nullptr); m_highlight = nullptr;   // the highlighted dropdown item is the locator; no red ring

    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = ComboListProc; wc.hInstance = AfxGetInstanceHandle();
        wc.lpszClassName = L"MpcTransComboList"; wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        ::RegisterClassW(&wc); reg = true;
    }
    if (!m_combo)
        m_combo = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MpcTransComboList", L"",
                                    WS_POPUP, 0, 0, 0, 0, ::GetAncestor(m_dlg, GA_ROOT), nullptr,
                                    AfxGetInstanceHandle(), nullptr);
    ::SetWindowLongPtrW(m_combo, GWLP_USERDATA, (LONG_PTR)this);

    HDC dc = ::GetDC(m_combo);
    int dpi = ::GetDeviceCaps(dc, LOGPIXELSY), padX = ::MulDiv(6, dpi, 96), padY = ::MulDiv(3, dpi, 96);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) }; ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT f = ::CreateFontIndirectW(&ncm.lfMessageFont); HGDIOBJ of = ::SelectObject(dc, f);
    TEXTMETRICW tm{}; ::GetTextMetricsW(dc, &tm);
    int rowH = tm.tmHeight + padY * 2, textW = 0;
    for (const auto& it : items) { SIZE sz{}; ::GetTextExtentPoint32W(dc, it, it.GetLength(), &sz); textW = max(textW, (int)sz.cx); }
    ::SelectObject(dc, of); ::DeleteObject(f); ::ReleaseDC(m_combo, dc);

    RECT cr; ::GetWindowRect(ctl, &cr);            // drop the list directly under the combo, left-aligned
    int w = max((int)(cr.right - cr.left), textW + padX * 2 + 2);
    int htot = rowH * (int)items.size() + 2;       // +2 for the 1px frame top/bottom
    ::SetWindowPos(m_combo, HWND_TOP, cr.left, cr.bottom, w, htot, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(m_combo, nullptr, TRUE); ::UpdateWindow(m_combo);
}

// Fill the real (themed) report-view list with the rows, so a runtime-built list renders through the
// actual control -- exact row height/theming from MPC-HC's control. The Name column takes most of the
// width; Value is short (as in the app).
void LivePreview::PopulateReportList(long long listId, const CString& col1, const CString& col2,
                                     const std::vector<std::pair<CString, CString>>& rows, int selRow) {
    HWND list = m_dlg ? ::GetDlgItem(m_dlg, (int)listId) : nullptr;
    if (!list) return;
    ListView_DeleteAllItems(list);
    while (ListView_DeleteColumn(list, 0)) {}
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    RECT rc; ::GetClientRect(list, &rc);
    int w = rc.right - rc.left - ::GetSystemMetrics(SM_CXVSCROLL), c1w = w * 74 / 100;
    LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPWSTR)(LPCWSTR)col1; c.cx = c1w; c.iSubItem = 0; ListView_InsertColumn(list, 0, &c);
    c.pszText = (LPWSTR)(LPCWSTR)col2; c.cx = w - c1w; c.iSubItem = 1; ListView_InsertColumn(list, 1, &c);
    for (int i = 0; i < (int)rows.size(); ++i) {
        LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (LPWSTR)(LPCWSTR)rows[i].first;
        ListView_InsertItem(list, &it);
        ListView_SetItemText(list, i, 1, (LPWSTR)(LPCWSTR)rows[i].second);
    }
    if (selRow >= 0) {
        ListView_SetItemState(list, selRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        int per = ListView_GetCountPerPage(list);         // leave room below the selection for the bubble
        ListView_EnsureVisible(list, min((int)rows.size() - 1, selRow + per / 2), FALSE);
        ListView_EnsureVisible(list, selRow, FALSE);
    }
}
// Hover the description bubble by list row `item` — below it, or above when the row is near the list
// bottom (so it isn't clipped).
void LivePreview::ShowListItemTooltip(long long listId, int item, const CString& text) {
    HWND list = m_dlg ? ::GetDlgItem(m_dlg, (int)listId) : nullptr;
    if (!list || item < 0 || text.IsEmpty()) { HideTooltip(); return; }
    RECT r; if (!ListView_GetItemRect(list, item, &r, LVIR_BOUNDS)) { HideTooltip(); return; }
    RECT lr; ::GetClientRect(list, &lr);
    bool above = r.bottom > lr.top + (lr.bottom - lr.top) * 3 / 5;   // bottom 40% of the list -> flip up
    ::MapWindowPoints(list, nullptr, (POINT*)&r, 2);           // client -> screen
    HDC dc = ::GetDC(m_dlg); int dpi = ::GetDeviceCaps(dc, LOGPIXELSY); ::ReleaseDC(m_dlg, dc);
    tipAt(r.left + ::MulDiv(24, dpi, 96), above ? r.top - ::MulDiv(1, dpi, 96) : r.bottom + ::MulDiv(1, dpi, 96),
          text, above);
}
// Move an inline editor (combo) over row `item`'s value cell and show it -- the way the list edits a cell.
// Returns the editor's control id (for ShowComboDropdown), or -1.
long long LivePreview::PositionOverListValue(long long listId, long long ctrlId, int item) {
    HWND list = m_dlg ? ::GetDlgItem(m_dlg, (int)listId) : nullptr;
    HWND ctl = m_dlg ? ::GetDlgItem(m_dlg, (int)ctrlId) : nullptr;
    if (!list || !ctl || item < 0) return -1;
    RECT r; if (!ListView_GetSubItemRect(list, item, 1, LVIR_BOUNDS, &r)) return -1;
    ::MapWindowPoints(list, m_dlg, (POINT*)&r, 2);            // list-client -> dialog-client
    ::SetWindowPos(ctl, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW);
    return ctrlId;
}

// Owns click-to-edit: a control click surfaces as WM_PARENTNOTIFY; clicks on HTTRANSPARENT
// children (statics/groupboxes) surface as WM_LBUTTONDOWN on the dialog itself.
INT_PTR CALLBACK LivePreview::PreviewDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {   // MPC theme: background + text for the dialog and non-owner-drawn controls
        case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
            return (INT_PTR)Theme::windowCtl((HDC)wp);
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
            return (INT_PTR)Theme::contentCtl((HDC)wp);
    }
    auto* self = (LivePreview*)::GetWindowLongPtr(dlg, DWLP_USER);
    if (!self || !self->OnControlClicked) return FALSE;
    if (msg == WM_PARENTNOTIFY && LOWORD(wp) == WM_LBUTTONDOWN) {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HWND child = ::ChildWindowFromPointEx(dlg, pt, CWP_SKIPINVISIBLE);
        if (child && child != dlg) self->OnControlClicked(child);
        return TRUE;
    }
    if (msg == WM_LBUTTONDOWN) {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HWND child = ::ChildWindowFromPointEx(dlg, pt, CWP_SKIPINVISIBLE);
        if (child && child != dlg) self->OnControlClicked(child);
        return TRUE;
    }
    return FALSE;
}

static std::string read_all(const CString& path, bool binary) {
    std::ifstream f((const wchar_t*)path, binary ? std::ios::binary : std::ios::in);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

bool LivePreview::SetRcSourceBytes(const std::string& rcBytes, const std::string& resourceHText) {
    if (rcBytes.empty()) return false;
    m_rcDialogs = mpctrans::RcParser::parse(rcBytes, resourceHText);   // parser auto-detects UTF-16/UTF-8
    m_rcMenus = mpctrans::rc_parse_menus(rcBytes, resourceHText);
    m_useRc = !m_rcDialogs.empty();
    return m_useRc;
}
bool LivePreview::SetRcSource(const CString& rcPath, const CString& resourceHPath) {
    return SetRcSourceBytes(read_all(rcPath, /*binary=*/true), read_all(resourceHPath, /*binary=*/false));
}

// The label/control pairs MPC-HC repositions per property page (from each CPPage*::AdjustDynamicWidgets;
// symbols resolved to ids via the parsed RC). Keep in sync with upstream if pages change.
namespace {
struct WPair { const char* left; const char* right; };
struct DlgPairs { const char* idd; std::vector<WPair> pairs; };
const std::vector<DlgPairs>& widget_pairs() {
    static const std::vector<DlgPairs> t = {
        {"IDD_PPAGECAPTURE",  {{"IDC_STATIC1","IDC_COMBO1"},{"IDC_STATIC2","IDC_COMBO2"},{"IDC_STATIC3","IDC_COMBO9"}}},
        {"IDD_PPAGEMISC",     {{"IDC_STATIC5","IDC_EDIT1"}}},
        {"IDD_PPAGEPLAYBACK", {{"IDC_STATIC5","IDC_VOLUMESTEP"},{"IDC_STATIC6","IDC_SPEEDSTEP"},{"IDC_STATIC7","IDC_COMBO3"},
                               {"IDC_CHECK5","IDC_COMBO1"},{"IDC_STATIC4","IDC_COMBO4"},{"IDC_STATIC8","IDC_EDIT2"},{"IDC_STATIC9","IDC_EDIT3"}}},
        {"IDD_PPAGESUBMISC",  {{"IDC_STATIC1","IDC_EDIT2"},{"IDC_STATIC2","IDC_EDIT3"}}},
        {"IDD_PPAGESUBSTYLE", {{"IDC_STATIC1","IDC_EDIT3"},{"IDC_STATIC2","IDC_EDIT4"},{"IDC_STATIC3","IDC_EDIT5"},{"IDC_STATIC4","IDC_EDIT6"},
                               {"IDC_STATIC5","IDC_EDIT1"},{"IDC_STATIC6","IDC_EDIT2"},{"IDC_STATIC7","IDC_EDIT7"},{"IDC_STATIC8","IDC_EDIT8"},
                               {"IDC_STATIC9","IDC_EDIT9"},{"IDC_STATIC10","IDC_EDIT10"}}},
        {"IDD_PPAGESUBTITLES",{{"IDC_STATIC9","IDC_SUBPIC_TO_BUFFER"},{"IDC_STATIC21","IDC_COMBO1"},{"IDC_STATIC1","IDC_EDIT2"},{"IDC_STATIC3","IDC_EDIT3"}}},
        {"IDD_PPAGETHEME",    {{"IDC_STATIC22","IDC_MODERNSEEKBARHEIGHT"},{"IDC_STATIC2","IDC_COMBO1"},{"IDC_STATIC5","IDC_COMBO2"},
                               {"IDC_STATIC11","IDC_COMBO3"},{"IDC_STATIC7","IDC_EDIT4"},{"IDC_STATIC6","IDC_COMBO5"},{"IDC_STATIC23","IDC_COMBO6"}}},
        {"IDD_PPAGETOOLBAR",  {{"IDC_STATIC3","IDC_EDIT1"}}},
    };
    return t;
}

// Port of CMPCThemeUtil::AdjustDynamicWidgetPair: grow the left label to fit its (translated) text
// and slide the right control over so they don't overlap — exactly MPC-HC's runtime behavior.
void adjust_pair(HWND dlg, HWND lw, HWND rw, int dpi) {
    const int dynamicSpace = ::MulDiv(5, dpi, 96);
    LONG lstyle = (LONG)::GetWindowLongPtr(lw, GWL_STYLE);
    bool leftCheckbox = (::SendMessage(lw, WM_GETDLGCODE, 0, 0) & DLGC_BUTTON) &&
                        ((lstyle & BS_TYPEMASK) == BS_CHECKBOX || (lstyle & BS_TYPEMASK) == BS_AUTOCHECKBOX);
    wchar_t cls[64]; ::GetClassNameW(rw, cls, 64);
    bool rightCombo = _wcsicmp(cls, L"ComboBox") == 0;  (void)rightCombo;

    RECT l, r; ::GetWindowRect(lw, &l); ::GetWindowRect(rw, &r);
    ::MapWindowPoints(nullptr, dlg, (POINT*)&l, 2);
    ::MapWindowPoints(nullptr, dlg, (POINT*)&r, 2);
    RECT cl = l, cr = r;

    int left = l.left + (leftCheckbox ? ::GetSystemMetrics(SM_CXMENUCHECK) + 2 : 0);
    HDC dc = ::GetDC(lw);
    HFONT f = (HFONT)::SendMessage(lw, WM_GETFONT, 0, 0);
    HGDIOBJ oldF = f ? ::SelectObject(dc, f) : nullptr;
    TEXTMETRICW tm{}; ::GetTextMetricsW(dc, &tm);
    wchar_t txt[512] = {}; int n = ::GetWindowTextW(lw, txt, 512);
    SIZE sz{}; ::GetTextExtentPoint32W(dc, txt, n, &sz);
    if (oldF) ::SelectObject(dc, oldF);
    ::ReleaseDC(lw, dc);

    LONG leftWantsRight = left + sz.cx + tm.tmAveCharWidth;
    LONG rightWantsLeft = r.left;   // (combos: MPC widens to the dropdown content; empty here, so skip)
    bool rightAlignedText = !leftCheckbox && ((lstyle & SS_RIGHT) == SS_RIGHT);

    if (!(leftWantsRight > rightWantsLeft - dynamicSpace ||
          (leftWantsRight < l.right && rightWantsLeft > r.left) || rightAlignedText)) {
        l.right = leftWantsRight;
        r.left = std::min<LONG>(rightWantsLeft, std::max<LONG>(l.right + dynamicSpace, r.left));
    }
    l.top = r.top; l.bottom += (r.bottom - r.top) - (cl.bottom - cl.top);   // vertically center the label
    if (!leftCheckbox) ::SetWindowLongPtr(lw, GWL_STYLE, lstyle | SS_CENTERIMAGE);

    if (memcmp(&l, &cl, sizeof l)) ::MoveWindow(lw, l.left, l.top, l.right - l.left, l.bottom - l.top, TRUE);
    if (memcmp(&r, &cr, sizeof r)) ::MoveWindow(rw, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}
} // namespace

void LivePreview::ApplyDarkTheme(HWND dlg) {
    // Backgrounds + text come from the dialog's WM_CTLCOLOR* (in PreviewDlgProc); the owner-drawn
    // control classes (buttons/checks/radios/groups, combos, spinners) are handled by the shared
    // Theme module so the preview matches the rest of the app.
    Theme::ApplyToChildren(dlg);
}

void LivePreview::ApplyWidgetPairs(HWND dlg, long long dialogId) {
    // Resolve the pair symbols to control ids from the parsed RC for this dialog.
    const RcDialog* d = nullptr;
    for (const auto& x : m_rcDialogs) if (x.id == dialogId) { d = &x; break; }
    if (!d) return;
    const DlgPairs* dp = nullptr;
    for (const auto& e : widget_pairs()) if (e.idd == d->sym) { dp = &e; break; }
    if (!dp) return;
    std::unordered_map<std::string, long long> sym2id;
    for (const auto& c : d->controls) if (!c.sym.empty()) sym2id.emplace(c.sym, c.id);
    HDC dc = ::GetDC(dlg); int dpi = dc ? ::GetDeviceCaps(dc, LOGPIXELSX) : 96; if (dc) ::ReleaseDC(dlg, dc);
    for (const auto& p : dp->pairs) {
        auto li = sym2id.find(p.left), ri = sym2id.find(p.right);
        if (li == sym2id.end() || ri == sym2id.end()) continue;
        HWND lw = ::GetDlgItem(dlg, (int)li->second), rw = ::GetDlgItem(dlg, (int)ri->second);
        if (lw && rw) adjust_pair(dlg, lw, rw, dpi);
    }
}

// From a raw template a spin (up-down) control isn't buddied, so it sits at its designed spot with a
// gap. Buddy each up-down to the nearest edit on its left with UDS_ALIGNRIGHT, so Windows lays them
// out natively (up-down flush against the right edge, buddy shrunk) exactly as MPC-HC's dialogs do —
// then the shared drawing (draw_updown) frames the pair like CMPCThemeSpinButtonCtrl.
void LivePreview::IntegrateSpinners(HWND dlg) {
    struct Item { HWND h; RECT r; bool isUp; };
    std::vector<Item> items;
    ::EnumChildWindows(dlg, [](HWND c, LPARAM lp) -> BOOL {
        auto* v = (std::vector<Item>*)lp; wchar_t cls[32]; ::GetClassNameW(c, cls, 32);
        bool up = !_wcsicmp(cls, L"msctls_updown32"), ed = !_wcsicmp(cls, L"Edit");
        if (up || ed) { RECT r; ::GetWindowRect(c, &r); v->push_back({ c, r, up }); }
        return TRUE;
    }, (LPARAM)&items);
    for (const auto& u : items) {
        if (!u.isUp) continue;
        HWND buddy = nullptr; int best = 100000;         // nearest edit to the left, vertically overlapping
        for (const auto& e : items) {
            if (e.isUp || e.r.bottom <= u.r.top || e.r.top >= u.r.bottom) continue;
            int dx = u.r.left - e.r.right;                // edit sits just left of (or overlapping) the spinner
            if (dx > -8 && dx < best) { best = dx; buddy = e.h; }
        }
        if (!buddy) continue;
        LONG st = (LONG)::GetWindowLongPtr(u.h, GWL_STYLE);
        ::SetWindowLongPtr(u.h, GWL_STYLE, (st & ~UDS_ALIGNLEFT) | UDS_ALIGNRIGHT);
        ::SendMessageW(u.h, UDM_SETBUDDY, (WPARAM)buddy, 0);   // native ALIGNRIGHT layout + shrink buddy
    }
}

HWND LivePreview::RenderDialog(long long dialogNum, CWnd* parent,
                               const ControlIndex& idx, const PoFile& po) {
    DestroyPreview();

    // 1. Get the DLGTEMPLATEEX bytes — emitted from the parsed RC (Approach C) or the pinned DLL —
    //    then patch its style so a top-level template embeds as a child of the preview host.
    std::vector<unsigned char> buf;
    if (m_useRc) {
        const mpctrans::RcDialog* d = nullptr;
        for (const auto& x : m_rcDialogs) if (x.id == dialogNum) { d = &x; break; }
        if (!d) return nullptr;
        buf = mpctrans::emit_dlgtemplate(*d);
    } else {
        if (!m_neutral) return nullptr;
        HRSRC hr = ::FindResource(m_neutral, MAKEINTRESOURCE((WORD)dialogNum), RT_DIALOG);
        if (!hr) return nullptr;
        DWORD size = ::SizeofResource(m_neutral, hr);
        auto* src = (const unsigned char*)::LockResource(::LoadResource(m_neutral, hr));
        if (!src || size < 16) return nullptr;
        buf.assign(src, src + size);
    }
    if (buf.size() < 16) return nullptr;
    DWORD exStyle, style;              // DLGTEMPLATEEX: dlgVer(2) sig(2) helpID(4) exStyle(4) style(4)
    memcpy(&exStyle, buf.data() + 8, 4);
    memcpy(&style, buf.data() + 12, 4);
    // Create HIDDEN: substitute text + run the widget-pair repositioning while invisible, then show
    // once — otherwise the dialog paints controls at their designed spots first and the subsequent
    // moves leave ghosts of the old positions.
    style &= ~(WS_POPUP | WS_SYSMENU | WS_VISIBLE | DS_ABSALIGN | DS_CENTER | DS_CENTERMOUSE);
    // Drop the caption/frame chrome too: embedded in our pane, the light system title bar + modal
    // frame clash with the dark theme (property pages have none; only modal dialogs like About do).
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_DLGFRAME | WS_BORDER | DS_MODALFRAME);
    style |= WS_CHILD;
    exStyle &= ~(WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_STATICEDGE);
    memcpy(buf.data() + 8, &exStyle, 4);
    memcpy(buf.data() + 12, &style, 4);

    // 2. Create it modeless. hInstance = the APP (system/registered classes; the datafile
    //    HMODULE is not a real module and must not be passed here).
    HWND dlg = ::CreateDialogIndirect(AfxGetInstanceHandle(), (LPCDLGTEMPLATE)buf.data(),
                                      parent ? parent->GetSafeHwnd() : nullptr, PreviewDlgProc);
    if (!dlg) return nullptr;
    ::SetWindowLongPtr(dlg, DWLP_USER, (LONG_PTR)this);
    m_dlg = dlg;

    // 2b. Match MPC-HC's dialog font: it renders in the system message font (Segoe UI), not the
    //     template's MS Shell Dlg. Apply it before substitution so widget-pair measuring uses it too.
    if (!m_dlgFont) {
        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        m_dlgFont = ::CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (m_dlgFont) {
        ::SendMessageW(dlg, WM_SETFONT, (WPARAM)m_dlgFont, FALSE);
        ::EnumChildWindows(dlg, [](HWND c, LPARAM lp) -> BOOL {
            ::SendMessageW(c, WM_SETFONT, (WPARAM)lp, FALSE); return TRUE;
        }, (LPARAM)m_dlgFont);
    }

    // 3. Capture the ORIGINAL English text per child (HitTest keys), then substitute captions
    //    from the .po via the control-index. Rects stay template-sized => overflow measurable.
    struct Ctx { LivePreview* self; const ControlIndex* idx; const PoFile* po; long long dlg; }
        ctx{ this, &idx, &po, dialogNum };
    ::EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t buf[512]; ::GetWindowTextW(child, buf, 512);
        std::string english = CW2A(buf, CP_UTF8);
        c->self->m_english.emplace_back(child, english);
        if (auto* rec = c->idx->dialog_lookup(c->dlg, ::GetDlgCtrlID(child), english))
            if (auto* e = c->po->find(rec->msgctxt, rec->msgid); e && !e->msgstr.empty())
                ::SetWindowTextW(child, CA2W(e->msgstr.c_str(), CP_UTF8));
        return TRUE;
    }, (LPARAM)&ctx);

    // 4. Dialog caption from the .po.
    if (auto* cap = idx.dialog_caption(dialogNum))
        if (auto* e = po.find(cap->msgctxt, cap->msgid); e && !e->msgstr.empty())
            ::SetWindowTextW(dlg, CA2W(e->msgstr.c_str(), CP_UTF8));

    // 5. MPC-HC's runtime label/control repositioning, so the preview shows the spacing a longer
    //    translation actually gets (only meaningful with the parsed RC, which carries the symbols).
    if (m_useRc) ApplyWidgetPairs(dlg, dialogNum);
    IntegrateSpinners(dlg);   // after the edits reach their final positions

    // 6. MPC-HC "Modern" dark theming (backgrounds via WM_CTLCOLOR in PreviewDlgProc; buttons/checks/
    //    groups owner-drawn). Applied while hidden, like the widget pairs, so it shows clean.
    ApplyDarkTheme(dlg);

    // TODO: render target-language font + WS_EX_LAYOUTRTL for ar/he (accurate overflow).
    ::ShowWindow(dlg, SW_SHOWNA);
    return dlg;
}

const DialogRecord* LivePreview::HitTest(HWND, HWND clicked, long long dialogNum, const ControlIndex& idx) {
    // match by the ENGLISH text captured at render time (live text is already translated)
    for (const auto& [hwnd, english] : m_english)
        if (hwnd == clicked)
            return idx.dialog_lookup(dialogNum, ::GetDlgCtrlID(clicked), english);
    return nullptr;
}

// ---- menus ----
bool LivePreview::HasMenu(long long menuId) const {
    return m_neutral && ::FindResource(m_neutral, MAKEINTRESOURCE((WORD)menuId), RT_MENU) != nullptr;
}
HMENU LivePreview::LoadRawMenu(long long menuId) const {
    return m_neutral ? ::LoadMenu(m_neutral, MAKEINTRESOURCE((WORD)menuId)) : nullptr;
}

// Single-line text controls whose current text is wider than their client rect.
// The mnemonic '&' a prefix-processing control (buttons; statics without SS_NOPREFIX) draws as an
// underline under the next letter, not as a '&' glyph — and '&&' draws as one literal '&'. Since
// GetTextExtentPoint32W is not prefix-aware, measuring the raw window text over-counts every
// accelerator-bearing label/button by an '&' glyph (~7px), making the fit gate falsely conservative.
// Return the text as actually drawn so the measurement matches the rendered width.
static std::wstring displayText(const wchar_t* s, int n, bool prefixProcessed) {
    std::wstring o; o.reserve(n);
    for (int r = 0; r < n; ++r) {
        if (prefixProcessed && s[r] == L'&') {
            if (r + 1 < n && s[r + 1] == L'&') { o.push_back(L'&'); ++r; }   // '&&' -> literal '&'
            // else: a lone '&' is the mnemonic marker — dropped
        } else o.push_back(s[r]);
    }
    return o;
}
// Whether the control consumes '&' as a mnemonic prefix at draw time: buttons always do; statics do
// unless SS_NOPREFIX. (cls/style as read from the live control.)
static bool prefixProcesses(const wchar_t* cls, LONG style) {
    if (!_wcsicmp(cls, L"Button")) return true;
    if (!_wcsicmp(cls, L"Static")) return !(style & SS_NOPREFIX);
    return false;
}

std::vector<LivePreview::Overflow> LivePreview::DetectOverflow(HWND dlg) {
    std::vector<Overflow> out;
    if (!dlg) return out;
    ::EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL {
        auto* out = (std::vector<Overflow>*)lp;
        wchar_t cls[64]; ::GetClassNameW(child, cls, 64);
        bool textual = !_wcsicmp(cls, L"Button") || !_wcsicmp(cls, L"Static");
        LONG st = (LONG)::GetWindowLongPtr(child, GWL_STYLE);
        if (!_wcsicmp(cls, L"Static") && ((st & SS_TYPEMASK) > SS_RIGHT)) textual = false;  // icons etc.
        if (!textual) return TRUE;
        wchar_t txt[512]; int n = ::GetWindowTextW(child, txt, 512);
        if (n <= 0 || wcschr(txt, L'\n')) return TRUE;      // multi-line statics wrap; skip
        std::wstring dt = displayText(txt, n, prefixProcesses(cls, st));   // drop the mnemonic '&'
        HDC dc = ::GetDC(child);
        HFONT f = (HFONT)::SendMessage(child, WM_GETFONT, 0, 0);
        HGDIOBJ old = f ? ::SelectObject(dc, f) : nullptr;
        SIZE sz{}; ::GetTextExtentPoint32W(dc, dt.c_str(), (int)dt.size(), &sz);
        if (old) ::SelectObject(dc, old);
        ::ReleaseDC(child, dc);
        RECT rc; ::GetClientRect(child, &rc);
        int avail = rc.right - rc.left;
        if (!_wcsicmp(cls, L"Button")) avail -= 8;          // borders/margins
        if (sz.cx > avail && avail > 0) out->push_back({ child });
        return TRUE;
    }, (LPARAM)&out);
    return out;
}

// Re-measure arbitrary replacement text against a specific control's current font + client rect.
// Mirrors DetectOverflow's Button-padding rule; const (read-only).
LivePreview::TextFit LivePreview::MeasureControlText(HWND ctrl, const CString& text) const {
    TextFit fit;
    if (!ctrl || !::IsWindow(ctrl)) return fit;
    wchar_t cls[64]; ::GetClassNameW(ctrl, cls, 64);
    LONG st = (LONG)::GetWindowLongPtr(ctrl, GWL_STYLE);
    std::wstring dt = displayText(text, text.GetLength(), prefixProcesses(cls, st));   // drop the mnemonic '&'
    HDC dc = ::GetDC(ctrl);
    HFONT f = (HFONT)::SendMessage(ctrl, WM_GETFONT, 0, 0);
    HGDIOBJ old = f ? ::SelectObject(dc, f) : nullptr;
    SIZE sz{}; ::GetTextExtentPoint32W(dc, dt.c_str(), (int)dt.size(), &sz);
    if (old) ::SelectObject(dc, old);
    ::ReleaseDC(ctrl, dc);
    RECT rc; ::GetClientRect(ctrl, &rc);
    int avail = rc.right - rc.left;
    if (!_wcsicmp(cls, L"Button")) avail -= 8;
    fit.renderedPx = (int)sz.cx;
    fit.availablePx = avail;
    fit.measured = true;
    return fit;
}

// Single-line, width-constrained fit measurement for every translatable control in the CURRENTLY
// RENDERED dialog `dlg` — see the header comment for the full scope/grouping rules.
std::vector<LivePreview::FitMeasurement> LivePreview::MeasureFit(HWND dlg, long long dialogId,
                                                                  const mpctrans::ControlIndex& idx) {
    std::vector<FitMeasurement> out;
    if (!dlg || !::IsWindow(dlg)) return out;

    HDC hdc = ::GetDC(dlg);
    int dpi = ::GetDeviceCaps(hdc, LOGPIXELSY);
    HGDIOBJ of = m_dlgFont ? ::SelectObject(hdc, m_dlgFont) : nullptr;
    TEXTMETRICW tm{}; ::GetTextMetricsW(hdc, &tm);
    if (of) ::SelectObject(hdc, of);
    ::ReleaseDC(dlg, hdc);
    int lineH = tm.tmHeight > 0 ? tm.tmHeight : 16;
    int rowTolerance = ::MulDiv(4, dpi, 96);
    int gapTolerance = ::MulDiv(20, dpi, 96);
    const double heightRatioMax = 1.6;

    struct Cand {
        HWND hwnd; RECT rc;                  // rc in dialog-client coords
        const mpctrans::DialogRecord* rec;
        int renderedPx, availablePx;
    };
    std::vector<Cand> cands;

    for (const auto& [hwnd, english] : m_english) {
        if (!hwnd || !::IsWindow(hwnd) || !::IsChild(dlg, hwnd)) continue;   // stale-call guard
        wchar_t cls[64]; ::GetClassNameW(hwnd, cls, 64);
        bool isButton = !_wcsicmp(cls, L"Button");
        bool isStatic = !_wcsicmp(cls, L"Static");
        if (!isButton && !isStatic) continue;
        RECT crc; ::GetClientRect(hwnd, &crc);
        if (isStatic && (crc.bottom - crc.top) > (int)(lineH * heightRatioMax))
            continue;   // taller than ~1 line -> multi-line/wrapping label, excluded

        int ctrlId = ::GetDlgCtrlID(hwnd);
        const mpctrans::DialogRecord* rec = idx.dialog_lookup(dialogId, (long long)ctrlId, english);
        if (!rec) continue;   // not a translatable control per the build_index.py selection

        wchar_t txt[512] = {}; int n = ::GetWindowTextW(hwnd, txt, 512);
        LONG st = (LONG)::GetWindowLongPtr(hwnd, GWL_STYLE);
        std::wstring dt = displayText(txt, n, prefixProcesses(cls, st));   // drop the mnemonic '&'
        HDC dc = ::GetDC(hwnd);
        HFONT f = (HFONT)::SendMessage(hwnd, WM_GETFONT, 0, 0);
        HGDIOBJ oldf = f ? ::SelectObject(dc, f) : nullptr;
        SIZE sz{}; ::GetTextExtentPoint32W(dc, dt.c_str(), (int)dt.size(), &sz);
        if (oldf) ::SelectObject(dc, oldf);
        ::ReleaseDC(hwnd, dc);

        int avail = crc.right - crc.left;
        if (isButton) avail -= 8;

        RECT wr; ::GetWindowRect(hwnd, &wr);
        ::MapWindowPoints(nullptr, dlg, (POINT*)&wr, 2);

        cands.push_back({ hwnd, wr, rec, (int)sz.cx, avail });
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.rc.top != b.rc.top) return a.rc.top < b.rc.top;
        return a.rc.left < b.rc.left;
    });

    // Bucket into rows by top-proximity to the row's first (topmost) member.
    std::vector<std::vector<size_t>> rows;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!rows.empty() && cands[i].rc.top - cands[rows.back().front()].rc.top <= rowTolerance)
            rows.back().push_back(i);
        else
            rows.push_back({ i });
    }

    auto emitSingle = [&](const Cand& c) {
        FitMeasurement fm;
        fm.controlId = ::GetDlgCtrlID(c.hwnd);
        fm.controlSym = c.rec->control_sym;
        fm.msgctxt = c.rec->msgctxt;
        fm.msgid = c.rec->msgid;
        fm.renderedPx = c.renderedPx;
        fm.availablePx = c.availablePx;
        fm.grouped = false;
        out.push_back(std::move(fm));
    };
    auto emitGroup = [&](const std::vector<size_t>& row, size_t begin, size_t end) {
        int groupRendered = 0, groupAvail = 0;
        std::vector<std::string> ctxs;
        for (size_t m = begin; m < end; ++m) { groupRendered += cands[row[m]].renderedPx; groupAvail += cands[row[m]].availablePx; }
        for (size_t m = begin; m < end; ++m) ctxs.push_back(cands[row[m]].rec->msgctxt);
        for (size_t m = begin; m < end; ++m) {
            const Cand& c = cands[row[m]];
            FitMeasurement fm;
            fm.controlId = ::GetDlgCtrlID(c.hwnd);
            fm.controlSym = c.rec->control_sym;
            fm.msgctxt = c.rec->msgctxt;
            fm.msgid = c.rec->msgid;
            fm.renderedPx = c.renderedPx;
            fm.availablePx = c.availablePx;
            fm.grouped = true;
            fm.groupRenderedPx = groupRendered;
            fm.groupAvailablePx = groupAvail;
            for (const auto& peerCtx : ctxs) if (peerCtx != fm.msgctxt) fm.groupPeers.push_back(peerCtx);
            out.push_back(std::move(fm));
        }
    };

    for (auto& row : rows) {
        std::sort(row.begin(), row.end(), [&](size_t a, size_t b) { return cands[a].rc.left < cands[b].rc.left; });
        size_t chainStart = 0;
        for (size_t k = 1; k <= row.size(); ++k) {
            bool breakChain = (k == row.size()) ||
                (cands[row[k]].rc.left - cands[row[k - 1]].rc.right > gapTolerance);
            if (!breakChain) continue;
            size_t chainLen = k - chainStart;
            if (chainLen >= 2) emitGroup(row, chainStart, k);
            else               emitSingle(cands[row[chainStart]]);
            chainStart = k;
        }
    }
    return out;
}
