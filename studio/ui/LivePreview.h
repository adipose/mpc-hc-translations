// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <afxwin.h>
#include <functional>
#include <vector>
#include "mpctrans/control_index.h"
#include "mpctrans/po.h"
#include "mpctrans/rc_dialogs.h"

// Live-preview: instantiate a real IDD_ dialog template from the bundled NEUTRAL resource DLL and
// substitute each control's caption from the in-memory .po via the control-index — instant
// in-language preview. See plan-translation-studio "Live-preview substitution — DE-RISKED".
class LivePreview {
public:
    bool  LoadNeutralDll(const CString& dllPath);   // LoadLibraryEx(..., LOAD_LIBRARY_AS_IMAGE_RESOURCE)
    void  Unload();
    bool  Loaded() const { return m_neutral != nullptr; }

    // Approach C: render dialogs from a parsed .rc (emitted DLGTEMPLATEEX) instead of the pinned
    // neutral DLL — so upstream layout changes appear without a bundle rebuild. When set, RenderDialog
    // emits the template for the requested dialog from the parsed RC.
    bool  SetRcSource(const CString& rcPath, const CString& resourceHPath);   // true if parsed
    bool  SetRcSourceBytes(const std::string& rcBytes, const std::string& resourceHText);  // fetched bytes
    void  SetRcDialogs(std::vector<mpctrans::RcDialog> d) {   // already-parsed (e.g. background prefetch)
        m_rcDialogs = std::move(d); m_useRc = !m_rcDialogs.empty();
    }
    void  SetRcMenus(std::vector<mpctrans::RcMenu> m) { m_rcMenus = std::move(m); }
    const std::vector<mpctrans::RcMenu>& RcMenus() const { return m_rcMenus; }
    void  ClearRcSource() { m_useRc = false; m_rcDialogs.clear(); }
    bool  UsingRc() const { return m_useRc; }
    const std::vector<mpctrans::RcDialog>& RcDialogs() const { return m_rcDialogs; }

    // Create dialog `dialogNum` as a child of `parent` (template style patched WS_POPUP->WS_CHILD
    // so it embeds), then substitute strings from `po` (matched via `idx`). Returns the HWND.
    // `propSheetLayout`: when true, the template is laid out on COMCTL32's 8pt "MS Shell Dlg"
    // property-sheet grid (see RenderDialog).
    HWND  RenderDialog(long long dialogNum, CWnd* parent,
                       const mpctrans::ControlIndex& idx, const mpctrans::PoFile& po,
                       bool propSheetLayout = false);
    void  DestroyPreview();                          // tear down the current preview dialog
    HWND  CurrentDlg() const { return m_dlg; }

    // Draw a 1px red locator frame around the control whose ORIGINAL English is `english`, so the
    // editor can see where the selected string sits in the dialog. Empty/no-match clears it.
    void  HighlightControl(const std::string& english);

    // General-purpose form of the red locator ring: `rc` in SCREEN coordinates, nullptr hides it.
    // Unlike HighlightControl (which rings a preview-dialog child), this works whether or not a
    // preview dialog currently exists — e.g. the command-help usage list, which is shown after the
    // preview dialog has been torn down. The ring popup is owned by the app's main window, not the
    // (possibly absent) preview dialog.
    void  RingScreenRect(const RECT* rc);

    // Show a themed tooltip bubble hovering below the control whose ORIGINAL English is `english`
    // (for rendering a tooltip string in place over the control it annotates). Empty text hides it.
    void  ShowTooltip(const std::string& english, const CString& text);
    void  HideTooltip();

    // Populate a real report-view list control (e.g. IDD_PPAGEADVANCED's IDC_LIST1) with two columns
    // and `rows`, selecting `selRow` -- so a runtime-built list renders through the actual themed control
    // (exact dimensions/row height), not a hand-drawn mock. `col1`/`col2` are the column titles.
    void  PopulateReportList(long long listId, const CString& col1, const CString& col2,
                             const std::vector<std::pair<CString, CString>>& rows, int selRow);
    void  ShowListItemTooltip(long long listId, int item, const CString& text);   // bubble below a list row
    long long PositionOverListValue(long long listId, long long ctrlId, int item); // place an editor over the value cell

    // Render an expanded dropdown below the combo control `comboCtrlId` (a resolved control id in the
    // current preview dialog), listing `items` (already translated) with `selIndex` highlighted -- so
    // a combo-item string (e.g. IDS_TIME_ON_SEEKBAR_*) shows in context, the way the open combo looks.
    // Also rings the combo with the red locator frame. Empty items hides it.
    void  ShowComboDropdown(long long comboCtrlId, const std::vector<CString>& items, int selIndex);
    void  HideComboDropdown();
    void  PaintComboList(HDC dc, HWND h);   // owner-draw for the dropdown popup (called from its WndProc)

    // Click-to-edit: fired (from the preview's dlgproc) with the clicked child control.
    std::function<void(HWND ctrl)> OnControlClicked;

    // Click-to-edit: given a clicked child control, recover its .po entry.
    // NOTE: matches by the control's ORIGINAL English text, so it must be called with the text
    // captured at render time (see m_english) — after substitution the live text is translated.
    const mpctrans::DialogRecord* HitTest(HWND dlg, HWND clicked,
                                          long long dialogNum, const mpctrans::ControlIndex& idx);

    struct Overflow { HWND ctrl; };                 // text wider than the control rect
    std::vector<Overflow> DetectOverflow(HWND dlg); // for the edit-panel warning

    // One control's fit measurement for the Review Queue (see MeasureFit). `renderedPx` is the actual
    // text extent in the control's font; `availablePx` is its client-rect width (Button: minus 8px
    // border/margin, matching DetectOverflow). When `grouped` is true this control was chained (by
    // geometric row-adjacency) with `groupPeers` into one span — `groupRenderedPx`/`groupAvailablePx`
    // are the GROUP's summed totals (what actually gates hard/tight; the individual fields are still
    // useful for the evidence text).
    struct FitMeasurement {
        long long controlId = -1;
        std::string controlSym;                  // "IDC_..."
        std::string msgctxt, msgid;               // po key (from the ControlIndex)
        int renderedPx = 0, availablePx = 0;
        bool grouped = false;
        int groupRenderedPx = 0, groupAvailablePx = 0;
        std::vector<std::string> groupPeers;      // other controls' msgctxt in this control's group
    };
    // Single-line, width-constrained fit measurement for every translatable control in the CURRENTLY
    // RENDERED dialog `dlg` (i.e. call immediately after RenderDialog(dialogId, ...) — this reads the
    // live child HWNDs + m_english captured by that render, it does not re-render). Scope: Buttons
    // (always single-line) and single-line Statics only — a Static is "single-line" when its live
    // client-rect HEIGHT is no more than ~1.6x the dialog font's line height (taller -> it's a
    // multi-line/wrapping label, excluded, matching the spec's "~10 DU / one 9pt line" discriminator
    // without needing DU/RC geometry, so this works whether or not the RC source is loaded). Only
    // controls present in `idx.dialogs()` for `dialogId` are considered (that's exactly the
    // build_index.py-selected translatable set — anything else, e.g. decorative icons, is skipped for
    // free). Group-aware: controls in this candidate set are bucketed into rows by close vertical
    // alignment (control tops within ~4px at 96 DPI, DPI-scaled) then chained left-to-right where the
    // gap between one control's right edge and the next's left edge is small (~20px at 96 DPI,
    // DPI-scaled) — each chain of 2+ becomes a group; singletons have grouped=false.
    std::vector<FitMeasurement> MeasureFit(HWND dlg, long long dialogId, const mpctrans::ControlIndex& idx);

    // Re-measure arbitrary replacement `text` against a SPECIFIC control's CURRENT font + client rect
    // (the control must still exist in the currently rendered dialog) — used to re-check an AI-proposed
    // fix before offering it. Mirrors DetectOverflow's Button-padding rule.
    struct TextFit { int renderedPx = 0, availablePx = 0; bool measured = false; };
    TextFit MeasureControlText(HWND ctrl, const CString& text) const;

    // --- menus ---
    bool  HasMenu(long long menuId) const;
    // Load a menu resource (RT_MENU) from the neutral DLL. Untranslated (English); the caller
    // walks it, substitutes each item's text from the .po, and owns it (DestroyMenu).
    HMENU LoadRawMenu(long long menuId) const;

private:
    // Replicate MPC-HC's CMPCThemeUtil::AdjustDynamicWidgetPair for the label/control pairs it
    // repositions at runtime, so the preview shows the spacing a longer translation actually gets.
    void ApplyWidgetPairs(HWND dlg, long long dialogId);
    void IntegrateSpinners(HWND dlg);   // sit each up-down inside its buddy edit so they read as one field
    void ApplyDarkTheme(HWND dlg);   // MPC-HC "Modern" dark theming (bg/text + owner-drawn controls)
    static INT_PTR CALLBACK PreviewDlgProc(HWND, UINT, WPARAM, LPARAM);
    HMODULE m_neutral = nullptr;
    std::vector<mpctrans::RcDialog> m_rcDialogs;   // parsed RC source (Approach C)
    std::vector<mpctrans::RcMenu>   m_rcMenus;     // parsed RC menu hierarchy (live, for menu-in-context)
    bool    m_useRc = false;
    HWND    m_dlg = nullptr;
    HWND    m_highlight = nullptr;   // control ringed by the red locator frame (nullptr = none)
    HWND    m_frame = nullptr;       // the hollow red-ring overlay window (child of m_dlg)
    HWND    m_tip = nullptr;         // themed tooltip bubble popup hovering over a control
    HWND    m_combo = nullptr;       // expanded combo-dropdown popup below a combo control
    std::vector<CString> m_comboItems;  // items drawn in m_combo (translated)
    int     m_comboSel = -1;         // highlighted item index in m_combo
    void    ringFrame(HWND next);    // (re)position the red locator ring around `next`; nullptr hides it
    void    tipBelow(HWND ctl, const CString& text);   // tooltip bubble just below a control
    void    tipAt(int x, int y, const CString& text, bool above = false);  // bubble at (x,y); above flips it up
    HFONT   m_dlgFont = nullptr;     // system message font (Segoe UI) applied to the preview, MPC-style
    // English template text per child, captured BEFORE substitution (keys for HitTest).
    std::vector<std::pair<HWND, std::string>> m_english;
};
