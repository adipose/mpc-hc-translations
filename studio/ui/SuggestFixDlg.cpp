// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "SuggestFixDlg.h"
#include "resource.h"
#include "Theme.h"

using namespace mpctrans::corrections;

BEGIN_MESSAGE_MAP(SuggestFixDlg, CDialog)
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

SuggestFixDlg::SuggestFixDlg(CWnd* parent, const CString& msgctxt, const CString& english,
                             const CString& curMeaning, const CString& curFunction)
    : CDialog(IDD_SUGGEST_FIX, parent), m_msgctxt(msgctxt), m_msgid(english),
      m_curMeaning(curMeaning), m_curFunction(curFunction) {}

// Dark theme, same split as EditPanel::OnCtlColor: edits get the content colour, everything else
// (statics/buttons) the window colour. Buttons owner-draw via the Theme subclasses in ApplyToChildren.
HBRUSH SuggestFixDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
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
BOOL SuggestFixDlg::OnEraseBkgnd(CDC* pDC) {
    CRect rc; GetClientRect(&rc); pDC->FillSolidRect(rc, Theme::WINDOW_BG); return TRUE;
}

BOOL SuggestFixDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    Theme::InitTopWindow(GetSafeHwnd());
    { CClientDC dc(this); m_dpi = dc.GetDeviceCaps(LOGPIXELSX); }
    m_font.CreatePointFont(90, L"Segoe UI");   // 9pt, DPI-scaled

    // The DIALOGEX template's dialog-unit size is just a placeholder — size ourselves in real
    // device pixels (DPI-correct regardless of the template's font/DU conversion), then center.
    CRect wr(0, 0, S(480), S(430));
    ::AdjustWindowRectEx(&wr, (DWORD)GetStyle(), FALSE, (DWORD)GetExStyle());
    SetWindowPos(nullptr, 0, 0, wr.Width(), wr.Height(), SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow(GetParent());

    const DWORD ST = WS_CHILD | WS_VISIBLE;
    const DWORD ED = ST | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL;
    CRect z(0, 0, 0, 0);

    m_lblCtx.Create(L"Context: " + m_msgctxt, ST | SS_ENDELLIPSIS, z, this, 0);
    m_lblEnglish.Create(L"English", ST, z, this, 0);
    m_english.Create(ED | ES_READONLY | WS_TABSTOP, z, this, 0);
    m_english.SetWindowText(m_msgid);
    m_lblMeaning.Create(L"Meaning (semantic purpose)", ST, z, this, 0);
    m_meaning.Create(ED | WS_TABSTOP, z, this, 0);
    m_meaning.SetWindowText(m_curMeaning);
    m_lblFunction.Create(L"Function (functional purpose)", ST, z, this, 0);
    m_function.Create(ED | WS_TABSTOP, z, this, 0);
    m_function.SetWindowText(m_curFunction);
    m_lblNote.Create(L"Why / note (optional)", ST, z, this, 0);
    m_note.Create(ST | WS_BORDER | WS_TABSTOP, z, this, 0);
    m_btnSubmit.Create(L"&Submit", ST | WS_TABSTOP | BS_DEFPUSHBUTTON, z, this, IDOK);
    m_btnCancel.Create(L"Cancel", ST | WS_TABSTOP, z, this, IDCANCEL);

    for (CWnd* w : std::initializer_list<CWnd*>{ &m_lblCtx, &m_lblEnglish, &m_english, &m_lblMeaning,
             &m_meaning, &m_lblFunction, &m_function, &m_lblNote, &m_note, &m_btnSubmit, &m_btnCancel })
        w->SetFont(&m_font);
    Theme::ApplyToChildren(GetSafeHwnd());
    Layout();
    m_meaning.SetFocus();
    return FALSE;   // focus set explicitly above
}

void SuggestFixDlg::Layout() {
    CRect rc; GetClientRect(&rc);
    const int M = S(10), W = rc.Width() - 2 * M, gap = S(6);
    int y = M;
    auto place = [&](CWnd& w, int h) { w.MoveWindow(M, y, W, S(h)); y += S(h) + gap; };
    place(m_lblCtx, 18);
    place(m_lblEnglish, 16);
    place(m_english, 44);
    place(m_lblMeaning, 16);
    place(m_meaning, 70);
    place(m_lblFunction, 16);
    place(m_function, 70);
    place(m_lblNote, 16);
    place(m_note, 22);
    const int btnW = S(90), btnH = S(28);
    m_btnCancel.MoveWindow(rc.Width() - M - btnW, y, btnW, btnH);
    m_btnSubmit.MoveWindow(rc.Width() - M - 2 * btnW - S(8), y, btnW, btnH);
}

// Only fields whose text actually changed become corrections. Zero changes -> tell the user and
// keep the dialog open (no PR, no EndDialog) rather than silently closing with nothing to submit.
void SuggestFixDlg::OnOK() {
    CString meaning, function, note;
    m_meaning.GetWindowText(meaning);
    m_function.GetWindowText(function);
    m_note.GetWindowText(note);

    m_changed.clear();
    std::string ctx  = std::string(CW2A(m_msgctxt, CP_UTF8));
    std::string mid  = std::string(CW2A(m_msgid, CP_UTF8));
    std::string noteUtf8 = std::string(CW2A(note, CP_UTF8));
    if (meaning != m_curMeaning)
        m_changed.push_back({ ctx, mid, Field::SemanticPurpose,
                              std::string(CW2A(m_curMeaning, CP_UTF8)), std::string(CW2A(meaning, CP_UTF8)),
                              noteUtf8 });
    if (function != m_curFunction)
        m_changed.push_back({ ctx, mid, Field::FunctionalPurpose,
                              std::string(CW2A(m_curFunction, CP_UTF8)), std::string(CW2A(function, CP_UTF8)),
                              noteUtf8 });

    if (m_changed.empty()) {
        MessageBox(L"No changes to submit — edit the Meaning or Function text first.",
                  L"Suggest a correction", MB_ICONINFORMATION);
        return;   // keep the dialog open
    }
    CDialog::OnOK();
}
