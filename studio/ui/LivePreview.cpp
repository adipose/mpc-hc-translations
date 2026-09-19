// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include <uxtheme.h>
#include <vssym32.h>    // BP_CHECKBOX / BP_RADIOBUTTON part ids for the glyph-fit pass
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
    m_overlapClusters.clear();
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

    // If the string being edited lives on a currently-hidden overlap-cluster member (e.g. the user
    // picked the ComboBox variant's string while the Edit variant happens to be showing), reseed that
    // cluster with it BEFORE ringing it, so the ring lands on a visible control -- and repaint the
    // cluster's screen-rect union so the swap actually paints (ShowWindow alone doesn't repaint the
    // siblings it just hid/exposed).
    if (next && m_dlg && !::IsWindowVisible(next)) {
        for (auto& cluster : m_overlapClusters) {
            if (std::find(cluster.members.begin(), cluster.members.end(), next) == cluster.members.end())
                continue;
            RECT u{}; bool first = true;
            for (HWND m : cluster.members) {
                RECT r; ::GetWindowRect(m, &r);
                if (first) { u = r; first = false; } else ::UnionRect(&u, &u, &r);
            }
            ApplySeed(cluster, next);
            SyncSpinnerBuddies(m_dlg);
            ::MapWindowPoints(nullptr, m_dlg, (POINT*)&u, 2);   // screen -> dialog-client
            ::RedrawWindow(m_dlg, &u, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            break;
        }
    }

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
    switch (msg) {   // MPC theme: background + text for the dialog and non-owner-drawn controls.
        // DARK only -- in light the player draws dialogs natively (upstream da3aca546: gray modal
        // dialogs, tab-textured property pages), so return FALSE and let DefDlgProc pick the colors.
        case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
            if (!Theme::IsDark()) return FALSE;
            return (INT_PTR)Theme::windowCtl((HDC)wp);
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
            if (!Theme::IsDark()) return FALSE;
            return (INT_PTR)Theme::contentCtl((HDC)wp);
        case WM_NOTIFY: {
            // A msctls_trackbar32 child's reflected NM_CUSTOMDRAW arrives here, at the parent (see
            // Theme::TrackbarCustomDraw -- port of CMPCThemeSliderCtrl::OnNMCustomdraw). Real dialog
            // procs return WM_NOTIFY results via DWLP_MSGRESULT, not the return value itself.
            LRESULT ncdResult = 0;
            if (Theme::TrackbarCustomDraw((NMHDR*)lp, &ncdResult)) {
                ::SetWindowLongPtr(dlg, DWLP_MSGRESULT, ncdResult);
                return TRUE;
            }
            break;
        }
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
        // UDS_AUTOBUDDY spinners were already integrated NATIVELY at dialog creation (ALIGNRIGHT
        // shrank the buddy edit then) -- re-sending UDM_SETBUDDY re-applies the shrink and reduces a
        // narrow edit to a sliver (IDD_PPAGEPLAYBACK's 25-DLU min%/max% auto-fit edits ended up 11px
        // wide). Only pair spinners that have NO buddy yet.
        if (::SendMessageW(u.h, UDM_GETBUDDY, 0, 0)) continue;
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

// --- Overlap clusters -------------------------------------------------------------------------
// Some MPC-HC dialogs stack SEVERAL controls at the identical template rect because the player's C++
// only shows one "variant" at a time at runtime -- e.g. IDD_PPAGEADVANCED's bottom row overlays an
// Edit (numeric value, with an msctls_updown32 buddy), a ComboBox (enum values) and two radio Buttons
// ("True"/"False") at the SAME rect; CPPageAdvanced::OnInitDialog shows only the one matching the
// selected setting's type. Rendered from a raw template ALL of them show at once, stacked -- double
// borders, a stray combo-arrow box, radios drawn over the edit. This mirrors the runtime: detect the
// overlapping groups and show only one compatible subset per group, seeded by the string being edited.
namespace {
// Direct children eligible for clustering: excludes group boxes (layout chrome, not a runtime variant),
// msctls_updown32 (not a variant of its own -- tied to its buddy edit's visibility, see
// SyncSpinnerBuddies), and zero-area windows.
bool overlap_clusterable(HWND c) {
    RECT r; ::GetWindowRect(c, &r);
    if (r.right <= r.left || r.bottom <= r.top) return false;
    // Skip windows already hidden (their own WS_VISIBLE bit -- the dialog isn't shown yet, so
    // IsWindowVisible would be false for everyone): e.g. imageless icon statics hidden in step 6b.
    // ApplySeed must never re-show them.
    if (!(::GetWindowLongPtr(c, GWL_STYLE) & WS_VISIBLE)) return false;
    wchar_t cls[32]; ::GetClassNameW(c, cls, 32);
    if (!_wcsicmp(cls, L"msctls_updown32")) return false;
    LONG st = (LONG)::GetWindowLongPtr(c, GWL_STYLE);
    if (!_wcsicmp(cls, L"Button") && (st & BS_TYPEMASK) == BS_GROUPBOX) return false;
    return true;
}
// Two controls CONFLICT (occupy the same runtime "variant slot") if their window rects intersect over
// more than 40% of the smaller one's area -- a loose overlap (e.g. a spinner's shrunk buddy edit abutting
// its label) shouldn't cluster; the near-total overlap of stacked variants should.
bool overlap_conflicts(const RECT& a, const RECT& b) {
    RECT i;
    if (!::IntersectRect(&i, &a, &b)) return false;
    LONGLONG areaA = (LONGLONG)(a.right - a.left) * (a.bottom - a.top);
    LONGLONG areaB = (LONGLONG)(b.right - b.left) * (b.bottom - b.top);
    LONGLONG areaI = (LONGLONG)(i.right - i.left) * (i.bottom - i.top);
    LONGLONG smaller = min(areaA, areaB);   // (bare min: NOMINMAX isn't defined in this TU, see e.g. ShowComboDropdown's max())
    return smaller > 0 && areaI * 10 > smaller * 4;   // > 40%
}
} // namespace

// Detect this dialog's overlap clusters (connected components of overlap_conflicts among direct,
// clusterable children) and show each one's default seed (its first member in template/EnumChildWindows
// order). Called from RenderDialog after layout is final (substitution, AdjustDynamicWidgetPair,
// IntegrateSpinners) and before the dialog is shown. Dialogs with no overlaps get no clusters, so their
// children are untouched -- ShowWindow is only ever called on members of an actual cluster.
void LivePreview::ResolveOverlaps(HWND dlg) {
    m_overlapClusters.clear();

    struct Item { HWND h; RECT r; };
    std::vector<Item> items;
    ::EnumChildWindows(dlg, [](HWND c, LPARAM lp) -> BOOL {
        if (overlap_clusterable(c)) { RECT r; ::GetWindowRect(c, &r); ((std::vector<Item>*)lp)->push_back({ c, r }); }
        return TRUE;
    }, (LPARAM)&items);

    std::vector<bool> used(items.size(), false);
    for (size_t i = 0; i < items.size(); ++i) {
        if (used[i]) continue;
        std::vector<size_t> comp{ i };
        used[i] = true;
        for (size_t k = 0; k < comp.size(); ++k)              // BFS: pull in anything conflicting with any member
            for (size_t j = 0; j < items.size(); ++j)
                if (!used[j] && overlap_conflicts(items[comp[k]].r, items[j].r)) { used[j] = true; comp.push_back(j); }
        if (comp.size() < 2) continue;                        // singletons aren't clusters
        std::sort(comp.begin(), comp.end());                  // restore template order (BFS scrambles it)
        OverlapCluster oc;
        for (size_t ix : comp) oc.members.push_back(items[ix].h);
        m_overlapClusters.push_back(std::move(oc));
    }

    for (auto& c : m_overlapClusters) ApplySeed(c, c.members.front());   // default seed = first in template order
    SyncSpinnerBuddies(dlg);
}

// Show `seed`, then -- in template order -- also show any other cluster member that conflicts with
// NONE of the members shown so far (e.g. seeding on radio "True" also brings back its sibling radio
// "False": they don't conflict with each other, only with the Edit/ComboBox they're stacked inside).
// Everything else in the cluster stays/becomes hidden. Never resizes/moves a control -- only visibility.
void LivePreview::ApplySeed(OverlapCluster& cluster, HWND seed) {
    for (HWND m : cluster.members) ::ShowWindow(m, m == seed ? SW_SHOWNA : SW_HIDE);
    std::vector<HWND> shown{ seed };
    for (HWND m : cluster.members) {
        if (m == seed) continue;
        RECT mr; ::GetWindowRect(m, &mr);
        bool conflictsShown = false;
        for (HWND s : shown) {
            RECT sr; ::GetWindowRect(s, &sr);
            if (overlap_conflicts(mr, sr)) { conflictsShown = true; break; }
        }
        if (!conflictsShown) { ::ShowWindow(m, SW_SHOWNA); shown.push_back(m); }
    }
}

// Tie each msctls_updown32's visibility to its buddy edit's (the buddy pairing was set up by
// IntegrateSpinners via UDM_SETBUDDY, which must run before this). The spinner isn't a cluster member
// of its own -- it's part of whichever Edit variant currently shows, so it hides/shows with it.
void LivePreview::SyncSpinnerBuddies(HWND dlg) {
    ::EnumChildWindows(dlg, [](HWND c, LPARAM) -> BOOL {
        wchar_t cls[32]; ::GetClassNameW(c, cls, 32);
        if (_wcsicmp(cls, L"msctls_updown32")) return TRUE;
        HWND buddy = (HWND)::SendMessageW(c, UDM_GETBUDDY, 0, 0);
        if (!buddy) {
            // No UDM_SETBUDDY pairing (IntegrateSpinners only pairs a spinner sitting just RIGHT of
            // its edit; some templates place the spinner INSIDE the edit's right edge, e.g.
            // IDD_PPAGEADVANCED). Fall back to geometry: the Edit sibling whose rect overlaps or abuts
            // the spinner -- otherwise a hidden edit leaves an orphan floating spinner.
            RECT ur; ::GetWindowRect(c, &ur);
            struct Ctx { const RECT* u; HWND hit; } ctx{ &ur, nullptr };
            ::EnumChildWindows(::GetParent(c), [](HWND e, LPARAM lp) -> BOOL {
                auto* x = (Ctx*)lp;
                wchar_t ecls[32]; ::GetClassNameW(e, ecls, 32);
                if (_wcsicmp(ecls, L"Edit")) return TRUE;
                RECT er; ::GetWindowRect(e, &er);
                if (er.bottom <= x->u->top || er.top >= x->u->bottom) return TRUE;   // no vertical overlap
                if (er.right >= x->u->left - 8 && er.left <= x->u->right + 8) { x->hit = e; return FALSE; }
                return TRUE;
            }, (LPARAM)&ctx);
            buddy = ctx.hit;
        }
        // Test the buddy's OWN visible bit, not IsWindowVisible: ResolveOverlaps runs before the
        // dialog is shown, and IsWindowVisible is false for every child of a hidden parent -- it
        // would hide the spinner of a perfectly visible edit at render time.
        if (buddy) {
            bool on = (::GetWindowLongPtr(buddy, GWL_STYLE) & WS_VISIBLE) != 0;
            ::ShowWindow(c, on ? SW_SHOWNA : SW_HIDE);
        }
        return TRUE;
    }, 0);
}

// Advance past a DLGTEMPLATEEX sz_Or_Ord field (menu/class): 0x0000 -> empty (2 bytes),
// 0xFFFF -> ordinal (4 bytes), else a null-terminated UTF-16 string.
static size_t skip_sz_or_ord(const std::vector<unsigned char>& b, size_t pos) {
    if (pos + 2 > b.size()) return b.size();
    unsigned short w; memcpy(&w, b.data() + pos, 2);
    if (w == 0x0000) return pos + 2;
    if (w == 0xFFFF) return pos + 4;
    for (pos += 2; pos + 2 <= b.size(); ) { unsigned short c; memcpy(&c, b.data() + pos, 2); pos += 2; if (!c) break; }
    return pos;
}

// The property-sheet layout grid font comes from COMCTL32's OWN dialog template (#1006), and that
// template is MUI-LOCALIZED per Windows UI language: 8pt "MS Shell Dlg" for Western/Cyrillic
// locales (13px grid), but taller 9pt UI fonts for CJK + Thai -- which is how those locales'
// property sheets guarantee the native glyphs fit. Table dumped from the complete Win10 21H2
// comctl32.dll.mui set (37 cultures; only these five families deviate). The preview simulates the
// TARGET LANGUAGE's grid, so a translator sees the same layout and text font a user of that
// language gets, regardless of this machine's locale. (Hardcoding en-US here was issue #3's
// JP glyph-clip: an 11px MS UI Gothic grid the real player never uses.)
static void propsheet_grid_font(const CString& lang, CStringW& face, WORD& pt) {
    CStringW l(lang); l.MakeLower(); l.Replace(L'_', L'-');
    face = L"MS Shell Dlg"; pt = 8;
    if (l.Left(2) == L"ja")      { face = L"Yu Gothic UI";         pt = 9; }
    else if (l.Left(2) == L"ko") { face = L"Malgun Gothic";        pt = 9; }   // template says its localized name
    else if (l.Left(2) == L"th") { face = L"Leelawadee UI";        pt = 9; }
    else if (l.Left(2) == L"zh") {
        bool trad = l == L"zh-tw" || l == L"zh-hk" || l == L"zh-mo" || l.Find(L"hant") >= 0;
        face = trad ? L"Microsoft JhengHei UI" : L"Microsoft YaHei UI";
        pt = 9;
    }
}

// Force a DLGTEMPLATEEX's DS_SETFONT font to the comctl32 property-sheet grid font (see above) --
// the dialog-unit grid COMCTL32 uses for property-sheet pages regardless of the template's declared
// FONT (see RenderDialog's caller). Rebuilds the byte vector because the typeface length changes;
// the control array is kept DWORD-aligned so its self-contained, individually-aligned control
// entries copy verbatim without breaking alignment.
static void force_propsheet_font(std::vector<unsigned char>& buf, DWORD style, const CString& lang) {
    if (!(style & DS_SETFONT)) return;
    size_t pos = 26;                          // after dlgVer(2) sig(2) helpID(4) exStyle(4) style(4) cDlgItems(2) x/y/cx/cy(8)
    pos = skip_sz_or_ord(buf, pos);           // menu
    pos = skip_sz_or_ord(buf, pos);           // windowClass
    pos = skip_sz_or_ord(buf, pos);           // title
    if (pos + 6 > buf.size()) return;         // font block = pointsize(2) weight(2) italic(1) charset(1) typeface[]
    size_t tfEnd = pos + 6;
    for (; tfEnd + 2 <= buf.size(); ) { unsigned short c; memcpy(&c, buf.data() + tfEnd, 2); tfEnd += 2; if (!c) break; }
    size_t ctrlStart = (tfEnd + 3) & ~size_t(3);   // control array is DWORD-aligned from the template start
    CStringW face; WORD pt;
    propsheet_grid_font(lang, face, pt);
    // STUDIO_PROPSHEET_FONT ("face" or "face:pt") overrides for cross-locale testing -- e.g.
    // "MS UI Gothic" reproduces a JP system running the en-US comctl template (the old bug), while
    // "Yu Gothic UI:9" simulates the real Japanese comctl grid.
    wchar_t dbg[80];
    if (::GetEnvironmentVariableW(L"STUDIO_PROPSHEET_FONT", dbg, 80) > 0) {
        face = dbg;
        int colon = face.ReverseFind(L':');
        if (colon > 0) { pt = (WORD)_wtoi(face.Mid(colon + 1)); face = face.Left(colon); }
    }
    std::vector<unsigned char> nb(buf.begin(), buf.begin() + pos);
    auto pushW = [&](unsigned short w){ nb.push_back((unsigned char)(w & 0xFF)); nb.push_back((unsigned char)(w >> 8)); };
    pushW(pt);           // pointsize
    pushW(FW_REGULAR);   // weight (400)
    nb.push_back(0);     // italic
    nb.push_back(DEFAULT_CHARSET);
    for (const wchar_t* p = (const wchar_t*)face; ; ++p) { pushW((unsigned short)*p); if (!*p) break; }
    while (nb.size() & 3) nb.push_back(0);    // re-align the control array to DWORD
    nb.insert(nb.end(), buf.begin() + ctrlStart, buf.end());
    buf.swap(nb);
}

HWND LivePreview::RenderDialog(long long dialogNum, CWnd* parent,
                               const ControlIndex& idx, const PoFile& po,
                               bool propSheetLayout) {
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
    style |= WS_CHILD | WS_CLIPSIBLINGS;   // CLIPSIBLINGS: deterministic painting against the page
                                           // frame sibling behind it (both clip -> z-order wins)
    exStyle &= ~(WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_STATICEDGE);
    memcpy(buf.data() + 8, &exStyle, 4);
    memcpy(buf.data() + 12, &style, 4);

    // Property pages: lay out on COMCTL32's 8pt "MS Shell Dlg" property-sheet grid (see helper), so the
    // preview matches the real Options dialog (~8/9 the size of a naive 9pt-template layout). Text is
    // still drawn in the 9pt message font applied below. Modal dialogs keep their own template font.
    if (propSheetLayout) force_propsheet_font(buf, style, m_targetLang);

    // 2. Create it modeless. hInstance = the APP (system/registered classes; the datafile
    //    HMODULE is not a real module and must not be passed here).
    HWND dlg = ::CreateDialogIndirect(AfxGetInstanceHandle(), (LPCDLGTEMPLATE)buf.data(),
                                      parent ? parent->GetSafeHwnd() : nullptr, PreviewDlgProc);
    if (!dlg) return nullptr;
    ::SetWindowLongPtr(dlg, DWLP_USER, (LONG_PTR)this);
    m_dlg = dlg;

    // 2a. LIGHT theme renders every control natively themed, property pages included -- matching the
    //     player since its light-consistency overhaul (upstream ef3e901c0/631b46f48: custom painting is
    //     gated on drawThemedControls, true only in dark; nothing is stripped to classic anymore).
    //     Light property pages get the tab-body texture (white, like the player's native pages inside
    //     its themed sheet); light modal dialogs keep the native gray dialog background.
    if (propSheetLayout && !Theme::IsDark())
        ::EnableThemeDialogTexture(dlg, ETDT_ENABLETAB);

    // 2a'. Glyph-fit (issue #3, Japanese report): where FontSubstitutes maps the dialog font shorter
    //      than "Microsoft Sans Serif" (JP: "MS Shell Dlg" -> MS UI Gothic, 11px vs 13px at 8pt/96dpi),
    //      the DLU grid shrinks and template-height (8 DLU) radios/checks become SHORTER than the
    //      native glyph -- which centers and clips top+bottom. Grow such controls symmetrically to the
    //      glyph height; a no-op wherever the grid already fits (Western systems). The real player has
    //      the same latent clip in its light-native theme on JP systems (upstream candidate fix).
    ::EnumChildWindows(dlg, [](HWND c, LPARAM) -> BOOL {
        wchar_t cls[20]; ::GetClassNameW(c, cls, 20);
        if (_wcsicmp(cls, L"Button")) return TRUE;
        DWORD bt = (DWORD)(::GetWindowLongPtr(c, GWL_STYLE) & BS_TYPEMASK);
        bool radio = bt == BS_RADIOBUTTON || bt == BS_AUTORADIOBUTTON;
        bool check = bt == BS_CHECKBOX || bt == BS_AUTOCHECKBOX || bt == BS_3STATE || bt == BS_AUTO3STATE;
        if (!radio && !check) return TRUE;
        int glyphH = 0;
        if (HTHEME t = ::OpenThemeData(c, L"Button")) {
            SIZE s{};
            HDC hdc = ::GetDC(c);
            if (SUCCEEDED(::GetThemePartSize(t, hdc, radio ? BP_RADIOBUTTON : BP_CHECKBOX,
                                             1 /*UNCHECKEDNORMAL*/, nullptr, TS_TRUE, &s)))
                glyphH = s.cy;
            ::ReleaseDC(c, hdc);
            ::CloseThemeData(t);
        }
        if (!glyphH) glyphH = ::MulDiv(13, Theme::DpiOf(c), 96);   // classic fallback
        RECT wr; ::GetWindowRect(c, &wr);
        int deficit = glyphH - (wr.bottom - wr.top);
        if (deficit <= 0) return TRUE;
        ::MapWindowPoints(nullptr, ::GetParent(c), (POINT*)&wr, 2);
        ::SetWindowPos(c, nullptr, wr.left, wr.top - (deficit + 1) / 2,
                       wr.right - wr.left, (wr.bottom - wr.top) + deficit,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return TRUE;
    }, 0);

    // 2b. Fonts: honor the TEMPLATE font, exactly like the player. MPC-HC never re-fonts its dialogs
    //     at runtime (CMPCThemeUtil's DialogFont hack is #if 0), so what the template declares is what
    //     the player renders: 54 of 55 templates say 9pt Segoe UI; IDD_ADDCOMMAND_DLG says 8pt
    //     "MS Shell Dlg"; property pages are force_propsheet_font'ed to the sheet's 8pt above. A
    //     previous blanket WM_SETFONT override to the 9pt message font was a no-op for the Segoe
    //     templates but oversized IDD_ADDCOMMAND_DLG's text against its 8pt-sized layout.
    //     m_dlgFont stays lazily created for the measuring/tooltip paths that reference it.
    if (!m_dlgFont) {
        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        m_dlgFont = ::CreateFontIndirectW(&ncm.lfMessageFont);
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

    // 6b. Hide SS_ICON/SS_BITMAP statics that have NO image: their images are loaded at RUNTIME by
    //    the app (e.g. IDD_PPAGEOUTPUT's tick/cross renderer-capability indicators), and a real
    //    SS_ICON static SHRINKS to its icon when one is set -- which is how the player's oversized
    //    24x20-DLU placeholders never cover the labels they overlap in the template. With no image,
    //    ours kept the full placeholder rect and its background erase clipped the neighboring
    //    label's first characters ('KVA', 'creenshot'). An imageless image-static shows nothing
    //    anyway, so hide it.
    ::EnumChildWindows(dlg, [](HWND c, LPARAM) -> BOOL {
        wchar_t cls[16]; ::GetClassNameW(c, cls, 16);
        if (_wcsicmp(cls, L"Static")) return TRUE;
        LONG t = (LONG)::GetWindowLongPtr(c, GWL_STYLE) & SS_TYPEMASK;
        if ((t == SS_ICON && !::SendMessageW(c, STM_GETIMAGE, IMAGE_ICON, 0)) ||
            (t == SS_BITMAP && !::SendMessageW(c, STM_GETIMAGE, IMAGE_BITMAP, 0)))
            ::ShowWindow(c, SW_HIDE);
        return TRUE;
    }, 0);

    // 7. One-at-a-time overlap clusters (see the comment at ResolveOverlaps): some templates stack
    //    several controls at the identical rect because the player shows only one per setting's type.
    //    Layout is final now (widget pairs + spinners), so cluster detection sees real rects.
    ResolveOverlaps(dlg);

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

// The fit-measuring RULES themselves (mnemonic-aware text extent vs. client rect, row/gap grouping,
// combo-option measurement, hard/tight thresholds) now live in mpctrans::fit (studio/core/include/
// mpctrans/fit.h + src/fit.cpp) -- the ONE source of truth shared with the headless `fitscan` CLI
// (studio/cli/fitscan.cpp). Everything below is a thin wrapper supplying LivePreview's own state
// (m_english, m_dlgFont) to the portable functions.

std::vector<LivePreview::Overflow> LivePreview::DetectOverflow(HWND dlg) {
    std::vector<Overflow> out;
    if (!dlg) return out;
    ::EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL {
        auto* out = (std::vector<Overflow>*)lp;
        if (mpctrans::fit::control_overflows(child)) out->push_back({ child });
        return TRUE;
    }, (LPARAM)&out);
    return out;
}

// Re-measure arbitrary replacement text against a specific control's current font + client rect.
// Mirrors DetectOverflow's Button-padding rule; const (read-only).
LivePreview::TextFit LivePreview::MeasureControlText(HWND ctrl, const CString& text) const {
    return mpctrans::fit::measure_control_text(ctrl, std::wstring((LPCWSTR)text, text.GetLength()));
}

// One combo OPTION's fit against its combo's CLOSED field. Unlike MeasureFit (which reads the LIVE
// text already substituted into a rendered control), this measures arbitrary candidate strings — the
// combo's real items are filled at runtime in C++ (see combo_groups()'s comment in MainFrame.cpp), so
// there is no live control text to read; the caller supplies each option's translated text directly.
std::vector<LivePreview::ComboFitMeasurement> LivePreview::MeasureComboFit(HWND dlg, long long comboCtrlId,
        const std::vector<std::pair<std::string, CString>>& options) {
    std::vector<std::pair<std::string, std::wstring>> opts;
    opts.reserve(options.size());
    for (const auto& [msgctxt, text] : options)
        opts.emplace_back(msgctxt, std::wstring((LPCWSTR)text, text.GetLength()));
    return mpctrans::fit::measure_combo_fit(dlg, comboCtrlId, opts);
}

// Single-line, width-constrained fit measurement for every translatable control in the CURRENTLY
// RENDERED dialog `dlg` — see fit.h's measure_fit for the full scope/grouping rules. Passes m_dlgFont
// through so the multi-line-static line-height discriminator keeps its longstanding on-screen
// behavior byte-for-byte unchanged by this refactor (fitscan's headless render has no such cached
// font and falls back to the dialog's own WM_GETFONT instead).
std::vector<LivePreview::FitMeasurement> LivePreview::MeasureFit(HWND dlg, long long dialogId,
                                                                  const mpctrans::ControlIndex& idx) {
    return mpctrans::fit::measure_fit(dlg, dialogId, idx, m_english, m_dlgFont);
}
