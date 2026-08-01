// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "EditPanel.h"
#include "resource.h"
#include "Theme.h"

using namespace mpctrans;

// Embedded control characters render invisibly in the edit boxes, which misleads translators:
// tabs as silent tab stops (several strings are "switch\tdescription"), and bare-LF line breaks as
// NOTHING AT ALL (a Windows edit control only breaks on CRLF, so "a.\nb" displays as "a.b" — issue
// #5). Show both as visible glyphs, Transifex-style, and convert back when reading the value. The
// ⏎ glyph is decoration for the eye; the CRLF that follows it carries the actual break — so a
// translator who types plain Enter still gets a break, and deleting a lone ⏎ removes nothing but
// the marker's line break stays visible as the wrap it no longer has.
static const wchar_t* kTabGlyph = L"⇥";   // ⇥ (visible tab marker)
static const wchar_t* kNlGlyph  = L"⏎";   // ⏎ (visible line-break marker, precedes the real CRLF)
static CString showCtl(const CString& s) {
    CString t(s);
    t.Replace(L"\t", kTabGlyph);
    t.Replace(L"\r\n", L"\n");                          // normalize first (idempotent round-trips)
    t.Replace(L"\n", CString(kNlGlyph) + L"\r\n");
    return t;
}
static CString hideCtl(const CString& s) {
    CString t(s);
    t.Replace(kTabGlyph, L"\t");
    t.Replace(kNlGlyph, L"");                           // the break itself is the CRLF, glyph or not
    t.Replace(L"\r\n", L"\n");                          // PO strings carry bare \n
    return t;
}

BEGIN_MESSAGE_MAP(EditPanel, CWnd)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
    ON_BN_CLICKED(IDC_EP_APPLY, OnApply)
    ON_BN_CLICKED(IDC_EP_AI_ACCEPT, OnAcceptAi)
    ON_BN_CLICKED(IDC_EP_AI_SUGGEST, OnSuggestAiClicked)
    ON_LBN_DBLCLK(IDC_EP_REFS, OnRefDblClk)
    ON_BN_CLICKED(IDC_EP_INS_NL, OnInsertNl)
    ON_BN_CLICKED(IDC_EP_INS_TAB, OnInsertTab)
    ON_EN_CHANGE(IDC_EP_TRANSLATION, OnTranslationChanged)
END_MESSAGE_MAP()

BOOL EditPanel::CreatePanel(CWnd* parent, UINT id) {
    return Create(AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), Theme::windowBrush()),
                  L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                  CRect(0, 0, 100, 100), parent, id);
}

// Dark theme: panel background (OnEraseBkgnd) + labels/edits/listbox (OnCtlColor). Read-only edits
// (English source, validation strip) arrive as CTLCOLOR_STATIC, so route the "Edit" class to the
// content colour and everything else (real labels) to the window colour. Buttons owner-draw via
// the Theme subclasses installed at the end of OnCreate.
HBRUSH EditPanel::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
    if (nCtlColor == CTLCOLOR_EDIT || nCtlColor == CTLCOLOR_LISTBOX)
        return Theme::contentCtl(pDC->GetSafeHdc());
    if (nCtlColor == CTLCOLOR_STATIC) {
        wchar_t cls[16] = L""; if (pWnd) ::GetClassNameW(pWnd->GetSafeHwnd(), cls, 16);
        return _wcsicmp(cls, L"Edit") == 0 ? Theme::contentCtl(pDC->GetSafeHdc())
                                           : Theme::windowCtl(pDC->GetSafeHdc());
    }
    if (nCtlColor == CTLCOLOR_BTN)
        return Theme::windowCtl(pDC->GetSafeHdc());
    return CWnd::OnCtlColor(pDC, pWnd, nCtlColor);
}
BOOL EditPanel::OnEraseBkgnd(CDC* pDC) {
    CRect rc; GetClientRect(&rc); pDC->FillSolidRect(rc, Theme::WINDOW_BG); return TRUE;
}

// This is a plain CWnd (not a dialog), so nothing routes Tab between controls or handles Ctrl+A in
// the edits — do it here (PreTranslateMessage runs for keystrokes in any descendant).
BOOL EditPanel::PreTranslateMessage(MSG* pMsg) {
    if (pMsg->message == WM_KEYDOWN) {
        bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && pMsg->wParam == 'A') {                 // Ctrl+A -> select all in the focused edit
            wchar_t cls[16] = L""; ::GetClassNameW(pMsg->hwnd, cls, 16);
            if (_wcsicmp(cls, L"Edit") == 0) { ::SendMessage(pMsg->hwnd, EM_SETSEL, 0, (LPARAM)-1); return TRUE; }
        }
        if (!ctrl && pMsg->wParam == VK_TAB) {             // Tab / Shift+Tab -> move focus between controls
            bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (CWnd* next = GetNextDlgTabItem(GetFocus(), shift)) { next->SetFocus(); return TRUE; }
        }
    }
    return CWnd::PreTranslateMessage(pMsg);
}

int EditPanel::OnCreate(LPCREATESTRUCT lpcs) {
    if (CWnd::OnCreate(lpcs) == -1) return -1;
    { CClientDC dc(this); m_dpi = dc.GetDeviceCaps(LOGPIXELSX); }
    m_font.CreatePointFont(90, L"Segoe UI");            // 9pt, DPI-scaled
    const DWORD ST = WS_CHILD | WS_VISIBLE;
    const DWORD ED = ST | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL;
    CRect z(0, 0, 0, 0);

    m_ctx.Create(L"", ST | SS_ENDELLIPSIS, z, this, IDC_EP_CONTEXT);
    m_lblEnglish.Create(L"English", ST, z, this, 0);
    m_english.Create(ED | ES_READONLY | WS_TABSTOP, z, this, IDC_EP_ENGLISH);   // tabbable to read/copy
    m_lblCurrent.Create(L"Translation", ST, z, this, 0);
    // Transifex-style control-char chips: click to insert the (visible-glyph) char at the caret.
    m_btnInsNl.Create(kNlGlyph, ST | WS_TABSTOP, z, this, IDC_EP_INS_NL);
    m_btnInsTab.Create(kTabGlyph, ST | WS_TABSTOP, z, this, IDC_EP_INS_TAB);
    m_translation.Create(ED | WS_TABSTOP, z, this, IDC_EP_TRANSLATION);
    m_btnApply.Create(L"&Apply edit", ST | WS_TABSTOP | BS_DEFPUSHBUTTON, z, this, IDC_EP_APPLY);
    m_lblAi.Create(L"AI suggestion", ST, z, this, 0);
    m_btnAcceptAi.Create(L"Use AI &suggestion", ST | WS_TABSTOP, z, this, IDC_EP_AI_ACCEPT);
    m_btnAiSuggest.Create(L"Suggest with A&I", ST | WS_TABSTOP, z, this, IDC_EP_AI_SUGGEST);
    m_lblRefs.Create(L"Commonly used translations (Windows apps) — double-click to insert", ST, z, this, 0);
    m_refs.Create(ST | WS_BORDER | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | LBS_NOTIFY,
                  z, this, IDC_EP_REFS);
    m_info.Create(L"", ST, z, this, IDC_EP_INFO);
    m_validation.Create(ED | ES_READONLY | WS_TABSTOP, z, this, IDC_EP_VALIDATION);

    for (CWnd* w : std::initializer_list<CWnd*>{ &m_ctx, &m_lblEnglish, &m_english, &m_lblCurrent,
             &m_btnInsNl, &m_btnInsTab,
             &m_translation, &m_btnApply, &m_lblAi, &m_btnAcceptAi, &m_btnAiSuggest, &m_lblRefs, &m_refs,
             &m_info, &m_validation })
        w->SetFont(&m_font);
    m_symFont.CreatePointFont(90, L"Segoe UI Symbol");
    m_btnInsNl.SetFont(&m_symFont);
    m_btnInsTab.SetFont(&m_symFont);
    Theme::ApplyToChildren(GetSafeHwnd());   // owner-draw the Apply / Accept-AI buttons
    ClearString();
    return 0;
}

void EditPanel::Layout() {
    if (!m_translation.GetSafeHwnd()) return;
    CRect rc; GetClientRect(&rc);
    const int M = S(8), W = rc.Width() - 2 * M, gap = S(5);
    int y = M;
    auto place = [&](CWnd& w, int h) { w.MoveWindow(M, y, W, S(h)); y += S(h) + gap; };
    place(m_ctx, 20);
    place(m_lblEnglish, 18);
    place(m_english, 58);
    {   // Translation label row, with the ⏎ / ⇥ insert chips right-aligned on the same line
        int h = S(20), bw = S(30);
        m_lblCurrent.MoveWindow(M, y, W - 2 * bw - 2 * gap, S(18));
        m_btnInsNl.MoveWindow(M + W - 2 * bw - gap, y - S(2), bw, h);
        m_btnInsTab.MoveWindow(M + W - bw, y - S(2), bw, h);
        y += S(18) + gap;
    }
    place(m_translation, 76);            // the editable field gets the most room
    m_btnApply.MoveWindow(M, y, S(130), S(30)); y += S(30) + gap;
    place(m_lblAi, 40);                  // clean "AI suggestion: <text>" one-liner (wraps to ~2 lines
                                          // for long text); metrics/attribution live in m_validation now
    m_btnAcceptAi.MoveWindow(M, y, S(170), S(28));
    m_btnAiSuggest.MoveWindow(M + S(170) + gap, y, S(150), S(28));
    y += S(28) + gap;
    place(m_lblRefs, 18);
    place(m_refs, 120);
    place(m_info, 96);
    int rest = rc.Height() - y - M;
    m_validation.MoveWindow(M, y, W, rest > S(60) ? rest : S(60));
}
void EditPanel::OnSize(UINT t, int cx, int cy) { CWnd::OnSize(t, cx, cy); Layout(); }

void EditPanel::ShowString(const CString& msgctxt, const CString& english, const CString& current,
                           const std::vector<AiSuggestion>& ai,
                           const std::vector<ReferenceMatch>& refs, const StringInfo* info) {
    m_active = true;
    m_msgctxt = msgctxt;
    m_msgid = english;
    m_overflowNote.Empty();
    m_ctx.SetWindowText(L"Context: " + msgctxt);
    m_english.SetWindowText(showCtl(english));
    m_translation.SetWindowText(showCtl(current));
    m_translation.EnableWindow(TRUE);
    m_btnApply.EnableWindow(TRUE);
    m_btnInsNl.EnableWindow(TRUE);
    m_btnInsTab.EnableWindow(TRUE);

    // A fresh selection cancels any stale "requesting…" state left over from an on-demand AI
    // suggestion for the PREVIOUS string (OnAiSuggestDone drops stale results, but the UI mustn't be
    // left showing "requesting…" forever if the user navigated away before the reply arrived).
    m_aiRequestInFlight = false;
    m_btnAiSuggest.EnableWindow(TRUE);
    m_btnAiSuggest.ShowWindow(SW_SHOW);   // always available while a string is active

    // The suggestion (when the language pack has one) is a machine-proposed msgstr; the button
    // copies it into the Translation box and applies it as an edit. Hide the button entirely when
    // there is nothing to accept -- a permanently disabled "Accept AI" read as a mystery.
    if (!ai.empty()) {
        m_aiText = CA2W(ai.front().msgstr.c_str(), CP_UTF8);
        m_lblAi.SetWindowText(L"AI suggestion: " + m_aiText);
        m_btnAcceptAi.EnableWindow(TRUE);
        m_btnAcceptAi.ShowWindow(SW_SHOW);
    } else {
        m_aiText.Empty();
        m_lblAi.SetWindowText(L"AI suggestion: none for this string");
        m_btnAcceptAi.EnableWindow(FALSE);
        m_btnAcceptAi.ShowWindow(SW_HIDE);
    }

    m_refs.ResetContent();
    m_refTexts.clear();
    for (const auto& r : refs) {
        CString t(CA2W(r.msgstr.c_str(), CP_UTF8));   // the translation only -- source is not identified
        m_refs.AddString(t);
        m_refTexts.push_back(t);
    }

    CString ctx;
    if (info) {
        auto add = [&](const wchar_t* label, const std::string& v) {
            if (!v.empty()) ctx += CString(label) + CString(CA2W(v.c_str(), CP_UTF8)) + L"\n";
        };
        add(L"Where: ", info->ui_location);
        add(L"Purpose: ", info->semantic_purpose);
        add(L"Type: ", info->ui_type);
    }
    m_info.SetWindowText(ctx);
    RunInlineValidation();
}

void EditPanel::ClearString() {
    m_active = false;
    m_msgctxt.Empty(); m_msgid.Empty(); m_aiText.Empty(); m_overflowNote.Empty(); m_flagNote.Empty();
    m_ctx.SetWindowText(L"Context: (select a string)");
    m_english.SetWindowText(L"");
    m_translation.SetWindowText(L"");
    m_translation.EnableWindow(FALSE);
    m_btnApply.EnableWindow(FALSE);
    m_btnInsNl.EnableWindow(FALSE);
    m_btnInsTab.EnableWindow(FALSE);
    m_lblAi.SetWindowText(L"AI suggestion: none for this string");
    m_btnAcceptAi.EnableWindow(FALSE);
    m_btnAcceptAi.ShowWindow(SW_HIDE);
    m_aiRequestInFlight = false;
    m_btnAiSuggest.EnableWindow(FALSE);
    m_btnAiSuggest.ShowWindow(SW_HIDE);
    m_refs.ResetContent();
    m_refTexts.clear();
    m_info.SetWindowText(L"");
    m_validation.SetWindowText(L"");
}

void EditPanel::ShowOverflowWarning(bool overflowing) {
    m_overflowNote = overflowing
        ? L"OVERFLOW: translated text is wider than the control in the preview\r\n" : L"";
    RunInlineValidation();
}

void EditPanel::SetFlagNote(const CString& note) {
    m_flagNote = note.IsEmpty() ? CString() : (note + L"\r\n");
    RunInlineValidation();
}

CString EditPanel::GetEdited() const {
    CString s; m_translation.GetWindowText(s); return hideCtl(s);
}

void EditPanel::OnApply() {
    if (m_active && OnCommit) OnCommit(GetEdited());
}
void EditPanel::OnAcceptAi() {
    if (!m_active || m_aiText.IsEmpty()) return;
    m_translation.SetWindowText(showCtl(m_aiText));
    if (OnCommit) OnCommit(m_aiText);
}
// Double-clicked reference: insert its translation into the edit box but do NOT commit -- unlike an
// AI suggestion (generated for this exact string), a reference comes from another app and usually
// needs adapting (accelerator conventions, trailing colons) before Apply.
void EditPanel::OnRefDblClk() {
    int i = m_refs.GetCurSel();
    if (!m_active || i == LB_ERR || i >= (int)m_refTexts.size()) return;
    m_translation.SetWindowText(showCtl(m_refTexts[i]));
    m_translation.SetFocus();
    int n = m_translation.GetWindowTextLength();
    m_translation.SetSel(n, n);        // caret at the end, ready to adjust then Apply
    RunInlineValidation();
}
// The chips insert the DISPLAY form (glyph + CRLF / glyph); hideCtl converts on read like any typed
// text. The edit keeps its selection while unfocused, so clicking a chip replaces/inserts exactly at
// the translator's caret. ReplaceSel(TRUE) makes it a single Ctrl+Z-undoable edit and fires
// EN_CHANGE -> live validation.
void EditPanel::OnInsertNl() {
    if (!m_active) return;
    m_translation.SetFocus();
    m_translation.ReplaceSel(CString(kNlGlyph) + L"\r\n", TRUE);
}
void EditPanel::OnInsertTab() {
    if (!m_active) return;
    m_translation.SetFocus();
    m_translation.ReplaceSel(kTabGlyph, TRUE);
}
void EditPanel::OnTranslationChanged() { if (m_active) RunInlineValidation(); }

// "Suggest with AI" — MainFrame owns the provider config/API key and the background HTTP call; this
// panel only reflects the request's lifecycle (requesting / suggestion / error). Ignore repeat clicks
// while a request is already in flight for the current string.
void EditPanel::OnSuggestAiClicked() {
    if (m_active && !m_aiRequestInFlight && OnSuggestAi) OnSuggestAi();
}
void EditPanel::SetAiSuggestRequesting() {
    m_aiRequestInFlight = true;
    m_btnAiSuggest.EnableWindow(FALSE);
    m_lblAi.SetWindowText(L"AI suggestion: requesting\x2026");
}
void EditPanel::SetAiSuggestion(const CString& text) {
    m_aiRequestInFlight = false;
    m_aiText = text;
    m_lblAi.SetWindowText(L"AI suggestion: " + m_aiText);
    m_btnAcceptAi.EnableWindow(TRUE);
    m_btnAcceptAi.ShowWindow(SW_SHOW);
    m_btnAiSuggest.EnableWindow(TRUE);
}
void EditPanel::SetAiSuggestError(const CString& shortMessage) {
    m_aiRequestInFlight = false;
    m_btnAiSuggest.EnableWindow(TRUE);
    m_lblAi.SetWindowText(L"AI suggestion: " + shortMessage);
    // Deliberately leave m_aiText / m_btnAcceptAi untouched -- a failed request shouldn't clobber
    // whatever suggestion (bundled or a prior successful AI call) was already accepted/available.
}
void EditPanel::AppendValidationLine(const CString& line) {
    CString cur; m_validation.GetWindowText(cur);
    if (cur.IsEmpty() || cur == L"(no findings)") cur = line;
    else cur += L"\r\n" + line;
    m_validation.SetWindowText(cur);
}
void EditPanel::SetAiDetails(const CString& primaryModel, const CString& primaryText,
                             const CString& altModel, const CString& altText,
                             bool hasAgreement, double agreement,
                             bool hasBackScore, double backScore,
                             bool lowConfidence) {
    CString block;
    if (lowConfidence) block += L"Review recommended\r\n";
    block += L"Primary (" + primaryModel + L"): " + primaryText + L"\r\n";

    CString altTrim(altText); altTrim.Trim();
    CString primTrim(primaryText); primTrim.Trim();
    if (!altTrim.IsEmpty() && altTrim != primTrim)
        block += L"Alternate (" + altModel + L"): " + altText + L"\r\n";

    if (hasAgreement || hasBackScore) {
        CString scoreLine;
        if (hasAgreement) scoreLine.Format(L"Agreement %.2f", agreement);
        if (hasBackScore) {
            CString bt; bt.Format(L"Back-translation %.2f", backScore);
            scoreLine = scoreLine.IsEmpty() ? bt : (scoreLine + L" \xB7 " + bt);
        }
        block += scoreLine + L"\r\n";
    }

    CString cur; m_validation.GetWindowText(cur);
    if (cur.IsEmpty() || cur == L"(no findings)") cur = block;
    else cur += L"\r\n" + block;
    m_validation.SetWindowText(cur);
}
void EditPanel::ShowAiSuggestionValidation(const std::vector<validate::Finding>& findings) {
    if (findings.empty()) return;
    CString cur; m_validation.GetWindowText(cur);
    if (!cur.IsEmpty() && cur != L"(no findings)") cur += L"\r\n";
    else cur.Empty();
    cur += L"--- AI suggestion ---\r\n";
    for (const auto& f : findings) {
        const wchar_t* sev = f.sev == validate::Severity::Error   ? L"ERROR"
                           : f.sev == validate::Severity::Warning ? L"warning" : L"info";
        cur += CString(sev) + L" [" + CString(CA2W(f.rule.c_str(), CP_UTF8)) + L"] " +
               CString(CA2W(f.msg.c_str(), CP_UTF8)) + L"\r\n";
    }
    m_validation.SetWindowText(cur);
}

void EditPanel::RunInlineValidation() {
    if (!m_active) return;
    std::string ctx = CW2A(m_msgctxt, CP_UTF8), msgid = CW2A(m_msgid, CP_UTF8),
                msgstr = CW2A(GetEdited(), CP_UTF8);
    CString out = m_flagNote + m_overflowNote;
    for (const auto& f : validate::check_entry(ctx, msgid, msgstr)) {
        const wchar_t* sev = f.sev == validate::Severity::Error   ? L"ERROR"
                           : f.sev == validate::Severity::Warning ? L"warning" : L"info";
        out += CString(sev) + L" [" + CString(CA2W(f.rule.c_str(), CP_UTF8)) + L"] " +
               CString(CA2W(f.msg.c_str(), CP_UTF8)) + L"\r\n";
    }
    m_validation.SetWindowText(out.IsEmpty() ? L"(no findings)" : out);
}
