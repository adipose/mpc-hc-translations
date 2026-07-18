// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "StudioApp.h"
#include "MainFrame.h"
#include "resource.h"

StudioApp theApp;

BOOL StudioApp::InitInstance() {
    CWinApp::InitInstance();
    SetRegistryKey(L"MPC-HC Translation Studio");   // persist prefs (theme) under HKCU\Software\...
    auto* frame = new MainFrame();
    if (!frame->Create(nullptr, L"MPC-HC Translation Studio", WS_OVERLAPPEDWINDOW,
                       CFrameWnd::rectDefault, nullptr, MAKEINTRESOURCE(IDR_MAINFRAME)))
        return FALSE;
    m_pMainWnd = frame;
    frame->ShowWindow(SW_SHOWMAXIMIZED);
    frame->UpdateWindow();
    return TRUE;
}
int StudioApp::ExitInstance() { return CWinApp::ExitInstance(); }
