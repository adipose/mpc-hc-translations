// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <afxwin.h>
#include <optional>
#include "mpctrans/txapi.h"

// "Transifex token…" modal (File > Transifex token…), also popped by TxTokenDlg::Ensure when a
// background flow (TxSyncDlg's "Push to Transifex…") needs a Transifex API token and none is stored
// yet — the Transifex analogue of AiSettingsDlg's API-key field. Same programmatic, empty-DIALOGEX-
// template style as AiSettingsDlg/TxSyncDlg: every control is created in OnInitDialog, no RC control
// layout.
//
// On Save: if the edit still shows the "********" placeholder (meaning "leave the stored token
// alone"), nothing is written; otherwise the edit's text is stored via mpctrans::txapi::store_token.
// On Clear: mpctrans::txapi::clear_token() immediately, then the dialog closes. The real token value
// is never displayed once stored — only ever the masked placeholder.
class TxTokenDlg : public CDialog {
public:
    explicit TxTokenDlg(CWnd* parent);

    // Returns the already-stored token (no dialog shown) if one exists, else runs this modal and
    // returns whatever ends up stored afterward — nullopt if the user cancelled (or cleared) without
    // a token ending up stored. The Transifex analogue of TxSyncDlg.cpp's EnsureGithubToken.
    static std::optional<mpctrans::txapi::Token> Ensure(CWnd* owner);

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    afx_msg HBRUSH OnCtlColor(CDC*, CWnd*, UINT);
    afx_msg BOOL OnEraseBkgnd(CDC*);
    afx_msg void OnKeyChanged();
    afx_msg void OnClearClicked();
    DECLARE_MESSAGE_MAP()

private:
    void Layout();
    int  S(int v) const { return ::MulDiv(v, m_dpi, 96); }   // 96-DPI-logical px -> device px
    int  m_dpi = 96;
    CFont m_font;

    CStatic m_hint;
    CEdit   m_key;
    CButton m_btnSave, m_btnClear, m_btnCancel;

    // EN_CHANGE fires for both programmatic SetWindowText and real user typing; compare against the
    // last text WE set to tell them apart (same pattern as AiSettingsDlg's key field).
    CString m_lastSetKeyText;
    bool    m_keyFieldIsPlaceholder = false;
};
