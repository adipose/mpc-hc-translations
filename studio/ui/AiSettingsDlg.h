// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <afxwin.h>
#include <vector>
#include "mpctrans/ai_client.h"

// "AI provider…" modal (File > AI provider…), also popped automatically from EditPanel's "Suggest
// with AI" button when no API key is configured for the currently selected provider (see
// MainFrame::OnSuggestAiClicked). Same programmatic, empty-DIALOGEX-template style as SuggestFixDlg:
// every control is created in OnInitDialog, no RC control layout.
//
// On Save: persists the chosen provider id + model string via WriteProfileString (section "AI",
// entries "Provider" / "Model" — see MainFrame's persistence, same profile the theme mode uses) and,
// if the API-key field holds real (non-placeholder) user-typed text, stores it in the Windows
// Credential Manager via mpctrans::github::store_token against that provider's cred target
// (L"mpc-hc-translation-studio/ai/<provider_id>").
class AiSettingsDlg : public CDialog {
public:
    explicit AiSettingsDlg(CWnd* parent);

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    afx_msg HBRUSH OnCtlColor(CDC*, CWnd*, UINT);
    afx_msg BOOL OnEraseBkgnd(CDC*);
    afx_msg void OnProviderChanged();
    afx_msg void OnModelChanged();
    afx_msg void OnKeyChanged();
    DECLARE_MESSAGE_MAP()

private:
    void Layout();
    int  S(int v) const { return ::MulDiv(v, m_dpi, 96); }   // 96-DPI-logical px -> device px
    int  m_dpi = 96;
    CFont m_font;

    // Fills m_model with the persisted model (if selected provider matches the persisted provider
    // id and a model was saved) or the provider's default_model; also refreshes the API-key field's
    // placeholder state for the newly selected provider.
    void RefreshForSelectedProvider();
    const mpctrans::ProviderInfo* SelectedProvider() const;

    std::vector<mpctrans::ProviderInfo> m_providers;
    CString m_persistedProviderId, m_persistedModel;

    CStatic   m_lblProvider, m_lblModel, m_lblKey, m_hint;
    CComboBox m_provider;
    CEdit     m_model, m_key;
    CButton   m_btnSave, m_btnCancel;

    // EN_CHANGE fires for both programmatic SetWindowText and real user typing; compare against the
    // last text WE set to tell them apart (same pattern for both the model and key fields).
    CString m_lastSetModelText;
    bool    m_modelEdited = false;
    CString m_lastSetKeyText;
    bool    m_keyFieldIsPlaceholder = false;
};
