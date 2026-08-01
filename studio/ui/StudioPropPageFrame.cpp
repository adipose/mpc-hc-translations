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

    COLORREF clrPrev = pDC->SetTextColor(Theme::PROPPAGE_CAPTION_FG);
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

// Port of CMPCThemePropPageFrame::OnEraseBkgnd after the player's light-consistency overhaul
// (upstream a65aa71ce): dark fills the themed window background; light defers to the DEFAULT frame
// erase (themed TABP_BODY, matching its native pages). The frame border is drawn in BOTH modes with
// WindowBorderColorLight -- "the caption is themed in light too, so keep the matching frame border".
BOOL CStudioPropPageFrame::OnEraseBkgnd(CDC* pDC) {
    BOOL ret;
    if (Theme::IsDark()) {
        CRect rect; GetClientRect(&rect);
        pDC->FillSolidRect(rect, Theme::WINDOW_BG);
        ret = TRUE;
    } else {
        ret = __super::OnEraseBkgnd(pDC);
    }
    CRect rect; GetClientRect(&rect);
    CBrush border(Theme::FRAME_BORDER);
    pDC->FrameRect(rect, &border);
    return ret;
}
