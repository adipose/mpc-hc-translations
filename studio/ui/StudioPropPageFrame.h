// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TreePropSheet/PropPageFrameDefault.h"

// Dark-themed property-page frame -- Studio's port of upstream CMPCThemePropPageFrame (see
// upstream/src/mpc-hc/CMPCThemePropPageFrame.h/.cpp), hosted as a REAL sibling window sitting behind
// the rendered dialog page, exactly like the player's TreePropSheet: CThemedHostWnd (MainFrame.h)
// creates one as a WS_CHILD, sizes/positions it to the page-frame's outer rect, and the LivePreview-
// rendered dialog is a SEPARATE sibling positioned on top, starting below the caption band -- see
// CThemedHostWnd::SetFrame/RecalcAndReposition in MainFrame.cpp.
class CStudioPropPageFrame : public TreePropSheet::CPropPageFrameDefault {
public:
    virtual BOOL Create(DWORD dwWindowStyle, const RECT& rect, CWnd* pwndParent, UINT nID);
    virtual CWnd* GetWnd();

    virtual void DrawCaption(CDC* pDc, CRect rect, LPCTSTR lpszCaption, HICON hIcon);

protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    DECLARE_MESSAGE_MAP()
};
