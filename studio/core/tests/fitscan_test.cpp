// SPDX-License-Identifier: GPL-3.0-or-later
// THE FITSCAN GATE: in-process exercise of the SAME core APIs studio/cli/fitscan.cpp (the headless
// fit-scan tool potool/fitscan.ps1 forwards to) is built from -- RcParser + rc_dialog_records +
// ControlIndex::from_records to reconstruct the translatable-control index, then
// mpctrans::fit::render_dialog/measure_fit/measure_combo_fit/classify (studio/core/include/mpctrans/
// fit.h) to render every dialog off-screen and flag captions that don't fit. Scans only "pl" and "de"
// (the pinned RC's ~55 dialogs x all languages would make this gate slow) against the checked-out
// upstream PO. SKIPs cleanly (exit 0) when the gitignored RC/PO submodule content is missing, same
// convention as rc_render_test.cpp / rc_index_test.cpp.
//   Usage: fitscan_test [RC] [RESOURCE_H] [PO_DIR]
#include "mpctrans/control_index.h"
#include "mpctrans/fit.h"
#include "mpctrans/po.h"
#include "mpctrans/rc_dialogs.h"

#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' "                       \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "                    \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace mpctrans;
namespace fs = std::filesystem;

namespace {

std::string read_file(const std::string& p, bool binary) {
    std::ifstream f(p, binary ? std::ios::binary : std::ios::in);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

struct Rec { std::string lang, res, msgctxt; int renderedPx, availablePx; };

} // namespace

int main(int argc, char** argv) {
    std::string rc = argc > 1 ? argv[1] : "upstream/src/mpc-hc/mpc-hc.rc";
    std::string rh = argc > 2 ? argv[2] : "upstream/src/mpc-hc/resource.h";
    std::string poDir = argc > 3 ? argv[3] : "upstream/src/mpc-hc/mpcresources/PO";
    if (!fs::exists(rc)) { std::printf("SKIP: %s not found\n", rc.c_str()); return 0; }
    if (!fs::exists(poDir)) { std::printf("SKIP: %s not found\n", poDir.c_str()); return 0; }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_LINK_CLASS |
        ICC_STANDARD_CLASSES | ICC_DATE_CLASSES | ICC_USEREX_CLASSES |
        ICC_COOL_CLASSES | ICC_INTERNET_CLASSES };
    ::InitCommonControlsEx(&icc);

    std::vector<RcDialog> dialogs = RcParser::parse(read_file(rc, true), read_file(rh, false));
    if (dialogs.empty()) { std::printf("SKIP: no dialogs parsed from %s\n", rc.c_str()); return 0; }
    ControlIndex idx = ControlIndex::from_records(rc_dialog_records(dialogs), {});

    WNDCLASSW hc{}; hc.lpfnWndProc = ::DefWindowProcW; hc.hInstance = ::GetModuleHandleW(nullptr);
    hc.lpszClassName = L"FitscanTestHost";
    ::RegisterClassW(&hc);

    std::vector<Rec> records;
    for (const char* lang : { "pl", "de" }) {
        fs::path dPath = fs::path(poDir) / (std::string("mpc-hc.") + lang + ".dialogs.po");
        fs::path sPath = fs::path(poDir) / (std::string("mpc-hc.") + lang + ".strings.po");
        if (!fs::exists(dPath)) { std::printf("SKIP: %s not found\n", dPath.string().c_str()); continue; }
        PoFile dialogsPo = PoFile::parse_bytes(read_file(dPath.string(), true));
        PoFile stringsPo = fs::exists(sPath) ? PoFile::parse_bytes(read_file(sPath.string(), true)) : PoFile{};

        HWND host = ::CreateWindowExW(0, L"FitscanTestHost", L"", WS_POPUP, 0, 0, 10, 10,
                                      nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!host) { std::printf("SKIP: CreateWindowExW(host) failed (err %lu)\n", ::GetLastError()); return 0; }

        for (const auto& d : dialogs) {
            std::vector<std::pair<HWND, std::string>> english;
            HWND dlg = fit::render_dialog(d, host, idx, dialogsPo, english);
            if (!dlg) continue;

            for (const auto& m : fit::measure_fit(dlg, d.id, idx, english)) {
                int r = m.grouped ? m.groupRenderedPx : m.renderedPx;
                int a = m.grouped ? m.groupAvailablePx : m.availablePx;
                if (fit::classify(r, a) != fit::Kind::None)
                    records.push_back({ lang, "dialogs", m.msgctxt, r, a });
            }
            for (const auto& g : fit::combo_groups()) {
                if (g.dropdownWidened || d.sym != g.idd) continue;
                long long comboId = -1;
                for (const auto& c : d.controls) if (c.sym == g.combo) { comboId = c.id; break; }
                if (comboId < 0) continue;
                std::vector<std::pair<std::string, std::wstring>> options;
                for (const char* item : g.items) {
                    const PoEntry* found = nullptr;
                    for (const auto& e : stringsPo.entries) if (e.msgctxt == item) { found = &e; break; }
                    if (!found) continue;
                    std::string text = !found->msgstr.empty() ? found->msgstr : found->msgid;
                    int wn = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
                    std::wstring w(wn > 0 ? wn - 1 : 0, L'\0');
                    if (wn > 0) ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), wn);
                    options.emplace_back(found->msgctxt, w);
                }
                if (options.empty()) continue;
                for (const auto& m : fit::measure_combo_fit(dlg, comboId, options))
                    if (fit::classify(m.renderedPx, m.availablePx) != fit::Kind::None)
                        records.push_back({ lang, "strings", m.msgctxt, m.renderedPx, m.availablePx });
            }
            ::DestroyWindow(dlg);
        }
        ::DestroyWindow(host);
    }

    std::printf("%zu fit record(s) across pl+de\n", records.size());
    int fails = 0;

    if (records.empty()) { std::printf("FAIL: expected >= 1 record\n"); ++fails; }

    for (const auto& r : records) {
        if (r.availablePx <= 0) {
            std::printf("FAIL: %s %s %s availablePx=%d (expected > 0)\n",
                        r.lang.c_str(), r.res.c_str(), r.msgctxt.c_str(), r.availablePx);
            ++fails;
        }
        if (!(r.renderedPx > r.availablePx * 0.9)) {
            std::printf("FAIL: %s %s %s renderedPx=%d not > availablePx*0.9=%.1f\n",
                        r.lang.c_str(), r.res.c_str(), r.msgctxt.c_str(), r.renderedPx, r.availablePx * 0.9);
            ++fails;
        }
    }

    bool plTooltipCombo = false;
    for (const auto& r : records)
        if (r.lang == "pl" && r.res == "strings" &&
            (r.msgctxt == "IDS_TIME_TOOLTIP_ABOVE" || r.msgctxt == "IDS_TIME_TOOLTIP_BELOW"))
            plTooltipCombo = true;
    if (!plTooltipCombo) {
        std::printf("FAIL: expected a pl strings-res combo record for IDS_TIME_TOOLTIP_ABOVE/BELOW"
                    " (known unwidened-combo overflow on the theme page)\n");
        ++fails;
    }

    std::printf("%s\n", fails ? "FAIL" : "FITSCAN GATE GREEN");
    return fails ? 1 : 0;
}
