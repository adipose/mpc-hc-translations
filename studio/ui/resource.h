// SPDX-License-Identifier: GPL-3.0-or-later
// Studio's OWN chrome ids — NOT the translated MPC-HC resources (those come from the neutral
// DLL at runtime and keep their upstream ids).
#pragma once

#define IDI_STUDIO           1     // app icon; lowest icon id = what Explorer shows for the exe
#define IDR_MAINFRAME        128

#define ID_FILE_SUBMITPR     32771
#define ID_FILE_RENDERRC     32772
#define ID_FILE_RENDERBUNDLE 32773
#define ID_FILE_DEMO_SELECT  32774

#define ID_VIEW_THEME_DARK    32781
#define ID_VIEW_THEME_LIGHT   32782
#define ID_VIEW_THEME_SYSTEM  32783

#define ID_FILE_AI_PROVIDER   32784
#define ID_FILE_GENERATE_AI   32785
#define ID_FILE_GENERATE_AI_ALL 32786
#define ID_FILE_EXPORT_AI_CTX      32787
#define ID_FILE_EXPORT_AI_CTX_LANG 32788
#define ID_FILE_CHECK_DATA_UPDATE  32789
#define ID_FILE_TXSYNC             32790
#define ID_FILE_TXSYNC_PR          32791

// MainFrame children
#define IDC_LANG_COMBO       1001
#define IDC_BTN_CHECKOUT     1002
#define IDC_TAB_SURFACE      1004
#define IDC_DLG_COMBO        1005
#define IDC_STR_LIST         1006
#define IDC_STATUS_TEXT      1008
#define IDC_EDIT_PANEL       1009
#define IDC_MENU_TREE        1010
#define IDC_PROGRESS         1011
#define IDC_CMDHELP          1012
#define IDC_RESEARCH         1013
#define IDC_BTN_SUGGESTFIX   1014
#define IDC_BTN_DISMISS      1015
#define IDC_BTN_MENU_PREVIEW 1016
#define IDC_SYNC_FIT_FILTER  1017
#define IDC_SYNC_TREE_FILTER 1018

// "Suggest a correction" modal (SuggestFixDlg) — an empty DIALOGEX template (Studio.rc); every
// control is created programmatically in OnInitDialog, same style as EditPanel.
#define IDD_SUGGEST_FIX      4001
// "AI provider…" modal (AiSettingsDlg) — same empty-DIALOGEX-template style.
#define IDD_AI_SETTINGS      4002
// "Transifex sync…" modal (TxSyncDlg) — same empty-DIALOGEX-template style.
#define IDD_TXSYNC           4003

// EditPanel children
#define IDC_EP_CONTEXT       2001
#define IDC_EP_ENGLISH       2002
#define IDC_EP_TRANSLATION   2003
#define IDC_EP_APPLY         2004
#define IDC_EP_AI_TEXT       2005
#define IDC_EP_AI_ACCEPT     2006
#define IDC_EP_REFS          2007
#define IDC_EP_INFO          2008
#define IDC_EP_VALIDATION    2009
#define IDC_EP_AI_SUGGEST    2010
#define IDC_EP_INS_NL        2011
#define IDC_EP_INS_TAB       2012

// AiSettingsDlg children (Save/Cancel use IDOK/IDCANCEL directly, same as SuggestFixDlg's
// m_btnSubmit/m_btnCancel, so CDialog's default OnOK/OnCancel command routing + Enter/Esc apply)
#define IDC_AI_PROVIDER      2101
#define IDC_AI_MODEL         2102
#define IDC_AI_KEY           2103
#define IDC_AI_HINT          2104

// TxSyncDlg children (Close uses IDCANCEL directly, same rationale as above)
#define IDC_TXSYNC_STATUS         2201
#define IDC_TXSYNC_PROGRESS_TEXT  2202
#define IDC_TXSYNC_LIST           2203
#define IDC_TXSYNC_SHOW_PROTECTED 2204
#define IDC_TXSYNC_BTN_UPDATE     2205
#define IDC_TXSYNC_BTN_PR         2206
#define IDC_TXSYNC_BTN_CLOSE      2207
#define IDC_TXSYNC_FORK           2208
#define IDC_TXSYNC_BRANCH         2209

// Dark-theme checkbox / radio sprite strips (Theme.cpp), copied verbatim from upstream
// res/darktheme/checkboxes-*.png and radios-*.png — see CMPCTheme::ThemeCheckBoxes / ThemeRadios.
#define IDB_DT_CB_96          3001
#define IDB_DT_CB_120         3002
#define IDB_DT_CB_144         3003
#define IDB_DT_CB_192         3004
#define IDB_DT_RADIO_96       3011
#define IDB_DT_RADIO_120      3012
#define IDB_DT_RADIO_144      3013
#define IDB_DT_RADIO_192      3014
