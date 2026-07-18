// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>

// MPC-HC "Modern" dark theme, shared across the whole Studio UI (preview dialogs, the main frame
// chrome, and the edit panel). Palette + owner-draw routines mirror CMPCTheme / CMPCThemeUtil so a
// translator sees the app the way MPC-HC renders it. See LivePreview / MainFrame / EditPanel callers.
namespace Theme {
// The app is always themed with an MPC palette. The user's preference is Dark, Light, or System
// (follow the Windows app theme); System resolves to Dark or Light at apply time.
enum class Mode { Dark, Light, System };
void  SetMode(Mode pref);      // store the preference + apply the resolved palette/brushes
Mode  Preference();            // the user's stored choice (may be System)
bool  IsDark();                // the effective palette is the dark one

// --- palette (set per-mode by SetMode; values below are the active theme's colors) ---
extern COLORREF WINDOW_BG;     // dialog / page / frame background
extern COLORREF CONTENT_BG;    // edit / combo / list / tree interior
extern COLORREF CONTENT_SEL;   // selected row (CMPCTheme ContentSelectedColor)
extern COLORREF CONTENT_DISABLED; // disabled list row band (CMPCTheme ListCtrlDisabledBGColor)
extern COLORREF GRID_LINE;     // list-view grid lines (CMPCTheme ListCtrlGridColor)
extern COLORREF TAB_INACTIVE;  // unselected tab fill (CMPCTheme TabCtrlInactiveColor)
extern COLORREF TAB_BORDER;    // tab outline / content baseline (CMPCTheme TabCtrlBorderColor)
extern COLORREF TEXT;
extern COLORREF TEXT_DIM;      // faded text (CMPCTheme TextFGColorFade — unfocused menubar caption, etc.)
extern COLORREF TEXT_DISABLED; // disabled button/check/combo label (CMPCTheme ButtonDisabledFGColor)
extern COLORREF GROUP_BORDER;
extern COLORREF GROUP_DISABLED; // disabled group-box title text (CMPCTheme ContentTextDisabledFGColorFade)
extern COLORREF CTRL_BORDER;   // edit / combo outline
extern COLORREF BTN_FILL;
extern COLORREF BTN_OUTER;
extern COLORREF BTN_INNER;
extern COLORREF BTN_INNER_FOCUS; // focused (not hot/pressed) push-button border (ButtonBorderInnerFocusedColor)
extern COLORREF BTN_FILL_HOVER;  // ButtonFillHoverColor
extern COLORREF BTN_FILL_SEL;    // ButtonFillSelectedColor (pressed / open combo)
extern COLORREF BTN_DOT;         // keyboard-focus dotted rect, default (ButtonBorderKBFocusColor)
extern COLORREF BTN_DOT_SEL;     // ... while pressed (ButtonBorderSelectedKBFocusColor)
extern COLORREF BTN_DOT_HOVER;   // ... while hot (ButtonBorderHoverKBFocusColor)
extern COLORREF CHK_BG;
extern COLORREF CHK_BORDER;
extern COLORREF CHK_BG_HOVER;
extern COLORREF CHK_BORDER_HOVER;
extern COLORREF CHK_MARK;
extern COLORREF COMBO_ARROW;          // theme-invariant (CMPCTheme::ComboboxArrowColor)
extern COLORREF COMBO_ARROW_DISABLED; // theme-invariant (CMPCTheme::ComboboxArrowColorDisabled)
extern COLORREF HEADER_HOT;    // hot-tracked column header (CMPCTheme ColumnHeaderHotColor)
extern COLORREF HEADER_GRID;   // header column separators (CMPCTheme HeaderCtrlGridColor)
extern COLORREF HEADER_SORT_ARROW; // theme-invariant (CMPCTheme::HeaderCtrlSortArrowColor)
// menus (owner-drawn like CMPCThemeMenu)
extern COLORREF MENU_BG;       // popup submenu background (CMPCTheme MenuBGColor)
extern COLORREF MENU_SEL;      // popup item hover (CMPCTheme MenuSelectedColor)
extern COLORREF MENUBAR_BG;    // top menu-bar background (CMPCTheme MenubarBGColor — differs from MENU_BG in light mode)
extern COLORREF MENUBAR_SEL;   // top menu-bar item hover (CMPCTheme MenubarSelectedBGColor)
extern COLORREF MENU_SEP;
extern COLORREF MENU_DISABLED;
extern COLORREF MENU_BORDER;
extern COLORREF MENU_ARROW;

HBRUSH windowBrush();          // current-palette background brushes (recreated on SetMode)
HBRUSH contentBrush();

// Call once on the top-level frame: opt the process into dark mode (so common-control scrollbars /
// selection follow DarkMode_Explorer) and set the title bar per the current mode. No-ops on old OS.
void InitTopWindow(HWND top);
void SetDarkTitleBar(HWND top, bool dark);   // force the immersive dark title bar on/off
void ApplyTitleBar(HWND top);                // set the title bar per the current mode (Windows->OS)
// Paint over the light 1px seam Windows draws under the menu bar (call after default NC painting).
void PaintMenuBarBottomLine(HWND top);

int S(HDC dc, int v);   // scale a 96-DPI-logical px to the DC's DPI

// WM_CTLCOLOR* helpers: set the DC's text/bk colors and return the matching background brush.
HBRUSH windowCtl(HDC dc);    // dialog / static / button surfaces (WINDOW_BG + white text)
HBRUSH contentCtl(HDC dc);   // edit / listbox interiors (CONTENT_BG + white text)

// Walk every descendant of `parent` and dark-theme it by window class: BUTTON / COMBOBOX /
// msctls_updown32 / SysHeader32 / SysTabControl32 get owner-draw subclasses; SysListView32 /
// SysTreeView32 get their interior colors set. Idempotent — safe to call more than once.
void ApplyToChildren(HWND parent);

// Dark-theme a native menu the MPC way: a dark background brush (MIM_BACKGROUND) + every item made
// MFT_OWNERDRAW (so Windows keeps all native hover/click/keyboard behaviour, we just draw dark).
// Recurses into submenus. The owning window must forward WM_MEASUREITEM / WM_DRAWITEM to the two
// handlers below. Idempotent per item.
void ThemeMenu(HMENU menu, bool isMenubar);
bool MenuMeasureItem(MEASUREITEMSTRUCT* mis);   // true if it handled an ODT_MENU item
bool MenuDrawItem(DRAWITEMSTRUCT* dis);         // true if it handled an ODT_MENU item
}
