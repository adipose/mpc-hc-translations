// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "TxTokenDlg.h"
#include "resource.h"
#include "Theme.h"

using namespace mpctrans;

BEGIN_MESSAGE_MAP(TxTokenDlg, CDialog)
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
    ON_EN_CHANGE(IDC_TXTOKEN_KEY, OnKeyChanged)
    ON_BN_CLICKED(IDC_TXTOKEN_CLEAR, OnClearClicked)
END_MESSAGE_MAP()

TxTokenDlg::TxTokenDlg(CWnd* parent) : CDialog(IDD_TXTOKEN, parent) {}

// Dark theme, same split as every other code-driven dialog here (edits get the content colour,
// everything else the window colour) -- copied from AiSettingsDlg.
HBRUSH TxTokenDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
    if (nCtlColor == CTLCOLOR_EDIT || nCtlColor == CTLCOLOR_LISTBOX)
        return Theme::contentCtl(pDC->GetSafeHdc());
    if (nCtlColor == CTLCOLOR_STATIC) {
        wchar_t cls[16] = L""; if (pWnd) ::GetClassNameW(pWnd->GetSafeHwnd(), cls, 16);
        return _wcsicmp(cls, L"Edit") == 0 ? Theme::contentCtl(pDC->GetSafeHdc())
                                           : Theme::windowCtl(pDC->GetSafeHdc());
    }
    if (nCtlColor == CTLCOLOR_BTN || nCtlColor == CTLCOLOR_DLG)
        return Theme::windowCtl(pDC->GetSafeHdc());
    return CDialog::OnCtlColor(pDC, pWnd, nCtlColor);
}
BOOL TxTokenDlg::OnEraseBkgnd(CDC* pDC) {
    CRect rc; GetClientRect(&rc); pDC->FillSolidRect(rc, Theme::WINDOW_BG); return TRUE;
}

BOOL TxTokenDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    Theme::InitTopWindow(GetSafeHwnd());
    { CClientDC dc(this); m_dpi = dc.GetDeviceCaps(LOGPIXELSX); }
    m_font.CreatePointFont(90, L"Segoe UI");

    CRect wr(0, 0, S(420), S(190));
    ::AdjustWindowRectEx(&wr, (DWORD)GetStyle(), FALSE, (DWORD)GetExStyle());
    SetWindowPos(nullptr, 0, 0, wr.Width(), wr.Height(), SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow(GetParent());

    const DWORD ST = WS_CHILD | WS_VISIBLE;
    CRect z(0, 0, 0, 0);

    m_hint.Create(L"Transifex API token (generate at https://app.transifex.com/user/settings/api/). "
                 L"Stored encrypted in Windows Credential Manager, used only for Transifex API calls.",
                 ST | SS_LEFT, z, this, IDC_TXTOKEN_HINT);
    m_key.Create(ST | WS_BORDER | WS_TABSTOP | ES_PASSWORD, z, this, IDC_TXTOKEN_KEY);
    m_btnSave.Create(L"&Save", ST | WS_TABSTOP | BS_DEFPUSHBUTTON, z, this, IDOK);
    m_btnClear.Create(L"C&lear", ST | WS_TABSTOP, z, this, IDC_TXTOKEN_CLEAR);
    m_btnCancel.Create(L"Cancel", ST | WS_TABSTOP, z, this, IDCANCEL);

    // Pre-fill the masked placeholder when a token is already stored -- the real value is never
    // shown, here or anywhere else.
    if (txapi::load_token()) {
        m_keyFieldIsPlaceholder = true;
        m_lastSetKeyText = L"********";
    } else {
        m_keyFieldIsPlaceholder = false;
        m_lastSetKeyText.Empty();
    }
    m_key.SetWindowText(m_lastSetKeyText);

    for (CWnd* w : std::initializer_list<CWnd*>{ &m_hint, &m_key, &m_btnSave, &m_btnClear, &m_btnCancel })
        w->SetFont(&m_font);
    Theme::ApplyToChildren(GetSafeHwnd());

    Layout();
    m_key.SetFocus();
    return FALSE;   // focus set explicitly above
}

void TxTokenDlg::Layout() {
    CRect rc; GetClientRect(&rc);
    const int M = S(10), W = rc.Width() - 2 * M, gap = S(8);
    int y = M;
    auto place = [&](CWnd& w, int h) { w.MoveWindow(M, y, W, S(h)); y += S(h) + gap; };
    place(m_hint, 54);
    place(m_key, 24);
    const int btnW = S(90), btnH = S(28);
    m_btnCancel.MoveWindow(rc.Width() - M - btnW, y, btnW, btnH);
    m_btnSave.MoveWindow(rc.Width() - M - 2 * btnW - S(8), y, btnW, btnH);
    m_btnClear.MoveWindow(rc.Width() - M - 3 * btnW - S(16), y, btnW, btnH);
}

// Real typing vs. our own SetWindowText -- same EN_CHANGE disambiguation AiSettingsDlg's key field
// uses: once the user has typed anything different from the placeholder, it's no longer "unchanged".
void TxTokenDlg::OnKeyChanged() {
    CString cur; m_key.GetWindowText(cur);
    if (cur != m_lastSetKeyText) m_keyFieldIsPlaceholder = false;
}

// Clear is a direct action (not gated behind Save) -- wipes the stored token right away and closes,
// same as clicking Save would with an empty, non-placeholder field.
void TxTokenDlg::OnClearClicked() {
    txapi::clear_token();
    EndDialog(IDOK);
}

void TxTokenDlg::OnOK() {
    CString key; m_key.GetWindowText(key);
    if (!key.IsEmpty() && !m_keyFieldIsPlaceholder)
        txapi::store_token(txapi::Token{ std::string(CW2A(key, CP_UTF8)) });
    // Placeholder still showing (unchanged) or field left empty (nothing typed) -- either way, leave
    // whatever is already stored (or not) alone.
    CDialog::OnOK();
}
void TxTokenDlg::OnCancel() { CDialog::OnCancel(); }   // no persistence

std::optional<txapi::Token> TxTokenDlg::Ensure(CWnd* owner) {
    if (auto tok = txapi::load_token()) return tok;
    TxTokenDlg dlg(owner);
    if (dlg.DoModal() != IDOK) return std::nullopt;
    return txapi::load_token();
}
