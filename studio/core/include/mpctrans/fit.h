// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "mpctrans/control_index.h"
#include "mpctrans/po.h"
#include "mpctrans/rc_dialogs.h"

#ifdef _WIN32
#include <windows.h>
#else
// Opaque stand-ins so this header's signatures compile on non-Windows builds (mpctrans is a portable
// core lib -- see mpctrans/github.h's Windows/non-Windows split). Every function below that takes one
// of these is Windows-only and stubs out in fit.cpp's #else half; non-Windows callers never construct
// a real value for them.
using HWND = void*;
using HFONT = void*;
#endif

// Single source of truth for the "does a translated caption fit its control" rule -- the SAME check
// LivePreview shows live in the Studio's Review tab (EnsureFitScan/MeasureFit/MeasureComboFit) and the
// headless `fitscan` CLI (studio/cli/fitscan.cpp, potool/fitscan.ps1) run outside the app. This module
// was carved out of studio/ui/LivePreview.cpp (MeasureFit/MeasureComboFit/DetectOverflow/
// MeasureControlText) and studio/ui/MainFrame.cpp (combo_groups, the hard/tight thresholds in
// AppendFitFlags/AppendComboFitFlags) so the app and the tool can never drift -- LivePreview's methods
// are now thin wrappers over these free functions; see LivePreview.cpp/.h for the wrapper signatures.
namespace mpctrans::fit {

// One control's fit measurement -- == the old LivePreview::FitMeasurement (kept there as a type alias).
// `renderedPx` is the actual text extent in the control's font; `availablePx` is its client-rect width
// (Button: minus 8px border/margin, matching control_overflows). When `grouped` is true this control
// was chained (by geometric row-adjacency) with `groupPeers` into one span -- `groupRenderedPx`/
// `groupAvailablePx` are the GROUP's summed totals (what actually gates hard/tight; the individual
// fields are still useful for evidence text).
struct Measurement {
    long long controlId = -1;
    std::string controlSym;                 // "IDC_..."
    std::string msgctxt, msgid;              // po key (from the ControlIndex)
    int renderedPx = 0, availablePx = 0;
    bool grouped = false;
    int groupRenderedPx = 0, groupAvailablePx = 0;
    std::vector<std::string> groupPeers;     // other controls' msgctxt in this control's group
};

// One combo OPTION's fit against its combo's CLOSED field -- == the old LivePreview::ComboFitMeasurement.
struct ComboMeasurement { std::string msgctxt; long long controlId = -1; int renderedPx = 0, availablePx = 0; };

// Re-measure arbitrary replacement `text` against a SPECIFIC control's CURRENT font + client rect --
// == the old LivePreview::TextFit (used to re-check an AI-proposed fix before offering it).
struct TextFit { int renderedPx = 0, availablePx = 0; bool measured = false; };

// One entry of the combo-dropdown-item table -- MOVED VERBATIM from MainFrame.cpp's combo_groups()
// (see fit.cpp for the full comment on `dropdownWidened`'s upstream-convention rationale). `idd`/
// `combo`/`items` are string literals owned by the static table, safe to keep as raw pointers.
struct ComboGroup { const char* idd; const char* combo; bool dropdownWidened; std::vector<const char*> items; };
const std::vector<ComboGroup>& combo_groups();

// Thresholds shared with the Studio's Review-queue flags (formerly MainFrame::AppendFitFlags'/
// AppendComboFitFlags' inline logic): hard = rendered > avail; tight = ratio >= 0.90; else None (fits
// comfortably, or avail <= 0 and unmeasurable).
enum class Kind { None, Hard, Tight };
Kind classify(int rendered, int avail);

// Single-line text controls whose CURRENT text is wider than their client rect -- the rule
// LivePreview::DetectOverflow applies per control (mnemonic-aware; Button gets an 8px allowance).
// Only Buttons and single-line Statics are "textual"; multi-line statics (embedded '\n') are excluded.
bool control_overflows(HWND ctrl);

// Re-measure arbitrary replacement `text` against `ctrl`'s CURRENT font + client rect. Mirrors
// control_overflows' Button-padding rule; `ctrl` must still exist.
TextFit measure_control_text(HWND ctrl, const std::wstring& text);

// Verbatim port of LivePreview::MeasureFit: single-line, width-constrained fit measurement for every
// translatable control in the CURRENTLY RENDERED dialog `dlg` (Buttons + single-line Statics only --
// a Static is "single-line" when its live client-rect HEIGHT is no more than ~1.6x the reference
// font's line height, taller being a multi-line/wrapping label). Group-aware: candidates are bucketed
// into rows by close vertical alignment (tops within ~4px at 96 DPI, DPI-scaled) then chained
// left-to-right where the gap between one control's right edge and the next's left edge is small
// (~20px at 96 DPI, DPI-scaled) -- each chain of 2+ becomes a group; singletons have grouped=false.
// `english` is the ORIGINAL English caption captured per child HWND before translation substitution
// (needed to disambiguate the IDC_STATIC==-1 duplicates via idx.dialog_lookup -- LivePreview keeps
// this as m_english; the fitscan CLI builds the same list in fit::render_dialog). `lineHeightFont`
// selects the font used only to compute the multi-line-static discriminator's line height; null means
// "query dlg's own WM_GETFONT" (what CreateDialogIndirect's DS_SETFONT already applied to every
// child) -- LivePreview passes its cached message font (m_dlgFont) instead, to keep its longstanding
// on-screen behavior byte-for-byte unchanged by this refactor.
std::vector<Measurement> measure_fit(HWND dlg, long long dialogId, const ControlIndex& idx,
                                     const std::vector<std::pair<HWND, std::string>>& english,
                                     HFONT lineHeightFont = nullptr);

// Verbatim port of LivePreview::MeasureComboFit: one combo OPTION's fit against its combo's CLOSED
// field (the part that clips -- the dropdown list can be wider, but the selected item renders in the
// field). `options` is (msgctxt, translated text). availablePx = combo client width minus the drop
// button (SM_CXVSCROLL) and ~8px edges/indent (mirrors control_overflows' Button margin rule). No
// mnemonic stripping -- combo items have none.
std::vector<ComboMeasurement> measure_combo_fit(HWND dlg, long long comboCtrlId,
    const std::vector<std::pair<std::string, std::wstring>>& options);

// Headless rendering for the fitscan CLI (no MFC, no neutral DLL, no widget-pair/dark-theme cosmetics
// -- those don't affect fit): emits `d`'s DLGTEMPLATEEX (mpctrans::emit_dlgtemplate), patches its style
// WS_POPUP->WS_CHILD (like rc_render_test.cpp), CreateDialogIndirectParamW's it onto `host`
// (caller-owned, a never-shown WS_POPUP), then substitutes every translatable caption from `dialogsPo`
// via idx.dialog_lookup(dialogId, ctrlId, englishCaption) -> po.find(msgctxt, msgid) ->
// SetWindowTextW(msgstr) (empty/missing msgstr leaves the English caption). Registers the
// "MfcMaskedEdit" stub window class (LivePreview's one custom class upstream templates use) the first
// time it's called in the process. `englishOut` receives the (HWND, original English) pairs captured
// BEFORE substitution -- pass straight through to measure_fit's `english` parameter. Returns the
// dialog HWND (caller DestroyWindow) or null on failure.
HWND render_dialog(const RcDialog& d, HWND host, const ControlIndex& idx, const PoFile& dialogsPo,
                   std::vector<std::pair<HWND, std::string>>& englishOut);

} // namespace mpctrans::fit
