// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <afxwin.h>

// MFC application object. UI + rendering only — all PO / validation / GitHub logic lives in
// libmpctrans (the portable core). Ships as one static native .exe (Path B).
class StudioApp : public CWinApp {
public:
    BOOL InitInstance() override;   // create MainFrame; ensure a bundle is present; restore token
    int  ExitInstance() override;
};
