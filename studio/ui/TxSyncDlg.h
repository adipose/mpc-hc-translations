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
    afx_msg LRESULT OnComputeProgress(WPARAM done, LPARAM total);
    afx_msg LRESULT OnComputeDone(WPARAM, LPARAM);
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

    MainFrame* m_mainFrame = nullptr;

    mpctrans::txsync::TxSyncResult m_result;
    bool m_haveResult = false;
    std::thread m_computeThread;
    // Index into m_result.decisions for each row currently shown in m_list (post-filter); the
    // checkbox column's LVN_ITEMCHANGED handler writes back through this to the right decision.
    std::vector<size_t> m_visible;

    CStatic   m_status;          // top summary line (counts) -- see RefreshStatusText
    CStatic   m_progressText;    // "Fetching 132 files… 40/132", hidden once the compute finishes
    CListCtrl m_list;
    CButton   m_showProtected;
    CButton   m_btnUpdateBranch, m_btnProposePr, m_btnClose;
};
