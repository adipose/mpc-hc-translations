// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "StudioPropPageFrame.h"
#include "Theme.h"

BEGIN_MESSAGE_MAP(CStudioPropPageFrame, CPropPageFrameDefault)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

BOOL CStudioPropPageFrame::Create(DWORD dwWindowStyle, const RECT& rect, CWnd* pwndParent, UINT nID) {
    return CWnd::Create(
        AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW, ::LoadCursor(nullptr, IDC_ARROW), 0),
        _T("Studio Page Frame"),
        dwWindowStyle, rect, pwndParent, nID);
}

CWnd* CStudioPropPageFrame::GetWnd() { return static_cast<CWnd*>(this); }

// Verbatim port of CMPCThemePropPageFrame::DrawCaption (upstream/src/mpc-hc/CMPCThemePropPageFrame.cpp):
// CMPCTheme::ContentSelectedColor/ContentBGColor/PropPageCaptionFGColor -> Theme::CONTENT_SEL/
// CONTENT_BG/TEXT, and GetMessageFont(&lf) (MPC-HC's DSUtil helper) -> the SPI_GETNONCLIENTMETRICS
// lfMessageFont lookup it wraps (same pattern MainFrame.cpp already uses). `rect` (the caption band)
// comes from the inherited CalcCaptionArea() -- unmodified from upstream, so it already carries the
// same themed tab-pane content-rect inset the real player's caption relies on. FillGradientRectH is
// the protected helper inherited from CPropPageFrameDefault (TreePropSheet/PropPageFrameDefault.cpp).
void CStudioPropPageFrame::DrawCaption(CDC* pDC, CRect rect, LPCTSTR lpszCaption, HICON hIcon) {
    COLORREF    clrLeft = Theme::CONTENT_SEL;
    COLORREF    clrRight = Theme::CONTENT_BG;
    FillGradientRectH(pDC, rect, clrLeft, clrRight);

    rect.left += 2;

    COLORREF clrPrev = pDC->SetTextColor(Theme::TEXT);
    int nBkStyle = pDC->SetBkMode(TRANSPARENT);

    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONT lf = ncm.lfMessageFont;
    lf.lfHeight = static_cast<long>(-.8f * rect.Height());
    lf.lfWeight = FW_BOLD;
    CFont f;
    f.CreateFontIndirectW(&lf);
    CFont* oldFont = pDC->SelectObject(&f);

    TEXTMETRIC GDIMetrics;
    GetTextMetricsW(pDC->GetSafeHdc(), &GDIMetrics);
    while (GDIMetrics.tmHeight > rect.Height() && abs(lf.lfHeight) > 10) {
        pDC->SelectObject(oldFont);
        f.DeleteObject();
        lf.lfHeight++;
        f.CreateFontIndirectW(&lf);
        pDC->SelectObject(&f);
        GetTextMetricsW(pDC->GetSafeHdc(), &GDIMetrics);
    }

    rect.top -= GDIMetrics.tmDescent - 1;

    pDC->DrawTextW(lpszCaption, rect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS); // DT_NOPREFIX not needed

    pDC->SetTextColor(clrPrev);
    pDC->SelectObject(oldFont);
    pDC->SetBkMode(nBkStyle);
}

void CStudioPropPageFrame::OnPaint() {
    CPaintDC dc(this);
    Draw(&dc);
}

// Replaces CMPCThemePropPageFrame::OnEraseBkgnd's MPCThemeUtil::MPCThemeEraseBkgnd + WindowBorderColorLight
// (both MPC-HC-only) with Studio's own Theme:: equivalents: fill the client, then frame it with the
// grid-line color -- the same look the old hand-painted CThemedHostWnd chrome drew, now painted natively
// by this real sibling window.
BOOL CStudioPropPageFrame::OnEraseBkgnd(CDC* pDC) {
    CRect rect; GetClientRect(&rect);
    pDC->FillSolidRect(rect, Theme::WINDOW_BG);
    CBrush border(Theme::GRID_LINE);
    pDC->FrameRect(rect, &border);
    return TRUE;
}
