// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <afxwin.h>
#include <afxcmn.h>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "mpctrans/txsync.h"

class MainFrame;

// "Transifex sync…" modal (File > Transifex sync…) — runs mpctrans::txsync::tx_compute on a
// background thread (network: ~3 files/language from two CDNs), shows the merge decisions in a
// checkbox list, and lets the user either push the merge straight to the transifex branch
// (github::update_transifex_branch) or propose it as a PR against upstream (github::open_pr, same
// review-on-GitHub flow as MainFrame::SubmitPr). Same programmatic, empty-DIALOGEX-template style as
// SuggestFixDlg/AiSettingsDlg: every control is created in OnInitDialog, no RC control layout.
class TxSyncDlg : public CDialog {
public:
    // `mainFrame` is used only for double-click navigation (NavigateToString) -- never for anything
    // that would mutate its state on this (UI) thread from the background compute, see OnInitDialog.
    explicit TxSyncDlg(MainFrame* mainFrame);
    ~TxSyncDlg();

protected:
    BOOL OnInitDialog() override;
    void OnCancel() override;   // Close / Esc / titlebar X -- joins the worker if still running
    afx_msg HBRUSH OnCtlColor(CDC*, CWnd*, UINT);
    afx_msg BOOL OnEraseBkgnd(CDC*);
    afx_msg void OnShowProtectedClicked();
    afx_msg void OnListDoubleClick(NMHDR*, LRESULT*);
    afx_msg void OnListItemChanged(NMHDR*, LRESULT*);   // checkbox state -> m_result row (LVIF_STATE)
    afx_msg void OnUpdateBranchClicked();
    afx_msg void OnProposePrClicked();
    afx_msg void OnRefreshPrClicked();
    afx_msg void OnForkChanged();     // fork combo -> refetch that fork's branches, then recompute
    afx_msg void OnBranchChanged();   // branch combo (same fork) -> recompute only
    afx_msg LRESULT OnComputeProgress(WPARAM done, LPARAM total);
    afx_msg LRESULT OnComputeDone(WPARAM, LPARAM);
    afx_msg LRESULT OnForksDone(WPARAM, LPARAM);      // initial fork+branch list resolved -> populate both combos
    afx_msg LRESULT OnBranchesDone(WPARAM, LPARAM);   // fork changed -> that fork's branch list resolved
    DECLARE_MESSAGE_MAP()

private:
    void Layout();
    int  S(int v) const { return ::MulDiv(v, m_dpi, 96); }   // 96-DPI-logical px -> device px
    int  m_dpi = 96;
    CFont m_font;

    void RefreshStatusText();
    CString BuildSummaryText() const;              // counts sentence -- top static AND the PR body
    void RebuildRows();                          // apply the "Show protected" filter -> m_visible
    void RepopulateList();                        // m_visible -> the SysListView32 rows
    static CString KindLabel(mpctrans::txsync::TxDecision::Kind);
    // Disables the fork/branch combos + both action buttons for the duration of a background task
    // (initial fetch+compute, a fork change's branch-refetch+compute, or a plain recompute) -- mirrors
    // the buttons' existing busy gating, extended to the combos so the compute-matches-selection
    // invariant can never be raced by the user picking a different fork/branch mid-flight.
    void SetBusy(bool busy);
    void StartCompute(const std::string& owner, const std::string& repo, const std::string& branch);

    MainFrame* m_mainFrame = nullptr;

    mpctrans::txsync::TxSyncResult m_result;
    bool m_haveResult = false;
    std::string m_login;   // signed-in GitHub user ("" until known) -- gates the branch-update button
    std::thread m_computeThread;
    // Index into m_result.decisions for each row currently shown in m_list (post-filter); the
    // checkbox column's LVN_ITEMCHANGED handler writes back through this to the right decision.
    std::vector<size_t> m_visible;

    // Snapshotted once in OnInitDialog (see LanguageList's UI-thread-only rationale), reused as-is by
    // every recompute (fork change / branch change) so those don't need MainFrame again.
    std::vector<std::string> m_langs;
    // The fork ("owner/repo") + branch the CURRENTLY SHOWN m_result/m_list came from -- echoed back by
    // the worker in ComputeResult (see TxSyncDlg.cpp) rather than read live off the combos, so the
    // action handlers can never act on a selection newer than what was actually computed (the
    // compute-matches-selection invariant the feature spec calls out). Valid once m_haveResult is true.
    std::string m_txOwner, m_txRepo, m_txBranch;

    CStatic   m_status;          // top summary line (counts) -- see RefreshStatusText
    CStatic   m_lblFork, m_lblBranch;
    CComboBox m_forkCombo, m_branchCombo;
    CStatic   m_progressText;    // "Fetching 132 files… 40/132", hidden once the compute finishes
    CListCtrl m_list;
    CButton   m_showProtected;
    CButton   m_btnUpdateBranch, m_btnProposePr, m_btnClose;
    CButton   m_btnRefreshPr;
};
