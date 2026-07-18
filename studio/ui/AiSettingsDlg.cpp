// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "AiSettingsDlg.h"
#include "resource.h"
#include "Theme.h"
#include "mpctrans/github.h"

using namespace mpctrans;

BEGIN_MESSAGE_MAP(AiSettingsDlg, CDialog)
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
    ON_CBN_SELCHANGE(IDC_AI_PROVIDER, OnProviderChanged)
    ON_EN_CHANGE(IDC_AI_MODEL, OnModelChanged)
    ON_EN_CHANGE(IDC_AI_KEY, OnKeyChanged)
END_MESSAGE_MAP()

// Cred target for a provider's stored API key — the 4 distinct targets noted in ai_client.h's
// header comment (one per provider id: claude/gpt/glm/openrouter). Provider ids are plain ASCII.
static std::wstring AiCredTarget(const std::string& provider_id) {
    return std::wstring(L"mpc-hc-translation-studio/ai/") + std::wstring(provider_id.begin(), provider_id.end());
}

AiSettingsDlg::AiSettingsDlg(CWnd* parent) : CDialog(IDD_AI_SETTINGS, parent) {}

HBRUSH AiSettingsDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
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
BOOL AiSettingsDlg::OnEraseBkgnd(CDC* pDC) {
    CRect rc; GetClientRect(&rc); pDC->FillSolidRect(rc, Theme::WINDOW_BG); return TRUE;
}

BOOL AiSettingsDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    Theme::InitTopWindow(GetSafeHwnd());
    { CClientDC dc(this); m_dpi = dc.GetDeviceCaps(LOGPIXELSX); }
    m_font.CreatePointFont(90, L"Segoe UI");

    CRect wr(0, 0, S(420), S(280));
    ::AdjustWindowRectEx(&wr, (DWORD)GetStyle(), FALSE, (DWORD)GetExStyle());
    SetWindowPos(nullptr, 0, 0, wr.Width(), wr.Height(), SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow(GetParent());

    const DWORD ST = WS_CHILD | WS_VISIBLE;
    CRect z(0, 0, 0, 0);

    m_providers = list_providers();
    CWinApp* app = AfxGetApp();
    m_persistedProviderId = app ? app->GetProfileString(L"AI", L"Provider", L"claude") : CString(L"claude");
    m_persistedModel = app ? app->GetProfileString(L"AI", L"Model", L"") : CString();

    m_lblProvider.Create(L"Provider", ST, z, this, 0);
    m_provider.Create(ST | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, z, this, IDC_AI_PROVIDER);
    m_lblModel.Create(L"Model", ST, z, this, 0);
    m_model.Create(ST | WS_BORDER | WS_TABSTOP, z, this, IDC_AI_MODEL);
    m_lblKey.Create(L"API key", ST, z, this, 0);
    m_key.Create(ST | WS_BORDER | WS_TABSTOP | ES_PASSWORD, z, this, IDC_AI_KEY);
    m_hint.Create(L"Your API key is stored in the Windows Credential Manager, not in this app's "
                 L"settings file.", ST, z, this, IDC_AI_HINT);
    m_btnSave.Create(L"&Save", ST | WS_TABSTOP | BS_DEFPUSHBUTTON, z, this, IDOK);
    m_btnCancel.Create(L"Cancel", ST | WS_TABSTOP, z, this, IDCANCEL);

    int selIdx = 0;
    for (int i = 0; i < (int)m_providers.size(); ++i) {
        CString name = CString(CA2W(m_providers[i].display_name.c_str(), CP_UTF8));
        int pos = m_provider.AddString(name);
        m_provider.SetItemData(pos, (DWORD_PTR)i);
        if (CString(CA2W(m_providers[i].id.c_str(), CP_UTF8)) == m_persistedProviderId) selIdx = pos;
    }
    if (m_provider.GetCount() > 0) m_provider.SetCurSel(selIdx);

    for (CWnd* w : std::initializer_list<CWnd*>{ &m_lblProvider, &m_provider, &m_lblModel, &m_model,
             &m_lblKey, &m_key, &m_hint, &m_btnSave, &m_btnCancel })
        w->SetFont(&m_font);
    Theme::ApplyToChildren(GetSafeHwnd());

    RefreshForSelectedProvider();
    Layout();
    m_model.SetFocus();
    return FALSE;   // focus set explicitly above
}

void AiSettingsDlg::Layout() {
    CRect rc; GetClientRect(&rc);
    const int M = S(10), W = rc.Width() - 2 * M, gap = S(8);
    int y = M;
    auto place = [&](CWnd& w, int h) { w.MoveWindow(M, y, W, S(h)); y += S(h) + gap; };
    place(m_lblProvider, 16);
    place(m_provider, 24);
    place(m_lblModel, 16);
    place(m_model, 24);
    place(m_lblKey, 16);
    place(m_key, 24);
    place(m_hint, 36);
    const int btnW = S(90), btnH = S(28);
    m_btnCancel.MoveWindow(rc.Width() - M - btnW, y, btnW, btnH);
    m_btnSave.MoveWindow(rc.Width() - M - 2 * btnW - S(8), y, btnW, btnH);
}

const ProviderInfo* AiSettingsDlg::SelectedProvider() const {
    int sel = m_provider.GetCurSel();
    if (sel == CB_ERR) return nullptr;
    int idx = (int)m_provider.GetItemData(sel);
    return (idx >= 0 && idx < (int)m_providers.size()) ? &m_providers[idx] : nullptr;
}

// Provider switch: reset the model field to the persisted model (if it belongs to THIS provider) or
// the provider's default, unless the user has manually edited the model text since the dialog opened.
// Refresh the key field's placeholder state ("(unchanged)" when a key is already stored for this
// provider) regardless -- the key field is per-provider, so it always reflects the new selection.
void AiSettingsDlg::RefreshForSelectedProvider() {
    const ProviderInfo* p = SelectedProvider();
    if (!p) return;

    if (!m_modelEdited) {
        CString providerId = CString(CA2W(p->id.c_str(), CP_UTF8));
        CString model = (providerId == m_persistedProviderId && !m_persistedModel.IsEmpty())
                       ? m_persistedModel : CString(CA2W(p->default_model.c_str(), CP_UTF8));
        m_lastSetModelText = model;
        m_model.SetWindowText(model);
    }

    std::wstring target = AiCredTarget(p->id);
    if (github::load_token(target.c_str())) {
        m_keyFieldIsPlaceholder = true;
        m_lastSetKeyText = L"(unchanged)";
    } else {
        m_keyFieldIsPlaceholder = false;
        m_lastSetKeyText.Empty();
    }
    m_key.SetWindowText(m_lastSetKeyText);
}

void AiSettingsDlg::OnProviderChanged() { RefreshForSelectedProvider(); }

void AiSettingsDlg::OnModelChanged() {
    CString cur; m_model.GetWindowText(cur);
    if (cur != m_lastSetModelText) m_modelEdited = true;
}
void AiSettingsDlg::OnKeyChanged() {
    CString cur; m_key.GetWindowText(cur);
    if (cur != m_lastSetKeyText) m_keyFieldIsPlaceholder = false;
}

void AiSettingsDlg::OnOK() {
    const ProviderInfo* p = SelectedProvider();
    if (!p) { CDialog::OnCancel(); return; }

    CString providerId = CString(CA2W(p->id.c_str(), CP_UTF8));
    CString model; m_model.GetWindowText(model);
    if (CWinApp* app = AfxGetApp()) {
        app->WriteProfileString(L"AI", L"Provider", providerId);
        app->WriteProfileString(L"AI", L"Model", model);
    }

    CString key; m_key.GetWindowText(key);
    if (!key.IsEmpty() && !m_keyFieldIsPlaceholder) {
        std::wstring target = AiCredTarget(p->id);
        github::Token tok{ std::string(CW2A(key, CP_UTF8)) };
        github::store_token(tok, target.c_str());
    }
    CDialog::OnOK();
}
void AiSettingsDlg::OnCancel() { CDialog::OnCancel(); }   // no persistence
