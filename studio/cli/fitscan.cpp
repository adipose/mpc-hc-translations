// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless fit scan: renders every IDD_* dialog from a parsed mpc-hc.rc (no MFC, no neutral DLL,
// no running Studio.exe) for each language's .po, measures every translatable control against its
// mpctrans::fit rules (studio/core/include/mpctrans/fit.h -- the SAME code LivePreview::MeasureFit/
// MeasureComboFit use inside the app), and writes one JSON record per string that doesn't fit its
// control. This is the entry point potool/fitscan.ps1 forwards to, and what the mpc-hc-tests
// translations suite's Invoke-FitScan plugs into (see that repo's suites/translations/README.md).
//
// Progress/status goes to stderr; the JSON report is written to --out (default fitscan.json); a
// one-line total summary goes to stdout.
#include <windows.h>
#include <commctrl.h>

#include "mpctrans/control_index.h"
#include "mpctrans/fit.h"
#include "mpctrans/po.h"
#include "mpctrans/rc_dialogs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <json.hpp>

// Themed common controls (SysLink etc. need v6) -- same manifest pragma as rc_render_test.cpp /
// fit_overflow_scenario.cpp, so real MPC-HC dialogs create and theme the way the Studio's preview does.
#pragma comment(linker, "/manifestdependency:\"type='win32' "                       \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "                    \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using namespace mpctrans;
using nlohmann::json;

namespace {

void print_usage() {
    std::fprintf(stderr,
        "usage: fitscan --rc RC --resource-h RESOURCE_H --po-dir DIR [--out fitscan.json]\n"
        "               [--lang de,pl] [--fail-on-hard]\n"
        "\n"
        "Renders every IDD_* dialog parsed from RC for each language's .po (headless, off-screen) and\n"
        "measures every translatable control against the SAME fit rules LivePreview uses in the Studio\n"
        "(mpctrans::fit -- see studio/core/include/mpctrans/fit.h). Writes one JSON record per string\n"
        "wider than its control (\"hard\" overflow) or within 90%% of it (\"tight\").\n"
        "\n"
        "  --rc            path to mpc-hc.rc\n"
        "  --resource-h    path to resource.h (same checkout as --rc)\n"
        "  --po-dir        directory holding mpc-hc.<lang>.dialogs.po / .strings.po\n"
        "  --out           JSON report path (default fitscan.json)\n"
        "  --lang          comma-separated language codes to scan (default: every language found)\n"
        "  --fail-on-hard  exit 2 if any \"hard\" (actually clipped) record was found\n"
        "\n"
        "Exit codes: 0 scan ran (records may or may not exist); 1 usage/IO error;\n"
        "            2 with --fail-on-hard when at least one hard record exists.\n");
}

std::string read_file(const fs::path& p, bool binary) {
    std::ifstream f(p, binary ? std::ios::binary : std::ios::in);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// mirrors MainFrame::PopulateLanguages / txsync_cli's enumerate_languages: every
// mpc-hc.<lang>.dialogs.po file in --po-dir contributes <lang>, sorted + de-duplicated.
std::vector<std::string> enumerate_languages(const fs::path& poDir) {
    std::vector<std::string> langs;
    const std::string pre = "mpc-hc.", suf = ".dialogs.po";
    std::error_code ec;
    if (!fs::exists(poDir, ec) || ec) return langs;
    for (auto& p : fs::directory_iterator(poDir, ec)) {
        if (ec) break;
        std::string fn = p.path().filename().string();
        if (fn.size() > pre.size() + suf.size() &&
            fn.compare(0, pre.size(), pre) == 0 &&
            fn.compare(fn.size() - suf.size(), suf.size(), suf) == 0)
            langs.push_back(fn.substr(pre.size(), fn.size() - pre.size() - suf.size()));
    }
    std::sort(langs.begin(), langs.end());
    langs.erase(std::unique(langs.begin(), langs.end()), langs.end());
    return langs;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) if (!item.empty()) out.push_back(item);
    return out;
}

std::string iso8601_utc_now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{}; gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

const char* kind_name(fit::Kind k) { return k == fit::Kind::Hard ? "hard" : "tight"; }

// One JSON record -- see fitscan.ps1's header comment / the README this feeds (mpc-hc-tests'
// suites/translations) for the schema.
struct Record {
    std::string lang, res, dialog, control, msgctxt, msgid, msgstr, kind;
    int renderedPx = 0, availablePx = 0, overflowPx = 0;
    std::vector<std::string> group;   // peer msgctxts; empty => omitted from JSON
};

json to_json(const Record& r) {
    json j{
        {"lang", r.lang}, {"res", r.res}, {"dialog", r.dialog}, {"control", r.control},
        {"msgctxt", r.msgctxt}, {"msgid", r.msgid}, {"msgstr", r.msgstr}, {"kind", r.kind},
        {"renderedPx", r.renderedPx}, {"availablePx", r.availablePx}, {"overflowPx", r.overflowPx},
    };
    if (!r.group.empty()) j["group"] = r.group;
    return j;
}

// Scan every dialog for ONE language's already-loaded .po pair, appending records to `out`. Returns
// the number of dialogs successfully rendered (for the per-language summary line).
int scan_language(const std::string& lang, const std::vector<RcDialog>& dialogs, const ControlIndex& idx,
                  const PoFile& dialogsPo, const PoFile& stringsPo, std::vector<Record>& out) {
    // One hidden WS_POPUP host for this language's dialogs -- never shown, so nothing paints/flashes
    // (same off-screen technique as MainFrame::EnsureFitScan's worker / rc_render_test.cpp's host).
    static const wchar_t* kHostClass = L"FitscanHost";
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSW hc{}; hc.lpfnWndProc = ::DefWindowProcW; hc.hInstance = ::GetModuleHandleW(nullptr);
        hc.lpszClassName = kHostClass;
        ::RegisterClassW(&hc);
        classRegistered = true;
    }
    HWND host = ::CreateWindowExW(0, kHostClass, L"", WS_POPUP, 0, 0, 10, 10,
                                  nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!host) throw std::runtime_error("CreateWindowExW(fitscan host) failed for lang " + lang);

    int rendered = 0;
    for (const auto& d : dialogs) {
        std::vector<std::pair<HWND, std::string>> english;
        HWND dlg = fit::render_dialog(d, host, idx, dialogsPo, english);
        if (!dlg) continue;
        ++rendered;

        for (const auto& m : fit::measure_fit(dlg, d.id, idx, english)) {
            int r = m.grouped ? m.groupRenderedPx : m.renderedPx;
            int a = m.grouped ? m.groupAvailablePx : m.availablePx;
            fit::Kind k = fit::classify(r, a);
            if (k == fit::Kind::None) continue;
            const PoEntry* e = dialogsPo.find(m.msgctxt, m.msgid);
            Record rec;
            rec.lang = lang; rec.res = "dialogs"; rec.dialog = d.sym; rec.control = m.controlSym;
            rec.msgctxt = m.msgctxt; rec.msgid = m.msgid;
            rec.msgstr = (e && !e->msgstr.empty()) ? e->msgstr : m.msgid;
            rec.kind = kind_name(k); rec.renderedPx = r; rec.availablePx = a; rec.overflowPx = r - a;
            rec.group = m.groupPeers;
            out.push_back(std::move(rec));
        }

        // Combo-option text is filled at runtime in C++ (fit::combo_groups()'s comment) so it never
        // appears as live control text measure_fit can read -- every UNWIDENED group on this dialog is
        // measured directly against its combo's closed field (mirrors MainFrame::AppendComboFitFlags).
        for (const auto& g : fit::combo_groups()) {
            if (g.dropdownWidened || d.sym != g.idd) continue;
            long long comboId = -1;
            for (const auto& c : d.controls) if (c.sym == g.combo) { comboId = c.id; break; }
            if (comboId < 0) continue;

            std::vector<std::pair<std::string, std::wstring>> options;
            std::map<std::string, std::string> msgidByCtx;
            for (const char* item : g.items) {
                const PoEntry* found = nullptr;
                for (const auto& e : stringsPo.entries) if (e.msgctxt == item) { found = &e; break; }
                if (!found) continue;
                std::string text = !found->msgstr.empty() ? found->msgstr : found->msgid;
                int wn = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
                std::wstring w(wn > 0 ? wn - 1 : 0, L'\0');
                if (wn > 0) ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), wn);
                options.emplace_back(found->msgctxt, w);
                msgidByCtx[found->msgctxt] = found->msgid;
            }
            if (options.empty()) continue;

            for (const auto& m : fit::measure_combo_fit(dlg, comboId, options)) {
                fit::Kind k = fit::classify(m.renderedPx, m.availablePx);
                if (k == fit::Kind::None) continue;
                Record rec;
                rec.lang = lang; rec.res = "strings"; rec.dialog = d.sym; rec.control = g.combo;
                rec.msgctxt = m.msgctxt; rec.msgid = msgidByCtx[m.msgctxt];
                const PoEntry* e = nullptr;
                for (const auto& se : stringsPo.entries) if (se.msgctxt == m.msgctxt) { e = &se; break; }
                rec.msgstr = (e && !e->msgstr.empty()) ? e->msgstr : rec.msgid;
                rec.kind = kind_name(k); rec.renderedPx = m.renderedPx; rec.availablePx = m.availablePx;
                rec.overflowPx = m.renderedPx - m.availablePx;
                out.push_back(std::move(rec));
            }
        }
        ::DestroyWindow(dlg);
    }
    ::DestroyWindow(host);
    return rendered;
}

} // namespace

int main(int argc, char** argv) {
    std::string rcPath, resourceHPath, poDir, outPath = "fitscan.json";
    std::vector<std::string> langFilter;
    bool failOnHard = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " needs an argument");
            return argv[++i];
        };
        if (a == "--rc")               rcPath = next("--rc");
        else if (a == "--resource-h")  resourceHPath = next("--resource-h");
        else if (a == "--po-dir")      poDir = next("--po-dir");
        else if (a == "--out")         outPath = next("--out");
        else if (a == "--lang")        langFilter = split_csv(next("--lang"));
        else if (a == "--fail-on-hard") failOnHard = true;
        else if (a == "--help" || a == "-h") { print_usage(); return 0; }
        else { std::fprintf(stderr, "error: unknown option: %s\n", a.c_str()); print_usage(); return 1; }
    }
    if (rcPath.empty() || resourceHPath.empty() || poDir.empty()) {
        std::fprintf(stderr, "error: --rc, --resource-h and --po-dir are all required\n");
        print_usage();
        return 1;
    }

    try {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_LINK_CLASS |
            ICC_STANDARD_CLASSES | ICC_DATE_CLASSES | ICC_USEREX_CLASSES |
            ICC_COOL_CLASSES | ICC_INTERNET_CLASSES };
        ::InitCommonControlsEx(&icc);

        std::vector<RcDialog> dialogs = RcParser::parse(read_file(rcPath, true), read_file(resourceHPath, false));
        if (dialogs.empty()) {
            std::fprintf(stderr, "error: no IDD_* dialogs parsed from %s\n", rcPath.c_str());
            return 1;
        }
        ControlIndex idx = ControlIndex::from_records(rc_dialog_records(dialogs), {});

        std::vector<std::string> languages = enumerate_languages(poDir);
        if (!langFilter.empty()) {
            std::vector<std::string> kept;
            for (auto& l : languages)
                if (std::find(langFilter.begin(), langFilter.end(), l) != langFilter.end()) kept.push_back(l);
            languages = kept;
        }
        if (languages.empty()) {
            std::fprintf(stderr, "error: no languages found under --po-dir '%s'"
                                 " (expected mpc-hc.<lang>.dialogs.po files)\n", poDir.c_str());
            return 1;
        }
        std::fprintf(stderr, "scanning %zu dialog(s) x %zu language(s)\n", dialogs.size(), languages.size());

        std::vector<Record> records;
        int totalHard = 0, totalTight = 0;
        for (const auto& lang : languages) {
            fs::path dPath = fs::path(poDir) / ("mpc-hc." + lang + ".dialogs.po");
            fs::path sPath = fs::path(poDir) / ("mpc-hc." + lang + ".strings.po");
            PoFile dialogsPo = PoFile::parse_bytes(read_file(dPath, true));
            PoFile stringsPo = fs::exists(sPath) ? PoFile::parse_bytes(read_file(sPath, true)) : PoFile{};

            size_t before = records.size();
            int rendered = scan_language(lang, dialogs, idx, dialogsPo, stringsPo, records);
            int hard = 0, tight = 0;
            for (size_t i = before; i < records.size(); ++i)
                (records[i].kind == "hard" ? hard : tight)++;
            totalHard += hard; totalTight += tight;
            std::fprintf(stderr, "  %-8s %d dialog(s) rendered, %d hard, %d tight\n",
                        lang.c_str(), rendered, hard, tight);
        }

        std::stable_sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
            if (a.lang != b.lang) return a.lang < b.lang;
            return a.overflowPx > b.overflowPx;
        });

        HDC screenDc = ::GetDC(nullptr);
        int dpi = screenDc ? ::GetDeviceCaps(screenDc, LOGPIXELSY) : 96;
        if (screenDc) ::ReleaseDC(nullptr, screenDc);

        json out;
        out["generated"] = iso8601_utc_now();
        out["rc"] = rcPath;
        out["po_dir"] = poDir;
        out["dpi"] = dpi;
        out["languages"] = languages.size();
        out["dialogs"] = dialogs.size();
        json recs = json::array();
        for (const auto& r : records) recs.push_back(to_json(r));
        out["records"] = recs;

        std::ofstream f(outPath, std::ios::binary);
        if (!f) { std::fprintf(stderr, "error: cannot write %s\n", outPath.c_str()); return 1; }
        f << out.dump(2);
        f.close();

        std::printf("%d hard, %d tight across %zu language(s) -> %s\n",
                    totalHard, totalTight, languages.size(), outPath.c_str());
        if (failOnHard && totalHard > 0) return 2;
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
