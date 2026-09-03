// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "TxSyncDlg.h"
#include "resource.h"
#include "Theme.h"
#include "MainFrame.h"
#include "TxTokenDlg.h"
#include "mpctrans/config.h"
#include "mpctrans/github.h"
#include "mpctrans/po.h"
#include "mpctrans/txapi.h"
#include "mpctrans/validate.h"

#include <algorithm>
#include <commctrl.h>
#include <memory>
#include <optional>

using namespace mpctrans;
using mpctrans::txsync::TxDecision;

// Local to this dialog (its own HWND, its own message map) -- no relation to MainFrame's WM_APP_*
// range, same posture as every other CDialog worker-thread handoff in the Studio (AiSettingsDlg has
// none; SuggestFixDlg posts back to MainFrame instead -- this is the first dialog that owns its own
// background compute end-to-end).
#define WM_APP_TXSYNC_PROGRESS  (WM_APP + 100)
#define WM_APP_TXSYNC_DONE      (WM_APP + 101)
#define WM_APP_TXSYNC_FORKS     (WM_APP + 102)   // initial fork list + resolved branch list, see OnInitDialog
#define WM_APP_TXSYNC_BRANCHES  (WM_APP + 103)   // fork changed -> that fork's branch list, see OnForkChanged
#define WM_APP_TXSYNC_PUSHPLAN  (WM_APP + 104)   // OnPushTxClicked's plan worker finished
#define WM_APP_TXSYNC_PUSHDONE  (WM_APP + 105)   // OnPushPlanDone's apply worker finished

namespace {
// Heap-allocated by the worker thread, ownership passed to the UI thread via WM_APP_TXSYNC_DONE (same
// pattern as MainFrame's PrefetchResult/DataUpdateResult/etc). `ok=false` means tx_compute (or one of
// its fetchers) threw -- e.g. a transient network failure -- and `error` carries what()'s text.
// owner/repo/branch echo back exactly what this compute ran against -- see TxSyncDlg.h's m_txOwner/
// m_txRepo/m_txBranch comment for why the dialog trusts THIS over live combo state.
struct ComputeResult {
    txsync::TxSyncResult result;
    bool ok = true;
    CString error;
    std::string login;   // signed-in GitHub user from the stored token; "" = unknown
    std::string owner, repo, branch;
};
// WM_APP_TXSYNC_FORKS payload: the resolved fork list + the branch list for whichever fork ended up
// selected (saved profile pref if still valid, else the list's first entry -- see OnInitDialog).
struct ForksResult {
    std::vector<std::string> forks, branches;
    std::string selFork, selBranch;
};
// WM_APP_TXSYNC_BRANCHES payload: OnForkChanged's branch-list refetch for the newly picked fork.
struct BranchesResult {
    std::vector<std::string> branches;
    std::string selBranch;
};
// WM_APP_TXSYNC_PUSHPLAN payload: OnPushTxClicked's worker planned the reverse push. `result` is the
// MUTATED copy of m_result the worker planned against (tx_reverse_push_plan flips include flags on
// the decisions it resolves) -- see TxSyncDlg.h's OnPushPlanDone comment for why the UI thread copies
// this back into m_result regardless of what the user does with `plan` next.
struct PushPlanResult {
    txsync::TxSyncResult result;
    txsync::ReversePushPlan plan;
    bool ok = true;
    CString error;
};
// WM_APP_TXSYNC_PUSHDONE payload: OnPushPlanDone's apply worker (tx_apply_reverse_push) finished.
// `pushed` counts report(item, true, ...) calls; `skippedOrFailed` holds a capped, human-readable
// line per report(item, false, ...) call (a DB-changed-since-plan skip or a PATCH failure -- see
// tx_apply_reverse_push's doc comment, both come back as ok=false there).
struct PushApplyResult {
    bool ok = true;   // false only if tx_apply_reverse_push itself threw (defensive; it doesn't today)
    CString error;
    int pushed = 0;
    std::vector<CString> skippedOrFailed;
};

void SplitOwnerRepo(const std::string& s, std::string& owner, std::string& repo) {
    size_t slash = s.find('/');
    if (slash == std::string::npos) { owner = s; repo.clear(); }
    else { owner = s.substr(0, slash); repo = s.substr(slash + 1); }
}

// Shared tail of every background task variant (initial load, fork change, branch change): run
// tx_compute against the given (owner,repo,branch) tx source and post the result back. Factored out
// so the three worker-thread lambdas in this file can't drift on what "run a compute" means.
void RunComputeAndPost(HWND hwnd, const std::vector<std::string>& langs, const std::string& owner,
                       const std::string& repo, const std::string& branch, const github::Token& tok) {
    auto fetchUpstream = [&](const std::string& path) {
        return github::fetch_latest(tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                    config::UPSTREAM_BRANCH, path);
    };
    auto fetchTx = [&](const std::string& path) {
        return github::fetch_latest(tok, owner, repo, branch, path);
    };
    auto progress = [hwnd](int done, int total) {
        ::PostMessage(hwnd, WM_APP_TXSYNC_PROGRESS, (WPARAM)done, (LPARAM)total);
    };
    auto* out = new ComputeResult;
    out->owner = owner; out->repo = repo; out->branch = branch;
    try { out->result = txsync::tx_compute(fetchUpstream, fetchTx, langs, progress); }
    catch (const std::exception& ex) { out->ok = false; out->error = CString(CA2W(ex.what(), CP_UTF8)); }
    // Only the target repo's owner can move its branch; resolve who is signed in (stored token only --
    // never an interactive sign-in from a worker thread) so the button can be gated up front. Absent/
    // failed = "unknown": the click path re-checks after its own sign-in.
    if (auto stored = github::load_token()) {
        try { out->login = github::whoami(*stored); } catch (const std::exception&) {}
    }
    if (!::PostMessage(hwnd, WM_APP_TXSYNC_DONE, (WPARAM)out, 0)) delete out;   // dialog gone
}
} // namespace

BEGIN_MESSAGE_MAP(TxSyncDlg, CDialog)
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
    ON_BN_CLICKED(IDC_TXSYNC_SHOW_PROTECTED, OnShowProtectedClicked)
    ON_NOTIFY(NM_DBLCLK, IDC_TXSYNC_LIST, OnListDoubleClick)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_TXSYNC_LIST, OnListItemChanged)
    ON_BN_CLICKED(IDC_TXSYNC_BTN_UPDATE, OnUpdateBranchClicked)
    ON_BN_CLICKED(IDC_TXSYNC_BTN_PR, OnProposePrClicked)
    ON_BN_CLICKED(IDC_TXSYNC_BTN_REFRESH, OnRefreshPrClicked)
    ON_BN_CLICKED(IDC_TXSYNC_BTN_PUSHTX, OnPushTxClicked)
    ON_CBN_SELCHANGE(IDC_TXSYNC_FORK, OnForkChanged)
    ON_CBN_SELCHANGE(IDC_TXSYNC_BRANCH, OnBranchChanged)
    ON_MESSAGE(WM_APP_TXSYNC_PROGRESS, OnComputeProgress)
    ON_MESSAGE(WM_APP_TXSYNC_DONE, OnComputeDone)
    ON_MESSAGE(WM_APP_TXSYNC_FORKS, OnForksDone)
    ON_MESSAGE(WM_APP_TXSYNC_BRANCHES, OnBranchesDone)
    ON_MESSAGE(WM_APP_TXSYNC_PUSHPLAN, OnPushPlanDone)
    ON_MESSAGE(WM_APP_TXSYNC_PUSHDONE, OnPushApplyDone)
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

    m_lblFork.Create(L"Fork:", ST | SS_LEFT, z, this, 0);
    m_forkCombo.Create(ST | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, z, this, IDC_TXSYNC_FORK);
    m_lblBranch.Create(L"Branch:", ST | SS_LEFT, z, this, 0);
    m_branchCombo.Create(ST | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, z, this, IDC_TXSYNC_BRANCH);
    m_forkCombo.EnableWindow(FALSE);     // enabled once the initial fork/branch fetch lands (OnForksDone)
    m_branchCombo.EnableWindow(FALSE);

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
    m_btnRefreshPr.Create(L"Refresh open PR…", ST | WS_TABSTOP, z, this, IDC_TXSYNC_BTN_REFRESH);
    m_btnPushTx.Create(L"Push to Transifex…", ST | WS_TABSTOP, z, this, IDC_TXSYNC_BTN_PUSHTX);
    m_btnClose.Create(L"Close", ST | WS_TABSTOP | BS_DEFPUSHBUTTON, z, this, IDCANCEL);
    m_btnUpdateBranch.EnableWindow(FALSE);   // enabled once the background compute lands (OnComputeDone)
    m_btnProposePr.EnableWindow(FALSE);
    m_btnRefreshPr.EnableWindow(FALSE);
    m_btnPushTx.EnableWindow(FALSE);

    for (CWnd* w : std::initializer_list<CWnd*>{ &m_status, &m_lblFork, &m_forkCombo, &m_lblBranch,
             &m_branchCombo, &m_progressText, &m_showProtected, &m_btnUpdateBranch, &m_btnProposePr,
             &m_btnRefreshPr, &m_btnPushTx, &m_btnClose })
        w->SetFont(&m_font);
    m_list.SetFont(&m_font);
    Theme::ApplyToChildren(GetSafeHwnd());
    Layout();

    // Snapshot the language list on the UI thread -- MainFrame's combo isn't safe to read from the
    // worker (see StartPrefetch's identical rationale). Reuses MainFrame's own known-good language
    // set (the bundle's .po dir listing, see MainFrame::LanguageList/PopulateLanguages) rather than
    // duplicating that discovery here. Kept as a member (m_langs) so the fork/branch-change recomputes
    // (OnForkChanged/OnBranchChanged) can reuse it without going back to MainFrame.
    m_langs.clear();
    if (m_mainFrame)
        for (const CString& c : m_mainFrame->LanguageList()) m_langs.push_back(std::string(CW2A(c, CP_UTF8)));

    // Saved fork/branch selection (defaults to the app's long-standing TRANSIFEX_* constants) -- read
    // on the UI thread, then handed to the worker to validate/resolve against the actual fork/branch
    // lists (a saved pref that no longer exists, e.g. a deleted branch, falls back gracefully there).
    CWinApp* app = AfxGetApp();
    CString defaultFork = CString(config::TRANSIFEX_OWNER) + L"/" + CString(config::TRANSIFEX_REPO);
    CString savedForkCs = app ? app->GetProfileString(L"txsync", L"fork", defaultFork) : defaultFork;
    CString savedBranchCs = app ? app->GetProfileString(L"txsync", L"branch", CString(config::TRANSIFEX_BRANCH))
                                : CString(config::TRANSIFEX_BRANCH);
    std::string savedFork = std::string(CW2A(savedForkCs, CP_UTF8));
    std::string savedBranch = std::string(CW2A(savedBranchCs, CP_UTF8));

    m_status.SetWindowText(L"Transifex sync: loading forks…");
    HWND hwnd = GetSafeHwnd();
    m_computeThread = std::thread([hwnd, langs = m_langs, savedFork, savedBranch]() {
        // Stored token (if any) buys the higher authenticated rate limit for the /repos listing calls
        // below; anonymous still works (60/hr), same tolerance as MainFrame::head_sha.
        auto stored = github::load_token();
        github::Token tok = stored.value_or(github::Token{});

        std::vector<std::string> forks = github::list_forks(tok);
        std::string selFork = savedFork;
        if (selFork.empty() || std::find(forks.begin(), forks.end(), selFork) == forks.end())
            selFork = forks.empty() ? (std::string(config::TRANSIFEX_OWNER) + "/" + config::TRANSIFEX_REPO)
                                    : forks.front();

        std::string owner, repo; SplitOwnerRepo(selFork, owner, repo);
        std::vector<std::string> branches = github::list_branches(tok, owner, repo);
        std::string selBranch = savedBranch;
        if (selBranch.empty() || std::find(branches.begin(), branches.end(), selBranch) == branches.end()) {
            auto it = std::find(branches.begin(), branches.end(), "transifex");
            selBranch = it != branches.end() ? *it
                       : branches.empty() ? std::string(config::TRANSIFEX_BRANCH) : branches.front();
        }

        auto* fr = new ForksResult{ forks, branches, selFork, selBranch };
        if (!::PostMessage(hwnd, WM_APP_TXSYNC_FORKS, (WPARAM)fr, 0)) { delete fr; return; }  // dialog gone

        RunComputeAndPost(hwnd, langs, owner, repo, selBranch, tok);
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

    // Fork:/Branch: row -- fixed-width labels + two dropdown combos, left to right.
    const int rowH = 22, lblFork = 40, comboFork = S(340), lblBranch = 55, comboBranch = S(200);
    int x = M, rowY = y;
    m_lblFork.MoveWindow(x, rowY + S(3), S(lblFork), S(rowH)); x += S(lblFork);
    m_forkCombo.MoveWindow(x, rowY, comboFork, S(200)); x += comboFork + S(10);
    m_lblBranch.MoveWindow(x, rowY + S(3), S(lblBranch), S(rowH)); x += S(lblBranch);
    m_branchCombo.MoveWindow(x, rowY, comboBranch, S(200));
    y += S(rowH) + gap;

    place(m_progressText, 18);
    place(m_showProtected, 20);
    const int btnW = S(160), btnH = S(28);
    int by = rc.Height() - M - btnH;
    m_list.MoveWindow(M, y, W, by - gap - y);
    m_btnClose.MoveWindow(rc.Width() - M - btnW, by, btnW, btnH);
    m_btnProposePr.MoveWindow(rc.Width() - M - 2 * btnW - S(8), by, btnW, btnH);
    m_btnUpdateBranch.MoveWindow(rc.Width() - M - 3 * btnW - S(16), by, btnW, btnH);
    m_btnRefreshPr.MoveWindow(rc.Width() - M - 4 * btnW - S(24), by, btnW, btnH);
    m_btnPushTx.MoveWindow(rc.Width() - M - 5 * btnW - S(32), by, btnW, btnH);
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
        m_forkCombo.EnableWindow(TRUE); m_branchCombo.EnableWindow(TRUE);   // let the user try a different selection
        AfxMessageBox(L"Transifex sync failed:\n" + out->error, MB_ICONERROR);
        return 0;
    }
    m_result = std::move(out->result);
    m_haveResult = true;
    m_login = out->login;
    // Echoed back from the worker -- see ComputeResult's comment: this IS the selection that produced
    // m_result, regardless of whatever the combos show by the time this message is processed.
    m_txOwner = out->owner; m_txRepo = out->repo; m_txBranch = out->branch;
    m_progressText.SetWindowText(L"");
    m_forkCombo.EnableWindow(TRUE);
    m_branchCombo.EnableWindow(TRUE);
    // The branch update writes straight to the SELECTED fork -- owner-only. A known non-owner login
    // disables the button outright; unknown (not signed in yet) leaves it enabled and the click path
    // verifies after its sign-in. The PR path stays open to everyone (it forks).
    bool ownerKnownForeign = !m_login.empty() && _stricmp(m_login.c_str(), m_txOwner.c_str()) != 0;
    m_btnUpdateBranch.EnableWindow(!ownerKnownForeign);
    if (ownerKnownForeign) {
        CString hint; hint.Format(L"Signed in as %s — the %s/%s branch update is owner (%s) only.",
                                  (LPCWSTR)CString(CA2W(m_login.c_str(), CP_UTF8)),
                                  (LPCWSTR)CString(CA2W(m_txOwner.c_str(), CP_UTF8)),
                                  (LPCWSTR)CString(CA2W(m_txRepo.c_str(), CP_UTF8)),
                                  (LPCWSTR)CString(CA2W(m_txOwner.c_str(), CP_UTF8)));
        m_progressText.SetWindowText(hint);
    }
    m_btnProposePr.EnableWindow(TRUE);
    m_btnRefreshPr.EnableWindow(TRUE);
    m_btnPushTx.EnableWindow(TRUE);
    RebuildRows();
    RepopulateList();
    RefreshStatusText();
    // Hand MainFrame a COPY so the maintainer's later review (Sync review tab) + "Propose Transifex
    // sync PR..." survive this (modal) dialog closing -- see MainFrame::SetTxSyncResult's comment.
    // Same UI thread as MainFrame (this dialog IS modal to it), so a direct call is safe.
    if (m_mainFrame) m_mainFrame->SetTxSyncResult(m_result, m_txOwner, m_txRepo, m_txBranch);
    return 0;
}

// WM_APP_TXSYNC_FORKS: the initial fork+branch discovery landed (see OnInitDialog) -- populate both
// combos and let the (already-launched, same worker thread) compute continue in the background.
LRESULT TxSyncDlg::OnForksDone(WPARAM wp, LPARAM) {
    std::unique_ptr<ForksResult> fr((ForksResult*)wp);
    m_forkCombo.ResetContent();
    for (const auto& f : fr->forks) m_forkCombo.AddString(CString(CA2W(f.c_str(), CP_UTF8)));
    int fi = m_forkCombo.FindStringExact(-1, CString(CA2W(fr->selFork.c_str(), CP_UTF8)));
    m_forkCombo.SetCurSel(fi != CB_ERR ? fi : 0);

    m_branchCombo.ResetContent();
    for (const auto& b : fr->branches) m_branchCombo.AddString(CString(CA2W(b.c_str(), CP_UTF8)));
    int bi = m_branchCombo.FindStringExact(-1, CString(CA2W(fr->selBranch.c_str(), CP_UTF8)));
    m_branchCombo.SetCurSel(bi != CB_ERR ? bi : 0);

    // Combos stay disabled (SetBusy(true) is still in effect from OnInitDialog) until OnComputeDone --
    // the compute this fork/branch feeds is already running on the same worker thread.
    m_status.SetWindowText(L"Transifex sync: computing…");
    return 0;
}

// WM_APP_TXSYNC_BRANCHES: OnForkChanged's branch-list refetch landed -- populate the branch combo;
// the compute for the newly resolved branch is already running (same worker thread, see OnForkChanged).
LRESULT TxSyncDlg::OnBranchesDone(WPARAM wp, LPARAM) {
    std::unique_ptr<BranchesResult> br((BranchesResult*)wp);
    m_branchCombo.ResetContent();
    for (const auto& b : br->branches) m_branchCombo.AddString(CString(CA2W(b.c_str(), CP_UTF8)));
    int bi = m_branchCombo.FindStringExact(-1, CString(CA2W(br->selBranch.c_str(), CP_UTF8)));
    m_branchCombo.SetCurSel(bi != CB_ERR ? bi : 0);

    if (CWinApp* app = AfxGetApp())
        app->WriteProfileString(L"txsync", L"branch", CString(CA2W(br->selBranch.c_str(), CP_UTF8)));
    return 0;
}

void TxSyncDlg::SetBusy(bool busy) {
    m_forkCombo.EnableWindow(!busy);
    m_branchCombo.EnableWindow(!busy);
    // Re-enabling is OnComputeDone's job (it re-applies the owner-gate) -- SetBusy only ever tightens.
    if (busy) {
        m_btnUpdateBranch.EnableWindow(FALSE); m_btnProposePr.EnableWindow(FALSE);
        m_btnRefreshPr.EnableWindow(FALSE); m_btnPushTx.EnableWindow(FALSE);
    }
}

// Plain recompute (branch changed, same fork -- no branch-list refetch needed): used directly by
// OnBranchChanged, and as OnForkChanged's tail once its branch-list refetch resolves.
void TxSyncDlg::StartCompute(const std::string& owner, const std::string& repo, const std::string& branch) {
    if (m_computeThread.joinable()) m_computeThread.join();   // defensive; disabled combos already prevent this
    SetBusy(true);
    m_progressText.SetWindowText(L"Fetching files…");
    HWND hwnd = GetSafeHwnd();
    std::vector<std::string> langs = m_langs;
    auto stored = github::load_token();
    github::Token tok = stored.value_or(github::Token{});
    m_computeThread = std::thread([hwnd, langs, owner, repo, branch, tok]() {
        RunComputeAndPost(hwnd, langs, owner, repo, branch, tok);
    });
}

// Fork combo changed: refetch that fork's branch list (background -- everything stays disabled via
// SetBusy), auto-select "transifex" if present else the fork's first branch, then recompute against
// it. The whole thing is one worker thread, same shape as OnInitDialog's initial sequence.
void TxSyncDlg::OnForkChanged() {
    int sel = m_forkCombo.GetCurSel();
    if (sel == CB_ERR) return;
    CString forkCs; m_forkCombo.GetLBText(sel, forkCs);
    if (CWinApp* app = AfxGetApp()) app->WriteProfileString(L"txsync", L"fork", forkCs);

    if (m_computeThread.joinable()) m_computeThread.join();   // defensive; disabled combos already prevent this
    SetBusy(true);
    m_progressText.SetWindowText(L"Loading branches…");

    std::string owner, repo; SplitOwnerRepo(std::string(CW2A(forkCs, CP_UTF8)), owner, repo);
    HWND hwnd = GetSafeHwnd();
    std::vector<std::string> langs = m_langs;
    auto stored = github::load_token();
    github::Token tok = stored.value_or(github::Token{});
    m_computeThread = std::thread([hwnd, langs, owner, repo, tok]() {
        std::vector<std::string> branches = github::list_branches(tok, owner, repo);
        auto it = std::find(branches.begin(), branches.end(), "transifex");
        std::string selBranch = it != branches.end() ? *it
                                : branches.empty() ? std::string(config::TRANSIFEX_BRANCH) : branches.front();

        auto* br = new BranchesResult{ branches, selBranch };
        if (!::PostMessage(hwnd, WM_APP_TXSYNC_BRANCHES, (WPARAM)br, 0)) { delete br; return; }  // dialog gone

        RunComputeAndPost(hwnd, langs, owner, repo, selBranch, tok);
    });
}

// Branch combo changed (same fork): no branch-list refetch needed, just recompute.
void TxSyncDlg::OnBranchChanged() {
    int selF = m_forkCombo.GetCurSel(); if (selF == CB_ERR) return;
    int selB = m_branchCombo.GetCurSel(); if (selB == CB_ERR) return;
    CString forkCs; m_forkCombo.GetLBText(selF, forkCs);
    CString branchCs; m_branchCombo.GetLBText(selB, branchCs);
    if (CWinApp* app = AfxGetApp()) app->WriteProfileString(L"txsync", L"branch", branchCs);

    std::string owner, repo; SplitOwnerRepo(std::string(CW2A(forkCs, CP_UTF8)), owner, repo);
    StartCompute(owner, repo, std::string(CW2A(branchCs, CP_UTF8)));
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

    {
        std::vector<txsync::CategoryMismatch> mism;
        for (const auto& e : edits) {
            const std::string& p = e.repo_path;
            const std::string suf = ".strings.po";
            if (p.size() < suf.size() || p.compare(p.size()-suf.size(), suf.size(), suf) != 0) continue;
            // lang between "mpc-hc." and ".strings.po"
            size_t a = p.rfind("mpc-hc."); if (a==std::string::npos) continue; a += 7;
            size_t b = p.rfind(".strings.po");
            std::string lang = p.substr(a, b-a);
            PoFile po = PoFile::parse_bytes(e.content);
            for (auto& f : validate::analyze_category_tree(po)) mism.push_back({ lang, f });
        }
        if (!mism.empty()) {
            CString rep = L"This branch can't be updated — the Options tree would break (missing / merged "
                          L"nodes). Fix these in the translation source, then re-run:\n";
            int shown = 0;
            for (const auto& m : mism) {
                if (shown++ >= 25) { rep += L"\n…and more."; break; }
                rep += L"\n• " + CString(CA2W(m.lang.c_str(), CP_UTF8)) + L"  " +
                       CString(CA2W(m.finding.msgctxt.c_str(), CP_UTF8)) + L": " +
                       CString(CA2W(m.finding.message.c_str(), CP_UTF8));
            }
            rep += L"\n\nUse the \"Show only Options-tree mismatches\" filter on the Sync review tab to inspect them.";
            MessageBox(rep, L"Cannot update branch — Options-tree mismatch", MB_ICONERROR);
            return;
        }
    }

    // m_txOwner/m_txRepo/m_txBranch, not the live combo selection -- see their comment in TxSyncDlg.h:
    // this must match what m_result was actually computed against.
    CString txRepo = CString(CA2W(m_txOwner.c_str(), CP_UTF8)) + L"/" + CString(CA2W(m_txRepo.c_str(), CP_UTF8)) +
                     L"@" + CString(CA2W(m_txBranch.c_str(), CP_UTF8));
    CString msg; msg.Format(L"Push %d changed file(s) directly to the %s branch?\n\n"
                            L"This updates the branch immediately -- there is no PR review step.",
                            (int)edits.size(), (LPCWSTR)txRepo);
    if (MessageBox(msg, L"Update Transifex branch", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    try {
        CWaitCursor wait;
        auto tok = EnsureGithubToken(this);
        if (!tok) return;
        // Authoritative owner check on the JUST-authenticated identity (the up-front button gating
        // only covers a token that was already stored when the dialog computed).
        std::string login = github::whoami(*tok);
        if (_stricmp(login.c_str(), m_txOwner.c_str()) != 0) {
            MessageBox(L"Signed in as \"" + CString(CA2W(login.c_str(), CP_UTF8)) +
                       L"\" — only " + CString(CA2W(m_txOwner.c_str(), CP_UTF8)) +
                       L" can update this branch.\n\nUse \"Propose upstream PR…\" instead.",
                       L"Update Transifex branch", MB_ICONWARNING);
            m_login = login;
            m_btnUpdateBranch.EnableWindow(FALSE);
            return;
        }
        std::string sha = github::update_transifex_branch(*tok, m_txOwner, m_txRepo, m_txBranch, edits,
            "Merge upstream develop + Transifex sync (Studio)");
        MessageBox(L"Updated the branch.\n\nNew commit: " + CString(CA2W(sha.c_str(), CP_UTF8)),
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

// Updates the OPEN sync PR's branch in place with any new valid translations, instead of opening a
// brand-new PR -- so the review window accumulates translations rather than restarting each time.
// Translations that would break the Options tree are held back (reverted to the develop-base value)
// by tx_build_edits_validated rather than blocking the whole refresh.
void TxSyncDlg::OnRefreshPrClicked() {
    if (!m_haveResult) return;
    auto tok = EnsureGithubToken(this);
    if (!tok) return;
    std::string login = github::whoami(*tok);
    // Find the open sync PR branch on the user's fork of upstream.
    std::vector<std::string> branches = github::list_branches(*tok, login, config::UPSTREAM_REPO);
    std::vector<std::string> sync;
    for (auto& b : branches) if (b.rfind("transifex-sync-", 0) == 0) sync.push_back(b);
    if (sync.empty()) {
        MessageBox(L"No open Transifex-sync PR branch (transifex-sync-*) found on " +
                   CString(CA2W(login.c_str(), CP_UTF8)) + L"/" + CString(config::UPSTREAM_REPO) +
                   L".\n\nUse “Propose upstream PR…” to open one first.",
                   L"Refresh open PR", MB_ICONINFORMATION);
        return;
    }
    std::sort(sync.begin(), sync.end());          // "<prefix>-YYYYMMDD-HHMM" sorts chronologically
    std::string branch = sync.back();             // newest
    // Branches outlive their PRs: refuse to top up the dead branch of an already-merged sync PR.
    int prNum = github::open_pr_number(*tok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO, login, branch);
    if (prNum == 0) {
        MessageBox(L"No OPEN pull request found for " + CString(CA2W(branch.c_str(), CP_UTF8)) +
                   L" on " + CString(config::UPSTREAM_OWNER) + L"/" + CString(config::UPSTREAM_REPO) +
                   L" — it was probably merged or closed.\n\nUse “Propose upstream PR…” to open a new sync PR.",
                   L"Refresh open PR", MB_ICONINFORMATION);
        return;
    }
    std::vector<txsync::HeldTranslation> held;
    std::vector<github::FileEdit> edits = txsync::tx_build_edits_validated(m_result, held);
    if (edits.empty()) {
        CString m; m.Format(L"Nothing new to add to %s.", (LPCWSTR)CString(CA2W(branch.c_str(), CP_UTF8)));
        if (!held.empty()) { CString h; h.Format(L"\n\n%zu translation(s) were held back (would break the Options tree).", held.size()); m += h; }
        MessageBox(m, L"Refresh open PR", MB_ICONINFORMATION);
        return;
    }
    CString msg; msg.Format(L"Update PR branch %s on %s/%s with %d changed file(s)?%s\n\n"
                            L"The open pull request will pick up the new translations automatically.",
        (LPCWSTR)CString(CA2W(branch.c_str(), CP_UTF8)), (LPCWSTR)CString(CA2W(login.c_str(), CP_UTF8)),
        (LPCWSTR)CString(config::UPSTREAM_REPO), (int)edits.size(),
        held.empty() ? L"" : L"  (some translations held back — see next.)");
    if (!held.empty()) {
        CString h; int shown = 0;
        for (auto& x : held) { if (shown++ >= 20) { h += L"\n  …and more."; break; }
            h += L"\n  • " + CString(CA2W(x.lang.c_str(),CP_UTF8)) + L" " +
                 CString(CA2W(x.msgctxt.c_str(),CP_UTF8)) + L" — " + CString(CA2W(x.reason.c_str(),CP_UTF8)); }
        msg += L"\n\nHeld back:" + h;
    }
    if (MessageBox(msg, L"Refresh open PR", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    try {
        CWaitCursor wait;
        std::string sha = github::rebase_pr_branch(*tok, login, config::UPSTREAM_REPO, branch, edits,
            "Refresh Transifex sync (Studio)");
        MessageBox(L"Refreshed " + CString(CA2W(branch.c_str(), CP_UTF8)) + L".\n\nNew commit: " +
                   CString(CA2W(sha.c_str(), CP_UTF8)) + L"\n\nThe open PR now reflects the new translations.",
                   L"Refresh open PR", MB_ICONINFORMATION);
    } catch (const std::exception& ex) {
        MessageBox(L"Refresh failed:\n" + CString(CA2W(ex.what(), CP_UTF8)) +
                   L"\n\n(If the branch moved since this dialog computed, close and reopen Transifex sync to recompute, then retry.)",
                   L"Refresh open PR", MB_ICONERROR);
    }
}

// Reverse sync: push upstream develop's translations INTO the live Transifex DB wherever upstream
// should win -- see mpctrans::txsync::tx_reverse_push_plan's doc comment for why this is needed at
// all (Transifex's GitHub integration is one-way DB->branch). Mirrors the CLI's push-to-transifex
// subcommand (studio/cli/txsync_cli.cpp's cmd_push_to_transifex), but async: the plan alone is
// ~(languages x resources) list_translations calls plus one blame_line_dates GraphQL call per
// changed file, easily north of a thousand HTTP round-trips -- far too slow to run on the UI thread.
void TxSyncDlg::OnPushTxClicked() {
    if (!m_haveResult) return;
    // Blame evidence needs GitHub; the plan/apply need Transifex -- both sign-ins happen up front, on
    // the UI thread, same posture as EnsureGithubToken everywhere else in this file.
    auto gh = EnsureGithubToken(this);
    if (!gh) return;
    auto tx = TxTokenDlg::Ensure(this);
    if (!tx) return;

    if (m_computeThread.joinable()) m_computeThread.join();   // defensive; disabled controls already prevent this
    SetBusy(true);
    m_status.SetWindowText(L"Planning Transifex push…");
    m_progressText.SetWindowText(L"");

    HWND hwnd = GetSafeHwnd();
    txsync::TxSyncResult resultCopy = m_result;   // the worker mutates its OWN copy (include flags) --
                                                   // never m_result directly, see PushPlanResult's comment
    github::Token ghTok = *gh;
    txapi::Token txTok = *tx;
    m_computeThread = std::thread([hwnd, resultCopy = std::move(resultCopy), ghTok, txTok]() mutable {
        auto* out = new PushPlanResult;
        try {
            auto listTx = [&](int res, const std::string& lang) {
                return txapi::list_translations(txTok, res, lang);
            };
            auto blameDates = [&](const std::string& path) {
                return github::blame_line_dates(ghTok, config::UPSTREAM_OWNER, config::UPSTREAM_REPO,
                                                config::UPSTREAM_BRANCH, path);
            };
            out->plan = txsync::tx_reverse_push_plan(resultCopy, listTx, blameDates);
            out->result = std::move(resultCopy);
        } catch (const std::exception& ex) {
            out->ok = false; out->error = CString(CA2W(ex.what(), CP_UTF8));
        }
        if (!::PostMessage(hwnd, WM_APP_TXSYNC_PUSHPLAN, (WPARAM)out, 0)) delete out;   // dialog gone
    });
}

// WM_APP_TXSYNC_PUSHPLAN: the reverse-push plan finished. On success the mutated result is copied
// back into m_result and the list is rebuilt REGARDLESS of what the user does with the confirm
// prompt below -- the flipped include flags are the plan's verdict on what upstream should keep
// (see tx_reverse_push_plan's doc comment), not conditional on the push actually landing; the CLI's
// cmd_update_branch applies them the same way before its own tx_build_edits call.
LRESULT TxSyncDlg::OnPushPlanDone(WPARAM wp, LPARAM) {
    std::unique_ptr<PushPlanResult> out((PushPlanResult*)wp);
    if (m_computeThread.joinable()) m_computeThread.join();
    SetBusy(false);
    m_progressText.SetWindowText(L"");

    if (!out->ok) {
        RefreshStatusText();
        MessageBox(L"Planning the Transifex push failed:\n" + out->error, L"Push to Transifex", MB_ICONERROR);
        return 0;
    }

    m_result = std::move(out->result);
    RebuildRows();
    RepopulateList();
    RefreshStatusText();

    const txsync::ReversePushPlan& plan = out->plan;
    if (plan.push.empty()) {
        MessageBox(L"Nothing to push: Transifex already agrees with upstream.",
                   L"Push to Transifex", MB_ICONINFORMATION);
        return 0;
    }

    CString msg; msg.Format(L"%d translation(s) to push into Transifex, %d kept (Transifex newer / "
                            L"sync-origin), %d note(s).\n",
                            (int)plan.push.size(), (int)plan.keepTransifex.size(), (int)plan.notes.size());
    int shown = 0;
    for (const auto& it : plan.push) {
        if (shown++ >= 15) { msg += L"\n  …and more."; break; }
        msg += L"\n  " + CString(CA2W(it.lang.c_str(), CP_UTF8)) + L" " +
               CString(CA2W(config::RESOURCES[it.res], CP_UTF8)) + L" " +
               CString(CA2W(it.msgctxt.c_str(), CP_UTF8)) + L" — " + CString(CA2W(it.reason.c_str(), CP_UTF8));
    }
    msg += L"\n\nPush these into Transifex now?";
    if (MessageBox(msg, L"Push to Transifex", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;

    // Re-check the token rather than reusing the one captured for planning -- it's been moments, but
    // TxTokenDlg::Ensure/Clear could in principle have run again via the File menu while this modal
    // dialog... can't actually happen (this whole dialog is itself modal), but load_token() is cheap
    // and this is the one write path, so check rather than assume.
    auto tx = txapi::load_token();
    if (!tx) {
        MessageBox(L"No Transifex token stored — use File > Transifex token… first.",
                   L"Push to Transifex", MB_ICONERROR);
        return 0;
    }

    SetBusy(true);
    m_status.SetWindowText(L"Pushing to Transifex…");
    HWND hwnd = GetSafeHwnd();
    txapi::Token txTok = *tx;
    txsync::ReversePushPlan planCopy = plan;
    m_computeThread = std::thread([hwnd, planCopy = std::move(planCopy), txTok]() mutable {
        auto* out2 = new PushApplyResult;
        try {
            txsync::tx_apply_reverse_push(txTok, planCopy,
                [&](const txsync::ReversePushItem& it, bool ok, const std::string& itMsg) {
                    if (ok) { ++out2->pushed; return; }
                    if (out2->skippedOrFailed.size() < 30)
                        out2->skippedOrFailed.push_back(
                            CString(CA2W(it.lang.c_str(), CP_UTF8)) + L" " +
                            CString(CA2W(config::RESOURCES[it.res], CP_UTF8)) + L" " +
                            CString(CA2W(it.msgctxt.c_str(), CP_UTF8)) + L" — " +
                            CString(CA2W(itMsg.c_str(), CP_UTF8)));
                });
        } catch (const std::exception& ex) {
            out2->ok = false; out2->error = CString(CA2W(ex.what(), CP_UTF8));
        }
        if (!::PostMessage(hwnd, WM_APP_TXSYNC_PUSHDONE, (WPARAM)out2, 0)) delete out2;   // dialog gone
    });
    return 0;
}

// WM_APP_TXSYNC_PUSHDONE: the apply worker finished -- report what actually happened in Transifex.
// (m_result/the list were already updated back in OnPushPlanDone; nothing more to reconcile here.)
LRESULT TxSyncDlg::OnPushApplyDone(WPARAM wp, LPARAM) {
    std::unique_ptr<PushApplyResult> out((PushApplyResult*)wp);
    if (m_computeThread.joinable()) m_computeThread.join();
    SetBusy(false);
    RefreshStatusText();
    m_progressText.SetWindowText(L"");

    if (!out->ok) {
        MessageBox(L"Pushing to Transifex failed:\n" + out->error, L"Push to Transifex", MB_ICONERROR);
        return 0;
    }

    CString msg; msg.Format(L"Pushed %d translation(s) to Transifex.", out->pushed);
    if (!out->skippedOrFailed.empty()) {
        msg += L"\n\nSkipped/failed:";
        int shown = 0;
        for (const auto& l : out->skippedOrFailed) {
            if (shown++ >= 20) { msg += L"\n  …and more."; break; }
            msg += L"\n  " + l;
        }
    }
    MessageBox(msg, L"Push to Transifex", MB_ICONINFORMATION);
    return 0;
}
