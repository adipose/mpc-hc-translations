// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <afxwin.h>
#include <vector>
#include "mpctrans/corrections.h"

// "Suggest fix…" modal — invoked from the research panel (MainFrame::m_research) for the currently
// selected string. Shows the string (context + English, read-only) and two multiline edits prefilled
// with the CURRENT Meaning (semantic_purpose) / Function (functional_purpose), plus an optional
// single-line "why" note. Submit is a no-op (dialog stays open) unless at least one field's text
// actually changed. No RC control layout — IDD_SUGGEST_FIX is an empty DIALOGEX template; every
// child is created programmatically in OnInitDialog, same control-creation style as EditPanel.
class SuggestFixDlg : public CDialog {
public:
    SuggestFixDlg(CWnd* parent, const CString& msgctxt, const CString& english,
                 const CString& curMeaning, const CString& curFunction);

    // Valid after DoModal() returns IDOK: only the field(s) whose text actually differs from the
    // prefilled current value (0, 1, or 2 entries). Empty means "no changes" (OnOK already told the
    // user and kept the dialog open, so IDOK is only ever returned with >=1 entry here).
    const std::vector<mpctrans::corrections::Correction>& Changed() const { return m_changed; }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg HBRUSH OnCtlColor(CDC*, CWnd*, UINT);
    afx_msg BOOL OnEraseBkgnd(CDC*);
    DECLARE_MESSAGE_MAP()

private:
    void Layout();
    int  S(int v) const { return ::MulDiv(v, m_dpi, 96); }   // 96-DPI-logical px -> device px
    int  m_dpi = 96;
    CFont m_font;

    CString m_msgctxt, m_msgid, m_curMeaning, m_curFunction;

    CStatic m_lblCtx, m_lblEnglish, m_lblMeaning, m_lblFunction, m_lblNote;
    CEdit   m_english, m_meaning, m_function, m_note;
    CButton m_btnSubmit, m_btnCancel;

    std::vector<mpctrans::corrections::Correction> m_changed;
};
