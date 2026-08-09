// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "TxSyncDlg.h"
#include "resource.h"
#include "Theme.h"
#include "MainFrame.h"
#include "mpctrans/config.h"
#include "mpctrans/github.h"

#include <commctrl.h>
#include <memory>
#include <optional>

using namespace mpctrans;
using mpctrans::txsync::TxDecision;

// Local to this dialog (its own HWND, its own message map) -- no relation to MainFrame's WM_APP_*
// range, same posture as every other CDialog worker-thread handoff in the Studio (AiSettingsDlg has
// none; SuggestFixDlg posts back to MainFrame instead -- this is the first dialog that owns its own
// background compute end-to-end).
#define WM_APP_TXSYNC_PROGRESS (WM_APP + 100)
#define WM_APP_TXSYNC_DONE     (WM_APP + 101)

namespace {
// Heap-allocated by the worker thread, ownership passed to the UI thread via WM_APP_TXSYNC_DONE (same
// pattern as MainFrame's PrefetchResult/DataUpdateResult/etc). `ok=false` means tx_compute (or one of
// its fetchers) threw -- e.g. a transient network failure -- and `error` carries what()'s text.
struct ComputeResult {
    txsync::TxSyncResult result;
    bool ok = true;
    CString error;
};
} // namespace

BEGIN_MESSAGE_MAP(TxSyncDlg, CDialog)
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
    ON_BN_CLICKED(IDC_TXSYNC_SHOW_PROTECTED, OnShowProtectedClicked)
    ON_NOTIFY(NM_DBLCLK, IDC_TXSYNC_LIST, OnListDoubleClick)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_TXSYNC_LIST, OnListItemChanged)
    ON_BN_CLICKED(IDC_TXSYNC_BTN_UPDATE, OnUpdateBranchClicked)
    ON_BN_CLICKED(IDC_TXSYNC_BTN_PR, OnProposePrClicked)
    ON_MESSAGE(WM_APP_TXSYNC_PROGRESS, OnComputeProgress)
    ON_MESSAGE(WM_APP_TXSYNC_DONE, OnComputeDone)
END_MESSAGE_MAP()

TxSyncDlg::TxSyncDlg(MainFrame* mainFrame) : CDialog(IDD_TXSYNC, mainFrame), m_mainFrame(mainFrame) {}
TxSyncDlg::~TxSyncDlg() { if (m_computeThread.joinable()) m_computeThread.join(); }

// Dark theme, same split as every other code-driven dialog here (edits get the content colour,
// everything else the window colour).
HBRUSH TxSyncDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
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
BOOL TxSyncDlg::OnEraseBkgnd(CDC* pDC) {
    CRect rc; GetClientRect(&rc); pDC->FillSolidRect(rc, Theme::WINDOW_BG); return TRUE;
}

BOOL TxSyncDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    Theme::InitTopWindow(GetSafeHwnd());
    { CClientDC dc(this); m_dpi = dc.GetDeviceCaps(LOGPIXELSX); }
    m_font.CreatePointFont(90, L"Segoe UI");

    CRect wr(0, 0, S(900), S(620));
    ::AdjustWindowRectEx(&wr, (DWORD)GetStyle(), FALSE, (DWORD)GetExStyle());
    SetWindowPos(nullptr, 0, 0, wr.Width(), wr.Height(), SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow(GetParent());

    const DWORD ST = WS_CHILD | WS_VISIBLE;
    CRect z(0, 0, 0, 0);

    m_status.Create(L"Transifex sync: computing…", ST | SS_LEFT, z, this, IDC_TXSYNC_STATUS);
    m_progressText.Create(L"", ST | SS_LEFT, z, this, IDC_TXSYNC_PROGRESS_TEXT);
    m_showProtected.Create(L"Show protected (upstream-only translations)",
                           ST | WS_TABSTOP | BS_AUTOCHECKBOX, z, this, IDC_TXSYNC_SHOW_PROTECTED);

    // LVS_EX_CHECKBOXES: the "Include" column is implicit in column 0 (see Theme.cpp's draw_list,
    // extended to paint the checkbox glyph -- comctl32's own paint is fully replaced there).
    m_list.Create(ST | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS, z, this, IDC_TXSYNC_LIST);
    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
    m_list.InsertColumn(0, L"Lang", LVCFMT_LEFT, S(70));
    m_list.InsertColumn(1, L"Res", LVCFMT_LEFT, S(70));
    m_list.InsertColumn(2, L"Context", LVCFMT_LEFT, S(230));
    m_list.InsertColumn(3, L"Kind", LVCFMT_LEFT, S(100));
    m_list.InsertColumn(4, L"Upstream", LVCFMT_LEFT, S(220));
    m_list.InsertColumn(5, L"Transifex", LVCFMT_LEFT, S(220));

    m_btnUpdateBranch.Create(L"Update Transifex branch", ST | WS_TABSTOP, z, this, IDC_TXSYNC_BTN_UPDATE);
    m_btnProposePr.Create(L"Propose upstream PR…", ST | WS_TABSTOP, z, this, IDC_TXSYNC_BTN_PR);
    m_btnClose.Create(L"Close", ST | WS_TABSTOP | BS_DEFPUSHBUTTON, z, this, IDCANCEL);
    m_btnUpdateBranch.EnableWindow(FALSE);   // enabled once the background compute lands (OnComputeDone)
    m_btnProposePr.EnableWindow(FALSE);

    for (CWnd* w : std::initializer_list<CWnd*>{ &m_status, &m_progressText, &m_showProtected,
             &m_btnUpdateBranch, &m_btnProposePr, &m_btnClose })
        w->SetFont(&m_font);
    m_list.SetFont(&m_font);
    Theme::ApplyToChildren(GetSafeHwnd());
    Layout();

    // Snapshot the language list on the UI thread -- MainFrame's combo isn't safe to read from the
    // worker (see StartPrefetch's identical rationale). Reuses MainFrame's own known-good language
    // set (the bundle's .po dir listing, see MainFrame::LanguageList/PopulateLanguages) rather than
    // duplicating that discovery here.
    std::vector<std::string> langs;
    if (m_mainFrame)
        for (const CString& c : m_mainFrame->LanguageList()) langs.push_back(std::string(CW2A(c, CP_UTF8)));

    HWND hwnd = GetSafeHwnd();
    m_computeThread = std::thread([hwnd, langs = std::move(langs)]() {
        github::Token tok{};   // fetch_latest's raw.githubusercontent.com path works tokenless
        auto fetchUpstream = [&](const std::string& path) {
            return github::fetch_latest(tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                        config::UPSTREAM_BRANCH, path);
        };
        auto fetchTx = [&](const std::string& path) {
            return github::fetch_latest(tok, config::TRANSIFEX_OWNER, config::TRANSIFEX_REPO,
                                        config::TRANSIFEX_BRANCH, path);
        };
        auto progress = [hwnd](int done, int total) {
            ::PostMessage(hwnd, WM_APP_TXSYNC_PROGRESS, (WPARAM)done, (LPARAM)total);
        };
        auto* out = new ComputeResult;
        try { out->result = txsync::tx_compute(fetchUpstream, fetchTx, langs, progress); }
        catch (const std::exception& ex) { out->ok = false; out->error = CString(CA2W(ex.what(), CP_UTF8)); }
        if (!::PostMessage(hwnd, WM_APP_TXSYNC_DONE, (WPARAM)out, 0)) delete out;   // dialog gone
    });

    return TRUE;   // no control needs the initial focus more than the list will once it's populated
}

void TxSyncDlg::OnCancel() {
    // The dialog is modal (blocks the whole app already), so joining here is safe -- no cancel flag
    // exists in tx_compute today (the compute is a handful of minutes at worst; see the class comment
    // for why the fetchers are the plain per-call github::fetch_latest, not a pooled RawSession).
    if (m_computeThread.joinable()) m_computeThread.join();
    CDialog::OnCancel();
}

void TxSyncDlg::Layout() {
    CRect rc; GetClientRect(&rc);
    const int M = S(10), W = rc.Width() - 2 * M, gap = S(8);
    int y = M;
    auto place = [&](CWnd& w, int h) { w.MoveWindow(M, y, W, S(h)); y += S(h) + gap; };
    place(m_status, 34);
    place(m_progressText, 18);
    place(m_showProtected, 20);
    const int btnW = S(160), btnH = S(28);
    int by = rc.Height() - M - btnH;
    m_list.MoveWindow(M, y, W, by - gap - y);
    m_btnClose.MoveWindow(rc.Width() - M - btnW, by, btnW, btnH);
    m_btnProposePr.MoveWindow(rc.Width() - M - 2 * btnW - S(8), by, btnW, btnH);
    m_btnUpdateBranch.MoveWindow(rc.Width() - M - 3 * btnW - S(16), by, btnW, btnH);
}

LRESULT TxSyncDlg::OnComputeProgress(WPARAM done, LPARAM total) {
    CString s; s.Format(L"Fetching %d files… %d/%d", (int)total, (int)done, (int)total);
    m_progressText.SetWindowText(s);
    return 0;
}

LRESULT TxSyncDlg::OnComputeDone(WPARAM wp, LPARAM) {
    std::unique_ptr<ComputeResult> out((ComputeResult*)wp);
    if (m_computeThread.joinable()) m_computeThread.join();

    if (!out->ok) {
        m_progressText.SetWindowText(L"Fetch failed.");
        m_status.SetWindowText(L"Transifex sync failed.");
        AfxMessageBox(L"Transifex sync failed:\n" + out->error, MB_ICONERROR);
        return 0;
    }
    m_result = std::move(out->result);
    m_haveResult = true;
    m_progressText.SetWindowText(L"");
    m_btnUpdateBranch.EnableWindow(TRUE);
    m_btnProposePr.EnableWindow(TRUE);
    RebuildRows();
    RepopulateList();
    RefreshStatusText();
    return 0;
}

CString TxSyncDlg::KindLabel(TxDecision::Kind k) {
    switch (k) {
        case TxDecision::TxNew:      return L"New";
        case TxDecision::TxWins:     return L"Tx wins";
        case TxDecision::Discarded:  return L"Discarded";
        case TxDecision::Protected:  return L"Protected";
    }
    return L"";
}

// Default filter: only TxNew/TxWins/Discarded -- Protected rows are numerous (~950, see the class
// comment / feature spec) and, being un-actionable (never applied -- see tx_build_edits), are just
// noise until the user explicitly asks to see them via m_showProtected.
void TxSyncDlg::RebuildRows() {
    m_visible.clear();
    bool showProtected = m_showProtected.GetCheck() == BST_CHECKED;
    for (size_t i = 0; i < m_result.decisions.size(); ++i) {
        if (m_result.decisions[i].kind == TxDecision::Protected && !showProtected) continue;
        m_visible.push_back(i);
    }
}

void TxSyncDlg::RepopulateList() {
    m_list.DeleteAllItems();
    for (int row = 0; row < (int)m_visible.size(); ++row) {
        const TxDecision& d = m_result.decisions[m_visible[row]];
        CString lang = CString(CA2W(d.lang.c_str(), CP_UTF8));
        CString res  = CString(CA2W(config::RESOURCES[d.res], CP_UTF8));
        CString ctx  = CString(CA2W(d.msgctxt.c_str(), CP_UTF8));
        CString up   = CString(CA2W(d.upstreamStr.c_str(), CP_UTF8));
        CString tx   = CString(CA2W(d.txStr.c_str(), CP_UTF8));

        int item = m_list.InsertItem(row, lang);
        m_list.SetItemText(item, 1, res);
        m_list.SetItemText(item, 2, ctx);
        m_list.SetItemText(item, 3, KindLabel(d.kind));
        m_list.SetItemText(item, 4, up);
        m_list.SetItemText(item, 5, tx);
        m_list.SetItemData(item, (DWORD_PTR)m_visible[row]);   // -> m_result.decisions index

        // Checked for TxNew/TxWins (their `include`, true by default); Discarded/Protected rows are
        // shown unchecked -- their checkbox is decorative (tx_build_edits never reads it), per spec.
        bool checked = (d.kind == TxDecision::TxNew || d.kind == TxDecision::TxWins) && d.include;
        ListView_SetCheckState(m_list.GetSafeHwnd(), item, checked);
    }
}

CString TxSyncDlg::BuildSummaryText() const {
    int nNew = 0, nWins = 0, nDiscarded = 0, nProtected = 0;
    for (const auto& d : m_result.decisions) {
        switch (d.kind) {
            case TxDecision::TxNew:     ++nNew; break;
            case TxDecision::TxWins:    ++nWins; break;
            case TxDecision::Discarded: ++nDiscarded; break;
            case TxDecision::Protected: ++nProtected; break;
        }
    }
    CString s;
    s.Format(L"Transifex sync: %d new, %d conflicts (Transifex wins), %d discarded (bad placeholders), "
             L"%d protected upstream-only translations, %d unchanged.",
             nNew, nWins, nDiscarded, nProtected, m_result.unchanged);
    return s;
}
void TxSyncDlg::RefreshStatusText() { m_status.SetWindowText(BuildSummaryText()); }

void TxSyncDlg::OnShowProtectedClicked() {
    if (!m_haveResult) return;
    RebuildRows();
    RepopulateList();
}

// Checkbox toggles land here (LVN_ITEMCHANGED with an LVIF_STATE change carrying the state-image
// bits) -- write straight back into the decision so tx_build_edits (called later, from the button
// handlers) sees the user's choice. Harmless no-op for Discarded/Protected rows: tx_build_edits only
// ever reads `include` on TxNew/TxWins kinds.
void TxSyncDlg::OnListItemChanged(NMHDR* hdr, LRESULT* res) {
    *res = 0;
    auto* lv = (NMLISTVIEW*)hdr;
    if (!(lv->uChanged & LVIF_STATE)) return;
    UINT oldChecked = (lv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
    UINT newChecked = (lv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
    if (newChecked == 0 || newChecked == oldChecked) return;
    size_t idx = (size_t)m_list.GetItemData(lv->iItem);
    if (idx < m_result.decisions.size()) m_result.decisions[idx].include = (newChecked == 2);
}

// Navigate MainFrame to this row's string, per the feature spec -- close is NOT required, the modal
// stays open so the user can keep reviewing (MainFrame updates its selection live behind it).
void TxSyncDlg::OnListDoubleClick(NMHDR* hdr, LRESULT* res) {
    *res = 0;
    auto* nm = (NMITEMACTIVATE*)hdr;
    if (!m_mainFrame || nm->iItem < 0 || nm->iItem >= (int)m_visible.size()) return;
    const TxDecision& d = m_result.decisions[m_visible[nm->iItem]];
    m_mainFrame->NavigateToString(CString(CA2W(d.lang.c_str(), CP_UTF8)), d.msgctxt, d.res);
}

// GitHub device-flow sign-in, identical sequence to MainFrame::SubmitPr/OnSuggestFixClicked (this
// dialog has no token of its own -- same Credential Manager entry the rest of the app uses).
static std::optional<github::Token> EnsureGithubToken(CWnd* owner) {
    auto tok = github::load_token();
    if (tok) return tok;
    try {
        github::DeviceCode dc = github::request_device_code(config::DEVICE_SCOPE);
        github::Token t = github::poll_for_token(dc, [owner](const github::DeviceCode& d) {
            ::ShellExecuteW(nullptr, L"open", CA2W(d.verification_uri.c_str(), CP_UTF8),
                            nullptr, nullptr, SW_SHOWNORMAL);
            owner->MessageBox(L"A browser was opened at " +
                              CString(CA2W(d.verification_uri.c_str(), CP_UTF8)) +
                              L"\n\nEnter this code to authorize the Studio:\n\n        " +
                              CString(CA2W(d.user_code.c_str(), CP_UTF8)) +
                              L"\n\nClick OK AFTER you have authorized.", L"GitHub sign-in");
        });
        github::store_token(t);
        return t;
    } catch (const std::exception& ex) {
        owner->MessageBox(L"GitHub sign-in failed:\n" + CString(CA2W(ex.what(), CP_UTF8)),
                          L"GitHub sign-in", MB_ICONERROR);
        return std::nullopt;
    }
}

void TxSyncDlg::OnUpdateBranchClicked() {
    if (!m_haveResult) return;
    std::vector<github::FileEdit> edits = txsync::tx_build_edits(m_result.upstreamPoBytes, m_result);
    if (edits.empty()) { AfxMessageBox(L"No changes selected to apply.", MB_ICONINFORMATION); return; }

    CString txRepo = CString(config::TRANSIFEX_OWNER) + L"/" + CString(config::TRANSIFEX_REPO) +
                     L"@" + CString(config::TRANSIFEX_BRANCH);
    CString msg; msg.Format(L"Push %d changed file(s) directly to the %s branch?\n\n"
                            L"This updates the branch immediately -- there is no PR review step.",
                            (int)edits.size(), (LPCWSTR)txRepo);
    if (MessageBox(msg, L"Update Transifex branch", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    try {
        CWaitCursor wait;
        auto tok = EnsureGithubToken(this);
        if (!tok) return;
        std::string sha = github::update_transifex_branch(*tok, edits,
            "Merge upstream develop + Transifex sync (Studio)");
        MessageBox(L"Updated the transifex branch.\n\nNew commit: " + CString(CA2W(sha.c_str(), CP_UTF8)),
                  L"Update Transifex branch", MB_ICONINFORMATION);
    } catch (const std::exception& ex) {
        MessageBox(L"Update failed:\n" + CString(CA2W(ex.what(), CP_UTF8)),
                  L"Update Transifex branch", MB_ICONERROR);
    }
}

void TxSyncDlg::OnProposePrClicked() {
    if (!m_haveResult) return;
    std::vector<github::FileEdit> edits = txsync::tx_build_edits(m_result.upstreamPoBytes, m_result);
    if (edits.empty()) { AfxMessageBox(L"No changes selected to propose.", MB_ICONINFORMATION); return; }

    CString upRepo = CString(config::UPSTREAM_OWNER) + L"/" + CString(config::UPSTREAM_REPO) +
                     L"@" + CString(config::UPSTREAM_BRANCH);
    CString msg; msg.Format(L"Open a pull request against %s with %d changed file(s)?",
                            (LPCWSTR)upRepo, (int)edits.size());
    if (MessageBox(msg, L"Propose upstream PR", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    try {
        CWaitCursor wait;
        auto tok = EnsureGithubToken(this);
        if (!tok) return;
        std::string body = std::string(CW2A(BuildSummaryText(), CP_UTF8));
        std::string url = github::open_pr(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                          config::UPSTREAM_BRANCH, "transifex-sync", edits,
                                          "Transifex updates", body);
        ::ShellExecuteW(nullptr, L"open", CA2W(url.c_str(), CP_UTF8), nullptr, nullptr, SW_SHOWNORMAL);
        MessageBox(L"Your changes were pushed to a branch and GitHub's “Open a pull request” "
                  L"page was opened in your browser.\n\nReview the diff there and click "
                  L"“Create pull request” to submit — nothing is submitted until you do.",
                  L"Propose upstream PR", MB_ICONINFORMATION);
    } catch (const std::exception& ex) {
        MessageBox(L"Propose PR failed:\n" + CString(CA2W(ex.what(), CP_UTF8)),
                  L"Propose upstream PR", MB_ICONERROR);
    }
}
