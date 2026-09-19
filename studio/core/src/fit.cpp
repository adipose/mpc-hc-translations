// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/fit.h"

#include <algorithm>

// Portable parts (combo_groups' static table, classify's arithmetic) build on any platform -- pure
// data/arithmetic, no Win32. Everything that touches a live HWND is Windows-only and stubs out on the
// #else half below, same split as mpctrans::github/ai_client.

namespace mpctrans::fit {

// ---- combo_groups(): MOVED VERBATIM from studio/ui/MainFrame.cpp (see there for the pre-move
// history) -- combo dropdowns are filled at runtime in C++ (PPage*::DoDataExchange ->
// AddString(ResStr(IDS_...))), so the RC dialog template can't tell us that e.g. IDS_TIME_ON_SEEKBAR_*
// live in IDC_TIMEONSEEKBAR on IDD_PPAGETHEME. This table -- generated from upstream's PPage*.cpp --
// maps each option-list combo to its (dialog, control, ordered items). Keep in sync with upstream if a
// page's combo changes.
//
// `dropdownWidened`: whether upstream widens the combo's dropped list to its longest item
// (CorrectComboListWidth / SetDroppedWidth in the page's OnInitDialog, or PlayerListCtrl for the
// Advanced page's in-list combo). Upstream's convention -- cf. PPageTheme.cpp "the longest option is
// wider than the combo field" -- is that an option clipping in the CLOSED field is acceptable as long
// as the dropdown shows it in full. So the fit scan (measure_combo_fit's callers) only flags UNWIDENED
// combos, where a long option is unreadable everywhere. Audited against clsid2/mpc-hc develop
// 2026-09-02; the three `false` entries below are the ones missing the call (see upstream #4141's
// follow-up) -- flip them to true once that lands.
const std::vector<ComboGroup>& combo_groups() {
    static const std::vector<ComboGroup> t = {
        {"IDD_PPAGECAPTURE",  "IDC_COMBO6", true,  {"IDS_PPAGE_CAPTURE_FG0","IDS_PPAGE_CAPTURE_FG1","IDS_PPAGE_CAPTURE_FG2"}},
        {"IDD_PPAGECAPTURE",  "IDC_COMBO7", true,  {"IDS_PPAGE_CAPTURE_SFG0","IDS_PPAGE_CAPTURE_SFG1","IDS_PPAGE_CAPTURE_SFG2"}},
        {"IDD_PPAGEOUTPUT",   "IDC_AUDRND_COMBO", true, {"IDS_PPAGE_OUTPUT_SYS_DEF","IDS_PPAGE_OUTPUT_AUD_INTERNAL_REND",
            "IDS_PPAGE_OUTPUT_AUD_MPC_REND","IDS_PPAGE_OUTPUT_AUD_NULL_COMP","IDS_PPAGE_OUTPUT_AUD_NULL_UNCOMP"}},
        {"IDD_PPAGEOUTPUT",   "IDC_DX9RESIZER_COMBO", false, {"IDS_PPAGE_OUTPUT_RESIZE_NN","IDS_PPAGE_OUTPUT_RESIZER_BILIN",
            "IDS_PPAGE_OUTPUT_RESIZER_BIL_PS","IDS_PPAGE_OUTPUT_RESIZER_BICUB1","IDS_PPAGE_OUTPUT_RESIZER_BICUB2","IDS_PPAGE_OUTPUT_RESIZER_BICUB3"}},
        {"IDD_PPAGEOUTPUT",   "IDC_DX_SURFACE", true, {"IDS_PPAGE_OUTPUT_SURF_OFFSCREEN","IDS_PPAGE_OUTPUT_SURF_2D","IDS_PPAGE_OUTPUT_SURF_3D"}},
        // The post-July-2026 renderer-settings dialog hosts the same combos under a new IDD (upstream
        // moved them off IDD_PPAGEOUTPUT); same control symbols, so both generations of the RC resolve.
        // The combo-option clipping reported in upstream #4141 lives here (surface: widened -> face-only).
        {"IDD_PPAGEVIDEORENDERER", "IDC_DX9RESIZER_COMBO", false, {"IDS_PPAGE_OUTPUT_RESIZE_NN","IDS_PPAGE_OUTPUT_RESIZER_BILIN",
            "IDS_PPAGE_OUTPUT_RESIZER_BIL_PS","IDS_PPAGE_OUTPUT_RESIZER_BICUB1","IDS_PPAGE_OUTPUT_RESIZER_BICUB2","IDS_PPAGE_OUTPUT_RESIZER_BICUB3"}},
        {"IDD_PPAGEVIDEORENDERER", "IDC_DX_SURFACE", true, {"IDS_PPAGE_OUTPUT_SURF_OFFSCREEN","IDS_PPAGE_OUTPUT_SURF_2D","IDS_PPAGE_OUTPUT_SURF_3D"}},
        {"IDD_PPAGEPLAYBACK", "IDC_COMBO1", true,  {"IDS_ZOOM_25","IDS_ZOOM_50","IDS_ZOOM_100","IDS_ZOOM_200","IDS_ZOOM_AUTOFIT"}},
        {"IDD_PPAGEPLAYBACK", "IDC_COMBO2", true,  {"IDS_AFTER_PLAYBACK_DO_NOTHING","IDS_AFTER_PLAYBACK_PLAY_NEXT","IDS_AFTER_PLAYBACK_REWIND",
            "IDS_AFTER_PLAYBACK_MONITOROFF","IDS_AFTER_PLAYBACK_CLOSE","IDS_AFTER_PLAYBACK_EXIT"}},
        {"IDD_PPAGEPLAYBACK", "IDC_COMBO3", true,  {"IDS_PLAY_LOOPMODE_FILE","IDS_PLAY_LOOPMODE_PLAYLIST"}},
        {"IDD_PPAGEPLAYBACK", "IDC_COMBO4", true,  {"IDS_VERTICAL_ALIGN_VIDEO_MIDDLE","IDS_VERTICAL_ALIGN_VIDEO_TOP","IDS_VERTICAL_ALIGN_VIDEO_BOTTOM"}},
        {"IDD_PPAGETHEME",    "IDC_COMBO1", false, {"IDS_THEMEMODE_DARK","IDS_THEMEMODE_LIGHT","IDS_THEMEMODE_WINDOWS"}},
        {"IDD_PPAGETHEME",    "IDC_COMBO3", false, {"IDS_TIME_TOOLTIP_ABOVE","IDS_TIME_TOOLTIP_BELOW"}},
        {"IDD_PPAGETHEME",    "IDC_TIMEONSEEKBAR", true, {"IDS_TIME_ON_SEEKBAR_NEVER","IDS_TIME_ON_SEEKBAR_ALWAYS","IDS_TIME_ON_SEEKBAR_WHEN_STATUSBAR_HIDDEN"}},
        {"IDD_PPAGETWEAKS",   "IDC_COMBO4", false, {"IDS_FASTSEEK_LATEST","IDS_FASTSEEK_NEAREST"}},
        {"IDD_PPAGEADVANCED", "IDC_COMBO1", true,  {"IDS_STARTUP_PRESET_REMEMBER","IDS_AG_VIEW_MINIMAL","IDS_AG_VIEW_COMPACT","IDS_AG_VIEW_NORMAL","IDS_AG_VIEW_CUSTOM"}},
        {"IDD_PPAGEFULLSCREEN","IDC_COMBO2", true, {"IDS_PPAGEFULLSCREEN_SHOWNEVER","IDS_PPAGEFULLSCREEN_SHOWMOVED","IDS_PPAGEFULLSCREEN_SHOHHOVERED"}},
    };
    return t;
}

// Hard/tight thresholds shared by the Studio's Review-queue flags and the fitscan CLI's JSON. Formerly
// inlined in MainFrame::AppendFitFlags/AppendComboFitFlags; moved here verbatim so the two call sites
// (and the headless tool) can never disagree on what counts as "doesn't fit".
Kind classify(int rendered, int avail) {
    if (avail <= 0) return Kind::None;          // unmeasurable -- not flagged
    if (rendered > avail) return Kind::Hard;
    double ratio = (double)rendered / avail;
    if (ratio >= 0.90) return Kind::Tight;
    return Kind::None;                          // fits comfortably
}

} // namespace mpctrans::fit

#ifdef _WIN32

#include <windowsx.h>   // (kept for parity with LivePreview.cpp's includes; not strictly required)

namespace mpctrans::fit {

namespace {

// ---- verbatim port of LivePreview.cpp's displayText()/prefixProcesses() (studio/ui/LivePreview.cpp,
// formerly ~lines 885-901) -- see there for the mnemonic-prefix rationale. ----
std::wstring displayText(const wchar_t* s, int n, bool prefixProcessed) {
    std::wstring o; o.reserve(n);
    for (int r = 0; r < n; ++r) {
        if (prefixProcessed && s[r] == L'&') {
            if (r + 1 < n && s[r + 1] == L'&') { o.push_back(L'&'); ++r; }   // '&&' -> literal '&'
            // else: a lone '&' is the mnemonic marker -- dropped
        } else o.push_back(s[r]);
    }
    return o;
}
bool prefixProcesses(const wchar_t* cls, LONG style) {
    if (!_wcsicmp(cls, L"Button")) return true;
    if (!_wcsicmp(cls, L"Static")) return !(style & SS_NOPREFIX);
    return false;
}

// Register the one custom window class upstream templates reference (MfcMaskedEdit) so
// CreateDialogIndirect doesn't fail outright on it -- same stub LivePreview::LoadNeutralDll and
// rc_render_test.cpp register. Renders as a plain themed box; irrelevant to fit (never a translatable
// Button/Static candidate).
void ensure_stub_class_registered() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSW wc{};
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MfcMaskedEdit";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32513));   // IDC_IBEAM -- explicit W
                                                                          // ordinal: the CMake build
                                                                          // doesn't define UNICODE, so
                                                                          // the IDC_IBEAM macro would
                                                                          // resolve to an ANSI LPSTR here.
    ::RegisterClassW(&wc);   // already-registered is fine
}

} // namespace

bool control_overflows(HWND ctrl) {
    if (!ctrl || !::IsWindow(ctrl)) return false;
    wchar_t cls[64]; ::GetClassNameW(ctrl, cls, 64);
    bool textual = !_wcsicmp(cls, L"Button") || !_wcsicmp(cls, L"Static");
    LONG st = (LONG)::GetWindowLongPtr(ctrl, GWL_STYLE);
    if (!_wcsicmp(cls, L"Static") && ((st & SS_TYPEMASK) > SS_RIGHT)) textual = false;   // icons etc.
    if (!textual) return false;
    wchar_t txt[512]; int n = ::GetWindowTextW(ctrl, txt, 512);
    if (n <= 0 || wcschr(txt, L'\n')) return false;   // multi-line statics wrap; skip
    std::wstring dt = displayText(txt, n, prefixProcesses(cls, st));   // drop the mnemonic '&'
    HDC dc = ::GetDC(ctrl);
    HFONT f = (HFONT)::SendMessage(ctrl, WM_GETFONT, 0, 0);
    HGDIOBJ old = f ? ::SelectObject(dc, f) : nullptr;
    SIZE sz{}; ::GetTextExtentPoint32W(dc, dt.c_str(), (int)dt.size(), &sz);
    if (old) ::SelectObject(dc, old);
    ::ReleaseDC(ctrl, dc);
    RECT rc; ::GetClientRect(ctrl, &rc);
    int avail = rc.right - rc.left;
    if (!_wcsicmp(cls, L"Button")) avail -= 8;   // borders/margins
    return sz.cx > avail && avail > 0;
}

TextFit measure_control_text(HWND ctrl, const std::wstring& text) {
    TextFit fit;
    if (!ctrl || !::IsWindow(ctrl)) return fit;
    wchar_t cls[64]; ::GetClassNameW(ctrl, cls, 64);
    LONG st = (LONG)::GetWindowLongPtr(ctrl, GWL_STYLE);
    std::wstring dt = displayText(text.c_str(), (int)text.size(), prefixProcesses(cls, st));
    HDC dc = ::GetDC(ctrl);
    HFONT f = (HFONT)::SendMessage(ctrl, WM_GETFONT, 0, 0);
    HGDIOBJ old = f ? ::SelectObject(dc, f) : nullptr;
    SIZE sz{}; ::GetTextExtentPoint32W(dc, dt.c_str(), (int)dt.size(), &sz);
    if (old) ::SelectObject(dc, old);
    ::ReleaseDC(ctrl, dc);
    RECT rc; ::GetClientRect(ctrl, &rc);
    int avail = rc.right - rc.left;
    if (!_wcsicmp(cls, L"Button")) avail -= 8;
    fit.renderedPx = (int)sz.cx;
    fit.availablePx = avail;
    fit.measured = true;
    return fit;
}

std::vector<ComboMeasurement> measure_combo_fit(HWND dlg, long long comboCtrlId,
        const std::vector<std::pair<std::string, std::wstring>>& options) {
    std::vector<ComboMeasurement> out;
    HWND ctrl = ::GetDlgItem(dlg, (int)comboCtrlId);
    if (!ctrl) return out;
    RECT rc; ::GetClientRect(ctrl, &rc);
    int avail = (rc.right - rc.left) - ::GetSystemMetrics(SM_CXVSCROLL) - 8;
    HDC dc = ::GetDC(ctrl);
    HFONT f = (HFONT)::SendMessage(ctrl, WM_GETFONT, 0, 0);
    HGDIOBJ old = f ? ::SelectObject(dc, f) : nullptr;
    for (const auto& [msgctxt, text] : options) {
        SIZE sz{}; ::GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
        out.push_back({ msgctxt, comboCtrlId, (int)sz.cx, avail });
    }
    if (old) ::SelectObject(dc, old);
    ::ReleaseDC(ctrl, dc);
    return out;
}

std::vector<Measurement> measure_fit(HWND dlg, long long dialogId, const ControlIndex& idx,
                                     const std::vector<std::pair<HWND, std::string>>& english,
                                     HFONT lineHeightFont) {
    std::vector<Measurement> out;
    if (!dlg || !::IsWindow(dlg)) return out;

    HDC hdc = ::GetDC(dlg);
    int dpi = ::GetDeviceCaps(hdc, LOGPIXELSY);
    HFONT lhf = lineHeightFont ? lineHeightFont : (HFONT)::SendMessage(dlg, WM_GETFONT, 0, 0);
    HGDIOBJ of = lhf ? ::SelectObject(hdc, lhf) : nullptr;
    TEXTMETRICW tm{}; ::GetTextMetricsW(hdc, &tm);
    if (of) ::SelectObject(hdc, of);
    ::ReleaseDC(dlg, hdc);
    int lineH = tm.tmHeight > 0 ? tm.tmHeight : 16;
    int rowTolerance = ::MulDiv(4, dpi, 96);
    int gapTolerance = ::MulDiv(20, dpi, 96);
    const double heightRatioMax = 1.6;

    struct Cand {
        HWND hwnd; RECT rc;                  // rc in dialog-client coords
        const DialogRecord* rec;
        int renderedPx, availablePx;
    };
    std::vector<Cand> cands;

    for (const auto& [hwnd, eng] : english) {
        if (!hwnd || !::IsWindow(hwnd) || !::IsChild(dlg, hwnd)) continue;   // stale-call guard
        wchar_t cls[64]; ::GetClassNameW(hwnd, cls, 64);
        bool isButton = !_wcsicmp(cls, L"Button");
        bool isStatic = !_wcsicmp(cls, L"Static");
        if (!isButton && !isStatic) continue;
        RECT crc; ::GetClientRect(hwnd, &crc);
        if (isStatic && (crc.bottom - crc.top) > (int)(lineH * heightRatioMax))
            continue;   // taller than ~1 line -> multi-line/wrapping label, excluded

        int ctrlId = ::GetDlgCtrlID(hwnd);
        const DialogRecord* rec = idx.dialog_lookup(dialogId, (long long)ctrlId, eng);
        if (!rec) continue;   // not a translatable control per the build_index.py selection

        wchar_t txt[512] = {}; int n = ::GetWindowTextW(hwnd, txt, 512);
        LONG st = (LONG)::GetWindowLongPtr(hwnd, GWL_STYLE);
        std::wstring dt = displayText(txt, n, prefixProcesses(cls, st));   // drop the mnemonic '&'
        HDC dc = ::GetDC(hwnd);
        HFONT f = (HFONT)::SendMessage(hwnd, WM_GETFONT, 0, 0);
        HGDIOBJ oldf = f ? ::SelectObject(dc, f) : nullptr;
        SIZE sz{}; ::GetTextExtentPoint32W(dc, dt.c_str(), (int)dt.size(), &sz);
        if (oldf) ::SelectObject(dc, oldf);
        ::ReleaseDC(hwnd, dc);

        int avail = crc.right - crc.left;
        if (isButton) avail -= 8;

        RECT wr; ::GetWindowRect(hwnd, &wr);
        ::MapWindowPoints(nullptr, dlg, (POINT*)&wr, 2);

        cands.push_back({ hwnd, wr, rec, (int)sz.cx, avail });
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.rc.top != b.rc.top) return a.rc.top < b.rc.top;
        return a.rc.left < b.rc.left;
    });

    // Bucket into rows by top-proximity to the row's first (topmost) member.
    std::vector<std::vector<size_t>> rows;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!rows.empty() && cands[i].rc.top - cands[rows.back().front()].rc.top <= rowTolerance)
            rows.back().push_back(i);
        else
            rows.push_back({ i });
    }

    auto emitSingle = [&](const Cand& c) {
        Measurement fm;
        fm.controlId = ::GetDlgCtrlID(c.hwnd);
        fm.controlSym = c.rec->control_sym;
        fm.msgctxt = c.rec->msgctxt;
        fm.msgid = c.rec->msgid;
        fm.renderedPx = c.renderedPx;
        fm.availablePx = c.availablePx;
        fm.grouped = false;
        out.push_back(std::move(fm));
    };
    auto emitGroup = [&](const std::vector<size_t>& row, size_t begin, size_t end) {
        int groupRendered = 0, groupAvail = 0;
        std::vector<std::string> ctxs;
        for (size_t m = begin; m < end; ++m) { groupRendered += cands[row[m]].renderedPx; groupAvail += cands[row[m]].availablePx; }
        for (size_t m = begin; m < end; ++m) ctxs.push_back(cands[row[m]].rec->msgctxt);
        for (size_t m = begin; m < end; ++m) {
            const Cand& c = cands[row[m]];
            Measurement fm;
            fm.controlId = ::GetDlgCtrlID(c.hwnd);
            fm.controlSym = c.rec->control_sym;
            fm.msgctxt = c.rec->msgctxt;
            fm.msgid = c.rec->msgid;
            fm.renderedPx = c.renderedPx;
            fm.availablePx = c.availablePx;
            fm.grouped = true;
            fm.groupRenderedPx = groupRendered;
            fm.groupAvailablePx = groupAvail;
            for (const auto& peerCtx : ctxs) if (peerCtx != fm.msgctxt) fm.groupPeers.push_back(peerCtx);
            out.push_back(std::move(fm));
        }
    };

    for (auto& row : rows) {
        std::sort(row.begin(), row.end(), [&](size_t a, size_t b) { return cands[a].rc.left < cands[b].rc.left; });
        size_t chainStart = 0;
        for (size_t k = 1; k <= row.size(); ++k) {
            bool breakChain = (k == row.size()) ||
                (cands[row[k]].rc.left - cands[row[k - 1]].rc.right > gapTolerance);
            if (!breakChain) continue;
            size_t chainLen = k - chainStart;
            if (chainLen >= 2) emitGroup(row, chainStart, k);
            else               emitSingle(cands[row[chainStart]]);
            chainStart = k;
        }
    }
    return out;
}

HWND render_dialog(const RcDialog& d, HWND host, const ControlIndex& idx, const PoFile& dialogsPo,
                   std::vector<std::pair<HWND, std::string>>& englishOut) {
    englishOut.clear();
    ensure_stub_class_registered();

    std::vector<unsigned char> buf = emit_dlgtemplate(d);
    if (buf.size() < 16) return nullptr;
    DWORD exStyle, style;   // DLGTEMPLATEEX: dlgVer(2) sig(2) helpID(4) exStyle(4) style(4)
    memcpy(&exStyle, buf.data() + 8, 4);
    memcpy(&style, buf.data() + 12, 4);
    // Embed as a child of `host` (never shown) -- same patch rc_render_test.cpp applies. Fit only
    // needs real client rects + fonts, not chrome/theming, so widget-pair/dark-theme cosmetics are
    // intentionally NOT reproduced here (they don't change control widths/text).
    style &= ~(DWORD)(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | DS_ABSALIGN | DS_CENTER | DS_CENTERMOUSE);
    style |= WS_CHILD;
    exStyle &= ~(DWORD)(WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_STATICEDGE);
    memcpy(buf.data() + 8, &exStyle, 4);
    memcpy(buf.data() + 12, &style, 4);

    HWND dlg = ::CreateDialogIndirectParamW(::GetModuleHandleW(nullptr), (LPCDLGTEMPLATE)buf.data(),
                                            host, nullptr, 0);
    if (!dlg) return nullptr;

    // Capture the ORIGINAL English text per child (needed for idx.dialog_lookup's IDC_STATIC==-1
    // disambiguation -- see fit.h's measure_fit comment), then substitute captions from the .po via
    // the control-index -- mirrors LivePreview::RenderDialog's step 3 exactly.
    struct Ctx {
        const ControlIndex* idx; const PoFile* po; long long dlg;
        std::vector<std::pair<HWND, std::string>>* out;
    } ctx{ &idx, &dialogsPo, d.id, &englishOut };
    ::EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t buf[512]; ::GetWindowTextW(child, buf, 512);
        int n = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string english(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, english.data(), n, nullptr, nullptr);
        c->out->emplace_back(child, english);
        if (const DialogRecord* rec = c->idx->dialog_lookup(c->dlg, ::GetDlgCtrlID(child), english)) {
            if (const PoEntry* e = c->po->find(rec->msgctxt, rec->msgid); e && !e->msgstr.empty()) {
                int wn = ::MultiByteToWideChar(CP_UTF8, 0, e->msgstr.c_str(), -1, nullptr, 0);
                std::wstring w(wn > 0 ? wn - 1 : 0, L'\0');
                if (wn > 0) ::MultiByteToWideChar(CP_UTF8, 0, e->msgstr.c_str(), -1, w.data(), wn);
                ::SetWindowTextW(child, w.c_str());
            }
        }
        return TRUE;
    }, (LPARAM)&ctx);

    return dlg;
}

} // namespace mpctrans::fit

#else // !_WIN32 -- keep non-Windows builds (validators/gates) linking, same split as github.cpp

namespace mpctrans::fit {

bool control_overflows(HWND) { return false; }
TextFit measure_control_text(HWND, const std::wstring&) { return {}; }
std::vector<ComboMeasurement> measure_combo_fit(HWND, long long,
    const std::vector<std::pair<std::string, std::wstring>>&) { return {}; }
std::vector<Measurement> measure_fit(HWND, long long, const ControlIndex&,
                                     const std::vector<std::pair<HWND, std::string>>&, HFONT) { return {}; }
HWND render_dialog(const RcDialog&, HWND, const ControlIndex&, const PoFile&,
                   std::vector<std::pair<HWND, std::string>>& englishOut) {
    englishOut.clear();
    return nullptr;
}

} // namespace mpctrans::fit

#endif
