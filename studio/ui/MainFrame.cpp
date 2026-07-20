// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "MainFrame.h"
#include "resource.h"
#include "Theme.h"
#include "SuggestFixDlg.h"
#include "AiSettingsDlg.h"
#include "mpctrans/config.h"
#include "mpctrans/github.h"
#include "mpctrans/fetch_cache.h"
#include "mpctrans/validate.h"
#include "mpctrans/drafts.h"
#include "mpctrans/corrections.h"
#include "mpctrans/ai_client.h"
#include "mpctrans/suggestion_store.h"
#include "mpctrans/data_update.h"

#include <uxtheme.h>    // SetWindowTheme (strip the progress bar's visual style so our colors apply)
#include <commctrl.h>   // SetWindowSubclass / DefSubclassProc (m_cmdHelp scroll subclass -> keep the ring aligned)
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
namespace fs = std::filesystem;
using namespace mpctrans;

#define WM_APP_PREFETCH_DONE     (WM_APP + 1)
#define WM_APP_PREFETCH_PROGRESS (WM_APP + 2)
#define WM_APP_AUTO_SELECT       (WM_APP + 3)   // automation: wParam=tab, lParam=row (like the demo hooks)
#define WM_APP_SUGGESTFIX_DONE   (WM_APP + 4)   // background research-correction PR submit completed
#define WM_APP_AI_SUGGEST_DONE   (WM_APP + 5)   // background on-demand AI-suggestion HTTP call completed
#define WM_APP_FIT_SCAN_PROGRESS (WM_APP + 6)   // background Review-Queue fit scan progress
#define WM_APP_FIT_SCAN_DONE     (WM_APP + 7)   // background Review-Queue fit scan completed
#define WM_APP_GEN_PROGRESS      (WM_APP + 8)   // background M2 bulk AI-suggestion generation progress
#define WM_APP_GEN_DONE          (WM_APP + 9)   // background M2 bulk AI-suggestion generation completed
#define WM_APP_BATCH_PREP_PO     (WM_APP + 10)  // M4 ALL-languages worker -> UI thread: load a language's po (no network)
#define WM_APP_BATCH_PREP_CTX    (WM_APP + 11)  // M4 ALL-languages worker -> UI thread: build research context for a worklist
#define WM_APP_GEN_ALL_PROGRESS  (WM_APP + 12)  // M4 ALL-languages bulk-generation progress
#define WM_APP_GEN_ALL_DONE      (WM_APP + 13)  // M4 ALL-languages bulk-generation completed
#define WM_APP_EXPORT_PROGRESS   (WM_APP + 14)  // background whole-language AI-prep export progress
#define WM_APP_EXPORT_DONE       (WM_APP + 15)  // background whole-language AI-prep export completed
#define WM_APP_DATA_UPDATE_DONE  (WM_APP + 16)  // background "Check for data updates" completed

// The prompt's version tag, stored alongside every generated suggestion (suggestion_store's
// prompt_version column) — bump this if AiSystemPrompt()/BuildAiUserPromptCore's rules change so old
// and new suggestions are distinguishable later (cross-model/cross-version comparison). Bumped to
// v3.1 for M3: BuildAiUserPromptCore now folds in fidelity-ranked sibling-language grounding.
// Bumped to v3.2: the reference-match translation pool was removed from BOTH the system prompt
// (rules 1 and 8 no longer mention "reference translations") and BuildAiUserPromptCore's generated
// user prompt (the "Reference translations..." section and its 4KB-trimmer pool are
// gone) -- the back-translation confidence axis also changed from a string-similarity SCORER to an
// LLM semantic-equivalence judge (see semantic_equivalence_judge in ai_client.h), though that doesn't
// change AiSystemPrompt()/BuildAiUserPromptCore's own rules, just how back_translation_score is
// computed.
// Bumped to v3.3: BuildAiUserPromptCore's generated user prompt may now append a "Translator hints:"
// block right after the Meaning line, when mpctrans::CoreEnrichment::hint_for() has data for the
// string (the new `string_hints` table -- see StringHint in enrichment.h). AiSystemPrompt()'s own
// rules are unchanged; only the USER prompt content did.
static const char* kAiPromptVersion = "v3.3";

// Result of a background M2/M3 bulk-generation run (see OnGenerateAiSuggestions). Heap-allocated by
// the worker, ownership passed to the UI thread via WM_APP_GEN_DONE.
struct GenerateResult {
    CString lang;
    struct Item { CString msgctxt, msgid, text; };
    std::vector<Item> items;      // successfully generated+stored -- for the session cache (m_aiSuggestCache)
    int generated = 0, skipped = 0, flaggedByGates = 0;
    int high = 0, low = 0;        // M3: per-primary-row confidence tally (mpctrans::confidence::classify)
    bool cancelled = false;
};

// M4: blocking UI-thread round-trip request for Phase A of the ALL-languages batch worker (see
// MainFrame::OnGenerateAiSuggestionsAll's comment for the two-phase handoff design) -- "load this
// language's .po, no network". File-scope (unlike BatchPrepCtxRequest, nested in MainFrame.h): its
// fields are all public mpctrans:: types, no private-member access needed. The worker thread owns this
// struct on its OWN STACK for the whole WaitForSingleObject wait, so a raw pointer through WPARAM is
// safe -- it never outlives the wait.
struct BatchPrepPoRequest {
    CString lang;
    std::optional<std::array<mpctrans::PoFile, 3>> outPo;   // 3 == MainFrame::RES_COUNT (private; see LoadLanguagePoNoNetwork)
    HANDLE done = nullptr;
};

// M4: result of the ALL-languages batch worker (see OnGenerateAiSuggestionsAll). Heap-allocated by the
// worker, ownership passed to the UI thread via WM_APP_GEN_ALL_DONE. Item/LangSummary are plain value
// types (no private MainFrame members), so -- like GenerateResult/PrefetchResult -- this stays file
// scope.
struct BatchGenerateResult {
    struct Item { CString lang, msgctxt, msgid, text; };   // seeds m_aiSuggestCache across ALL generated cells
    struct LangSummary {
        CString lang;
        int generated = 0, skipped = 0, flaggedByGates = 0, high = 0, low = 0, alreadySuggested = 0;
        bool notLoaded = false;
    };
    std::vector<Item> items;
    std::vector<LangSummary> perLang;
    int totalGenerated = 0, totalErrors = 0, totalAlreadySuggested = 0, totalGateFlagged = 0;
    int totalHigh = 0, totalLow = 0;
    int languagesNotLoaded = 0;
    bool cancelled = false;
};

// M4: one WM_APP_GEN_ALL_PROGRESS tick -- "language i/N — code: cellDone/cellTotal". Heap-allocated per
// tick (same posture as the *Result structs' ownership-transfer pattern); OnGenProgressAll deletes it.
struct BatchProgress {
    int langIndex = 0, langTotal = 0;
    CString langCode;
    int cellDone = 0, cellTotal = 0;
};

// Subclasses m_cmdHelp (defined near ShowCommandHelp): keeps the red locator ring aligned with the
// selected usage-list entry as the edit scrolls, resizes, or takes keyboard/wheel input that moves
// its scroll position.
static LRESULT CALLBACK CmdHelpSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref);

// Result of a background whole-language "Export AI-prep context" run (see OnExportAiContextLanguage).
// Heap-allocated by the worker, ownership passed to the UI thread via WM_APP_EXPORT_DONE. Deliberately
// tiny compared to GenerateResult/BatchGenerateResult -- there's no AI call, no suggestion_store write,
// nothing to seed a session cache with; just a summary for the status bar.
struct ExportResult {
    CString lang, path;
    int count = 0;
};

// Result of a background bulk-download: the parsed live RC + every language's parsed .po.
// Heap-allocated by the worker, ownership passed to the UI thread via WM_APP_PREFETCH_DONE.
struct PrefetchResult {
    std::vector<RcDialog> rcDialogs;                                   // empty if the RC fetch failed
    std::vector<mpctrans::RcMenu> rcMenus;                             // live menu hierarchy from the RC
    std::map<std::wstring, std::array<PoFile, 3>> po;                  // per language code
    // Status wording in OnPrefetchDone (see StartPrefetch's dumb-check-first flow):
    bool cacheStale = false;   // offline; head_sha failed, served entirely from the disk cache
    bool cacheFast  = false;   // online; upstream sha == the cached manifest sha -> no content downloads
};

// Result of a background research-correction PR submit (see OnSuggestFixClicked). Heap-allocated by
// the worker, ownership passed to the UI thread via WM_APP_SUGGESTFIX_DONE.
struct SuggestFixResult {
    bool ok = false;
    std::string url;     // valid when ok
    std::string error;   // valid when !ok
};

// Result of a background "Check for data updates" run (see OnCheckDataUpdate). Heap-allocated by the
// worker, ownership passed to the UI thread via WM_APP_DATA_UPDATE_DONE.
struct DataUpdateResult {
    bool    ok = false;         // false -- the manifest fetch/parse itself failed (see `error`)
    bool    upToDate = false;   // valid when ok -- true if the manifest listed nothing to change
    int     updated = 0, failed = 0;
    CString remoteVersion, localVersion;
    std::vector<CString> errors;   // per-file failures (sha mismatch / write failure / download error)
    CString error;               // valid when !ok
};

// Result of a background on-demand "Suggest with AI" call (see OnSuggestAiClicked). Heap-allocated
// by the worker, ownership passed to the UI thread via WM_APP_AI_SUGGEST_DONE. Carries the (msgctxt,
// msgid, lang) cache key it was computed for so OnAiSuggestDone can detect a stale reply (the
// translator moved on to a different string / language while the HTTP call was in flight).
struct AiSuggestResult {
    bool ok = false;
    CString text;              // valid when ok -- the extracted translation
    std::string error;         // valid when !ok
    CString msgctxt, msgid, lang;
    CString providerLabel;     // e.g. "claude/claude-sonnet-4-5" -- EditPanel::SetAiSuggestion's sourceLabel
};

// Fetch `repo_path`, honoring the disk cache (mpctrans::fetch_cache): when `dirty` is false, a cache
// hit skips the network entirely; a cache MISS always falls through to a real fetch (which is also
// how a language that failed to cache last run gets retried this run). A fresh fetch is stored so
// later runs can reuse it. `fetchFn` is either github::fetch_latest or a RawSession::fetch bound call.
template <class FetchFn>
static std::string cached_fetch(const std::string& repo_path, bool dirty, FetchFn&& fetchFn) {
    if (!dirty) {
        if (auto cached = fetch_cache::load(repo_path)) return *cached;
    }
    std::string bytes = fetchFn(repo_path);
    fetch_cache::store(repo_path, bytes);
    return bytes;
}

// ---------- Bundle ----------
static bool exists(const CString& p) { return ::GetFileAttributes(p) != INVALID_FILE_ATTRIBUTES; }

Bundle Bundle::Locate() {
    wchar_t exe[MAX_PATH]; ::GetModuleFileName(nullptr, exe, MAX_PATH);
    fs::path dir = fs::path(exe).parent_path();
    for (int up = 0; up < 8 && !dir.empty(); ++up, dir = dir.parent_path()) {
        Bundle b;
        // release layout: artifacts beside the exe
        if (fs::exists(dir / "control-index.json")) {
            b.index_json  = (dir / "control-index.json").c_str();
            b.neutral_dll = (dir / "mpcresources.neutral.dll").c_str();
            b.core_sqlite = (dir / "core-enrichment.sqlite").c_str();
            b.lang_dir    = (dir / "lang").c_str();
            b.po_dir      = (dir / "po").c_str();   // cached .po shipped in the bundle
            b.ok = true;
            return b;
        }
        // dev checkout: repo root with dist/ + enrichment/lang + the submodule PO as the cache
        if (fs::exists(dir / "dist" / "control-index.json")) {
            b.index_json  = (dir / "dist" / "control-index.json").c_str();
            b.neutral_dll = (dir / "dist" / "mpcresources.neutral.dll").c_str();
            b.core_sqlite = (dir / "dist" / "core-enrichment.sqlite").c_str();
            b.lang_dir    = (dir / "enrichment" / "lang").c_str();
            b.po_dir      = (dir / "upstream" / "src" / "mpc-hc" / "mpcresources" / "PO").c_str();
            b.ok = true;
            return b;
        }
    }
    return {};
}

static std::string read_file_lf(const CString& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    std::string s = ss.str(), o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        o += s[i];
    }
    return o;
}
static std::string read_file_raw(const CString& path) {
    std::ifstream f(path, std::ios::binary); std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static void write_file_raw(const CString& path, const std::string& s) {
    std::ofstream f(path, std::ios::binary); f.write(s.data(), (std::streamsize)s.size());
}

// M4: message-pumping join for m_genThread. The M4 ALL-languages batch worker's per-language handoff
// (see OnGenerateAiSuggestionsAll's threading comment) blocks the WORKER thread on
// WaitForSingleObject(..., INFINITE) waiting for THIS (UI) thread to service a posted
// WM_APP_BATCH_PREP_PO/CTX message and SetEvent() it awake. A plain std::thread::join() call on the UI
// thread stops that thread pumping messages the instant it's called -- if the worker happens to be
// mid-wait at that moment, the queued handoff message never gets dispatched, SetEvent() never fires,
// and join() blocks forever while the worker waits forever: a real deadlock (same CLASS of bug the
// existing "why not SendMessage" comment on OnGenerateAiSuggestions already warns about, just reached
// via join() instead of SendMessage). The single-language M2/M3 worker never blocks on a UI-thread
// response, so a plain join() was always safe for it -- but m_genThread is shared between both paths,
// so every UI-thread join() of it must now assume it MIGHT be the batch worker mid-handoff. Fix: keep
// pumping the message queue (which services any pending handoff, waking the worker so it can notice
// cancellation and finish) while polling the thread's native handle for termination via
// MsgWaitForMultipleObjects, then join() (which returns immediately once the thread has actually
// exited).
static void JoinGenThreadPumping(std::thread& t) {
    if (!t.joinable()) return;
    HANDLE h = (HANDLE)t.native_handle();
    for (;;) {
        DWORD wr = ::MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT);
        if (wr == WAIT_OBJECT_0) break;   // the thread itself signaled (terminated)
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessage(&msg); }
    }
    t.join();
}

// ---------- MainFrame ----------
BEGIN_MESSAGE_MAP(MainFrame, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_SIZE()
    ON_WM_MOVE()
    ON_WM_DESTROY()
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
    ON_WM_MEASUREITEM()
    ON_WM_DRAWITEM()
    ON_WM_SETTINGCHANGE()
    ON_WM_NCPAINT()
    ON_WM_NCACTIVATE()
    ON_BN_CLICKED(IDC_BTN_CHECKOUT, OnCheckoutClicked)
    ON_BN_CLICKED(IDC_BTN_SUGGESTFIX, OnSuggestFixClicked)
    ON_CBN_SELCHANGE(IDC_LANG_COMBO, OnLanguageChanged)
    ON_COMMAND(ID_FILE_SUBMITPR, OnSubmitPrCmd)
    ON_COMMAND(ID_FILE_RENDERRC, OnRenderFromRc)
    ON_COMMAND(ID_FILE_RENDERBUNDLE, OnRenderFromBundle)
    ON_COMMAND(ID_FILE_AI_PROVIDER, OnFileAiProvider)
    ON_COMMAND(ID_FILE_GENERATE_AI, OnGenerateAiSuggestions)
    ON_COMMAND(ID_FILE_GENERATE_AI_ALL, OnGenerateAiSuggestionsAll)
    ON_COMMAND(ID_FILE_EXPORT_AI_CTX, OnExportAiContextString)
    ON_COMMAND(ID_FILE_EXPORT_AI_CTX_LANG, OnExportAiContextLanguage)
    ON_COMMAND(ID_FILE_CHECK_DATA_UPDATE, OnCheckDataUpdate)
    ON_COMMAND(ID_FILE_DEMO_SELECT, OnDemoSelect)
    ON_COMMAND_RANGE(ID_VIEW_THEME_DARK, ID_VIEW_THEME_SYSTEM, OnViewTheme)
    ON_UPDATE_COMMAND_UI_RANGE(ID_VIEW_THEME_DARK, ID_VIEW_THEME_SYSTEM, OnUpdateViewTheme)
    ON_NOTIFY(TCN_SELCHANGE, IDC_TAB_SURFACE, OnTabChanged)
    ON_CBN_SELCHANGE(IDC_DLG_COMBO, OnDialogPicked)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_STR_LIST, OnStringSelected)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_STR_LIST, OnListCustomDraw)
    ON_NOTIFY(LVN_GETDISPINFO, IDC_STR_LIST, OnListGetDispInfo)
    ON_NOTIFY(TVN_SELCHANGED, IDC_MENU_TREE, OnMenuTreeSelChanged)
    ON_NOTIFY(NM_RCLICK, IDC_MENU_TREE, OnMenuTreeRClick)
    ON_BN_CLICKED(IDC_BTN_MENU_PREVIEW, OnMenuPreviewClicked)
    ON_MESSAGE(WM_APP_PREFETCH_DONE, OnPrefetchDone)
    ON_MESSAGE(WM_APP_PREFETCH_PROGRESS, OnPrefetchProgress)
    ON_MESSAGE(WM_APP_AUTO_SELECT, OnAutoSelect)
    ON_MESSAGE(WM_APP_SUGGESTFIX_DONE, OnSuggestFixDone)
    ON_MESSAGE(WM_APP_AI_SUGGEST_DONE, OnAiSuggestDone)
    ON_MESSAGE(WM_APP_FIT_SCAN_PROGRESS, OnFitScanProgress)
    ON_MESSAGE(WM_APP_FIT_SCAN_DONE, OnFitScanDone)
    ON_MESSAGE(WM_APP_GEN_PROGRESS, OnGenProgress)
    ON_MESSAGE(WM_APP_GEN_DONE, OnGenerateDone)
    ON_MESSAGE(WM_APP_BATCH_PREP_PO, OnBatchPrepPo)
    ON_MESSAGE(WM_APP_BATCH_PREP_CTX, OnBatchPrepCtx)
    ON_MESSAGE(WM_APP_GEN_ALL_PROGRESS, OnGenProgressAll)
    ON_MESSAGE(WM_APP_GEN_ALL_DONE, OnGenerateAllDone)
    ON_MESSAGE(WM_APP_EXPORT_PROGRESS, OnExportProgress)
    ON_MESSAGE(WM_APP_EXPORT_DONE, OnExportDone)
    ON_MESSAGE(WM_APP_DATA_UPDATE_DONE, OnDataUpdateDone)
    ON_BN_CLICKED(IDC_BTN_DISMISS, OnDismissClicked)
    ON_WM_COPYDATA()
END_MESSAGE_MAP()

int MainFrame::OnCreate(LPCREATESTRUCT lpcs) {
    if (CFrameWnd::OnCreate(lpcs) == -1) return -1;
    int saved = AfxGetApp() ? AfxGetApp()->GetProfileInt(L"theme", L"mode", (int)Theme::Mode::Dark)
                            : (int)Theme::Mode::Dark;
    Theme::SetMode((Theme::Mode)saved);    // restore the last-chosen theme (default MPC-HC Dark)
    Theme::InitTopWindow(GetSafeHwnd());   // dark mode (scrollbars) + title bar, before children
    { CClientDC dc(this); m_dpi = dc.GetDeviceCaps(LOGPIXELSX); }   // app is per-system DPI aware
    m_font.CreatePointFont(90, L"Segoe UI");                        // 9pt, DPI-scaled by the screen DC
    const DWORD ST = WS_CHILD | WS_VISIBLE;
    CRect z(0, 0, 0, 0);

    m_langCombo.Create(ST | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL, z, this, IDC_LANG_COMBO);
    m_btnCheckout.Create(L"&Get latest", ST | WS_TABSTOP, z, this, IDC_BTN_CHECKOUT);
    m_status.Create(L"", ST | SS_ENDELLIPSIS, z, this, IDC_STATUS_TEXT);
    m_btnMenuPreview.Create(L"Preview", ST | WS_TABSTOP, z, this, IDC_BTN_MENU_PREVIEW);   // shown by Layout only for the popup menus
    m_progress.Create(WS_CHILD | PBS_SMOOTH, z, this, IDC_PROGRESS);   // hidden until a download runs
    ::SetWindowTheme(m_progress.GetSafeHwnd(), L"", L"");   // strip the visual style so our colors apply
    m_progress.SetBkColor(Theme::WINDOW_BG);
    m_progress.SetBarColor(RGB(0, 120, 215));
    m_tabs.Create(ST | WS_TABSTOP, z, this, IDC_TAB_SURFACE);
    m_tabs.InsertItem(RES_DIALOGS, L"Dialogs");
    m_tabs.InsertItem(RES_MENUS, L"Menus");
    m_tabs.InsertItem(RES_STRINGS, L"All strings");
    m_tabs.InsertItem(RES_UNTRANS, L"Untranslated");
    m_tabs.InsertItem(RES_REVIEW, L"Review");
    m_dlgCombo.Create(ST | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, z, this, IDC_DLG_COMBO);
    m_list.Create(ST | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                  LVS_OWNERDATA, z, this, IDC_STR_LIST);   // virtual: instant fill of the huge list
    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    m_list.InsertColumn(0, L"Context", LVCFMT_LEFT, S(150));
    m_list.InsertColumn(1, L"English", LVCFMT_LEFT, S(150));
    m_list.InsertColumn(2, L"Translation", LVCFMT_LEFT, S(150));
    m_list.InsertColumn(3, L"Flag", LVCFMT_LEFT, S(220));   // Review tab only; harmless on other tabs
    m_previewHost.Create(AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), Theme::windowBrush()),
                         L"", ST | WS_BORDER | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL, z, this, 0);
    m_previewHost.OnScrolled = [this] { ReapplyHighlight(); };   // keep the ring synced to the scrolled child
    // The host is a custom AfxRegisterWndClass window, so Theme::ApplyToChildren (which keys on known
    // control classes) skips it -- theme its non-client scrollbars explicitly so they follow the mode.
    ::SetWindowTheme(m_previewHost.GetSafeHwnd(), Theme::IsDark() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    // command-line usage list (the "command dialog"): a read-only multiline edit shown over the
    // preview when an IDS_CMD_* string is selected. Parented to the frame so OnCtlColor themes it.
    m_cmdHelp.Create(WS_CHILD | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                     CRect(0, 0, 10, 10), this, IDC_CMDHELP);
    m_cmdHelp.SetFont(&m_font);
    ::SetWindowSubclass(m_cmdHelp.GetSafeHwnd(), CmdHelpSubclassProc, 1, (DWORD_PTR)this);
    // context mock-up (message box / OSD / status bar / tooltip) for loose strings, over the preview
    m_mockup.Create(AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), nullptr),
                    L"", WS_CHILD | WS_CLIPCHILDREN, CRect(0, 0, 10, 10), this, 0);
    m_mockup.SetFont(&m_font);
    // the selected string's enrichment write-up, under the rendering
    m_research.Create(WS_CHILD | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                      CRect(0, 0, 10, 10), this, IDC_RESEARCH);
    m_research.SetFont(&m_font);
    // docked at the research panel's top-right; enabled only once a string with research is selected
    m_btnSuggestFix.Create(L"Suggest fix\x2026", ST | WS_TABSTOP, z, this, IDC_BTN_SUGGESTFIX);
    m_btnSuggestFix.SetFont(&m_font);
    m_btnSuggestFix.EnableWindow(FALSE);
    // "Dismiss" — Review tab only; docks immediately to the left of "Suggest fix…" in the same strip.
    m_btnDismiss.Create(L"Dismiss", ST | WS_TABSTOP, z, this, IDC_BTN_DISMISS);
    m_btnDismiss.SetFont(&m_font);
    m_btnDismiss.EnableWindow(FALSE);
    m_btnDismiss.ShowWindow(SW_HIDE);
    m_menuTree.Create(WS_CHILD | WS_BORDER | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                      TVS_LINESATROOT | TVS_SHOWSELALWAYS, z, this, IDC_MENU_TREE);
    // The menu-bar preview is an OWNED popup (owner = this frame) positioned over the top of the
    // menu pane. We OWNER-DRAW it dark (a native Win32 menu bar can't be dark-themed by the OS),
    // drawing the translated top-level titles and dropping the real submenu on click.
    m_menuBar.CreateEx(0, AfxRegisterWndClass(0, ::LoadCursor(nullptr, IDC_ARROW), Theme::windowBrush()),
                       L"", WS_POPUP | WS_CLIPSIBLINGS, CRect(0, 0, 10, 10), this, 0);
    m_edit.CreatePanel(this, IDC_EDIT_PANEL);

    for (CWnd* w : std::initializer_list<CWnd*>{ &m_langCombo, &m_btnCheckout, &m_status,
             &m_tabs, &m_dlgCombo, &m_list, &m_menuTree, &m_btnMenuPreview })
        w->SetFont(&m_font);

    // Dark-theme the frame chrome: combos, the Get-latest button, the surface tabs, the string
    // list (+ its header), and the menu tree. The frame's own bg + statics come from OnCtlColor /
    // OnEraseBkgnd; the EditPanel themes its own children.
    Theme::ApplyToChildren(GetSafeHwnd());
    if (CMenu* fm = GetMenu()) { Theme::ThemeMenu(fm->GetSafeHmenu(), true); DrawMenuBar(); }

    m_edit.OnCommit = [this](const CString& s) { CommitEdit(s); };
    m_edit.OnSuggestAi = [this] { OnSuggestAiClicked(); };
    m_preview.OnControlClicked = [this](HWND c) { OnPreviewClick(c); };

    if (LoadBundle()) {
        PopulateLanguages();
        int sel = m_langCombo.GetCurSel();          // auto-load the default language from the cache
        if (sel != CB_ERR) {
            CString code; m_langCombo.GetLBText(sel, code); LoadLanguage(code, false);
            StartPrefetch();                        // then bulk-download all languages in the background
        } else SetStatus(L"Bundle loaded, but no language packs were found.");
    } else {
        SetStatus(L"BUNDLE MISSING: control-index.json / mpcresources.neutral.dll not found "
                  L"(dev: run bundle-build scripts).");
    }
    return 0;
}

void MainFrame::OnDestroy() {
    m_prefetchCancel = true;                         // stop the background download and wait for it
    if (m_prefetchThread.joinable()) m_prefetchThread.join();
    if (m_suggestFixThread.joinable()) m_suggestFixThread.join();   // a PR submit in flight
    if (m_aiSuggestThread.joinable()) m_aiSuggestThread.join();     // an AI suggestion call in flight
    if (m_dataUpdateThread.joinable()) m_dataUpdateThread.join();   // a data-update check/apply in flight
    if (m_fitThread.joinable()) { m_fitCancel = true; m_fitThread.join(); }   // a fit scan in flight
                                  // (no mid-loop cancel check -- blocks until the current dialog finishes)
    if (m_genThread.joinable()) { m_genCancel = true; JoinGenThreadPumping(m_genThread); }
                                  // a bulk generation in flight -- pumping join (NOT a plain join()):
                                  // the M4 ALL-languages worker may be mid per-language handoff, blocked
                                  // waiting for THIS thread to service a WM_APP_BATCH_PREP_PO/CTX message
                                  // (see JoinGenThreadPumping's comment); the M2/M3 single-language
                                  // worker never blocks on the UI thread, so this is a strict superset of
                                  // "safe to join directly" for it too.
    if (m_trackMenu) { ::DestroyMenu(m_trackMenu); m_trackMenu = nullptr; }
    if (m_barMenu)   { ::DestroyMenu(m_barMenu);   m_barMenu = nullptr; }
    m_preview.Unload();
    CFrameWnd::OnDestroy();
}

void MainFrame::Layout() {
    if (!m_list.GetSafeHwnd()) return;
    CRect rc; GetClientRect(&rc);
    const int W = rc.Width(), H = rc.Height();          // device px (per-monitor DPI)
    const int M = S(8), top = S(34), tabs = S(28), left = S(430), right = S(560);
    const int comboH = S(28), comboDrop = S(360), comboW = S(440);
    m_langCombo.MoveWindow(M, S(6), S(200), comboDrop);
    m_btnCheckout.MoveWindow(M + S(208), S(5), S(96), comboH);
    // Status text on top, with the download progress bar in a thin strip directly beneath it (only
    // visible while a download runs) so the bar sits under the text describing what's happening.
    const int sx = M + S(312), sw = W - M - sx;
    m_status.MoveWindow(sx, S(3), sw, S(15));
    m_progress.MoveWindow(sx, S(20), S(320), S(12));   // fixed, modest width under the text
    m_tabs.MoveWindow(0, top, W, tabs);
    int y = top + tabs + S(2), h = H - y;
    m_list.MoveWindow(0, y, left, h);
    int cx = left + S(6), cw = W - right - cx - S(6);
    int tab = m_tabs.GetCurSel();
    bool dlgTab = tab == RES_DIALOGS, menuTab = tab == RES_MENUS, untransTab = tab == RES_UNTRANS,
         strTab = tab == RES_STRINGS, reviewTab = tab == RES_REVIEW;
    bool previewTab = dlgTab || untransTab || strTab || reviewTab;   // tabs that render a string preview / mock-up
    const int resH = S(150), resGap = S(6);             // research write-up under the render
    // the picker combo serves both Dialogs (dialog list) and Menus (menu-resource list)
    m_dlgCombo.ShowWindow(dlgTab || menuTab ? SW_SHOW : SW_HIDE);
    m_previewHost.ShowWindow(previewTab ? SW_SHOW : SW_HIDE);
    m_menuTree.ShowWindow(menuTab ? SW_SHOW : SW_HIDE);
    m_research.ShowWindow(SW_SHOW);
    m_menuBarRect.SetRectEmpty();
    // centre pane, top to bottom: [ optional picker combo ] [ preview / mock-up / tree ] [ research ]
    int paneTop = y, paneH = h;
    if (dlgTab || menuTab) {   // a fixed, readable-width combo left-aligned above the preview
        m_dlgCombo.MoveWindow(cx, y, cw < comboW ? cw : comboW, comboDrop);
        paneTop = y + S(34); paneH = h - S(34);
    }
    if (menuTab && m_curMenu == 128 && m_checkedOut) {   // horizontal menu-bar strip above the tree
        int barH = ::GetSystemMetrics(SM_CYMENU) + S(8);
        m_menuBarRect.SetRect(cx, paneTop, cx + cw, paneTop + barH);
        paneTop += barH + S(4); paneH -= barH + S(4);
    }
    // The popup menus (IDR_POPUP*) have no bar to preview -- the real translated menu is shown by
    // this button (or by right-clicking the tree). It shares the picker combo's row, immediately to
    // its right, so it costs no vertical space.
    const bool showMenuPreview = menuTab && m_curMenu >= 0 && m_curMenu != 128 && m_checkedOut;
    m_btnMenuPreview.ShowWindow(showMenuPreview ? SW_SHOW : SW_HIDE);
    if (showMenuPreview) {
        int comboRight = cx + (cw < comboW ? cw : comboW);
        int btnW = S(90), left = comboRight + S(6);
        if (left + btnW > cx + cw) left = cx + cw - btnW;   // clamp inside the pane
        m_btnMenuPreview.MoveWindow(left, y, btnW, comboH);
    }
    int bodyH = paneH - resH - resGap;
    CWnd& body = menuTab ? static_cast<CWnd&>(m_menuTree) : static_cast<CWnd&>(m_previewHost);
    body.MoveWindow(cx, paneTop, cw, bodyH);
    // "Suggest fix…" docks in a thin header strip above the research edit, right-aligned, so it never
    // overlaps the write-up text.
    const int fixBtnH = S(20), fixBtnW = S(110), fixGap = S(4);
    const int dismissBtnW = S(80);
    int resTop = paneTop + bodyH + resGap;
    m_btnSuggestFix.MoveWindow(cx + cw - fixBtnW, resTop, fixBtnW, fixBtnH);
    m_btnDismiss.MoveWindow(cx + cw - fixBtnW - fixGap - dismissBtnW, resTop, dismissBtnW, fixBtnH);
    m_btnDismiss.ShowWindow(reviewTab ? SW_SHOW : SW_HIDE);
    m_research.MoveWindow(cx, resTop + fixBtnH + fixGap, cw, resH - fixBtnH - fixGap);
    m_edit.MoveWindow(W - right, y, right, h);
    if (m_cmdHelp.GetSafeHwnd() && (m_cmdHelp.GetStyle() & WS_VISIBLE)) {   // keep it over the preview
        CRect pr; m_previewHost.GetWindowRect(&pr); ScreenToClient(&pr); m_cmdHelp.MoveWindow(pr);
        UpdateCmdRing();                              // the ring is in screen coords; re-anchor after the move
    }
    if (m_mockup.GetSafeHwnd() && (m_mockup.GetStyle() & WS_VISIBLE)) {
        CRect pr; m_previewHost.GetWindowRect(&pr); ScreenToClient(&pr); m_mockup.MoveWindow(pr);
    }
    UpdateMenuBarPreview();
}
void MainFrame::OnSize(UINT t, int cx, int cy) { CFrameWnd::OnSize(t, cx, cy); Layout(); ReapplyHighlight(); }
void MainFrame::OnMove(int, int) { UpdateMenuBarPreview(); ReapplyHighlight(); }   // keep the owned bar + frame aligned
// The red locator frame is a top-level popup in screen coords, so re-anchor it to the current
// string's control whenever the window moves or resizes.
void MainFrame::ReapplyHighlight() {
    if (m_curRow >= 0 && m_curRow < (int)m_rows.size())
        m_preview.HighlightControl(m_rows[m_curRow].msgid);
    else
        m_preview.HighlightControl(std::string());
}

// Dark theme: the frame background (OnEraseBkgnd) and its statics/buttons (OnCtlColor). The list,
// tree, tabs and combos owner-draw themselves via the Theme subclasses installed in OnCreate.
HBRUSH MainFrame::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
    switch (nCtlColor) {
        case CTLCOLOR_EDIT: case CTLCOLOR_LISTBOX:
            return Theme::contentCtl(pDC->GetSafeHdc());
        case CTLCOLOR_STATIC: case CTLCOLOR_BTN: case CTLCOLOR_DLG:
            return Theme::windowCtl(pDC->GetSafeHdc());
    }
    return CFrameWnd::OnCtlColor(pDC, pWnd, nCtlColor);
}
BOOL MainFrame::OnEraseBkgnd(CDC* pDC) {
    CRect rc; GetClientRect(&rc); pDC->FillSolidRect(rc, Theme::WINDOW_BG); return TRUE;
}
// The frame owns the right-click popup (TrackPopupMenu) and its own File menu, so it forwards their
// owner-draw messages to Theme; the bar-preview popup is owned by m_menuBar (see MenuBarProc).
void MainFrame::OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT mis) {
    if (mis && mis->CtlType == ODT_MENU && Theme::MenuMeasureItem(mis)) return;
    CFrameWnd::OnMeasureItem(nIDCtl, mis);
}
void MainFrame::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT dis) {
    if (dis && dis->CtlType == ODT_MENU && Theme::MenuDrawItem(dis)) return;
    CFrameWnd::OnDrawItem(nIDCtl, dis);
}

BOOL MainFrame::LoadBundle() {
    m_bundle = Bundle::Locate();
    if (!m_bundle.ok) return FALSE;
    try {
        m_index = ControlIndex::load(std::string(CW2A(m_bundle.index_json, CP_UTF8)));
    } catch (const std::exception&) { return FALSE; }
    // Build version from data-manifest.json (beside the bundle) -> window title, so a user can tell
    // which build they're running (the release number 0.N; see make_manifest.py --version). Minimal
    // string extraction to avoid pulling a JSON parser into the UI TU.
    {
        CString mpath((fs::path((LPCWSTR)m_bundle.index_json).parent_path() / L"data-manifest.json")
                          .wstring().c_str());
        if (exists(mpath)) {
            std::string s = read_file_lf(mpath);
            size_t k = s.find("\"version\"");
            size_t q1 = (k == std::string::npos) ? std::string::npos : s.find('"', s.find(':', k) + 1);
            size_t q2 = (q1 == std::string::npos) ? std::string::npos : s.find('"', q1 + 1);
            if (q2 != std::string::npos)
                SetWindowText(CString(L"MPC-HC Translation Studio  \x2014  build ") +
                              CString(CA2W(s.substr(q1 + 1, q2 - q1 - 1).c_str(), CP_UTF8)));
        }
    }
    if (exists(m_bundle.core_sqlite)) {
        try {
            m_core = CoreEnrichment::open(std::string(CW2A(m_bundle.core_sqlite, CP_UTF8)));
            m_haveCore = true;
            // M3: empty when the table is absent -- fine (LanguageFidelity -> our own pair, unpacked
            // here so MainFrame.h doesn't need to know the enrichment.h struct shape).
            m_fidelity.clear();
            for (const auto& lf : m_core.language_fidelity()) m_fidelity.push_back({ lf.language, lf.pct });
        } catch (const std::exception&) { m_haveCore = false; }
    }
    return m_preview.LoadNeutralDll(m_bundle.neutral_dll);
}

void MainFrame::PopulateLanguages() {
    m_langCombo.ResetContent();
    // List languages that have actual .po content (mpc-hc.<code>.dialogs.po in the cache) — the set
    // you can translate. Enrichment packs are a superset (some langs have AI data but no .po yet),
    // so listing packs would offer dead entries. Fall back to packs only if no .po cache is present.
    const std::wstring pre = L"mpc-hc.", suf = L".dialogs.po";
    if (!m_bundle.po_dir.IsEmpty() && exists(m_bundle.po_dir)) {
        for (auto& p : fs::directory_iterator(std::wstring(m_bundle.po_dir))) {
            std::wstring fn = p.path().filename().wstring();
            if (fn.size() > pre.size() + suf.size() && fn.compare(0, pre.size(), pre) == 0 &&
                fn.compare(fn.size() - suf.size(), suf.size(), suf) == 0)
                m_langCombo.AddString(fn.substr(pre.size(), fn.size() - pre.size() - suf.size()).c_str());
        }
    } else if (exists(m_bundle.lang_dir)) {                 // no .po cache -> best effort from packs
        for (auto& p : fs::directory_iterator(std::wstring(m_bundle.lang_dir)))
            if (p.path().extension() == L".sqlite")
                m_langCombo.AddString(p.path().stem().c_str());
    }
    if (m_langCombo.SelectString(-1, L"de") == CB_ERR && m_langCombo.GetCount() > 0)
        m_langCombo.SetCurSel(0);
}

// Background bulk-download: fetch the live RC + every language's .po from GitHub (raw CDN, no rate
// cap) off the UI thread, then hand the parsed result to OnPrefetchDone. Keeps the app responsive;
// once it lands, every language is current and instant to switch. Get latest just re-runs it.
//
// Dumb-check-first, granular from there (see mpctrans::fetch_cache for the on-disk side):
//   1. One tiny API call — github::head_sha — gets the upstream branch's current commit.
//   2. If it matches the disk cache's manifest sha, NOTHING has changed upstream: every file is
//      read from disk, zero content downloads (a cache MISS on an individual file still forces a
//      fetch for just that file — e.g. a language that failed to cache last run).
//   3. Otherwise github::changed_paths diffs base...head and only the changed (or never-cached)
//      files are actually downloaded; everything else is read from disk.
//   4. If head_sha itself fails (offline) and a disk cache exists, serve everything from it (stale
//      is fine) without attempting ~135 more doomed network round-trips.
void MainFrame::StartPrefetch() {
    if (m_prefetchThread.joinable()) { m_prefetchCancel = true; m_prefetchThread.join(); }
    m_prefetchCancel = false;

    std::vector<std::string> langs;             // snapshot the language list on the UI thread
    for (int i = 0; i < m_langCombo.GetCount(); ++i) {
        CString c; m_langCombo.GetLBText(i, c); langs.push_back(std::string(CW2A(c, CP_UTF8)));
    }
    if (langs.empty()) return;
    SetStatus(L"Checking upstream for changes…");
    m_downloading = true;                       // reveal the progress bar (0 of N)
    m_progress.SetRange32(0, (int)langs.size());
    m_progress.SetPos(0);
    m_progress.ShowWindow(SW_SHOW);
    Layout();

    HWND hwnd = GetSafeHwnd();
    std::atomic<bool>* cancel = &m_prefetchCancel;
    m_prefetchThread = std::thread([hwnd, cancel, langs]() {
        static const char* kRes[3] = { "dialogs", "menus", "strings" };
        auto* res = new PrefetchResult;
        auto tokOpt = github::load_token();
        github::Token tok = tokOpt ? *tokOpt : github::Token{};

        std::string sha;
        bool haveSha = false;
        try { sha = github::head_sha(tok); haveSha = true; } catch (const std::exception&) {}
        auto cachedSha = fetch_cache::load_manifest_sha();

        if (!haveSha && cachedSha) {
            // Offline and we already know it (head_sha just failed) — serve the disk cache straight,
            // no per-file network fallback (that would mean ~135 doomed round-trips at 15s each).
            res->cacheStale = true;
            try {
                auto rc = fetch_cache::load("src/mpc-hc/mpc-hc.rc");
                auto rh = fetch_cache::load("src/mpc-hc/resource.h");
                if (rc && rh) {
                    res->rcDialogs = RcParser::parse(*rc, *rh);
                    res->rcMenus = mpctrans::rc_parse_menus(*rc, *rh);
                }
            } catch (const std::exception&) {}
            for (size_t i = 0; i < langs.size() && !cancel->load(); ++i) {
                try {
                    std::array<PoFile, 3> a;
                    bool ok = true;
                    for (int r = 0; r < 3 && ok; ++r) {
                        std::string name = "mpc-hc." + langs[i] + "." + std::string(kRes[r]) + ".po";
                        auto bytes = fetch_cache::load(std::string(config::PO_DIR) + "/" + name);
                        if (!bytes) { ok = false; break; }
                        a[r] = PoFile::parse_bytes(*bytes);
                    }
                    if (ok) res->po.emplace(std::wstring(CA2W(langs[i].c_str(), CP_UTF8)), std::move(a));
                } catch (const std::exception&) {}
                ::PostMessage(hwnd, WM_APP_PREFETCH_PROGRESS, (WPARAM)(i + 1), (LPARAM)langs.size());
            }
            if (!::PostMessage(hwnd, WM_APP_PREFETCH_DONE, (WPARAM)res, 0)) delete res;
            return;
        }

        // Dirty set: everything unless we have BOTH a current sha and a prior cache to diff against.
        bool allDirty = true;
        std::set<std::string> dirty;
        if (haveSha && cachedSha) {
            if (sha == *cachedSha) {
                allDirty = false;                                  // fast path: upstream unchanged
                res->cacheFast = true;
            } else {
                bool complete = false;
                auto paths = github::changed_paths(tok, *cachedSha, sha, complete);
                if (complete) { allDirty = false; dirty.insert(paths.begin(), paths.end()); }
            }
        }

        bool rcOk = false;
        try {
            std::string rcPath = "src/mpc-hc/mpc-hc.rc", rhPath = "src/mpc-hc/resource.h";
            auto fetchLatest = [&](const std::string& p) { return github::fetch_latest(tok, p); };
            std::string rc = cached_fetch(rcPath, allDirty || dirty.count(rcPath) != 0, fetchLatest);
            std::string rh = cached_fetch(rhPath, allDirty || dirty.count(rhPath) != 0, fetchLatest);
            res->rcDialogs = RcParser::parse(rc, rh);
            res->rcMenus = mpctrans::rc_parse_menus(rc, rh);
            rcOk = true;
        } catch (const std::exception&) {}

        // Fan the per-language .po fetches out across a few worker threads; each keeps ONE TLS
        // session (github::RawSession) alive for all its fetches, so the handshake is paid once
        // per thread, not per request. 4 connections is plenty and gentle on the CDN.
        fetch_cache::ensure_dir(config::PO_DIR);   // pre-create once — the pool writes into it concurrently
        std::vector<std::optional<std::array<PoFile, 3>>> slots(langs.size());
        std::atomic<size_t> next{0};
        std::atomic<int> completed{0};
        unsigned K = langs.size() < 4 ? (unsigned)langs.size() : 4u;
        std::vector<std::thread> pool;
        for (unsigned k = 0; k < K; ++k) pool.emplace_back([&]() {
            github::RawSession sess;                 // one keep-alive session per worker
            auto fetchRaw = [&](const std::string& p) { return sess.fetch(p); };
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= langs.size() || cancel->load()) break;
                try {
                    std::array<PoFile, 3> a;
                    for (int r = 0; r < 3; ++r) {
                        std::string name = "mpc-hc." + langs[i] + "." + std::string(kRes[r]) + ".po";
                        std::string path = std::string(config::PO_DIR) + "/" + name;
                        bool d = allDirty || dirty.count(path) != 0;
                        a[r] = PoFile::parse_bytes(cached_fetch(path, d, fetchRaw));
                    }
                    slots[i] = std::move(a);        // distinct index per thread -> no lock needed
                } catch (const std::exception&) {}  // skip a language that fails to fetch
                ::PostMessage(hwnd, WM_APP_PREFETCH_PROGRESS, ++completed, (LPARAM)langs.size());
            }
        });
        for (auto& t : pool) t.join();
        for (size_t i = 0; i < langs.size(); ++i)
            if (slots[i]) res->po.emplace(std::wstring(CA2W(langs[i].c_str(), CP_UTF8)), std::move(*slots[i]));

        // Only checkpoint the manifest once the layout files are good — a language that failed to
        // fetch/cache just stays a disk-cache miss, so it's naturally retried next run (see
        // cached_fetch: a MISS always fetches regardless of the dirty set). Skip the write entirely
        // when it would just restate the sha already on disk (the fast path) — keeps the manifest's
        // mtime honest and avoids a pointless temp-write+rename every relaunch.
        if (rcOk && haveSha && !(cachedSha && sha == *cachedSha)) fetch_cache::write_manifest_sha(sha);
        if (!::PostMessage(hwnd, WM_APP_PREFETCH_DONE, (WPARAM)res, 0)) delete res;  // window gone
    });
}

// UI thread: install the bulk-download result (live RC + all languages' .po) and refresh.
LRESULT MainFrame::OnPrefetchDone(WPARAM wp, LPARAM) {
    std::unique_ptr<PrefetchResult> res((PrefetchResult*)wp);
    if (m_prefetchCancel) return 0;                  // shutting down
    if (!res->rcDialogs.empty()) {
        m_preview.SetRcDialogs(res->rcDialogs);
        m_preview.SetRcMenus(std::move(res->rcMenus));   // live menu hierarchy for menu-in-context
        m_rcIndex = ControlIndex::from_records(rc_dialog_records(m_preview.RcDialogs()), m_index.menus());
        m_useRcIndex = true;
    }
    for (auto& kv : res->po) m_sessionPo[kv.first] = std::move(kv.second);
    if (m_checkedOut && m_sessionPo.count(std::wstring(m_lang)))
        LoadLanguage(m_lang, /*fromGithub=*/true, /*refreshRc=*/false);   // instant, from the session
    m_downloading = false;                           // hide the progress bar, give the status full width
    m_progress.ShowWindow(SW_HIDE);
    Layout();
    size_t n = m_sessionPo.size();
    CString s;
    if (res->cacheStale)
        s.Format(L"All %zu languages loaded from cache (offline — could not reach GitHub to check "
                 L"for updates).", n);
    else if (res->cacheFast)
        s.Format(L"All %zu languages up to date (cached; upstream unchanged).", n);
    else
        s.Format(L"All %zu languages up to date (latest via GitHub). Press 'Get latest' to refresh.", n);
    SetStatus(s);
    return 0;
}

// A worker finished a language; advance the bar (messages may arrive out of order, so take the max).
LRESULT MainFrame::OnPrefetchProgress(WPARAM done, LPARAM total) {
    if (!m_downloading) return 0;
    if ((int)total != 0) m_progress.SetRange32(0, (int)total);
    if ((int)done > m_progress.GetPos()) m_progress.SetPos((int)done);
    return 0;
}

// The language combo browses instantly from the cached (bundled) .po — no GitHub round-trip.
void MainFrame::OnLanguageChanged() {
    int sel = m_langCombo.GetCurSel();
    if (sel == CB_ERR) return;
    CString code; m_langCombo.GetLBText(sel, code);
    if (code == m_lang) return;
    // Pending edits are saved per language (drafts.json), so switching no longer loses them — the
    // current language's edits are already on disk and the target's are restored on load.
    // In "latest mode" (the live RC is loaded) fetch this language's .po fresh so its translations
    // — including new strings — are current too; otherwise browse instantly from the cache.
    LoadLanguage(code, /*fromGithub=*/m_useRcIndex, /*refreshRc=*/false);
}

// "Get latest" — refresh the selected language from upstream HEAD (the accurate base for a PR).
void MainFrame::OnCheckoutClicked() {
    int sel = m_langCombo.GetCurSel();
    if (sel == CB_ERR) { SetStatus(L"Pick a language first."); return; }
    CString code; m_langCombo.GetLBText(sel, code);
    // Get latest re-fetches the upstream base then re-applies your saved edits on top, so they persist.
    // An edit whose English changed upstream is silently discarded — that string is simply untranslated
    // again (it reappears in the Untranslated tab), since a translation is bound to a specific English.
    LoadLanguage(code, /*fromGithub=*/true, /*refreshRc=*/true);   // current language now, for feedback
    StartPrefetch();                                                // then refresh every language
}

bool MainFrame::LoadLanguage(const CString& code, bool fromGithub, bool refreshRc) {
    static const char* kRes[RES_COUNT] = { "dialogs", "menus", "strings" };
    CWaitCursor wait;
    std::string lang = CW2A(code, CP_UTF8);
    auto tok = fromGithub ? github::load_token() : std::optional<github::Token>{};
    bool gotGithub = fromGithub;

    PoFile parsed[RES_COUNT];
    std::wstring skey(code);
    // Instant re-visit: reuse this session's already-fetched .po (unless Get latest forces a refresh).
    auto cachedIt = (fromGithub && !refreshRc) ? m_sessionPo.find(skey) : m_sessionPo.end();
    if (cachedIt != m_sessionPo.end()) {
        for (int r = 0; r < RES_COUNT; ++r) parsed[r] = cachedIt->second[r];
    } else {
        // If the bulk prefetch already has a valid disk cache (manifest sha present), consult it
        // before hitting the network — this path just covers "user clicked a language before the
        // prefetch landed"; StartPrefetch owns SHA validation, so no HEAD check here.
        bool haveManifest = fetch_cache::load_manifest_sha().has_value();
        for (int r = 0; r < RES_COUNT; ++r) {
            std::string name = "mpc-hc." + lang + "." + std::string(kRes[r]) + ".po";
            std::string repoPath = std::string(config::PO_DIR) + "/" + name;
            CString cached = m_bundle.po_dir + L"\\" + CString(CA2W(name.c_str(), CP_UTF8));
            std::string bytes;
            bool haveCache = !m_bundle.po_dir.IsEmpty() && exists(cached);
            if (fromGithub) {
                auto diskCached = haveManifest ? fetch_cache::load(repoPath) : std::nullopt;
                if (diskCached) {
                    bytes = *diskCached;
                } else {
                    try {
                        bytes = github::fetch_latest(tok ? *tok : github::Token{}, repoPath);
                        fetch_cache::store(repoPath, bytes);
                    } catch (const std::exception& ex) {
                        gotGithub = false;                    // offline / rate-limited -> cached copy
                        if (!haveCache) {
                            MessageBox(L"Cannot fetch " + CString(CA2W(name.c_str(), CP_UTF8)) +
                                       L" from GitHub:\n" + CString(CA2W(ex.what(), CP_UTF8)) +
                                       L"\n\nand no cached copy is bundled.", L"Get latest failed",
                                       MB_ICONERROR);
                            return false;
                        }
                        bytes = read_file_lf(cached);
                    }
                }
            } else {
                if (!haveCache) {
                    MessageBox(L"No cached copy of " + CString(CA2W(name.c_str(), CP_UTF8)) +
                               L" is bundled. Use 'Get latest' to fetch it from GitHub.",
                               L"Language unavailable", MB_ICONERROR);
                    return false;
                }
                bytes = read_file_lf(cached);
            }
            parsed[r] = PoFile::parse_bytes(bytes);
        }
        if (fromGithub && gotGithub) {   // remember this session's fetch for instant re-visits
            std::array<PoFile, RES_COUNT> a;
            for (int r = 0; r < RES_COUNT; ++r) a[r] = parsed[r];
            m_sessionPo[skey] = std::move(a);
        }
    }

    for (int r = 0; r < RES_COUNT; ++r) { m_po[r] = std::move(parsed[r]); m_dirty[r].clear(); }
    m_lang = code;
    m_checkedOut = true;
    ApplyDrafts();                       // restore this language's locally-saved pending edits
    if (fromGithub && gotGithub) TransifexOverlay(code);   // offer to fill base-empty strings (online only)
    m_fromGithub = gotGithub;
    m_havePack = false;
    CString pack = m_bundle.lang_dir + L"\\" + code + L".sqlite";
    if (exists(pack)) {
        try { m_pack = LangPack::open(std::string(CW2A(pack, CP_UTF8))); m_havePack = true; }
        catch (const std::exception&) {}
    }

    // Approach C: on a GitHub fetch, also pull the latest mpc-hc.rc + resource.h and drive the
    // Dialogs surface from them. The RC (dialog LAYOUT) is language-independent, so once fetched it
    // PERSISTS across language switches — switching a language only swaps its .po (translations),
    // never the layout. That keeps new controls from flickering in and out as you change language.
    bool rcLive = m_useRcIndex;                 // keep the live layout if we already have it
    if (refreshRc && gotGithub) {               // (re)fetch the layout only on an explicit Get latest
        try {
            std::string rc = github::fetch_latest(tok ? *tok : github::Token{}, "src/mpc-hc/mpc-hc.rc");
            std::string rh = github::fetch_latest(tok ? *tok : github::Token{}, "src/mpc-hc/resource.h");
            if (m_preview.SetRcSourceBytes(rc, rh)) {
                m_rcIndex = ControlIndex::from_records(rc_dialog_records(m_preview.RcDialogs()),
                                                       m_index.menus());
                m_useRcIndex = rcLive = true;
            }
        } catch (const std::exception&) { /* keep whatever layout we already have */ }
    }
    RefreshAfterLoad();                          // Idx() reflects rcLive (persisted)

    CString sha = m_index.upstream_sha.empty() ? CString(L"bundle")
                : CString(CA2W(m_index.upstream_sha.substr(0, 8).c_str(), CP_UTF8));
    CString statusMsg;
    if (rcLive)
        statusMsg = L"Showing '" + code + L"' — latest upstream layout (live RC); translations " +
                   (gotGithub ? L"latest via GitHub." : L"cached (press 'Get latest' to refresh them).");
    else if (gotGithub)
        statusMsg = L"Showing '" + code + L"' — latest .po via GitHub (RC fetch failed; pinned layout).";
    else
        statusMsg = L"Showing '" + code + L"' — cached @ " + sha +
                   L". Press 'Get latest' for the newest before submitting a PR.";
    // M2: a cheap single COUNT query -- append the stored-suggestion count for this language if
    // nonzero (doesn't block/slow language switching; suggestion_store's own mutex serializes it
    // against any in-flight background generation for another language, harmlessly).
    if (long long stored = suggestion_store::count(std::string(CW2A(code, CP_UTF8))); stored > 0) {
        CString extra;
        extra.Format(L" · %lld stored AI suggestion%s for this language.", stored, stored == 1 ? L"" : L"s");
        statusMsg += extra;
    }
    SetStatus(statusMsg);
    return true;
}

// Re-render whatever surface is active after a language load (fixes stale preview on switch).
void MainFrame::RefreshAfterLoad() {
    int tab = m_tabs.GetCurSel();
    if (tab == RES_DIALOGS && m_dlgIds.empty()) PopulateDialogCombo();
    if (tab == RES_MENUS && m_menuIds.empty()) PopulateMenuCombo();
    PopulateList();
    EnsureFitScan();   // harmless no-op if already cached/running; keeps the Review tab's cache fresh
    if (tab == RES_DIALOGS)     RenderCurrentDialog();
    else if (tab == RES_MENUS)  BuildMenuTree();
    else                      { m_preview.DestroyPreview(); m_previewHost.SetFrame(L"", CSize()); }
    m_edit.ClearString();
}

// Bare IDD_* symbol -> dialog id, recovered from Idx().dialogs(): strip a trailing "_CAPTION" off a
// caption record's msgctxt, else "_" + control_sym off a control record's (first non-empty symbol
// wins per dialog). Built fresh each call (a few dozen dialogs, cheap) and shared by DialogForString
// (page-title string -> its dialog) and PopulateDialogCombo (labeling a captionless property page by
// its page title) so the stripping logic lives in one place.
std::map<std::string, long long> MainFrame::BuildDialogSymbolMap() {
    std::map<long long, std::string> symByDialog;
    for (const auto& r : Idx().dialogs()) {
        std::string& sym = symByDialog[r.dialog];
        if (!sym.empty()) continue;
        if (!r.control) {
            const std::string suffix = "_CAPTION";
            if (r.msgctxt.size() > suffix.size())
                sym = r.msgctxt.substr(0, r.msgctxt.size() - suffix.size());
        } else if (r.msgctxt.size() > r.control_sym.size() + 1) {
            sym = r.msgctxt.substr(0, r.msgctxt.size() - r.control_sym.size() - 1);
        }
    }
    std::map<std::string, long long> out;
    for (const auto& [dlg, sym] : symByDialog) if (!sym.empty()) out[sym] = dlg;
    return out;
}

static CString clean_menu_label(const std::wstring& s);   // defined below; strips '&' mnemonics

void MainFrame::PopulateDialogCombo() {
    m_dlgCombo.ResetContent();
    m_dlgIds.clear();
    // EVERY distinct dialog id in the index is renderable — property pages (IDD_PPAGE*) have no
    // CAPTION record (their titles live in the Options tree instead, as a STRING-table entry keyed
    // by the bare IDD_ symbol — see BuildDialogSymbolMap), so label those by that page TITLE
    // (translated, else English) when available, else fall back to the raw IDD_ symbol.
    struct Info { std::string caption, sym; };
    std::map<long long, Info> dlgs;
    for (const auto& r : Idx().dialogs()) {
        Info& d = dlgs[r.dialog];
        if (!r.control) d.caption = r.msgid;
    }
    for (const auto& [sym, dlg] : BuildDialogSymbolMap()) dlgs[dlg].sym = sym;
    std::vector<std::pair<CString, long long>> items;
    for (const auto& [id, info] : dlgs) {
        std::string label = info.caption;
        if (label.empty() && !info.sym.empty()) {
            // No CAPTION record -- a property page. Prefer its Options-tree page title (a
            // STRING-table entry whose msgctxt is this dialog's bare symbol) over the raw symbol.
            for (const auto& e : m_po[RES_STRINGS].entries)
                if (e.msgctxt == info.sym) { label = !e.msgstr.empty() ? e.msgstr : e.msgid; break; }
        }
        if (label.empty()) label = info.sym;
        // "<title>  —  <SYM>  (IDD <n>)" -- title first so the sort above still groups pages by
        // category; the symbol (when known) makes the raw IDD_ resource findable from the label
        // alone. Omit the "—" separator entirely (not just leave it blank) when the symbol is unknown,
        // and don't repeat it when label already IS the raw symbol (the last-resort fallback above).
        // Strip '&' mnemonics for display: a title like "&Details" is a real accelerator in the app
        // (D underlined), but a combo item renders the '&' literally, so drop it ("&&" -> "&").
        CString s = clean_menu_label(std::wstring((LPCWSTR)CString(CA2W(label.c_str(), CP_UTF8))));
        CString symPart;
        if (!info.sym.empty() && label != info.sym)
            symPart.Format(L"  \x2014  %s", (LPCWSTR)CString(CA2W(info.sym.c_str(), CP_UTF8)));
        CString num; num.Format(L"  (IDD %lld)", id);
        items.emplace_back(s + symPart + num, id);
    }
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first.CompareNoCase(b.first) < 0; });
    for (const auto& [label, id] : items) {
        m_dlgCombo.AddString(label);
        m_dlgIds.push_back(id);
    }
    if (!m_dlgIds.empty()) m_dlgCombo.SetCurSel(0);
    m_curDialog = m_dlgIds.empty() ? -1 : m_dlgIds[0];
}

void MainFrame::PopulateList() {
    m_rows.clear();
    m_curRow = -1;
    int tab = m_tabs.GetCurSel();
    if (tab == RES_DIALOGS) {
        for (const auto& r : Idx().dialogs())
            if (r.dialog == m_curDialog)
                m_rows.push_back({ r.msgctxt, r.msgid, RES_DIALOGS });
    } else if (tab == RES_MENUS) {
        for (const auto& r : m_index.menus())
            m_rows.push_back({ r.msgctxt, r.msgid, RES_MENUS });
    } else if (tab == RES_REVIEW) {
        PopulateReviewRows();   // sets m_rows + SetItemCountEx/EnsureVisible/Invalidate + its own status text
        return;
    } else {   // RES_STRINGS = every string; RES_UNTRANS = empty translations (+ ones you just filled,
               // kept visible with the edited highlight so you can see your progress)
        bool untransOnly = (tab == RES_UNTRANS);
        for (int res = 0; res < RES_COUNT; ++res)
            for (const auto& e : m_po[res].entries)
                if (!e.msgid.empty() &&
                    (!untransOnly || e.msgstr.empty() || IsEdited(e.msgctxt, e.msgid)))
                    m_rows.push_back({ e.msgctxt, e.msgid, res });
    }
    // Virtual list: hand the control the row count; cell text is pulled via LVN_GETDISPINFO. This
    // fills the "All strings" list (thousands of rows) instantly instead of inserting row by row.
    m_list.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    m_list.SetItemCountEx((int)m_rows.size(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    m_list.EnsureVisible(0, FALSE);
    m_list.Invalidate();
    if (tab == RES_UNTRANS) {
        CString s; s.Format(L"%zu untranslated string(s) in '%s'.", m_rows.size(), (LPCWSTR)m_lang);
        SetStatus(s);
    }
}

// (context / english / current translation) for a row — used by GetDispInfo and the row custom-draw.
CString MainFrame::RowText(int i, int col) {
    if (i < 0 || i >= (int)m_rows.size()) return CString();
    const Row& row = m_rows[i];
    if (col == 0) return CA2W(row.msgctxt.c_str(), CP_UTF8);
    if (col == 1) return CA2W(row.msgid.c_str(), CP_UTF8);
    if (col == 3) return row.evidence;
    if (const PoFile* po = PoFor(row.msgctxt, row.msgid))
        if (auto* e = po->find(row.msgctxt, row.msgid))
            return CA2W(e->msgstr.c_str(), CP_UTF8);
    return CString();
}
void MainFrame::OnListGetDispInfo(NMHDR* hdr, LRESULT* res) {
    *res = 0;
    auto* di = (NMLVDISPINFO*)hdr;
    if (di->item.mask & LVIF_TEXT) {
        CString s = RowText(di->item.iItem, di->item.iSubItem);
        ::lstrcpynW(di->item.pszText, s, di->item.cchTextMax);
    }
}
void MainFrame::RefreshListRow(int i) { m_list.RedrawItems(i, i); }   // virtual: re-pull + repaint

PoFile* MainFrame::PoFor(const std::string& ctx, const std::string& id, int* res) {
    if (!m_checkedOut) return nullptr;
    for (int r = 0; r < RES_COUNT; ++r)
        if (m_po[r].find(ctx, id)) { if (res) *res = r; return &m_po[r]; }
    return nullptr;
}
bool MainFrame::IsEdited(const std::string& ctx, const std::string& id) const {
    for (const auto& d : m_dirty) if (d.count({ ctx, id })) return true;
    return false;
}

// --- local persistence of pending edits (survive restarts; cleared once submitted as a PR) ---
CString MainFrame::DraftsPath() const {
    wchar_t* base = nullptr; size_t n = 0; _wdupenv_s(&base, &n, L"LOCALAPPDATA");
    CString dir = (base && *base) ? base : L".";
    free(base);
    dir += L"\\MPC-HC Translation Studio";
    ::CreateDirectory(dir, nullptr);
    return dir + L"\\drafts.json";
}
void MainFrame::SaveDrafts() {
    if (m_lang.IsEmpty()) return;
    Drafts d = parse_drafts(read_file_raw(DraftsPath()));   // preserve the other languages' drafts
    std::string lang = std::string(CW2A(m_lang, CP_UTF8));
    std::vector<DraftEdit> edits;
    for (int r = 0; r < RES_COUNT; ++r)
        for (const auto& key : m_dirty[r])
            if (const PoEntry* e = m_po[r].find(key.first, key.second))
                edits.push_back({ r, key.first, key.second, e->msgstr });
    if (edits.empty()) d.erase(lang); else d[lang] = std::move(edits);
    write_file_raw(DraftsPath(), serialize_drafts(d));
}
// Re-apply this language's saved edits after (re)loading its .po. A translation is bound to the exact
// English it was made against, so we re-attach only on an exact (ctx,id) match. If the English changed
// upstream (or the string was removed), the edit is silently discarded — that string is simply
// untranslated again and reappears in the Untranslated tab for a fresh translation.
void MainFrame::ApplyDrafts() {
    Drafts d = parse_drafts(read_file_raw(DraftsPath()));
    auto it = d.find(std::string(CW2A(m_lang, CP_UTF8)));
    if (it == d.end()) return;
    for (const auto& e : it->second) {
        if (e.res < 0 || e.res >= RES_COUNT) continue;
        for (auto& entry : m_po[e.res].entries)                   // exact key only
            if (entry.msgctxt == e.msgctxt && entry.msgid == e.msgid) {   // English changed/removed -> discard
                entry.msgstr = e.msgstr;
                m_dirty[e.res].insert({ e.msgctxt, e.msgid });
                break;
            }
    }
}

// Offer to fill strings the current base .po leaves EMPTY with translations from the maintainer's
// Transifex staging fork (config::TRANSIFEX_*) — a fresher Transifex snapshot than clsid2 develop.
// Called from LoadLanguage right after ApplyDrafts, only when the base was just fetched online: an
// offline load must never touch the network. Fill-empty only — an existing base translation (or one
// the translator already edited this session) is never overwritten. Asked at most once per language
// per session (m_txHandled), regardless of the user's answer.
void MainFrame::TransifexOverlay(const CString& code) {
    std::string lang = CW2A(code, CP_UTF8);
    if (m_txHandled.count(lang)) return;

    PoFile txPo[RES_COUNT];
    bool haveAny = false;
    try {
        for (int r = 0; r < RES_COUNT; ++r) {
            std::string path = std::string(config::PO_DIR) + "/mpc-hc." + lang + "." +
                               std::string(config::RESOURCES[r]) + ".po";
            auto bytes = github::fetch_latest(github::Token{}, config::TRANSIFEX_OWNER,
                                              config::TRANSIFEX_REPO, config::TRANSIFEX_BRANCH, path);
            if (!bytes) continue;   // absent on the fork for this resource -- normal, skip
            txPo[r] = PoFile::parse_bytes(*bytes);
            haveAny = true;
        }
    } catch (const std::exception&) {
        return;   // network error -- silently skip, never nag while offline
    }
    if (!haveAny) { m_txHandled.insert(lang); return; }

    struct Fill { int res; std::string ctx, id, msgstr; };
    std::vector<Fill> fills;
    for (int r = 0; r < RES_COUNT; ++r)
        for (const auto& e : txPo[r].entries) {
            if (e.msgid.empty() || e.msgstr.empty()) continue;
            const PoEntry* base = m_po[r].find(e.msgctxt, e.msgid);
            if (base && base->msgstr.empty() && !IsEdited(e.msgctxt, e.msgid))
                fills.push_back({ r, e.msgctxt, e.msgid, e.msgstr });
        }
    m_txHandled.insert(lang);   // mark handled regardless of the user's choice -- never renag
    if (fills.empty()) return;

    CString msg;
    msg.Format(L"Transifex has %zu translation(s) for '%s' that are missing from the current base.\n\n"
              L"Load them as editable drafts?", fills.size(), (LPCWSTR)code);
    if (MessageBox(msg, L"Transifex overlay", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    for (const auto& f : fills)
        for (auto& entry : m_po[f.res].entries)
            if (entry.msgctxt == f.ctx && entry.msgid == f.id) {
                entry.msgstr = f.msgstr;
                m_dirty[f.res].insert({ f.ctx, f.id });
                break;
            }
    SaveDrafts();     // persist as drafts immediately -- same call CommitEdit uses -- so the fills
                      // survive a later language switch
    PopulateList();   // refresh the visible list (list-wide, since fills can span multiple resources)

    CString status;
    status.Format(L"Loaded %zu Transifex translation(s) for '%s' as drafts.", fills.size(), (LPCWSTR)code);
    SetStatus(status);
}

void MainFrame::RenderCurrentDialog() {
    int tab = m_tabs.GetCurSel();
    // Dialogs / Untranslated / All strings can all pull a string's dialog into the preview (Menus uses
    // the tree; a loose string with no dialog gets a mock-up instead — see SelectRow).
    if (tab == RES_MENUS || m_curDialog < 0 || !m_checkedOut) {
        m_preview.DestroyPreview();
        m_previewHost.SetFrame(L"", CSize());     // no dialog -- no frame either
        m_previewHost.SyncScroll(true);   // no child now -> RecalcAndReposition hides any stale scrollbars
        m_previewHost.Invalidate();       // erase the destroyed dialog's leftover pixels
        return;
    }
    HWND dlg = m_preview.RenderDialog(m_curDialog, &m_previewHost, Idx(), m_po[RES_DIALOGS]);
    if (dlg) ::SetWindowPos(dlg, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Frame the preview like the player's Options property-page frame (CThemedHostWnd::SetFrame draws
    // it): the caption is the page's Options-tree title, LEAF ONLY -- a STRING-table entry keyed by the
    // dialog's bare IDD_ symbol (see BuildDialogSymbolMap), msgid "Category::Page" (the category prefix
    // is discarded -- the real property sheet's caption is the tree-item text, i.e. just "Page") or
    // plain "Page" with no category.
    // ONLY the Options sheet's own pages are framed: CPPageSheet (upstream PPageSheet.cpp) AddPage()s
    // the IDD_PPAGE* dialogs, and only those sit in the tree frame this draws. Other dialogs that
    // happen to own a bare-symbol title string belong to a DIFFERENT, tabbed sheet (the File
    // Properties tabs IDD_FILEMEDIAINFO / IDD_FILEPROP*) and modal dialogs have no such frame at all
    // -- neither gets the caption bar. Anything else -> no frame (SetFrame(L"", CSize())).
    CString page;
    bool isPropertyPage = false;
    std::map<long long, std::string> symByDialog;
    for (const auto& [sym, dlg2] : BuildDialogSymbolMap()) symByDialog[dlg2] = sym;
    if (auto it = symByDialog.find(m_curDialog);
        it != symByDialog.end() && it->second.rfind("IDD_PPAGE", 0) == 0) {
        for (const auto& e : m_po[RES_STRINGS].entries) {
            if (e.msgctxt != it->second) continue;
            std::string title = !e.msgstr.empty() ? e.msgstr : e.msgid;
            if (size_t sep = title.find("::"); sep != std::string::npos)
                page = CA2W(title.substr(sep + 2).c_str(), CP_UTF8);   // keep the split, discard the category
            else
                page = CA2W(title.c_str(), CP_UTF8);
            isPropertyPage = true;
            break;
        }
    }

    // The frame's content size: for a property page, the max IDD_PPAGE* template size in DLU (the
    // real sheet sizes its page area to fit the LARGEST page, so every page must be framed identically
    // -- that's what lets a translator judge whether a long translated title fits the caption, and
    // where content falls, not just how its own page happens to look). DLU -> px uses the CURRENT
    // rendered child as the scale reference; every division is guarded, falling back to the child's
    // own pixel size on ANY failure (missing record, zero DLU, or no RcDialogs at all -- the pinned
    // neutral-DLL path, which carries no DLU templates). A modal dialog (isPropertyPage==false) just
    // gets its own pixel size -- it isn't sized against sibling pages by any real property sheet.
    CSize contentPx;   // (0,0) with an empty `page` => SetFrame's no-frame condition
    CSize childPx;
    if (dlg) { CRect wr; ::GetWindowRect(dlg, &wr); childPx = wr.Size(); }
    if (!page.IsEmpty() && isPropertyPage) {
        int maxCx = 0, maxCy = 0, curCx = 0, curCy = 0;
        for (const auto& d : m_preview.RcDialogs()) {
            if (d.sym.rfind("IDD_PPAGE", 0) != 0) continue;
            maxCx = max(maxCx, d.cx); maxCy = max(maxCy, d.cy);
            if (d.id == m_curDialog) { curCx = d.cx; curCy = d.cy; }
        }
        if (dlg && maxCx > 0 && maxCy > 0 && curCx > 0 && curCy > 0) {
            double pxPerDluX = childPx.cx / (double)curCx, pxPerDluY = childPx.cy / (double)curCy;
            contentPx = CSize((int)(maxCx * pxPerDluX + 0.5), (int)(maxCy * pxPerDluY + 0.5));
        } else {
            contentPx = childPx;   // fallback: no RcDialogs / missing record / zero DLU
        }
    }
    // Never frame smaller than the thing being framed: a page's controls can sit outside the dialog's
    // own window rect (a dialog doesn't clip its children), so a frame sized purely from the template
    // leaves them spilling below/right of the border -- e.g. IDD_PPAGEADVANCED's wahr/falsch/Standard
    // row. Grow to cover the child; the max-page sizing above still governs when the child is smaller.
    if (!page.IsEmpty()) {
        contentPx.cx = max(contentPx.cx, childPx.cx);
        contentPx.cy = max(contentPx.cy, childPx.cy);
    }
    m_previewHost.SetFrame(page, contentPx);
    m_previewHost.SyncScroll(true);   // reset to top-left and compute the fresh dialog's scroll range
    // Force an immediate, FULL repaint of the host AND the freshly-created dialog tree. A plain
    // Invalidate() marks only the host's client, and WS_CLIPCHILDREN excludes the child dialog + its
    // controls -- so the new combos/edits stay unpainted (showing the prior page's pixels) until a
    // mouse event repaints them. RDW_ALLCHILDREN recurses into the dialog's controls; RDW_UPDATENOW
    // paints now rather than deferring. (RDW_ERASE redraws the frame chrome in OnEraseBkgnd.)
    m_previewHost.RedrawWindow(nullptr, nullptr,
                               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// Which IDD dialog (if any) a string belongs to — for pulling its dialog into the preview.
long long MainFrame::DialogForString(const std::string& ctx, const std::string& id) {
    for (const auto& r : Idx().dialogs())
        if (r.msgctxt == ctx && r.msgid == id) return r.dialog;
    // No control record matches -- ctx may be a property-page TITLE (msgctxt == the bare IDD_ symbol,
    // e.g. IDD_PPAGEPLAYER, msgid "Player::General") rather than a control. Fall back so selecting a
    // page-title string in "All strings" still pulls the dialog it titles into the preview.
    auto syms = BuildDialogSymbolMap();
    if (auto it = syms.find(ctx); it != syms.end()) return it->second;
    return -1;
}

// The command-line strings (IDS_CMD_*) aren't in any dialog — in MPC-HC they're listed in the
// command-line help dialog as "switch<tab>description". Reproduce that usage list (translated, or
// English where still untranslated) in a read-only edit over the preview, tab-aligned into columns.
// `selCtx` is the msgctxt of the entry the editor is actually translating: its position in the built
// text is recorded (m_cmdSelStart/End) so the red locator ring can be drawn around it and it can be
// scrolled into view — mirroring the ring LivePreview draws around dialog controls.
void MainFrame::ShowCommandHelp(const std::string& selCtx) {
    m_preview.DestroyPreview();
    HideMockup();
    m_preview.HideComboDropdown();                      // this surface owns the ring; neutralize siblings
    m_preview.HideTooltip();
    // Size + show FIRST, so the multiline edit computes its wrap/scroll range for the real size
    // before the text is set (otherwise the first fill lays out for the 10x10 creation rect and the
    // content + scrollbar only correct themselves on a later reflow, e.g. after an edit).
    CRect rc; m_previewHost.GetWindowRect(&rc); ScreenToClient(&rc);
    m_cmdHelp.MoveWindow(rc);
    m_cmdHelp.ShowWindow(SW_SHOW);
    CString text = L"Usage:\r\n";
    m_cmdSelStart = m_cmdSelEnd = -1;
    for (const auto& e : m_po[RES_STRINGS].entries) {
        if (e.msgctxt.rfind("IDS_CMD_", 0) != 0) continue;
        const std::string& v = e.msgstr.empty() ? e.msgid : e.msgstr;
        CString piece = CString(CA2W(v.c_str(), CP_UTF8));
        // Normalize THIS piece's line endings before concatenating — a global Replace over the final
        // text (the old approach) can grow/shrink the string and shift the char offsets recorded below.
        piece.Replace(L"\r\n", L"\n"); piece.Replace(L"\r", L""); piece.Replace(L"\n", L"\r\n");
        static const CString kSep = L"\r\n";
        int start = text.GetLength() + kSep.GetLength();
        text += kSep + piece;
        if (!selCtx.empty() && e.msgctxt == selCtx) { m_cmdSelStart = start; m_cmdSelEnd = start + piece.GetLength(); }
    }
    m_cmdHelp.SetTabStops(92);                          // description column past the longest switch
    m_cmdHelp.SetWindowText(text);
    if (m_cmdSelStart >= 0) {                           // scroll the selected entry into view
        int line = m_cmdHelp.LineFromChar(m_cmdSelStart);
        int target = max(0, line - 3);
        m_cmdHelp.LineScroll(target - m_cmdHelp.GetFirstVisibleLine());
    } else {
        m_cmdHelp.LineScroll(-m_cmdHelp.GetFirstVisibleLine());   // scroll to the top
    }
    m_cmdHelp.Invalidate();                             // repaint incl. the (dark) scrollbar
    UpdateCmdRing();
}
void MainFrame::HideCommandHelp() {
    m_cmdHelp.ShowWindow(SW_HIDE);
    m_preview.RingScreenRect(nullptr);
    m_cmdSelStart = m_cmdSelEnd = -1;
}

// (Re)position the red locator ring around the SELECTED usage-list entry (m_cmdSelStart/End) as a
// band spanning the edit's client width, from the entry's first line to its last — so a multi-line
// entry (switch on one line, wrapped description continuing below) is ringed as a whole. Hides the
// ring when the edit isn't visible, nothing is selected, or the entry has scrolled out of view.
void MainFrame::UpdateCmdRing() {
    if (m_cmdSelStart < 0 || !m_cmdHelp.GetSafeHwnd() || !(m_cmdHelp.GetStyle() & WS_VISIBLE)) {
        m_preview.RingScreenRect(nullptr);
        return;
    }
    LRESULT posStart = m_cmdHelp.SendMessage(EM_POSFROMCHAR, (WPARAM)m_cmdSelStart, 0);
    int lastChar = max(m_cmdSelStart, m_cmdSelEnd - 1);
    LRESULT posEnd = m_cmdHelp.SendMessage(EM_POSFROMCHAR, (WPARAM)lastChar, 0);
    int yTop = (int)(short)HIWORD(posStart);
    int yBot = (int)(short)HIWORD(posEnd);

    CClientDC dc(&m_cmdHelp);
    HGDIOBJ old = dc.SelectObject(&m_font);
    TEXTMETRIC tm{}; dc.GetTextMetrics(&tm);
    dc.SelectObject(old);

    CRect client; m_cmdHelp.GetClientRect(&client);
    CRect band(client.left + 2, yTop, client.right - 2, yBot + tm.tmHeight);
    CRect clipped;
    if (!clipped.IntersectRect(&band, &client) || clipped.IsRectEmpty()) {   // scrolled fully out of view
        m_preview.RingScreenRect(nullptr);
        return;
    }
    m_cmdHelp.ClientToScreen(&clipped);
    RECT screenRc = clipped;
    m_preview.RingScreenRect(&screenRc);
}
// See forward declaration near the top of the file. Runs DefSubclassProc first so the edit's own
// scroll/key/size handling completes, then re-anchors the ring to the (possibly moved) selection.
static LRESULT CALLBACK CmdHelpSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    LRESULT r = ::DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_KEYDOWN || msg == WM_SIZE)
        if (auto* self = (MainFrame*)ref) self->UpdateCmdRing();
    return r;
}

// The submenu HMENU that DIRECTLY contains command `cmd` (searching `m` recursively), or null.
static HMENU findSubmenu(HMENU m, UINT cmd) {
    int n = ::GetMenuItemCount(m);
    for (int i = 0; i < n; ++i) if ((UINT)::GetMenuItemID(m, i) == cmd) return m;
    for (int i = 0; i < n; ++i) if (HMENU s = ::GetSubMenu(m, i)) if (HMENU r = findSubmenu(s, cmd)) return r;
    return nullptr;
}
// A standalone copy of `src`'s items (labels already translated); sub-popups become empty popups so
// their arrow still draws. The caller owns the returned HMENU.
static HMENU copyMenu(HMENU src) {
    HMENU dst = ::CreatePopupMenu();
    int n = ::GetMenuItemCount(src);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW mi{ sizeof(mi) }; wchar_t buf[256] = L"";
        mi.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING; mi.dwTypeData = buf; mi.cch = 256;
        ::GetMenuItemInfoW(src, i, TRUE, &mi);
        if (mi.fType & MFT_SEPARATOR) { ::AppendMenuW(dst, MF_SEPARATOR, 0, nullptr); continue; }
        UINT f = MF_STRING;
        if (mi.fState & MFS_DISABLED) f |= MF_GRAYED;
        if (mi.fState & MFS_CHECKED)  f |= MF_CHECKED;
        if (mi.hSubMenu) ::AppendMenuW(dst, f | MF_POPUP, (UINT_PTR)::CreatePopupMenu(), buf);
        else             ::AppendMenuW(dst, f, mi.wID, buf);
    }
    return dst;
}

// The RC-menu item list that DIRECTLY contains a command with symbol `ctx` (searching recursively);
// sets `sel` to its index within that list. Null if not found.
static const std::vector<RcMenuItem>* findRcSubmenu(const std::vector<RcMenuItem>& items,
                                                    const std::string& ctx, int& sel) {
    for (int i = 0; i < (int)items.size(); ++i)
        if (!items[i].separator && items[i].items.empty() && items[i].sym == ctx) { sel = i; return &items; }
    for (const auto& it : items)
        if (!it.items.empty()) { int s = -1; if (auto* r = findRcSubmenu(it.items, ctx, s)) { sel = s; return r; } }
    return nullptr;
}

// Build an owner-draw HMENU for menu command `ctx`: the real (translated) submenu it lives in. Prefer
// the LIVE RC menu hierarchy (so a command added upstream after the bundle pin — e.g. View > Presets >
// Custom — renders in its actual spot); fall back to the pinned menu DLL, then to a single item.
// ThemeMenu makes it owner-drawn so CMockupWnd renders it exactly like a live MPC-HC menu. Caller owns it.
HMENU MainFrame::BuildMenuMockup(const std::string& ctx, const CString& text, int& selIndex) {
    HMENU result = nullptr; selIndex = -1;
    for (const auto& rm : m_preview.RcMenus()) {          // 1. live RC hierarchy
        int sel = -1;
        const std::vector<RcMenuItem>* items = findRcSubmenu(rm.items, ctx, sel);
        if (!items) continue;
        result = ::CreatePopupMenu();
        for (const auto& it : *items) {
            if (it.separator) { ::AppendMenuW(result, MF_SEPARATOR, 0, nullptr); continue; }
            std::string eng = mpctrans::rc_text_normalize(it.text);           // English caption
            std::string mctx = (it.sym == "POPUP") ? std::string("POPUP") : it.sym;
            std::string disp = eng;
            if (auto* e = m_po[RES_MENUS].find(mctx, eng); e && !e->msgstr.empty()) disp = e->msgstr;  // translate
            CString label(CA2W(disp.c_str(), CP_UTF8));
            if (!it.items.empty()) ::AppendMenuW(result, MF_STRING | MF_POPUP, (UINT_PTR)::CreatePopupMenu(), label);
            else                   ::AppendMenuW(result, MF_STRING, (UINT)it.command, label);
        }
        selIndex = sel;
        break;
    }
    if (!result) {                                        // 2. pinned menu DLL
        long long cmd = -1;
        for (const auto& r : Idx().menus()) if (r.msgctxt == ctx && r.command) { cmd = *r.command; break; }
        if (cmd >= 0)
            for (long long menuId : { 128LL, 130LL, 133LL }) {   // IDR_MAINFRAME / IDR_POPUP / IDR_POPUPMAIN
                HMENU m = m_preview.LoadRawMenu(menuId); if (!m) continue;
                SubstituteMenuInPlace(m);
                if (HMENU sub = findSubmenu(m, (UINT)cmd)) {
                    result = copyMenu(sub);
                    for (int i = 0, k = ::GetMenuItemCount(result); i < k; ++i)
                        if ((UINT)::GetMenuItemID(result, i) == (UINT)cmd) { selIndex = i; break; }
                }
                ::DestroyMenu(m);
                if (result) break;
            }
    }
    if (!result) {                                        // 3. single highlighted item
        result = ::CreatePopupMenu();
        ::AppendMenuW(result, MF_STRING, 1, text);
        selIndex = 0;
    }
    Theme::ThemeMenu(result, false);
    return result;
}

// Pick a context mock-up for a loose string from the enrichment (ui_type / ui_location) and show it
// over the preview. Returns false (nothing shown) if the string has no recognizable placement.
bool MainFrame::ShowMockup(const StringInfo* si, const std::string& ctx, const CString& text, int res) {
    auto low = [](std::string s) { for (auto& c : s) c = (char)::tolower((unsigned char)c); return s; };
    std::string ty = si ? low(si->ui_type) : std::string(), loc = si ? low(si->ui_location) : std::string();
    auto in = [](const std::string& h, const char* n) { return h.find(n) != std::string::npos; };
    bool error = ty == "error" || in(loc, "error");
    // MPC-HC tooltip strings are named with a "_TIP_" segment (e.g. IDS_ARS_TIP_PAUSE_KEEP_ACTIVE);
    // catch them even when the (snapshot) enrichment has no ui_type for a newly-added one.
    bool isTip = ctx.find("_TIP_") != std::string::npos ||
                 (ctx.size() >= 4 && ctx.compare(ctx.size() - 4, 4, "_TIP") == 0);
    CMockupWnd::Mode m = CMockupWnd::Mode::None;
    if      (res == RES_MENUS || ty == "menu")                      m = CMockupWnd::Mode::Menu;
    else if (ty == "tooltip" || isTip)                             m = CMockupWnd::Mode::Tooltip;
    else if (in(loc, "osd"))                                        m = CMockupWnd::Mode::OSD;
    else if (ty == "error" || in(loc, "message box") || in(loc, "error dialog")) m = CMockupWnd::Mode::MsgBox;
    else if (in(loc, "status bar") || in(loc, "status message"))    m = CMockupWnd::Mode::StatusBar;
    else if (ty == "message")                                       m = CMockupWnd::Mode::MsgBox;
    else                                                            m = CMockupWnd::Mode::Generic;
    // '&' is a mnemonic in menu/label/button/command contexts (Menu + Generic + message box) — render
    // it underlined; keep it literal only in prose/format-code contexts (OSD pill, status bar, tooltip,
    // which can hold ASS override codes like "&H000000&"). Mode-based, not ui_type-based, so a string
    // whose enrichment lookup missed still gets its mnemonic.
    bool accel = m == CMockupWnd::Mode::Menu || m == CMockupWnd::Mode::Generic || m == CMockupWnd::Mode::MsgBox;

    m_preview.DestroyPreview();
    HideCommandHelp();
    CRect rc; m_previewHost.GetWindowRect(&rc); ScreenToClient(&rc);
    m_mockup.MoveWindow(rc);
    // A menu string renders as a real (owner-draw) menu — its containing submenu, or a single item.
    if (m == CMockupWnd::Mode::Menu) {
        int selIndex = -1;
        HMENU menu = BuildMenuMockup(ctx, text, selIndex);   // sets selIndex — sequence before SetMenu
        m_mockup.SetMenu(menu, selIndex, m_dpi);
    } else {
        CString caption = si ? CString(CA2W(si->ui_location.c_str(), CP_UTF8)) : CString();
        if (m == CMockupWnd::Mode::Tooltip) {   // caption = the setting the tooltip hovers (drop "_TIP")
            std::string sib = ctx; size_t p = sib.find("_TIP_");
            if (p != std::string::npos) sib = sib.substr(0, p) + sib.substr(p + 4);
            else if (sib.size() >= 4 && sib.compare(sib.size() - 4, 4, "_TIP") == 0) sib.resize(sib.size() - 4);
            caption.Empty();
            for (int r = 0; r < RES_COUNT && caption.IsEmpty(); ++r)
                for (const auto& e : m_po[r].entries)
                    if (e.msgctxt == sib && !e.msgid.empty()) {
                        caption = CString(CA2W((e.msgstr.empty() ? e.msgid : e.msgstr).c_str(), CP_UTF8));
                        break;
                    }
        }
        m_mockup.SetContent(m, text, caption, error, accel, m_dpi);
    }
    m_mockup.ShowWindow(SW_SHOW);
    m_mockup.Invalidate();
    return true;
}
void MainFrame::HideMockup() { if (m_mockup.GetSafeHwnd()) m_mockup.ShowWindow(SW_HIDE); }

// The enrichment "research" for the selected string (how it's used across the app), under the render.
void MainFrame::ShowResearch(const StringInfo* si) {
    if (!m_research.GetSafeHwnd()) return;
    CString t;
    auto add = [&](const wchar_t* label, const std::string& v) {
        if (!v.empty()) { t += label; t += CString(CA2W(v.c_str(), CP_UTF8)); t += L"\r\n"; }
    };
    if (si) {
        add(L"Type:  ", si->ui_type);
        add(L"Category:  ", si->category);
        add(L"Where:  ", si->ui_location);
        add(L"Meaning:  ", si->semantic_purpose);
        add(L"Function:  ", si->functional_purpose);
    }
    if (t.IsEmpty()) t = L"(no research recorded for this string)";
    m_research.SetWindowText(t);
    m_research.LineScroll(-m_research.GetFirstVisibleLine());
    m_research.Invalidate();
}

// Automation hook (like the demo dialog hook): select a surface tab + a list row by index.
LRESULT MainFrame::OnAutoSelect(WPARAM tab, LPARAM row) {
    if (m_tabs.GetCurSel() != (int)tab) {   // only switch surfaces if needed (keeps a demo-picked dialog)
        m_tabs.SetCurSel((int)tab);
        LRESULT r = 0; OnTabChanged(nullptr, &r);
    }
    int i = (int)row;
    if (i >= 0 && i < (int)m_rows.size()) {
        m_list.EnsureVisible(i, FALSE);
        m_list.SetItemState(i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SelectRow(i);
    }
    return 0;
}

// Automation: WM_COPYDATA carrying "tab:msgctxt" (UTF-8) selects the first row on `tab` whose
// context == msgctxt -- so tests can target a string by name instead of an unstable row index.
BOOL MainFrame::OnCopyData(CWnd* wnd, COPYDATASTRUCT* cds) {
    if (!cds || !cds->lpData || cds->cbData == 0) return CFrameWnd::OnCopyData(wnd, cds);
    std::string msg((const char*)cds->lpData, cds->cbData);
    size_t colon = msg.find(':');
    if (colon == std::string::npos) return TRUE;
    int tab = atoi(msg.substr(0, colon).c_str());
    std::string ctx = msg.substr(colon + 1);
    if (m_tabs.GetCurSel() != tab) { m_tabs.SetCurSel(tab); LRESULT r = 0; OnTabChanged(nullptr, &r); }
    for (int i = 0; i < (int)m_rows.size(); ++i)
        if (m_rows[i].msgctxt == ctx) {
            m_list.EnsureVisible(i, FALSE);
            m_list.SetItemState(i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            SelectRow(i);
            break;
        }
    return TRUE;
}

void MainFrame::OnDialogPicked() {
    // the picker combo is shared by the Dialogs and Menus tabs
    int sel = m_dlgCombo.GetCurSel();
    if (sel == CB_ERR) return;
    if (m_tabs.GetCurSel() == RES_MENUS) {
        if (sel >= (int)m_menuIds.size()) return;
        m_curMenu = m_menuIds[sel];
        BuildMenuTree();
        m_edit.ClearString();
        return;
    }
    if (sel >= (int)m_dlgIds.size()) return;
    m_curDialog = m_dlgIds[sel];
    HideCommandHelp(); HideMockup();
    PopulateList();
    RenderCurrentDialog();
    m_edit.ClearString();
}

// --- Menus: picker + full-hierarchy tree + native popup preview ---
void MainFrame::PopulateMenuCombo() {
    m_dlgCombo.ResetContent();
    m_menuIds.clear();
    struct MI { long long id; const wchar_t* name; };
    static const MI known[] = {
        { 128, L"Main menu bar  (IDR_MAINFRAME)" },
        { 130, L"Player context menu  (IDR_POPUP)" },
        { 133, L"Main popup menu  (IDR_POPUPMAIN)" },
    };
    for (const auto& k : known)
        if (m_preview.HasMenu(k.id)) { m_dlgCombo.AddString(k.name); m_menuIds.push_back(k.id); }
    if (!m_menuIds.empty()) { m_dlgCombo.SetCurSel(0); m_curMenu = m_menuIds[0]; }
    else m_curMenu = -1;
}

// menu label for the tree: drop the mnemonic '&' (&& -> &) and the "\t<shortcut>" tail.
static CString clean_menu_label(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\t') break;
        if (s[i] == L'&') { if (i + 1 < s.size() && s[i + 1] == L'&') { o += L'&'; ++i; } continue; }
        o += s[i];
    }
    return CString(o.c_str());
}

// MPC-HC's popup menus (IDR_POPUP / IDR_POPUPMAIN) wrap their real items in a single unnamed
// POPUP ""; the player itself previews them via GetSubMenu(0) (see CMainFrame::GetShortMenu /
// the WM_CONTEXTMENU handler). Descend that wrapper so the tree + right-click show the real menu.
// The returned submenu is OWNED by `m` -- never DestroyMenu it separately.
static HMENU menu_root(HMENU m) {
    if (!m || ::GetMenuItemCount(m) != 1) return m;
    MENUITEMINFOW info{ sizeof(info) };
    info.fMask = MIIM_SUBMENU;
    ::GetMenuItemInfoW(m, 0, TRUE, &info);
    if (!info.hSubMenu) return m;
    wchar_t buf[512] = {};
    ::GetMenuStringW(m, 0, buf, 512, MF_BYPOSITION);
    return buf[0] ? m : info.hSubMenu;               // empty label + submenu -> it's the wrapper
}

// Walks `src` (the English source menu) once, building the tree under `parent`; when `dst` is
// non-null it ALSO appends the same resolved item to `dst`, so the tree and the native right-click
// popup (m_trackMenu) can never disagree -- see the bug this fixes at the top of BuildMenuTree.
// The tree label is cleaned (clean_menu_label) for display; the dst label keeps its '&' mnemonic
// and "\t<shortcut>" tail, mirroring SubstituteMenuInPlace.
void MainFrame::AddMenuNodes(HMENU src, HTREEITEM parent, HMENU dst) {
    int n = ::GetMenuItemCount(src);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW info{ sizeof(info) };
        info.fMask = MIIM_SUBMENU | MIIM_ID | MIIM_FTYPE | MIIM_STATE;
        ::GetMenuItemInfoW(src, i, TRUE, &info);
        if (info.fType & MFT_SEPARATOR) {
            m_menuTree.InsertItem(L"──────────", parent);
            if (dst) ::AppendMenuW(dst, MF_SEPARATOR, 0, nullptr);
            continue;
        }

        wchar_t buf[512] = {};
        ::GetMenuStringW(src, i, buf, 512, MF_BYPOSITION);
        std::wstring cur = buf;
        std::string english = CW2A(cur.c_str(), CP_UTF8), ctx, msgid, msgstr;

        if (info.hSubMenu) {
            if (auto* r = m_index.menu_popup(english)) { ctx = r->msgctxt; msgid = r->msgid; }
        } else if (auto* r = m_index.menu_command(info.wID)) { ctx = r->msgctxt; msgid = r->msgid; }
        if (!ctx.empty() || !msgid.empty())
            if (auto* e = m_po[RES_MENUS].find(ctx, msgid); e && !e->msgstr.empty()) msgstr = e->msgstr;

        std::wstring shown = msgstr.empty() ? cur : std::wstring((LPCWSTR)CA2W(msgstr.c_str(), CP_UTF8));
        HTREEITEM h = m_menuTree.InsertItem(clean_menu_label(shown), parent);
        if (!msgid.empty()) m_menuNodeKey[h] = { ctx, msgid };

        HMENU sub = nullptr;
        if (info.hSubMenu) {
            sub = dst ? ::CreatePopupMenu() : nullptr;
            AddMenuNodes(info.hSubMenu, h, sub);
            m_menuNodeSub[h] = dst ? sub : info.hSubMenu;
        }

        if (dst) {
            std::wstring label = shown;                        // keep '&' mnemonic (unlike the tree label)
            if (!msgstr.empty())
                if (auto t = cur.find(L'\t'); t != std::wstring::npos) label += cur.substr(t);   // shortcut tail
            if (info.hSubMenu) ::AppendMenuW(dst, MF_POPUP, (UINT_PTR)sub, label.c_str());
            else               ::AppendMenuW(dst, MF_STRING, info.wID, label.c_str());
            if ((info.fState & (MFS_CHECKED | MFS_DISABLED)) || (info.fType & MFT_RADIOCHECK)) {
                MENUITEMINFOW set{ sizeof(set) };
                set.fMask = MIIM_STATE | MIIM_FTYPE;
                set.fState = info.fState & (MFS_CHECKED | MFS_DISABLED);
                set.fType = MFT_STRING | (info.fType & MFT_RADIOCHECK);
                ::SetMenuItemInfoW(dst, ::GetMenuItemCount(dst) - 1, TRUE, &set);
            }
        }
    }
}

// Recursively replace each item's label with its .po translation (commands by id, popups by
// English text; a "\t<shortcut>" tail is preserved). Used for both the right-click popup and the
// menu-bar preview so both render the real translated menu.
void MainFrame::SubstituteMenuInPlace(HMENU m) {
    int n = ::GetMenuItemCount(m);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW info{ sizeof(info) };
        info.fMask = MIIM_SUBMENU | MIIM_ID | MIIM_FTYPE;
        ::GetMenuItemInfoW(m, i, TRUE, &info);
        if (info.fType & MFT_SEPARATOR) continue;
        wchar_t buf[512] = {};
        ::GetMenuStringW(m, i, buf, 512, MF_BYPOSITION);
        std::wstring cur = buf;
        std::string english = CW2A(cur.c_str(), CP_UTF8), ctx, msgid, msgstr;
        if (info.hSubMenu) {
            if (auto* r = m_index.menu_popup(english)) { ctx = r->msgctxt; msgid = r->msgid; }
            SubstituteMenuInPlace(info.hSubMenu);
        } else if (auto* r = m_index.menu_command(info.wID)) { ctx = r->msgctxt; msgid = r->msgid; }
        if (!ctx.empty() || !msgid.empty())
            if (auto* e = m_po[RES_MENUS].find(ctx, msgid); e && !e->msgstr.empty()) msgstr = e->msgstr;
        if (msgstr.empty()) continue;
        std::wstring label = (LPCWSTR)CA2W(msgstr.c_str(), CP_UTF8);
        if (auto t = cur.find(L'\t'); t != std::wstring::npos) label += cur.substr(t);
        MENUITEMINFOW set{ sizeof(set) };
        set.fMask = MIIM_STRING; set.dwTypeData = label.data();
        ::SetMenuItemInfoW(m, i, TRUE, &set);
    }
}

void MainFrame::BuildMenuTree() {
    m_menuTree.DeleteAllItems();
    m_menuNodeKey.clear();
    m_menuNodeSub.clear();
    m_barMenuId = -1;                                // force the bar preview to rebuild (fresh edits)
    if (m_trackMenu) { ::DestroyMenu(m_trackMenu); m_trackMenu = nullptr; }
    if (m_curMenu >= 0 && m_checkedOut) {
        if (HMENU raw = m_preview.LoadRawMenu(m_curMenu)) {
            m_trackMenu = ::CreatePopupMenu();                       // translated copy, built by the walk below
            AddMenuNodes(menu_root(raw), TVI_ROOT, m_trackMenu);     // tree + popup from ONE resolution (descend the IDR_POPUP* wrapper)
            ::DestroyMenu(raw);                                      // the English source is no longer needed
            Theme::ThemeMenu(m_trackMenu, /*isMenubar=*/false);   // dark owner-drawn popup
            if (HTREEITEM root = m_menuTree.GetRootItem()) {
                for (HTREEITEM it = root; it; it = m_menuTree.GetNextSiblingItem(it))
                    m_menuTree.Expand(it, TVE_EXPAND);   // top level open; submenus expand on click
                m_menuTree.SelectSetFirstVisible(root);  // start scrolled at the top
            }
        }
    }
    Layout();                                        // (re)place the menu-bar strip for this menu
}

// The horizontal menu bar preview: a translated menu set on the owned popup window, positioned
// over the strip Layout reserved. Only meaningful for the main menu bar (IDR_MAINFRAME).
void MainFrame::UpdateMenuBarPreview() {
    if (!m_menuBar.GetSafeHwnd()) return;
    HWND bar = m_menuBar.GetSafeHwnd();
    if (m_menuBarRect.IsRectEmpty()) { m_menuBar.ShowWindow(SW_HIDE); m_barMenuId = -1; return; }
    if (m_barMenuId != m_curMenu) {                 // rebuild only when the menu content changed
        if (m_barMenu) { ::DestroyMenu(m_barMenu); m_barMenu = nullptr; }
        if (HMENU bm = m_preview.LoadRawMenu(m_curMenu)) {
            if (menu_root(bm) != bm) {               // IDR_POPUP/IDR_POPUPMAIN: a context menu, not a bar
                ::DestroyMenu(bm);
                m_menuBar.ShowWindow(SW_HIDE);
                m_barMenuId = m_curMenu;              // don't rebuild every call
                return;
            }
            SubstituteMenuInPlace(bm);
            Theme::ThemeMenu(bm, /*isMenubar=*/true);   // dark: MIM_BACKGROUND + owner-drawn items
            ::SetMenu(bar, bm);
            m_barMenu = bm;
        }
        m_barMenuId = m_curMenu;
        ::DrawMenuBar(bar);
    }
    // No bar menu to show -- a context menu (IDR_POPUP*, hidden above) or a failed load. Without this
    // the SetWindowPos below would re-show the empty strip on every later call (Layout/resize), since
    // those take the m_barMenuId == m_curMenu path and skip the block above.
    if (!m_barMenu) { m_menuBar.ShowWindow(SW_HIDE); return; }
    CRect r = m_menuBarRect; ClientToScreen(&r);     // owned popup uses screen coordinates
    m_menuBar.SetWindowPos(&CWnd::wndTop, r.left, r.top, r.Width(), r.Height(),
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// The bar's menu items are owner-drawn, so Windows sends WM_MEASUREITEM/WM_DRAWITEM to this owning
// window; route them to the shared dark-menu drawing. Native tracking (hover/click/keyboard) stays.
BEGIN_MESSAGE_MAP(CMenuBarWnd, CWnd)
    ON_WM_MEASUREITEM()
    ON_WM_DRAWITEM()
END_MESSAGE_MAP()
void CMenuBarWnd::OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT mis) {
    if (mis && mis->CtlType == ODT_MENU && Theme::MenuMeasureItem(mis)) return;
    CWnd::OnMeasureItem(nIDCtl, mis);
}
void CMenuBarWnd::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT dis) {
    if (dis && dis->CtlType == ODT_MENU && Theme::MenuDrawItem(dis)) return;
    CWnd::OnDrawItem(nIDCtl, dis);
}
BOOL CMenuBarWnd::OnEraseBkgnd(CDC* pDC) {
    CRect rc; GetClientRect(&rc); pDC->FillSolidRect(rc, Theme::WINDOW_BG); return TRUE;
}

BEGIN_MESSAGE_MAP(CThemedHostWnd, CWnd)
    ON_WM_ERASEBKGND()
    ON_WM_VSCROLL()
    ON_WM_HSCROLL()
    ON_WM_MOUSEWHEEL()
    ON_WM_SIZE()
END_MESSAGE_MAP()
void CThemedHostWnd::SetFrame(const CString& page, CSize contentPx) {
    if (m_framePage == page && m_frameContentPx == contentPx) return;
    m_framePage = page; m_frameContentPx = contentPx;
    if (GetSafeHwnd()) Invalidate();   // caller (RenderCurrentDialog) always follows with SyncScroll(),
                                       // which recomputes the child's geometry against the new frame
}
int CThemedHostWnd::CaptionHeight() const {
    if (m_framePage.IsEmpty()) return 0;
    CClientDC dc(const_cast<CThemedHostWnd*>(this));
    int dpi = dc.GetDeviceCaps(LOGPIXELSY);
    return ::MulDiv(21, dpi, 96);   // TreePropSheet's hardcoded 96-DPI caption height, DPI-scaled
}
CSize CThemedHostWnd::FrameOuterSize() const {
    return CSize(m_frameContentPx.cx + 2, m_frameContentPx.cy + 2 + CaptionHeight());
}
CRect CThemedHostWnd::FrameContentRect() const {
    CRect rc; GetClientRect(&rc);
    if (m_framePage.IsEmpty()) return rc;   // no frame -- full client rect, unchanged
    int capH = CaptionHeight();
    return CRect(1, 1 + capH, 1 + m_frameContentPx.cx, 1 + capH + m_frameContentPx.cy);
}
BOOL CThemedHostWnd::OnEraseBkgnd(CDC* pDC) {
    CRect rc; GetClientRect(&rc); pDC->FillSolidRect(rc, Theme::WINDOW_BG);
    if (m_framePage.IsEmpty()) return TRUE;   // no frame -- today's plain background, unchanged

    // The frame is now the previewed object (sized to the real page area -- see RenderCurrentDialog),
    // so it scrolls as ONE artifact: draw it offset by the current scroll position, exactly like the
    // child dialog is positioned in RecalcAndReposition, so border/caption/content move together.
    CRect frameRc(CPoint(-m_scrollX, -m_scrollY), FrameOuterSize());

    CBrush borderBr(Theme::GRID_LINE);
    pDC->FrameRect(frameRc, &borderBr);
    int capH = CaptionHeight();
    CRect capRect(frameRc.left + 1, frameRc.top + 1, frameRc.right - 1, frameRc.top + 1 + capH);

    // Gradient caption bar, matching the DARK-THEME page frame the player actually uses
    // (CMPCThemePropPageFrame::DrawCaption, not the default CPropPageFrameDefault): it fades from
    // ContentSelectedColor (left) to ContentBGColor (right) -- our Theme::CONTENT_SEL -> CONTENT_BG,
    // NOT the OS COLOR_ACTIVECAPTION (a light blue that clashes with the dark palette).
    // Dependency-free per-column interpolation (mirrors FillGradientRectH) -- no msimg32/GdiGradientFill.
    COLORREF clrLeft = Theme::CONTENT_SEL, clrRight = Theme::CONTENT_BG;
    int steps = max(1, capRect.Width());
    double dR = GetRValue(clrLeft), dG = GetGValue(clrLeft), dB = GetBValue(clrLeft);
    double stepR = (GetRValue(clrRight) - dR) / steps, stepG = (GetGValue(clrRight) - dG) / steps,
           stepB = (GetBValue(clrRight) - dB) / steps;
    for (int x = capRect.left; x < capRect.right; ++x) {
        pDC->FillSolidRect(x, capRect.top, 1, capRect.Height(), RGB((BYTE)dR, (BYTE)dG, (BYTE)dB));
        dR += stepR; dG += stepG; dB += stepB;
    }

    // Caption text, as CMPCThemePropPageFrame::DrawCaption: themed caption FG (PropPageCaptionFGColor
    // ~= Theme::TEXT), transparent bkmode, bold message font shrunk to fit the caption height (the same
    // shrink loop), baseline nudged up by the descent, left-aligned with ellipsis. No DT_NOPREFIX: the
    // player doesn't pass it either, so an '&' underlines the next character as a mnemonic.
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = (long)(-.8f * capH);
    lf.lfWeight = FW_BOLD;
    CFont f; f.CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = pDC->SelectObject(&f);
    TEXTMETRICW tm{}; pDC->GetTextMetrics(&tm);
    while (tm.tmHeight > capH && abs(lf.lfHeight) > 10) {
        pDC->SelectObject(oldFont);
        f.DeleteObject();
        lf.lfHeight++;
        f.CreateFontIndirectW(&lf);
        pDC->SelectObject(&f);
        pDC->GetTextMetrics(&tm);
    }
    CRect textRect(capRect.left + 2, capRect.top, capRect.right, capRect.bottom);
    textRect.top -= tm.tmDescent - 1;
    pDC->SetTextColor(Theme::TEXT);
    pDC->SetBkMode(TRANSPARENT);
    pDC->DrawText(m_framePage, textRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    pDC->SelectObject(oldFont);
    return TRUE;
}
// Recompute the scroll range/scrollbars, then reposition the child by MOVING it (SWP_NOSIZE -- it is
// never resized) to the content rect's top-left minus the scroll offset. The scrolled ARTIFACT differs
// by mode: no frame -- the child's own natural (template) size against the host's full client rect
// (byte-for-byte today's behavior); framed -- the FRAME's outer size (border + caption + content, the
// real page area) against the client rect, so caption/border/content scroll together as one object (see
// OnEraseBkgnd). SIF_DISABLENOSCROLL deliberately omitted so Windows auto-hides a bar once its page
// covers the full range -- "no scrollbar when it fits", for free.
void CThemedHostWnd::RecalcAndReposition() {
    HWND child = ::GetWindow(m_hWnd, GW_CHILD);
    if (!child) { ShowScrollBar(SB_BOTH, FALSE); return; }
    int artW, artH;
    if (m_framePage.IsEmpty()) {
        RECT wr; ::GetWindowRect(child, &wr);
        artW = wr.right - wr.left; artH = wr.bottom - wr.top;
    } else {
        CSize outer = FrameOuterSize();
        artW = outer.cx; artH = outer.cy;
    }
    CRect client; GetClientRect(&client);
    CRect cc = FrameContentRect();
    int maxX = max(0, artW - client.Width()), maxY = max(0, artH - client.Height());
    m_scrollX = max(0, min(m_scrollX, maxX));
    m_scrollY = max(0, min(m_scrollY, maxY));
    SCROLLINFO si{ sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin = 0; si.nMax = artH > 0 ? artH - 1 : 0; si.nPage = client.Height(); si.nPos = m_scrollY;
    SetScrollInfo(SB_VERT, &si, TRUE);
    si.nMax = artW > 0 ? artW - 1 : 0; si.nPage = client.Width(); si.nPos = m_scrollX;
    SetScrollInfo(SB_HORZ, &si, TRUE);
    ::SetWindowPos(child, nullptr, cc.left - m_scrollX, cc.top - m_scrollY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // The whole frame (border + caption bar) is chrome painted in OnEraseBkgnd at the scroll offset, so
    // it MOVES with a scroll -- moving only the child leaves the old chrome smeared behind (stale caption/
    // border, half-drawn controls). Repaint the chrome on any framed scroll. WS_CLIPCHILDREN keeps the
    // erase off the child, so this only redraws the (thin) chrome, not the dialog -- no child flicker.
    if (!m_framePage.IsEmpty()) Invalidate(TRUE);
    if (OnScrolled) OnScrolled();
}
void CThemedHostWnd::SyncScroll(bool resetPos) {
    if (resetPos) m_scrollX = m_scrollY = 0;
    RecalcAndReposition();
}
void CThemedHostWnd::OnSize(UINT nType, int cx, int cy) {
    CWnd::OnSize(nType, cx, cy);
    RecalcAndReposition();           // preserve scroll position across resizes
}
void CThemedHostWnd::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar) {
    switch (nSBCode) {
        case SB_LINEUP:   m_scrollY -= 24; break;
        case SB_LINEDOWN: m_scrollY += 24; break;
        case SB_PAGEUP:   { CRect cc; GetClientRect(&cc); m_scrollY -= cc.Height(); break; }
        case SB_PAGEDOWN: { CRect cc; GetClientRect(&cc); m_scrollY += cc.Height(); break; }
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO si{ sizeof(SCROLLINFO), SIF_TRACKPOS };
            GetScrollInfo(SB_VERT, &si, SIF_TRACKPOS);   // 32-bit pos -- nPos param would truncate on large ranges
            m_scrollY = si.nTrackPos;
            break;
        }
        case SB_TOP:    m_scrollY = 0; break;
        case SB_BOTTOM: m_scrollY = 0x3FFFFFFF; break;   // clamped to maxY below
    }
    RecalcAndReposition();   // clamps m_scrollY into range
}
void CThemedHostWnd::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar) {
    switch (nSBCode) {
        case SB_LINEUP:   m_scrollX -= 24; break;
        case SB_LINEDOWN: m_scrollX += 24; break;
        case SB_PAGEUP:   { CRect cc; GetClientRect(&cc); m_scrollX -= cc.Width(); break; }
        case SB_PAGEDOWN: { CRect cc; GetClientRect(&cc); m_scrollX += cc.Width(); break; }
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO si{ sizeof(SCROLLINFO), SIF_TRACKPOS };
            GetScrollInfo(SB_HORZ, &si, SIF_TRACKPOS);   // 32-bit pos -- nPos param would truncate on large ranges
            m_scrollX = si.nTrackPos;
            break;
        }
        case SB_TOP:    m_scrollX = 0; break;
        case SB_BOTTOM: m_scrollX = 0x3FFFFFFF; break;   // clamped to maxX below
    }
    RecalcAndReposition();   // clamps m_scrollX into range
}
BOOL CThemedHostWnd::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt) {
    m_scrollY -= (zDelta / WHEEL_DELTA) * (3 * 24);
    RecalcAndReposition();
    return TRUE;
}

// ---- CMockupWnd: owner-drawn context mock-ups for loose strings ----
BEGIN_MESSAGE_MAP(CMockupWnd, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()
BOOL CMockupWnd::OnEraseBkgnd(CDC*) { return TRUE; }   // OnPaint fully repaints (double-buffered)

void CMockupWnd::OnPaint() {
    CPaintDC pdc(this);
    CRect rc; GetClientRect(&rc);
    CDC dc; dc.CreateCompatibleDC(&pdc);
    CBitmap bmp; bmp.CreateCompatibleBitmap(&pdc, rc.Width(), rc.Height());
    CBitmap* ob = dc.SelectObject(&bmp);
    // MPC-HC renders these in the system message font (Segoe UI) — match it, like the dialog preview.
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    CFont base; base.CreateFontIndirectW(&ncm.lfMessageFont);
    HGDIOBJ of = dc.SelectObject(&base);
    dc.SetBkMode(TRANSPARENT);
    switch (m_mode) {
        case Mode::MsgBox:    drawMsgBox(dc, rc);    break;
        case Mode::OSD:       drawOSD(dc, rc, ncm.lfMessageFont); break;
        case Mode::StatusBar: drawStatusBar(dc, rc); break;
        case Mode::Tooltip:   drawTooltip(dc, rc);   break;
        case Mode::Menu:      drawMenu(dc, rc);      break;
        case Mode::Generic:   drawGeneric(dc, rc);   break;
        case Mode::SynthDialog: drawAudioRenderer(dc, rc); break;
        default:              dc.FillSolidRect(rc, Theme::WINDOW_BG); break;
    }
    dc.SelectObject(of);
    pdc.BitBlt(0, 0, rc.Width(), rc.Height(), &dc, 0, 0, SRCCOPY);
    dc.SelectObject(ob);
}

// A message box: a floating dark dialog with a title bar, an error/info icon, the text, and OK.
void CMockupWnd::drawMsgBox(CDC& dc, CRect area) {
    dc.FillSolidRect(area, Theme::WINDOW_BG);
    const int pad = S(16), titleH = S(30), btnH = S(28), btnW = S(84), iconSz = S(32);
    int boxW = min(S(460), area.Width() - S(60));
    CRect meas(0, 0, boxW - pad * 3 - iconSz, S(600));
    dc.DrawText(m_text, &meas, DT_CALCRECT | DT_WORDBREAK | prefixFlag());
    int textH = max(iconSz, meas.Height());
    int boxH = titleH + pad + textH + pad + btnH + pad;
    CRect box((area.Width() - boxW) / 2, (area.Height() - boxH) / 2, 0, 0);
    box.right = box.left + boxW; box.bottom = box.top + boxH;

    dc.FillSolidRect(box, Theme::CONTENT_BG);
    { CBrush b(Theme::CTRL_BORDER); dc.FrameRect(box, &b); }
    CRect tb(box.left + 1, box.top + 1, box.right - 1, box.top + titleH);   // title bar
    dc.FillSolidRect(tb, Theme::MENU_BG);
    dc.SetTextColor(Theme::TEXT);
    CRect ttl = tb; ttl.left += pad;
    dc.DrawText(L"MPC-HC", &ttl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    CRect ic(box.left + pad, tb.bottom + pad, box.left + pad + iconSz, tb.bottom + pad + iconSz);  // icon
    COLORREF iclr = m_error ? RGB(226, 84, 72) : RGB(70, 140, 220);
    { CBrush fill(iclr); CPen pen(PS_SOLID, 1, iclr);
      HGDIOBJ op = dc.SelectObject(&pen), obr = dc.SelectObject(&fill); dc.Ellipse(ic);
      dc.SelectObject(op); dc.SelectObject(obr); }
    dc.SetTextColor(RGB(255, 255, 255));
    dc.DrawText(m_error ? L"!" : L"i", &ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    CRect txt(ic.right + pad, tb.bottom + pad, box.right - pad, tb.bottom + pad + textH);          // text
    dc.SetTextColor(Theme::TEXT);
    dc.DrawText(m_text, &txt, DT_LEFT | DT_TOP | DT_WORDBREAK | prefixFlag());

    CRect btn(box.right - pad - btnW, box.bottom - pad - btnH, box.right - pad, box.bottom - pad);  // OK
    dc.FillSolidRect(btn, Theme::BTN_FILL);
    { CBrush b(Theme::BTN_INNER); dc.FrameRect(btn, &b); }
    dc.SetTextColor(Theme::TEXT);
    dc.DrawText(L"OK", &btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// An OSD overlay: MPC's translucent-black text pill at the top-left of a mock video area. The OSD
// text is more prominent than dialog text, so scale the message font up.
void CMockupWnd::drawOSD(CDC& dc, CRect area, const LOGFONTW& baseLf) {
    dc.FillSolidRect(area, RGB(18, 18, 18));
    LOGFONTW lf = baseLf; lf.lfHeight = (LONG)(lf.lfHeight * 1.4);
    CFont osdFont; osdFont.CreateFontIndirectW(&lf);
    HGDIOBJ of = dc.SelectObject(&osdFont);
    const int pad = S(10), inset = S(18);
    CRect meas(0, 0, area.Width() - inset * 2 - pad * 2, S(400));
    dc.DrawText(m_text, &meas, DT_CALCRECT | DT_WORDBREAK | prefixFlag());
    CRect pill(area.left + inset, area.top + inset,
               area.left + inset + meas.Width() + pad * 2, area.top + inset + meas.Height() + pad * 2);
    { CBrush fill(RGB(0, 0, 0)); CPen pen(PS_SOLID, 1, RGB(64, 64, 64));
      HGDIOBJ op = dc.SelectObject(&pen), obr = dc.SelectObject(&fill);
      dc.RoundRect(pill, CPoint(S(6), S(6)));
      dc.SelectObject(op); dc.SelectObject(obr); }
    CRect tr(pill.left + pad, pill.top + pad, pill.right - pad, pill.bottom - pad);
    dc.SetTextColor(RGB(255, 255, 255));
    dc.DrawText(m_text, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | prefixFlag());
    dc.SelectObject(of);
}

// A status-bar strip along the bottom of a mock video area.
void CMockupWnd::drawStatusBar(CDC& dc, CRect area) {
    dc.FillSolidRect(area, RGB(18, 18, 18));
    const int h = S(26);
    CRect sb(area.left, area.bottom - h, area.right, area.bottom);
    dc.FillSolidRect(sb, Theme::MENU_BG);
    dc.FillSolidRect(CRect(sb.left, sb.top, sb.right, sb.top + 1), Theme::CTRL_BORDER);   // top edge
    CRect tr = sb; tr.left += S(10); tr.right -= S(60);
    dc.SetTextColor(Theme::TEXT);
    dc.DrawText(m_text, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | prefixFlag());
    CRect clock(sb.right - S(56), sb.top, sb.right - S(8), sb.bottom);                    // mock clock
    dc.SetTextColor(Theme::TEXT_DIM);
    dc.DrawText(L"00:00", &clock, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

// A tooltip bubble hovering below a mock control.
void CMockupWnd::drawTooltip(CDC& dc, CRect area) {
    dc.FillSolidRect(area, Theme::WINDOW_BG);
    const int pad = S(8), chk = S(16);
    CRect ctl(area.left + S(40), area.top + S(40), 0, 0);                    // mock checkbox + label
    ctl.right = ctl.left + chk; ctl.bottom = ctl.top + chk;
    dc.FillSolidRect(ctl, Theme::CONTENT_BG);
    { CBrush b(Theme::CTRL_BORDER); dc.FrameRect(ctl, &b); }
    CRect lbl(ctl.right + S(6), ctl.top - S(1), area.right - S(20), ctl.bottom + S(2));
    dc.SetTextColor(Theme::TEXT_DIM);
    dc.DrawText(m_caption.IsEmpty() ? CString(L"(hovered control)") : m_caption,
                &lbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    CRect meas(0, 0, min(S(380), area.Width() - S(120)), S(300));           // bubble
    dc.DrawText(m_text, &meas, DT_CALCRECT | DT_WORDBREAK | prefixFlag());
    CRect tip(ctl.left, ctl.bottom + S(10), 0, 0);
    tip.right = tip.left + meas.Width() + pad * 2; tip.bottom = tip.top + meas.Height() + pad * 2;
    dc.FillSolidRect(tip, Theme::MENU_BG);
    { CBrush b(Theme::CTRL_BORDER); dc.FrameRect(tip, &b); }
    CRect tr(tip.left + pad, tip.top + pad, tip.right - pad, tip.bottom - pad);
    dc.SetTextColor(Theme::TEXT);
    dc.DrawText(m_text, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | prefixFlag());
}

// The real menu: measure + draw each item of m_menu with the exact native owner-draw (the same
// Theme::MenuMeasureItem / MenuDrawItem MPC-HC uses for live menus), stacked as a static popup, with
// the string's item highlighted. Shown flat so no clicking is needed to see it in context.
void CMockupWnd::drawMenu(CDC& dc, CRect area) {
    dc.FillSolidRect(area, Theme::WINDOW_BG);
    if (!m_menu || !::IsMenu(m_menu)) return;
    int n = ::GetMenuItemCount(m_menu);
    std::vector<int> h(n); std::vector<UINT> ids(n); std::vector<ULONG_PTR> data(n);
    int menuW = 0, totH = 0;
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW q{ sizeof(q) }; q.fMask = MIIM_DATA | MIIM_ID; ::GetMenuItemInfoW(m_menu, i, TRUE, &q);
        ids[i] = q.wID; data[i] = q.dwItemData;
        MEASUREITEMSTRUCT mis{}; mis.CtlType = ODT_MENU; mis.itemID = q.wID; mis.itemData = q.dwItemData;
        Theme::MenuMeasureItem(&mis);
        h[i] = mis.itemHeight; menuW = max(menuW, (int)mis.itemWidth); totH += mis.itemHeight;
    }
    CRect box(area.left + S(30), area.top + S(20), area.left + S(30) + menuW, area.top + S(20) + totH);
    int yy = box.top;
    for (int i = 0; i < n; ++i) {
        DRAWITEMSTRUCT dis{}; dis.CtlType = ODT_MENU; dis.itemID = ids[i]; dis.itemData = data[i];
        dis.hDC = dc.m_hDC; dis.hwndItem = (HWND)m_menu; dis.itemAction = ODA_DRAWENTIRE;
        if (i == m_menuSelIndex) dis.itemState = ODS_SELECTED;
        dis.rcItem = CRect(box.left, yy, box.right, yy + h[i]);
        Theme::MenuDrawItem(&dis);
        yy += h[i];
    }
    HBRUSH b = ::CreateSolidBrush(Theme::MENU_BORDER); ::FrameRect(dc.m_hDC, box, b); ::DeleteObject(b);
}

// Catch-all for a loose string with no distinct placement (e.g. a dynamically-listed label): the
// text in a framed panel, with its ui_location as a caption so the translator still has the context.
void CMockupWnd::drawGeneric(CDC& dc, CRect area) {
    dc.FillSolidRect(area, Theme::WINDOW_BG);
    const int pad = S(14);
    int w = min(S(520), area.Width() - S(60));
    CRect cap(area.left + (area.Width() - w) / 2, area.top + S(60), area.left + (area.Width() + w) / 2, 0);
    cap.bottom = cap.top + S(20);
    dc.SetTextColor(Theme::TEXT_DIM);
    dc.DrawText(m_caption.IsEmpty() ? CString(L"Appears in the app as:") : (L"Appears in: " + m_caption),
                &cap, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    CRect meas(0, 0, w - pad * 2, S(600));
    dc.DrawText(m_text, &meas, DT_CALCRECT | DT_WORDBREAK | prefixFlag());
    CRect box(cap.left, cap.bottom + S(6), cap.right, cap.bottom + S(6) + meas.Height() + pad * 2);
    dc.FillSolidRect(box, Theme::CONTENT_BG);
    { CBrush b(Theme::CTRL_BORDER); dc.FrameRect(box, &b); }
    CRect tr(box.left + pad, box.top + pad, box.right - pad, box.bottom - pad);
    dc.SetTextColor(Theme::TEXT);
    dc.DrawText(m_text, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | prefixFlag());
}

// A synthesized dialog MPC-HC builds in code (no RC template) — the MPC Audio Renderer Settings dialog.
// Draws a titled, tabbed box and the active page's controls (m_rows), highlighting the selected one and
// hovering m_tip below it. Reproduces the dialog's shape closely enough to place each string in context.
void CMockupWnd::drawAudioRenderer(CDC& dc, CRect area) {
    dc.FillSolidRect(area, Theme::WINDOW_BG);
    // Dimensions mirror MpcAudioRendererSettingsWnd.cpp exactly: font-height controls, a 10px page inset,
    // 320px content width, inline combos 200px in / 120px wide, and h20/h25/h35 vertical steps.
    TEXTMETRICW tm{}; dc.GetTextMetrics(&tm);
    const int fh = tm.tmHeight, cbh = fh + S(6), rowH = S(20);
    int W = min(S(345), area.Width() - S(40));
    int L = area.left + (area.Width() - W) / 2, T = area.top + S(14), R = L + W;
    const int ind = L + S(10), innerR = R - S(10);       // content column (source x = 10 .. 330)
    auto stepOf = [&](SynthRow::Kind k) { return k == SynthRow::ComboFull ? S(20) + S(35)
                                               : k == SynthRow::ComboInline ? S(25) : S(20); };
    auto drawCombo = [&](CRect cb) {                      // a themed dropdown box with the arrow
        dc.FillSolidRect(cb, Theme::CONTENT_BG);
        { CBrush b(Theme::CTRL_BORDER); dc.FrameRect(cb, &b); }
        int a2 = S(4); CPoint a(cb.right - S(12), cb.top + (cb.Height() - a2) / 2);
        POINT tri[3] = { {a.x, a.y}, {a.x + a2 * 2, a.y}, {a.x + a2, a.y + a2} };
        CBrush ab(Theme::MENU_ARROW); HGDIOBJ ob = dc.SelectObject(&ab);
        dc.SetPolyFillMode(WINDING); dc.Polygon(tri, 3); dc.SelectObject(ob);
    };

    dc.SetTextColor(Theme::TEXT);
    CRect tcap(L, T, R, T + rowH);
    dc.DrawText(m_title, &tcap, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    int y = T + rowH + S(2);

    if (!m_tabs.empty()) {                                // tab strip
        int tx = L;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            CRect meas(0, 0, S(400), rowH); dc.DrawText(m_tabs[i], &meas, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            CRect tab(tx, y, tx + meas.Width() + S(24), y + rowH + S(4));
            dc.FillSolidRect(tab, i == m_activeTab ? Theme::WINDOW_BG : Theme::TAB_INACTIVE);
            { CBrush b(Theme::TAB_BORDER); dc.FrameRect(tab, &b); }
            dc.SetTextColor(i == m_activeTab ? Theme::TEXT : Theme::TEXT_DIM);
            dc.DrawText(m_tabs[i], &tab, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            tx = tab.right - S(1);
        }
        y += rowH + S(4);
    }

    int rowsH = 0; for (const auto& r : m_rows) rowsH += stepOf(r.kind);   // body sized to content
    CRect body(L, y, R, min((long)(y + S(10) + rowsH + S(10)), (long)(area.bottom - S(12))));
    dc.FillSolidRect(body, Theme::WINDOW_BG);
    { CBrush b(Theme::TAB_BORDER); dc.FrameRect(body, &b); }
    y += S(10);
    int selBottom = -1; CRect selRect;

    for (const auto& r : m_rows) {
        int rh = stepOf(r.kind);
        CRect row(L + S(4), y, R - S(4), y + rh);
        if (r.selected) { selRect = row; selBottom = row.bottom; }   // ringed in red after the loop
        dc.SetTextColor(Theme::TEXT);
        switch (r.kind) {
            case SynthRow::Check: {          // BS_LEFTTEXT: text left, box on the right edge
                int bs = min(fh, S(15));
                CRect bx(innerR - bs, y + (fh - bs) / 2, innerR, y + (fh - bs) / 2 + bs);
                dc.FillSolidRect(bx, Theme::CHK_BG);
                { CBrush b(Theme::CHK_BORDER); dc.FrameRect(bx, &b); }
                CRect tx2(ind, y, bx.left - S(6), y + fh);
                dc.DrawText(r.text, &tx2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                break;
            }
            case SynthRow::ComboInline: {    // label (200) + combo (120) on the right
                CRect lab(ind, y, ind + S(200), y + fh);
                dc.DrawText(r.text, &lab, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                drawCombo(CRect(ind + S(200), y, innerR, y + cbh));
                break;
            }
            case SynthRow::ComboFull: {      // label, then a full-width combo below it
                CRect lab(ind, y, innerR, y + fh);
                dc.DrawText(r.text, &lab, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                drawCombo(CRect(ind, y + S(20), innerR, y + S(20) + cbh));
                break;
            }
            case SynthRow::Group: {          // status-page section header + underline
                CRect line(ind, y, innerR, y + fh);
                dc.DrawText(r.text, &line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                dc.FillSolidRect(CRect(ind, y + fh, innerR, y + fh + S(1)), Theme::GROUP_BORDER);
                break;
            }
            case SynthRow::Value: {          // "Label:"  <value>
                CRect lab(ind + S(8), y, ind + S(8) + S(90), y + fh);
                dc.DrawText(r.text, &lab, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                CRect val(lab.right + S(6), y, innerR, y + fh);
                dc.SetTextColor(Theme::TEXT_DIM);
                dc.DrawText(r.value.IsEmpty() ? CString(L"—") : r.value, &val,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                break;
            }
            case SynthRow::Button: {         // source: Reset at x+240, 80 wide
                CRect bn(ind + S(240), y, min((long)(ind + S(240) + S(80)), (long)innerR), y + cbh + S(4));
                dc.FillSolidRect(bn, Theme::BTN_FILL);
                { CBrush b(Theme::BTN_OUTER); dc.FrameRect(bn, &b); }
                dc.SetTextColor(Theme::TEXT);
                dc.DrawText(r.text, &bn, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                break;
            }
            default: {
                CRect line(ind, y, innerR, y + fh);
                dc.DrawText(r.text, &line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        y += rh;
    }

    // The red locator ring around the selected control's row — the same marker used on real dialog
    // controls (not a list-selection band, which read as a listview highlight here).
    if (selBottom >= 0) {
        CBrush rb(RGB(230, 40, 40));
        dc.FrameRect(selRect, &rb);
    }

    // Tooltip bubble below the selected control.
    if (!m_tip.IsEmpty() && selBottom >= 0) {
        const int tpad = S(8);
        CRect meas(0, 0, min(S(300), W - tpad), S(400));
        dc.DrawText(m_tip, &meas, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        CRect bub(selRect.left + S(16), selBottom + S(2),
                  selRect.left + S(16) + meas.Width() + tpad * 2, selBottom + S(2) + meas.Height() + tpad);
        if (bub.right > R) bub.OffsetRect(R - bub.right, 0);
        dc.FillSolidRect(bub, Theme::MENU_BG);
        { CBrush b(Theme::CTRL_BORDER); dc.FrameRect(bub, &b); }
        dc.SetTextColor(Theme::TEXT);
        CRect tb(bub.left + tpad, bub.top + tpad / 2, bub.right - tpad, bub.bottom - tpad / 2);
        dc.DrawText(m_tip, &tb, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
}

// Switch the whole app to a new theme live: palette + title bar, re-theme every control, rebuild the
// (owner-drawn) menus, re-render the preview, then repaint.
void MainFrame::ApplyThemeChange(Theme::Mode m) {
    Theme::SetMode(m);
    Theme::ApplyTitleBar(m_hWnd);
    Theme::ApplyToChildren(GetSafeHwnd());          // re-applies SetWindowTheme + list/tree colors
    ::SetWindowTheme(m_previewHost.GetSafeHwnd(),    // custom class: ApplyToChildren skips it -> theme its scrollbars
                     Theme::IsDark() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    m_progress.SetBkColor(Theme::WINDOW_BG);
    ReloadFrameMenu();                              // File/View menu: owner-drawn or native per mode
    m_barMenuId = -1;                               // force the bar-preview menu to rebuild
    if (m_tabs.GetCurSel() == RES_MENUS) BuildMenuTree();       // re-theme the track popup
    if (m_tabs.GetCurSel() == RES_DIALOGS) RenderCurrentDialog();  // re-render dialog in new colors
    if (AfxGetApp()) AfxGetApp()->WriteProfileInt(L"theme", L"mode", (int)m);
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    UpdateMenuBarPreview();
}

void MainFrame::ReloadFrameMenu() {
    // Reload from the resource so any previous owner-draw flags are cleared, then re-theme with the
    // active palette (so a Dark<->Light switch recolors the File/View menu too).
    HMENU oldM = ::GetMenu(m_hWnd);
    HMENU newM = ::LoadMenu(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_MAINFRAME));
    if (!newM) return;
    Theme::ThemeMenu(newM, /*isMenubar=*/true);
    ::SetMenu(m_hWnd, newM);
    ::DrawMenuBar(m_hWnd);
    if (oldM) ::DestroyMenu(oldM);
}

static Theme::Mode ThemeModeFor(UINT id) {
    return id == ID_VIEW_THEME_LIGHT  ? Theme::Mode::Light
         : id == ID_VIEW_THEME_SYSTEM ? Theme::Mode::System
                                      : Theme::Mode::Dark;
}
void MainFrame::OnViewTheme(UINT id) {
    Theme::Mode m = ThemeModeFor(id);
    if (m != Theme::Preference()) ApplyThemeChange(m);
}
// In Follow-Windows mode, re-resolve when the OS flips its light/dark app theme.
void MainFrame::OnSettingChange(UINT uFlags, LPCTSTR lpszSection) {
    CFrameWnd::OnSettingChange(uFlags, lpszSection);
    if (Theme::Preference() == Theme::Mode::System && lpszSection &&
        _wcsicmp(lpszSection, L"ImmersiveColorSet") == 0) {
        bool wasDark = Theme::IsDark();
        Theme::SetMode(Theme::Mode::System);
        if (Theme::IsDark() != wasDark) ApplyThemeChange(Theme::Mode::System);
    }
}
void MainFrame::OnUpdateViewTheme(CCmdUI* pCmdUI) {
    pCmdUI->SetRadio(ThemeModeFor(pCmdUI->m_nID) == Theme::Preference());
}
// Windows repaints the light menu-bar seam during NC painting; cover it right afterwards.
void MainFrame::OnNcPaint() {
    Default();
    Theme::PaintMenuBarBottomLine(m_hWnd);
}
BOOL MainFrame::OnNcActivate(BOOL bActive) {
    BOOL r = CFrameWnd::OnNcActivate(bActive);
    Theme::PaintMenuBarBottomLine(m_hWnd);
    return r;
}

void MainFrame::OnMenuTreeSelChanged(NMHDR* hdr, LRESULT* res) {
    *res = 0;
    HTREEITEM it = ((NMTREEVIEW*)hdr)->itemNew.hItem;
    auto k = m_menuNodeKey.find(it);
    if (k == m_menuNodeKey.end()) return;
    for (int i = 0; i < (int)m_rows.size(); ++i)
        if (m_rows[i].msgctxt == k->second.first && m_rows[i].msgid == k->second.second) {
            m_list.SetItemState(i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            m_list.EnsureVisible(i, FALSE);
            break;
        }
}

// Right-click renders the REAL translated menu natively (TrackPopupMenu) — the actual popup/
// menu-bar look. Right-clicking a popup node shows THAT submenu; elsewhere shows the whole menu.
// TPM_RETURNCMD keeps it non-committal (we don't dispatch the command). SetForegroundWindow +
// the trailing WM_NULL are the standard fix so the popup doesn't dismiss itself immediately.
void MainFrame::OnMenuTreeRClick(NMHDR*, LRESULT* res) {
    *res = 0;
    if (!m_trackMenu) return;
    POINT pt; ::GetCursorPos(&pt);
    CPoint cpt(pt); m_menuTree.ScreenToClient(&cpt);
    UINT flags = 0;
    HMENU show = m_trackMenu;                        // already descended + translated (see BuildMenuTree)
    if (HTREEITEM it = m_menuTree.HitTest(cpt, &flags)) {
        auto s = m_menuNodeSub.find(it);
        if (s != m_menuNodeSub.end()) show = s->second;
    }
    ::SetForegroundWindow(m_hWnd);
    ::TrackPopupMenu(show, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
                     pt.x, pt.y, 0, m_hWnd, nullptr);
    ::PostMessage(m_hWnd, WM_NULL, 0, 0);
}

// "Preview" button -- same real translated menu as OnMenuTreeRClick (m_trackMenu is already the
// descended + translated copy built by BuildMenuTree), dropped under the button instead of the cursor.
void MainFrame::OnMenuPreviewClicked() {
    if (!m_trackMenu) return;
    CRect r; m_btnMenuPreview.GetWindowRect(&r);
    ::SetForegroundWindow(m_hWnd);
    ::TrackPopupMenu(m_trackMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
                     r.left, r.bottom, 0, m_hWnd, nullptr);
    ::PostMessage(m_hWnd, WM_NULL, 0, 0);   // standard fix so it doesn't self-dismiss
}

void MainFrame::OnTabChanged(NMHDR*, LRESULT* res) {
    *res = 0;
    // Batch the swap: showing a pane then filling it lets the empty pane repaint first (a blank
    // flash). Lock the window tree, rebuild, then repaint the whole thing once, atomically.
    ::LockWindowUpdate(m_hWnd);
    HideCommandHelp(); HideMockup();                 // both are per-selection, not per-tab
    int tab = m_tabs.GetCurSel();
    if (tab == RES_DIALOGS)     PopulateDialogCombo();
    else if (tab == RES_MENUS)  PopulateMenuCombo();
    Layout();
    PopulateList();
    if (tab == RES_DIALOGS)     RenderCurrentDialog();
    else if (tab == RES_MENUS)  BuildMenuTree();
    else                      { m_preview.DestroyPreview(); m_previewHost.SetFrame(L"", CSize()); }
    if (tab == RES_REVIEW) EnsureFitScan();
    m_edit.ClearString();
    m_btnDismiss.EnableWindow(FALSE);   // SelectRow re-enables it when appropriate for the Review tab
    if (m_research.GetSafeHwnd()) m_research.SetWindowText(L"");   // per-selection; clear on tab switch
    ::LockWindowUpdate(nullptr);
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// Dark rows (MPC-style): the DarkMode visual style paints its own accent selection over any color we
// hand back, so — like CMPCThemePlayerListCtrl — we draw the row ourselves (fill + each column's
// text) and skip the default. CONTENT_SEL for the selected row, CONTENT_BG otherwise, white text.
void MainFrame::OnListCustomDraw(NMHDR* hdr, LRESULT* res) {
    auto* cd = (NMLVCUSTOMDRAW*)hdr;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) { *res = CDRF_NOTIFYITEMDRAW; return; }
    if (cd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) { *res = CDRF_DODEFAULT; return; }

    int item = (int)cd->nmcd.dwItemSpec;
    const Row& r = m_rows[item];
    bool sel = (m_list.GetItemState(item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    bool edited = IsEdited(r.msgctxt, r.msgid);
    CDC* pDC = CDC::FromHandle(cd->nmcd.hdc);
    CRect row(cd->nmcd.rc);
    CRect client; m_list.GetClientRect(&client); row.right = client.right;   // fill the full row width
    pDC->FillSolidRect(row, sel ? Theme::CONTENT_SEL : Theme::CONTENT_BG);
    // a green left bar marks a pending (edited, unsubmitted) row
    if (edited) pDC->FillSolidRect(CRect(row.left, row.top, row.left + S(4), row.bottom), RGB(78, 201, 120));
    pDC->SetBkMode(TRANSPARENT);
    pDC->SetTextColor(sel ? RGB(255, 255, 255) : Theme::TEXT);   // white on the selection in both modes
    // column x-extents come from the header (they track horizontal scroll with the list)
    if (CHeaderCtrl* hc = m_list.GetHeaderCtrl()) {
        for (int c = 0, cols = hc->GetItemCount(); c < cols; ++c) {
            CRect hrc; hc->GetItemRect(c, &hrc);
            CRect cr(hrc.left, row.top, hrc.right, row.bottom);
            cr.DeflateRect(S(6), 0, S(4), 0);
            pDC->DrawText(RowText(item, c), cr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    *res = CDRF_SKIPDEFAULT;
}

void MainFrame::OnStringSelected(NMHDR* hdr, LRESULT* res) {
    *res = 0;
    auto* lv = (NMLISTVIEW*)hdr;
    // never let a C++ exception unwind back through the COMCTL32 list-view callback
    if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
        try { SelectRow(lv->iItem); }
        catch (const std::exception& ex) {
            SetStatus(L"Selection error: " + CString(CA2W(ex.what(), CP_UTF8)));
        }
    }
}

// M3: resolve ALL stored rows for a cell into the newest primary + its vote sibling (if any) + the
// classified confidence -- the single source of truth SelectRow's display reads from, so "high" vs
// "low" can never contradict mpctrans::confidence::classify's own rule.
std::optional<MainFrame::StoredView> MainFrame::LoadStoredView(const std::string& lang,
                                                                const std::string& msgctxt,
                                                                const std::string& msgid) {
    std::vector<suggestion_store::Suggestion> rows = suggestion_store::get_all(lang, msgctxt, msgid);
    if (rows.empty()) return std::nullopt;
    StoredView sv;
    const suggestion_store::Suggestion* primary = nullptr;
    for (const auto& r : rows) if (r.role != "vote") { primary = &r; break; }
    sv.primary = primary ? *primary : rows.front();   // defensive: all-vote rows fall back to newest
    for (const auto& r : rows) if (r.role == "vote") { sv.vote = r; break; }
    sv.conf = confidence::classify({ sv.primary.agreement, sv.primary.back_translation_score,
                                     !(sv.primary.fits_px && *sv.primary.fits_px > 0),
                                     !sv.primary.flags.empty() });
    return sv;
}

void MainFrame::SelectRow(int item) {
    if (item < 0 || item >= (int)m_rows.size() || !m_checkedOut) return;
    m_curRow = item;
    const Row& row = m_rows[item];
    int tab = m_tabs.GetCurSel();
    m_curRowFlag = (tab == RES_REVIEW) ? row.flag : Row::Flag::None;
    m_edit.SetFlagNote(CString());   // cleared unconditionally; set again below only for Review rows
    m_btnDismiss.EnableWindow(tab == RES_REVIEW);
    const PoFile* po = PoFor(row.msgctxt, row.msgid);
    const PoEntry* e = po ? po->find(row.msgctxt, row.msgid) : nullptr;

    std::vector<AiSuggestion> ai;
    std::vector<ReferenceMatch> refs;
    const StringInfo* infoPtr = nullptr;
    StringInfo info;
    if (m_haveCore) {
        if (auto si = m_core.by_key(row.msgctxt, row.msgid)) {
            info = *si; infoPtr = &info;
            // apply this session's accepted "Suggest fix" corrections on top (see m_researchOverrides —
            // the export hasn't run yet, so the bundled sqlite still holds the pre-correction text).
            auto ovIt = m_researchOverrides.find({ row.msgctxt, row.msgid });
            if (ovIt != m_researchOverrides.end()) {
                if (ovIt->second.semantic_purpose) info.semantic_purpose = *ovIt->second.semantic_purpose;
                if (ovIt->second.functional_purpose) info.functional_purpose = *ovIt->second.functional_purpose;
            }
            if (m_havePack) {
                ai = m_pack.ai_for(info.id);
                refs = m_pack.refs_for(info.id);
            }
        }
    }
    m_curInfo = info;
    m_haveCurInfo = infoPtr != nullptr && (!info.semantic_purpose.empty() || !info.functional_purpose.empty() ||
                                           !info.category.empty() || !info.ui_location.empty() ||
                                           !info.ui_type.empty());
    // Translator hint layer (M4): resolved alongside m_curInfo above -- infoPtr is only non-null when
    // m_haveCore was true (see the m_core.by_key branch above), so this is safe without re-checking
    // m_haveCore. hint_for() itself degrades gracefully (nullopt) if this bundle has no `string_hints`
    // table or no row for this string.
    m_curHint = infoPtr ? m_core.hint_for(info.id) : std::nullopt;
    if (m_btnSuggestFix.GetSafeHwnd()) m_btnSuggestFix.EnableWindow(m_haveCurInfo);
    m_edit.ShowString(CString(CA2W(row.msgctxt.c_str(), CP_UTF8)),
                      CString(CA2W(row.msgid.c_str(), CP_UTF8)),
                      e ? CString(CA2W(e->msgstr.c_str(), CP_UTF8)) : CString(),
                      ai, refs, infoPtr);

    // M2: a persistent AI suggestion (from the bulk "Generate AI suggestions" run) shows for the
    // CURRENT cell only if it's still eligible -- untranslated OR validation-flagged (the suppression
    // rule from data/README.md's provenance model, applied here at READ time since the store
    // itself is a dumb point-lookup with no idea what a .po file or a validation finding is). Uses the
    // SAME CellFlag() check the M2 worklist/confirmation pass uses, so eligibility never drifts
    // between "what gets generated" and "when a stored suggestion is allowed to show". If nothing's
    // eligible or nothing's stored, leave the AI panel exactly as ShowString above already left it --
    // no regression to the existing on-demand-only behavior for an accepted, unflagged cell.
    {
        bool isEmptyCell = !e || e->msgstr.empty();
        CString evOut; long long fdOut = -1, fcOut = -1;
        Row::Flag cellFlag = isEmptyCell ? Row::Flag::None
                                         : CellFlag(row.msgctxt, row.msgid, row.res, e->msgstr, evOut, fdOut, fcOut);
        bool eligible = isEmptyCell || cellFlag != Row::Flag::None;
        if (eligible) {
            std::string langUtf8(CW2A(m_lang, CP_UTF8));
            if (auto sv = LoadStoredView(langUtf8, row.msgctxt, row.msgid)) {
                const auto& stored = sv->primary;
                CString storedText = CString(CA2W(stored.suggestion.c_str(), CP_UTF8));
                CString primaryModel = CString(stored.model.c_str());
                m_edit.SetAiSuggestion(storedText);
                m_edit.ShowAiSuggestionValidation(validate::check_entry(row.msgctxt, row.msgid, stored.suggestion));
                if (stored.fits_px && *stored.fits_px > 0) {
                    CString px; px.Format(L"Suggested text still overflows by %dpx", *stored.fits_px);
                    m_edit.AppendValidationLine(px);
                }
                CString altModel, altText;
                if (sv->vote) {
                    altModel = CString(sv->vote->model.c_str());
                    altText = CString(CA2W(sv->vote->suggestion.c_str(), CP_UTF8));
                }
                m_edit.SetAiDetails(primaryModel, storedText, altModel, altText,
                                    stored.agreement.has_value(), stored.agreement.value_or(0.0),
                                    stored.back_translation_score.has_value(),
                                    stored.back_translation_score.value_or(0.0),
                                    sv->conf == confidence::Confidence::Low);
                // Seed the session cache too, so a manual "Suggest with AI" click on this cell reuses
                // it instead of spending a fresh network call.
                CString msgctxtCs = CString(CA2W(row.msgctxt.c_str(), CP_UTF8));
                CString msgidCs = CString(CA2W(row.msgid.c_str(), CP_UTF8));
                m_aiSuggestCache[{ msgctxtCs, msgidCs, m_lang }] = storedText;
            }
        }
    }

    // On the Untranslated / All-strings / Review surfaces, pull the string's context into the preview:
    // command strings get the usage list; a string that lives in a dialog gets that dialog; and a loose
    // string gets a mock-up of where it's used (message box / OSD / status bar / tooltip). The Dialogs
    // tab already shows the current dialog; Menus uses the tree.
    std::string ringEnglish;              // control (by English) to ring + optionally hover
    CString     tipText;                  // tooltip text to hover over that control (empty = none)
    bool        comboShown = false;       // a combo-item dropdown owns the ring/list this pass
    if (tab == RES_DIALOGS) {
        ringEnglish = row.msgid;          // dialog already shown; just ring the selected control
    } else if (tab == RES_UNTRANS || tab == RES_STRINGS || tab == RES_REVIEW) {
        CString text = CString(CA2W((e && !e->msgstr.empty() ? e->msgstr : row.msgid).c_str(), CP_UTF8));
        long long dlg = DialogForString(row.msgctxt, row.msgid);
        CString ctlEng; long long tdlg = -1;
        if (row.msgctxt.rfind("IDS_CMD_", 0) == 0) {
            ShowCommandHelp(row.msgctxt);
            comboShown = true;   // ShowCommandHelp owns the ring/scroll/tooltip for this surface
        } else if (dlg >= 0) {                                   // the string lives in a dialog
            HideCommandHelp(); HideMockup();
            m_curDialog = dlg; RenderCurrentDialog();
            ringEnglish = row.msgid;
        } else if ((tdlg = TooltipDialog(row.msgctxt, ctlEng)) >= 0) {   // a tooltip on a dialog control
            HideCommandHelp(); HideMockup();
            m_curDialog = tdlg; RenderCurrentDialog();
            ringEnglish = std::string(CW2A(ctlEng, CP_UTF8));
            tipText = text;                                     // hover the note over the control
        } else if (ShowComboItem(row.msgctxt)) {                // a combo-item: show its page, dropdown open
            comboShown = true;                                  // ring + list handled inside ShowComboItem
        } else if (ShowAdvancedSetting(row.msgctxt, text)) {    // an Advanced-page setting description
            comboShown = true;                                  // page + editor + tooltip handled inside
        } else if (ShowArsSetting(row.msgctxt)) {               // MPC Audio Renderer dialog (no RC template)
            comboShown = true;                                  // synthesized dialog mock handles it
        } else if (ShowPlaylistMenu(row.msgctxt)) {             // playlist context menu (built in code)
            comboShown = true;                                  // synthesized themed menu handles it
        } else if (!ShowMockup(infoPtr, row.msgctxt, text, row.res)) {
            HideCommandHelp(); HideMockup(); m_preview.DestroyPreview();
            m_previewHost.SetFrame(L"", CSize());   // no dialog, no mock-up -- nothing to frame either
        }
    }
    if (!comboShown) {
        m_preview.HideComboDropdown();
        m_preview.HighlightControl(ringEnglish);                // ring the control the editor is on
        m_preview.ShowTooltip(ringEnglish, tipText);           // hover the note (no-op if tipText empty)
    }
    if (tab == RES_REVIEW && !row.evidence.IsEmpty())
        m_edit.SetFlagNote(L"REVIEW: " + row.evidence);
    ShowResearch(infoPtr);   // the enrichment write-up under the rendering
}

// A tooltip string's control is a dialog control whose symbol == the tooltip's msgctxt (e.g.
// IDC_CACHESHADERS). Returns that control's dialog + its English caption; -1 if it isn't a dialog
// control (e.g. the dynamically-built audio-renderer settings, which have no RC dialog).
long long MainFrame::TooltipDialog(const std::string& ctx, CString& ctlEnglish) {
    for (const auto& r : Idx().dialogs())
        if (r.control_sym == ctx && !r.msgid.empty()) { ctlEnglish = CA2W(r.msgid.c_str(), CP_UTF8); return r.dialog; }
    return -1;
}

// Combo dropdowns are filled at runtime in C++ (PPage*::DoDataExchange -> AddString(ResStr(IDS_...))),
// so the RC dialog template can't tell us that e.g. IDS_TIME_ON_SEEKBAR_* live in IDC_TIMEONSEEKBAR on
// IDD_PPAGETHEME. This table -- generated from upstream's PPage*.cpp -- maps each option-list combo to
// its (dialog, control, ordered items). Keep in sync with upstream if a page's combo changes.
namespace {
struct ComboGroup { const char* idd; const char* combo; std::vector<const char*> items; };
const std::vector<ComboGroup>& combo_groups() {
    static const std::vector<ComboGroup> t = {
        {"IDD_PPAGECAPTURE",  "IDC_COMBO6", {"IDS_PPAGE_CAPTURE_FG0","IDS_PPAGE_CAPTURE_FG1","IDS_PPAGE_CAPTURE_FG2"}},
        {"IDD_PPAGECAPTURE",  "IDC_COMBO7", {"IDS_PPAGE_CAPTURE_SFG0","IDS_PPAGE_CAPTURE_SFG1","IDS_PPAGE_CAPTURE_SFG2"}},
        {"IDD_PPAGEOUTPUT",   "IDC_AUDRND_COMBO", {"IDS_PPAGE_OUTPUT_SYS_DEF","IDS_PPAGE_OUTPUT_AUD_INTERNAL_REND",
            "IDS_PPAGE_OUTPUT_AUD_MPC_REND","IDS_PPAGE_OUTPUT_AUD_NULL_COMP","IDS_PPAGE_OUTPUT_AUD_NULL_UNCOMP"}},
        {"IDD_PPAGEOUTPUT",   "IDC_DX9RESIZER_COMBO", {"IDS_PPAGE_OUTPUT_RESIZE_NN","IDS_PPAGE_OUTPUT_RESIZER_BILIN",
            "IDS_PPAGE_OUTPUT_RESIZER_BIL_PS","IDS_PPAGE_OUTPUT_RESIZER_BICUB1","IDS_PPAGE_OUTPUT_RESIZER_BICUB2","IDS_PPAGE_OUTPUT_RESIZER_BICUB3"}},
        {"IDD_PPAGEOUTPUT",   "IDC_DX_SURFACE", {"IDS_PPAGE_OUTPUT_SURF_OFFSCREEN","IDS_PPAGE_OUTPUT_SURF_2D","IDS_PPAGE_OUTPUT_SURF_3D"}},
        {"IDD_PPAGEPLAYBACK", "IDC_COMBO1", {"IDS_ZOOM_25","IDS_ZOOM_50","IDS_ZOOM_100","IDS_ZOOM_200","IDS_ZOOM_AUTOFIT"}},
        {"IDD_PPAGEPLAYBACK", "IDC_COMBO2", {"IDS_AFTER_PLAYBACK_DO_NOTHING","IDS_AFTER_PLAYBACK_PLAY_NEXT","IDS_AFTER_PLAYBACK_REWIND",
            "IDS_AFTER_PLAYBACK_MONITOROFF","IDS_AFTER_PLAYBACK_CLOSE","IDS_AFTER_PLAYBACK_EXIT"}},
        {"IDD_PPAGEPLAYBACK", "IDC_COMBO3", {"IDS_PLAY_LOOPMODE_FILE","IDS_PLAY_LOOPMODE_PLAYLIST"}},
        {"IDD_PPAGEPLAYBACK", "IDC_COMBO4", {"IDS_VERTICAL_ALIGN_VIDEO_MIDDLE","IDS_VERTICAL_ALIGN_VIDEO_TOP","IDS_VERTICAL_ALIGN_VIDEO_BOTTOM"}},
        {"IDD_PPAGETHEME",    "IDC_COMBO1", {"IDS_THEMEMODE_DARK","IDS_THEMEMODE_LIGHT","IDS_THEMEMODE_WINDOWS"}},
        {"IDD_PPAGETHEME",    "IDC_COMBO3", {"IDS_TIME_TOOLTIP_ABOVE","IDS_TIME_TOOLTIP_BELOW"}},
        {"IDD_PPAGETHEME",    "IDC_TIMEONSEEKBAR", {"IDS_TIME_ON_SEEKBAR_NEVER","IDS_TIME_ON_SEEKBAR_ALWAYS","IDS_TIME_ON_SEEKBAR_WHEN_STATUSBAR_HIDDEN"}},
        {"IDD_PPAGETWEAKS",   "IDC_COMBO4", {"IDS_FASTSEEK_LATEST","IDS_FASTSEEK_NEAREST"}},
        {"IDD_PPAGEADVANCED", "IDC_COMBO1", {"IDS_STARTUP_PRESET_REMEMBER","IDS_AG_VIEW_MINIMAL","IDS_AG_VIEW_COMPACT","IDS_AG_VIEW_NORMAL","IDS_AG_VIEW_CUSTOM"}},
        {"IDD_PPAGEFULLSCREEN","IDC_COMBO2", {"IDS_PPAGEFULLSCREEN_SHOWNEVER","IDS_PPAGEFULLSCREEN_SHOWMOVED","IDS_PPAGEFULLSCREEN_SHOHHOVERED"}},
    };
    return t;
}

// The Advanced page's list rows, in on-screen order (from upstream PPageAdvanced.cpp add*Item calls).
// `name` is the setting's registry-key label as MPC-HC shows it in the Name column (from SettingsDefines.h
// IDS_RS_*); `value` is the default shown in the Value column; `desc` is the row's description tooltip.
// Keep in sync with upstream.
struct AdvRow { const char* name; char type; const char* value; const char* desc; };
const std::vector<AdvRow>& adv_list() {
    static const std::vector<AdvRow> t = {
        {"RecentFilesNumber", 'i', "100", "IDS_PPAGEADVANCED_RECENT_FILES_NUMBER"},
        {"RememberPosForLongerThan", 'i', "5", "IDS_PPAGEADVANCED_FILE_POS_LONGER"},
        {"RememberPosForAudioFiles", 'b', "True", "IDS_PPAGEADVANCED_FILE_POS_AUDIO"},
        {"RememberExternalPlaylistPos", 'b', "True", "IDS_PPAGEADVANCED_FILEPOS_PLAYLIST"},
        {"RememberTrackSelection", 'b', "True", "IDS_PPAGEADVANCED_FILEPOS_TRACK_SELECTION"},
        {"FullscreenSeparateControls", 'b', "True", "IDS_PPAGEADVANCED_FULLSCREEN_SEPARATE_CONTROLS"},
        {"CoverArtSizeLimit", 'i', "600", "IDS_PPAGEADVANCED_COVER_SIZE_LIMIT"},
        {"BlockVSFilter", 'b', "True", "IDS_PPAGEADVANCED_BLOCK_VSFILTER"},
        {"BlockRDP", 'b', "True", "IDS_PPAGEADVANCED_BLOCKRDP"},
        {"LoopFolderOnPlayNextFile", 'b', "False", "IDS_PPAGEADVANCED_LOOP_FOLDER_NEXT_FILE"},
        {"OSDTransparency", 'i', "64", ""},
        {"OSDBorder", 'i', "1", ""},
        {"UseYDL", 'b', "True", "IDS_PPAGEADVANCED_USE_YDL"},
        {"YDLMaxHeight", 'i', "1440", "IDS_PPAGEADVANCED_YDL_MAX_HEIGHT"},
        {"YDLVideoFormat", 'i', "0", "IDS_PPAGEADVANCED_YDL_VIDEO_FORMAT"},
        {"YDLAudioFormat", 'i', "0", "IDS_PPAGEADVANCED_YDL_AUDIO_FORMAT"},
        {"YDLAudioOnly", 'b', "False", "IDS_PPAGEADVANCED_YDL_AUDIO_ONLY"},
        {"YDLExePath", 's', "", "IDS_PPAGEADVANCED_YDL_EXEPATH"},
        {"YDLSubsPreference", 's', "", "IDS_PPAGEADVANCED_YDL_SUBS_PREFERENCE"},
        {"UseAutomaticCaptions", 'b', "False", "IDS_PPAGEADVANCED_USE_AUTOMATIC_CAPTIONS"},
        {"SaveImagePosition", 'b', "True", "IDS_PPAGEADVANCED_SAVEIMAGE_POSITION"},
        {"SaveImageCurrentTime", 'b', "False", "IDS_PPAGEADVANCED_SAVEIMAGE_CURRENTTIME"},
        {"SnapshotSubtitles", 'b', "True", "IDS_PPAGEADVANCED_SNAPSHOTSUBTITLES"},
        {"SnapshotKeepVideoExtension", 'b', "True", "IDS_PPAGEADVANCED_SNAPSHOTKEEPVIDEOEXTENSION"},
        {"AddLanguageCodeWhenSaveSubtitles", 'b', "False", "IDS_PPAGEADVANCED_ADD_LANGCODE_WHEN_SAVE_SUBTITLES"},
        {"UseTitleInRecentFileList", 'b', "True", "IDS_PPAGEADVANCED_USE_TITLE_IN_RECENT_FILE_LIST"},
        {"MouseLeftUpDelay", 'i', "0", "IDS_PPAGEADVANCED_MOUSE_LEFTUP_DELAY"},
        {"LockNoPause", 'b', "False", "IDS_PPAGEADVANCED_LOCK_NOPAUSE"},
        {"PreventDisplaySleep", 'b', "True", "IDS_PPAGEADVANCED_PREVENT_DISPLAY_SLEEP"},
        {"ReloadAfterLongPause", 'i', "0", "IDS_PPAGEADVANCED_RELOAD_AFTER_LONG_PAUSE"},
        {"AllowInaccurateFastseek", 'b', "True", "IDS_PPAGEADVANCED_ALLOW_INACCURATE_FASTSEEK"},
        {"StillVideoDuration", 'i', "10", "IDS_PPAGEADVANCED_STILL_VIDEO_DURATION"},
        {"TimeRefreshInterval", 'i', "100", "IDS_PPAGEADVANCED_TIME_REFRESH_INTERVAL"},
        {"RedirectOpenToAppendThreshold", 'i', "1000", "IDS_PPAGEADVANCED_REDIRECT_OPEN_TO_APPEND_THRESHOLD"},
        {"EnableCrashReporter", 'b', "True", "IDS_PPAGEADVANCED_CRASHREPORTER"},
        {"DebugLogMask", 'i', "0", "IDS_PPAGEADVANCED_LOGGER"},
        {"FullscreenDelay", 'i', "0", "IDS_PPAGEADVANCED_FULLSCREEN_DELAY"},
        {"AutoDownloadScoreMovies", 'i', "0", "IDS_PPAGEADVANCED_SCORE"},
        {"AutoDownloadScoreSeries", 'i', "0", "IDS_PPAGEADVANCED_SCORE"},
        {"OpenRecordingPanelWhenOpeningDevice", 'b', "True", "IDS_PPAGEADVANCED_OPEN_REC_PANEL_WHEN_OPENING_DEVICE"},
        {"AlwaysUseShortMenu", 'b', "False", "IDS_PPAGEADVANCED_ALWAYS_USE_SHORT_MENU"},
        {"UseFreetype", 'b', "False", "IDS_PPAGEADVANCED_USE_FREETYPE"},
        {"UseMediainfoLoadFileDuration", 'b', "False", "IDS_PPAGEADVANCED_USE_MEDIAINFO_LOAD_FILE_DURATION"},
        {"CaptureDeinterlace", 'b', "False", "IDS_PPAGEADVANCED_CAPTURE_DEINTERLACE"},
        {"PauseWhileDraggingSeekbar", 'b', "True", "IDS_PPAGEADVANCED_PAUSE_WHILE_DRAGGING_SEEKBAR"},
        {"ConfirmFileDelete", 'b', "True", "IDS_PPAGEADVANCED_CONFIRM_FILE_DELETE"},
        {"UseLibassForSRT", 'b', "False", "IDS_PPAGEADVANCED_LIBASS_FOR_SRT"},
        {"ShowVolumePercentage", 'b', "True", "IDS_PPAGEADVANCED_SHOW_VOLUME_PERCENTAGE"},
        {"StartupPreset", 'c', "", "IDS_PPAGEADVANCED_STARTUP_PRESET"},
        {"TimeOnSeekBarLeft", 'b', "False", "IDS_PPAGEADVANCED_TIME_ON_SEEKBAR_LEFT"},
    };
    return t;
}
}  // namespace

// If `ctx` is one of the combo-item strings, render its host page and drop the combo's list open with
// the selected item highlighted (translated). Returns false if `ctx` isn't a combo item, or the host
// dialog/control couldn't be located in the parsed RC (falls back to the mock-up).
bool MainFrame::ShowComboItem(const std::string& ctx) {
    const ComboGroup* g = nullptr;
    for (const auto& cg : combo_groups())
        for (const char* s : cg.items) if (ctx == s) { g = &cg; break; }
    if (!g) return false;

    auto translate = [&](const std::string& sym) -> std::string {   // sym == msgctxt; look up msgstr
        for (int r = 0; r < RES_COUNT; ++r)
            for (const auto& e : m_po[r].entries)
                if (e.msgctxt == sym) return !e.msgstr.empty() ? e.msgstr : e.msgid;
        return sym;
    };
    std::vector<CString> items; int sel = -1;
    for (size_t i = 0; i < g->items.size(); ++i) {
        if (ctx == g->items[i]) sel = (int)i;
        items.push_back(CString(CA2W(translate(g->items[i]).c_str(), CP_UTF8)));
    }

    // The Advanced page is a runtime-built Name/Value list, not an RC dialog we can render live; show its
    // synthesized list with the StartupPreset row's dropdown expanded (see ShowAdvancedList).
    if (g->idd == std::string("IDD_PPAGEADVANCED"))
        return ShowAdvancedList("StartupPreset", CString(), items, sel);

    long long dlgId = -1, comboId = -1;                 // resolve dialog + control ids from the parsed RC
    for (const auto& d : m_preview.RcDialogs())
        if (d.sym == g->idd) {
            dlgId = d.id;
            for (const auto& c : d.controls) if (c.sym == g->combo) { comboId = c.id; break; }
            break;
        }
    if (dlgId < 0 || comboId < 0) return false;

    HideCommandHelp(); HideMockup();
    m_curDialog = dlgId; RenderCurrentDialog();
    m_preview.ShowComboDropdown(comboId, items, sel);
    return true;
}

// If `ctx` is an Advanced-page setting description (IDS_PPAGEADVANCED_*), show the synthesized Advanced
// list scrolled to that setting's row, with the description hovered as its row tooltip. Returns false if
// `ctx` isn't a known advanced setting.
bool MainFrame::ShowAdvancedSetting(const std::string& ctx, const CString& descText) {
    for (const auto& a : adv_list())
        if (a.desc && ctx == a.desc) return ShowAdvancedList(a.name, descText, {}, -1);
    return false;
}

// Recreate the Advanced page's Name/Value list (a runtime-built list control, not an RC dialog) as a
// synthesized mock: a window of rows centered on `selName`, that row highlighted, `tooltip` hovered
// below it, and — for the one combo setting — `dropItems` shown as an expanded dropdown (dropSel picked).
bool MainFrame::ShowAdvancedList(const std::string& selName, const CString& tooltip,
                                 const std::vector<CString>& dropItems, int dropSel) {
    long long dlgId = -1, listId = -1, comboId = -1;      // resolve the real dialog + its control ids
    auto idOf = [&](const char* sym) -> long long {
        for (const auto& d : m_preview.RcDialogs()) if (d.sym == std::string("IDD_PPAGEADVANCED"))
            for (const auto& c : d.controls) if (c.sym == sym) return c.id;
        return -1;
    };
    for (const auto& d : m_preview.RcDialogs()) if (d.sym == std::string("IDD_PPAGEADVANCED")) { dlgId = d.id; break; }
    if (dlgId < 0) return false;
    listId = idOf("IDC_LIST1"); comboId = idOf("IDC_COMBO1");
    if (listId < 0) return false;

    HideCommandHelp(); HideMockup();
    m_curDialog = dlgId; RenderCurrentDialog();          // real IDD_PPAGEADVANCED (exact dimensions/theming)

    auto tr = [&](const char* sym) -> CString {
        for (int r = 0; r < RES_COUNT; ++r)
            for (const auto& e : m_po[r].entries)
                if (e.msgctxt == sym) return CString(CA2W((e.msgstr.empty() ? e.msgid : e.msgstr).c_str(), CP_UTF8));
        return CString(CA2W(sym, CP_UTF8));
    };
    const auto& list = adv_list();
    std::vector<std::pair<CString, CString>> rows; int selRow = -1;
    for (int i = 0; i < (int)list.size(); ++i) {
        CString val = CString(CA2W(list[i].value, CP_UTF8));
        if (selName == "StartupPreset" && std::string(list[i].name) == "StartupPreset" &&
            dropSel >= 0 && dropSel < (int)dropItems.size()) val = dropItems[dropSel];
        rows.emplace_back(CString(CA2W(list[i].name, CP_UTF8)), val);
        if (selName == list[i].name) selRow = i;
    }
    m_preview.PopulateReportList(listId, tr("IDS_PPAGEADVANCED_COL_NAME"), tr("IDS_PPAGEADVANCED_COL_VALUE"), rows, selRow);

    // The template parks all inline value editors at one spot; hide them so they don't clutter the list.
    HWND dlg = m_preview.CurrentDlg();
    for (const char* s : { "IDC_EDIT1", "IDC_SPIN1", "IDC_COMBO1", "IDC_RADIO1", "IDC_RADIO2", "IDC_BUTTON1" }) {
        long long id = idOf(s);
        if (id >= 0 && dlg) if (HWND h = ::GetDlgItem(dlg, (int)id)) ::ShowWindow(h, SW_HIDE);
    }

    if (!dropItems.empty() && comboId >= 0 && selRow >= 0) {   // combo cell: editor over the value + dropdown
        long long cid = m_preview.PositionOverListValue(listId, comboId, selRow);
        if (cid >= 0) m_preview.ShowComboDropdown(cid, dropItems, dropSel);
    } else if (!tooltip.IsEmpty() && selRow >= 0) {           // description: bubble below the row
        m_preview.ShowListItemTooltip(listId, selRow, tooltip);
    }
    return true;
}

// The MPC Audio Renderer Settings dialog (IDS_ARS_*) is built entirely in C++ (thirdparty/
// MpcAudioRenderer/MpcAudioRendererSettingsWnd.cpp) with no RC template, so its checkboxes/combos
// render as loose strings. Recreate it as a synthesized two-tab dialog and highlight the selected
// control. Returns false if `ctx` is not an ARS string. Keep in sync with the upstream Wnd layout.
bool MainFrame::ShowArsSetting(const std::string& ctx) {
    if (ctx.rfind("IDS_ARS_", 0) != 0) return false;

    auto tr = [&](const char* sym) -> CString {
        for (int r = 0; r < RES_COUNT; ++r)
            for (const auto& e : m_po[r].entries)
                if (e.msgctxt == sym) return CString(CA2W((e.msgstr.empty() ? e.msgid : e.msgstr).c_str(), CP_UTF8));
        return CString(CA2W(sym, CP_UTF8));
    };
    // Resolve the selected control + any tooltip. A _TIP_ string maps to its checkbox and supplies the
    // bubble; the WASAPI-mode option strings map to that combo.
    std::string sel = ctx; CString tip;
    if (ctx.rfind("IDS_ARS_TIP_", 0) == 0) { sel = "IDS_ARS_" + ctx.substr(12); tip = tr(ctx.c_str()); }
    else if (ctx == "IDS_ARS_SHARED" || ctx == "IDS_ARS_EXCLUSIVE") sel = "IDS_ARS_WASAPI_MODE";

    static const std::set<std::string> statusPage = {
        "IDS_ARS_STATUS", "IDS_ARS_DEVICE", "IDS_ARS_MODE", "IDS_ARS_INPUT", "IDS_ARS_OUTPUT",
        "IDS_ARS_FORMAT", "IDS_ARS_SAMPLERATE", "IDS_ARS_CHANNELS" };
    bool status = statusPage.count(sel) > 0;

    std::vector<SynthRow> rows;
    auto add = [&](SynthRow::Kind k, const char* sym) {
        SynthRow r; r.kind = k; r.text = tr(sym); r.selected = (sel == sym); rows.push_back(r);
    };
    if (!status) {
        add(SynthRow::ComboFull,   "IDS_ARS_SOUND_DEVICE");
        add(SynthRow::ComboInline, "IDS_ARS_WASAPI_MODE");
        add(SynthRow::ComboInline, "IDS_ARS_WASAPI_METHOD");
        add(SynthRow::ComboInline, "IDS_ARS_DEVICE_PERIOD");
        for (const char* s : { "IDS_ARS_BITEXACT_OUTPUT", "IDS_ARS_SYSTEM_LAYOUT_CHANNELS",
             "IDS_ARS_ALT_CHECK_FORMAT", "IDS_ARS_RELEASE_DEVICE_IDLE", "IDS_ARS_CROSSFEED",
             "IDS_ARS_DUMMY_CHANNELS", "IDS_ARS_PAUSE_KEEP_ACTIVE" })
            add(SynthRow::Check, s);
        add(SynthRow::Button, "IDS_FILTER_RESET_SETTINGS");
    } else {
        add(SynthRow::Value, "IDS_ARS_DEVICE");
        add(SynthRow::Value, "IDS_ARS_MODE");
        add(SynthRow::Group, "IDS_ARS_INPUT");
        add(SynthRow::Value, "IDS_ARS_FORMAT"); add(SynthRow::Value, "IDS_ARS_SAMPLERATE"); add(SynthRow::Value, "IDS_ARS_CHANNELS");
        add(SynthRow::Group, "IDS_ARS_OUTPUT");
        add(SynthRow::Value, "IDS_ARS_FORMAT"); add(SynthRow::Value, "IDS_ARS_SAMPLERATE"); add(SynthRow::Value, "IDS_ARS_CHANNELS");
    }

    std::vector<CString> tabs = { CString(L"Settings"), tr("IDS_ARS_STATUS") };
    m_preview.DestroyPreview();
    HideCommandHelp();
    CRect rc; m_previewHost.GetWindowRect(&rc); ScreenToClient(&rc);
    m_mockup.MoveWindow(rc);
    m_mockup.SetSynthDialog(L"MPC Audio Renderer Settings", tabs, status ? 1 : 0, std::move(rows), tip, m_dpi);
    m_mockup.ShowWindow(SW_SHOW);
    m_mockup.Invalidate();
    return true;
}

// The playlist right-click menu is built in code (PlayerPlaylistBar.cpp), not the RC menus, so its items
// render as loose strings. Recreate it as a real themed popup menu: selecting a top-level item shows the
// whole menu with it highlighted; a Position/Sort child shows that submenu. Returns false if `ctx` isn't a
// playlist-menu string. Keep the item list in sync with upstream.
bool MainFrame::ShowPlaylistMenu(const std::string& ctx) {
    static const char* kMain[] = {
        "IDS_PLAYLIST_OPEN", "IDS_PLAYLIST_ADD", "IDS_PLAYLIST_REMOVE", "|",
        "IDS_PLAYLIST_CLEAR", "|", "IDS_PLAYLIST_COPYTOCLIPBOARD", "IDS_PLAYLIST_SHOWFOLDER",
        "IDS_FILE_RECYCLE", "IDS_PLAYLIST_ADDFOLDER", "|", "IDS_PLAYLIST_SAVE", "IDS_PLAYLIST_SAVEAS", "|",
        "IDS_PLAYLIST_SORTBYLABEL", "IDS_PLAYLIST_SORTBYPATH", "IDS_PLAYLIST_RANDOMIZE", "IDS_PLAYLIST_RESTORE",
        "|", "IDS_PLAYLIST_SHUFFLE", "|", "IDS_PLAYLIST_HIDEFS", "|", "IDS_PLAYLIST_POSITION" };
    static const char* kPosition[] = { "IDS_PLAYLIST_POSITION_LEFT", "IDS_PLAYLIST_POSITION_TOP",
        "IDS_PLAYLIST_POSITION_RIGHT", "IDS_PLAYLIST_POSITION_BOTTOM", "IDS_PLAYLIST_POSITION_FLOAT" };
    static const char* kSort[] = { "IDS_PLAYLIST_SORTBYLABEL", "IDS_PLAYLIST_SORTBYPATH",
        "IDS_PLAYLIST_RANDOMIZE", "IDS_PLAYLIST_RESTORE" };
    auto idxIn = [&](const char* const* a, int n) { for (int i = 0; i < n; ++i) if (ctx == a[i]) return i; return -1; };

    int posChild = idxIn(kPosition, 5);
    int mainItem = idxIn(kMain, (int)std::size(kMain));
    bool sortHdr = (ctx == "IDS_PLAYLIST_SORT");
    if (posChild < 0 && mainItem < 0 && !sortHdr) return false;

    auto tr = [&](const char* sym) -> CString {
        for (int r = 0; r < RES_COUNT; ++r)
            for (const auto& e : m_po[r].entries)
                if (e.msgctxt == sym) return CString(CA2W((e.msgstr.empty() ? e.msgid : e.msgstr).c_str(), CP_UTF8));
        return CString(CA2W(sym, CP_UTF8));
    };
    HMENU menu = ::CreatePopupMenu();
    int selIndex = -1;
    if (posChild >= 0) {                         // a Position child -> the Position submenu, that item picked
        for (int i = 0; i < 5; ++i) ::AppendMenuW(menu, MF_STRING, i + 1, tr(kPosition[i]));
        selIndex = posChild;
    } else if (sortHdr) {                        // the (collapsed) Sort submenu
        for (int i = 0; i < 4; ++i) ::AppendMenuW(menu, MF_STRING, i + 1, tr(kSort[i]));
    } else {                                     // the full playlist menu, top-level item picked
        int pos = 0, id = 1;
        for (int i = 0; i < (int)std::size(kMain); ++i) {
            if (std::string(kMain[i]) == "|") { ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); }
            else if (std::string(kMain[i]) == "IDS_PLAYLIST_POSITION") {
                HMENU sub = ::CreatePopupMenu();  // popup so it shows the submenu arrow
                for (int k = 0; k < 5; ++k) ::AppendMenuW(sub, MF_STRING, 100 + k, tr(kPosition[k]));
                ::AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, tr(kMain[i]));
            } else ::AppendMenuW(menu, MF_STRING, id++, tr(kMain[i]));
            if (ctx == kMain[i]) selIndex = pos;
            ++pos;
        }
    }
    Theme::ThemeMenu(menu, /*isMenubar=*/false);

    m_preview.DestroyPreview();
    HideCommandHelp();
    CRect rc; m_previewHost.GetWindowRect(&rc); ScreenToClient(&rc);
    m_mockup.MoveWindow(rc);
    m_mockup.SetMenu(menu, selIndex, m_dpi);     // m_mockup owns + destroys the menu
    m_mockup.ShowWindow(SW_SHOW);
    m_mockup.Invalidate();
    return true;
}

void MainFrame::OnPreviewClick(HWND ctrl) {
    if (m_curDialog < 0) return;
    const DialogRecord* rec = m_preview.HitTest(nullptr, ctrl, m_curDialog, Idx());
    if (!rec) return;
    for (int i = 0; i < (int)m_rows.size(); ++i)
        if (m_rows[i].msgctxt == rec->msgctxt && m_rows[i].msgid == rec->msgid) {
            m_list.SetItemState(i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            m_list.EnsureVisible(i, FALSE);
            m_list.SetFocus();
            break;
        }
}

void MainFrame::CommitEdit(const CString& msgstr) {
    if (m_curRow < 0 || m_curRow >= (int)m_rows.size()) return;
    const Row& row = m_rows[m_curRow];
    int res = -1;
    PoFile* po = PoFor(row.msgctxt, row.msgid, &res);
    if (!po) { SetStatus(L"Key not present in the checked-out .po (dropped upstream?)"); return; }
    std::string newVal = std::string(CW2A(msgstr, CP_UTF8));
    bool changed = false;
    for (auto& e : po->entries)
        if (e.msgctxt == row.msgctxt && e.msgid == row.msgid) {
            if (e.msgstr != newVal) { e.msgstr = newVal; changed = true; }
            break;
        }
    // No-op guard: applying a value identical to what's already there (e.g. Apply on a still-blank
    // cell) changes nothing -- don't mark it dirty (which would let Submit PR create an empty PR) and
    // don't trigger a re-render. Just report and return.
    if (!changed) { SetStatus(L"No change — the translation is unchanged."); return; }
    m_dirty[res].insert({ row.msgctxt, row.msgid });
    // M2: the cell just got a translation (Accept-AI or a plain manual edit alike) -- any stored AI
    // suggestion for it is obsolete (either just accepted, or superseded by a fresh human edit).
    // Unconditional/cheap no-op if nothing was stored; done BEFORE PopulateReviewRows/
    // RecomputeFitForDialog below so nothing downstream sees stale suggestion state.
    suggestion_store::remove(std::string(CW2A(m_lang, CP_UTF8)), row.msgctxt, row.msgid);
    SaveDrafts();                        // persist immediately so the edit survives a restart
    RefreshListRow(m_curRow);
    if (m_tabs.GetCurSel() == RES_UNTRANS && row.msgctxt.rfind("IDS_CMD_", 0) == 0) {
        ShowCommandHelp(row.msgctxt);   // refresh the usage list with the just-entered translation
    } else if (m_tabs.GetCurSel() == RES_DIALOGS || m_tabs.GetCurSel() == RES_UNTRANS ||
              m_tabs.GetCurSel() == RES_REVIEW) {
        RenderCurrentDialog();
        m_edit.ShowOverflowWarning(!m_preview.DetectOverflow(m_preview.CurrentDlg()).empty());
    } else if (m_tabs.GetCurSel() == RES_MENUS) {
        BuildMenuTree();   // reflect the new translation in the tree + native popup
    }
    // Targeted flag refresh: only this cell's deterministic (non-fit) status can have changed, and
    // only its OWN dialog's fit measurements (if any) can have changed. (`row` is still valid here —
    // it must stay so; PopulateReviewRows(), which invalidates it via m_rows.clear(), runs last.)
    long long dlg = DialogForString(row.msgctxt, row.msgid);
    if (dlg >= 0) RecomputeFitForDialog(dlg);
    if (m_tabs.GetCurSel() == RES_REVIEW) PopulateReviewRows();   // cheap: reads caches, no rendering
    size_t total = m_dirty[0].size() + m_dirty[1].size() + m_dirty[2].size();
    CString s;
    s.Format(L"%zu pending edit(s) — File > Submit PR when done", total);
    SetStatus(s);
}

// ---------- Review Queue (M1): deterministic flags (fit / placeholder / accelerator) ----------

// Convert MeasureFit's raw fit measurements into ReviewFlag entries (hard overflow / tight fit +
// evidence text). Shared between the background fit-scan worker (EnsureFitScan) and the synchronous
// single-dialog re-measure (RecomputeFitForDialog) so the hard/tight thresholds + evidence wording
// never drift between the two call sites. Static: touches no instance state, safe off the UI thread.
void MainFrame::AppendFitFlags(std::vector<ReviewFlag>& out, long long dialogId,
                               const std::vector<LivePreview::FitMeasurement>& measurements) {
    for (const auto& m : measurements) {
        bool grouped = m.grouped;
        int rendered = grouped ? m.groupRenderedPx : m.renderedPx;
        int avail    = grouped ? m.groupAvailablePx : m.availablePx;
        if (avail <= 0) continue;
        double ratio = (double)rendered / avail;
        Row::Flag kind;
        if (rendered > avail) kind = Row::Flag::FitHard;
        else if (ratio >= 0.90) kind = Row::Flag::FitTight;
        else continue;   // fits comfortably -- not flagged
        CString evidence;
        if (grouped) {
            CString peers;
            for (const auto& p : m.groupPeers) {
                if (!peers.IsEmpty()) peers += L", ";
                peers += CString(CA2W(p.c_str(), CP_UTF8));
            }
            if (kind == Row::Flag::FitHard)
                evidence.Format(L"group overflows by %dpx (%s)", rendered - avail, (LPCWSTR)peers);
            else
                evidence.Format(L"tight: group %d%% of width (%s)", (int)(ratio * 100), (LPCWSTR)peers);
        } else {
            if (kind == Row::Flag::FitHard)
                evidence.Format(L"overflows by %dpx", rendered - avail);
            else
                evidence.Format(L"tight: %d%% of width", (int)(ratio * 100));
        }
        ReviewFlag rf;
        rf.msgctxt = m.msgctxt; rf.msgid = m.msgid; rf.kind = kind; rf.evidence = evidence;
        rf.dialog = dialogId; rf.controlId = m.controlId; rf.overflowPx = rendered - avail;
        out.push_back(std::move(rf));
    }
}

// Kick off a background fit scan for the current language (if not already cached/running). Renders
// every dialog OFFSCREEN on a dedicated worker thread and measures each translatable Button/
// single-line-Static's text against its control, flagging hard overflow / tight fits.
void MainFrame::EnsureFitScan() {
    if (!m_checkedOut || m_fitScanRunning) return;
    if (m_fitCache.count(std::wstring(m_lang))) return;   // already computed for this language
    if (m_fitThread.joinable()) m_fitThread.join();       // a PREVIOUS language's scan finished
    m_fitScanRunning = true; m_fitCancel = false; m_fitScanDone = 0; m_fitScanTotal = 0;
    SetStatus(L"Computing fit for '" + m_lang + L"'\x2026");

    // Snapshot everything the worker needs by VALUE — it must never touch `this`/MainFrame members
    // after this point (it runs on a different thread for the scan's duration).
    ControlIndex idxCopy = Idx();
    std::vector<RcDialog> rcCopy = m_preview.RcDialogs();
    bool useRcCopy = m_preview.UsingRc();
    PoFile poCopy = m_po[RES_DIALOGS];
    CString neutralDll = m_bundle.neutral_dll;
    std::wstring langKey(m_lang);
    std::set<long long> dialogIds;
    for (const auto& r : idxCopy.dialogs()) dialogIds.insert(r.dialog);
    HWND hwnd = GetSafeHwnd();

    m_fitThread = std::thread([hwnd, idxCopy, rcCopy, useRcCopy, poCopy, neutralDll, langKey, dialogIds]() {
        // Off-screen render technique: a WS_POPUP host window that is NEVER shown (ShowWindow(SW_SHOW)
        // is never called on it). A WS_CHILD's on-screen visibility is gated by its whole ancestor
        // chain, so a child of a never-shown parent never paints/flashes — even though RenderDialog
        // unconditionally does ShowWindow(dlg, SW_SHOWNA) at the end. This lets the whole dialog-
        // render + substitution + widget-pair-repositioning pipeline run unmodified, off the UI thread,
        // with no visible artifact. This is the one genuinely novel technique in the Review Queue —
        // creating a raw CWnd off the UI thread is a well-precedented MFC pattern for offscreen
        // rendering (MFC's window-creation hook state is lazily thread-local, not tied to
        // AfxBeginThread), and LivePreview itself is almost entirely plain Win32 under its CWnd
        // parent parameter, so the risk is low — but this was not build/run-verified in this task.
        CWnd host;
        host.CreateEx(0, AfxRegisterWndClass(0, nullptr, nullptr, nullptr), L"",
                     WS_POPUP, 0, 0, 10, 10, nullptr, nullptr);

        LivePreview lp;
        lp.LoadNeutralDll(neutralDll);
        if (useRcCopy && !rcCopy.empty()) lp.SetRcDialogs(rcCopy);

        auto* result = new FitScanResult;
        result->lang = langKey;
        int doneCount = 0;
        // No mid-scan cancellation: a language switch just lets the OLD scan finish and its result is
        // used or ignored based on relevance in OnFitScanDone (~50 dialog renders is fast enough).
        for (long long dialogId : dialogIds) {
            HWND dlg = lp.RenderDialog(dialogId, &host, idxCopy, poCopy);
            if (dlg) {
                AppendFitFlags(result->flags, dialogId, lp.MeasureFit(dlg, dialogId, idxCopy));
                lp.DestroyPreview();
            }
            ++doneCount;
            ::PostMessage(hwnd, WM_APP_FIT_SCAN_PROGRESS, (WPARAM)doneCount, (LPARAM)dialogIds.size());
        }
        if (!::PostMessage(hwnd, WM_APP_FIT_SCAN_DONE, (WPARAM)result, 0)) delete result;
    });
}

// A worker tick — advance the progress status (Review tab only; the wording deliberately contains the
// literal substring "Computing fit" so automation can poll for it to disappear).
LRESULT MainFrame::OnFitScanProgress(WPARAM done, LPARAM total) {
    m_fitScanDone = (int)done; m_fitScanTotal = (int)total;
    if (m_tabs.GetCurSel() == RES_REVIEW) {
        CString s;
        s.Format(L"Computing fit for '%s': %d/%d dialogs\x2026",
                (LPCWSTR)m_lang, m_fitScanDone, m_fitScanTotal);
        SetStatus(s);
    }
    return 0;
}

// UI thread: install the fit-scan result into the per-language cache; refresh the list if the Review
// tab is showing THIS language right now (otherwise it's just cached silently for later).
LRESULT MainFrame::OnFitScanDone(WPARAM wp, LPARAM) {
    std::unique_ptr<FitScanResult> res((FitScanResult*)wp);
    if (m_fitThread.joinable()) m_fitThread.join();
    m_fitScanRunning = false;
    m_fitCache[res->lang] = std::move(res->flags);
    if (res->lang == std::wstring(m_lang) && m_tabs.GetCurSel() == RES_REVIEW) PopulateList();
    return 0;
}

// Synchronous, UI-thread, single-dialog re-measure (post-edit) — uses the already-loaded m_preview /
// Idx() / m_po[RES_DIALOGS], no thread/snapshot needed. Replaces this dialog's slice of the cache.
void MainFrame::RecomputeFitForDialog(long long dialogId) {
    auto& cache = m_fitCache[std::wstring(m_lang)];   // creates an empty entry if none yet -- fine
    cache.erase(std::remove_if(cache.begin(), cache.end(),
                               [&](const ReviewFlag& f) { return f.dialog == dialogId; }),
               cache.end());
    HWND dlg = m_preview.RenderDialog(dialogId, &m_previewHost, Idx(), m_po[RES_DIALOGS]);
    if (!dlg) return;
    AppendFitFlags(cache, dialogId, m_preview.MeasureFit(dlg, dialogId, Idx()));
    // Caller (CommitEdit) re-renders the CURRENT dialog again afterward via its own existing
    // RenderCurrentDialog() call for the active row, so it's fine that this leaves `m_preview`
    // pointed at `dialogId` rather than whatever was showing before.
}

// M2: the single-flag eligibility check shared by BuildGenWorklist (what gets generated) and SelectRow
// (when a stored suggestion may show) — see the declaration in MainFrame.h for the full rationale.
// Call only for a cell with a NON-EMPTY msgstr (the empty case is a different kind of "no flag" that
// callers handle separately -- Flag::None there means "nothing to validate yet", not "passes").
// Priority Placeholder > Accelerator > Fit(Hard/Tight), first match wins; mirrors (but does not share
// code with) PopulateReviewRows's own independent per-rule enumeration just below, which intentionally
// keeps producing MULTIPLE rows for a cell that trips more than one rule (a richer shape the M1 Review
// tab wants that a single Row::Flag can't express) -- left untouched to avoid any M1 regression.
MainFrame::Row::Flag MainFrame::CellFlag(const std::string& msgctxt, const std::string& msgid, int res,
                                         const std::string& msgstr, CString& evidenceOut,
                                         long long& flagDialogOut, long long& flagControlOut) {
    return CellFlagCore(msgctxt, msgid, res, msgstr, evidenceOut, flagDialogOut, flagControlOut,
                        m_fitCache.count(std::wstring(m_lang)) ? &m_fitCache[std::wstring(m_lang)] : nullptr);
}

// M4: static twin -- see the declaration in MainFrame.h for why (the ALL-languages batch worker runs
// this off the UI thread, one dialog-fit-vector per language, never m_fitCache/m_lang).
MainFrame::Row::Flag MainFrame::CellFlagCore(const std::string& msgctxt, const std::string& msgid, int res,
                                             const std::string& msgstr, CString& evidenceOut,
                                             long long& flagDialogOut, long long& flagControlOut,
                                             const std::vector<ReviewFlag>* fitFlags) {
    evidenceOut.Empty(); flagDialogOut = -1; flagControlOut = -1;
    for (const auto& f : validate::rule_format(msgctxt, msgid, msgstr)) {
        if (f.sev != validate::Severity::Error) continue;
        auto a = validate::format_specs(msgid);
        auto b = validate::format_specs(msgstr);
        std::multiset<std::string> mb2(b.begin(), b.end());
        std::string missing;
        for (auto& t : a) {
            auto it = mb2.find(t);
            if (it == mb2.end()) { missing = t; break; }
            mb2.erase(it);
        }
        evidenceOut = !missing.empty()
            ? (L"placeholder " + CString(CA2W(missing.c_str(), CP_UTF8)) + L" missing")
            : L"placeholder mismatch";
        return Row::Flag::Placeholder;
    }
    if (validate::accelerator_count(msgid) > validate::accelerator_count(msgstr)) {
        evidenceOut = L"missing '&' accelerator (English has one)";
        return Row::Flag::Accelerator;
    }
    if (res == RES_DIALOGS && fitFlags) {
        for (const auto& f : *fitFlags) {
            if (f.msgctxt != msgctxt || f.msgid != msgid) continue;
            if (f.kind != Row::Flag::FitHard && f.kind != Row::Flag::FitTight) continue;
            evidenceOut = f.evidence; flagDialogOut = f.dialog; flagControlOut = f.controlId;
            return f.kind;
        }
    }
    return Row::Flag::None;
}

// Build m_rows for the Review tab from validate:: (placeholder / accelerator — cheap, run fresh every
// call) + m_fitCache (fit — read-only; EnsureFitScan owns kicking off the scan, this never blocks).
// Row order: hard overflow, placeholder, accelerator, category, tight fit.
void MainFrame::PopulateReviewRows() {
    std::vector<Row> hard, placeholder, accel, category, tight;
    std::wstring langKey(m_lang);
    auto dismissed = [&](const std::string& ctx, const std::string& id) {
        return m_dismissed.count({ langKey, ctx, id }) != 0;
    };
    // -- deterministic, cheap: placeholder + missing-accelerator over ALL entries --
    for (int res = 0; res < RES_COUNT; ++res) {
        for (const auto& e : m_po[res].entries) {
            if (e.msgid.empty() || e.msgstr.empty() || dismissed(e.msgctxt, e.msgid)) continue;
            for (const auto& f : validate::rule_format(e.msgctxt, e.msgid, e.msgstr)) {
                if (f.sev != validate::Severity::Error) continue;
                auto a = validate::format_specs(e.msgid);
                auto b = validate::format_specs(e.msgstr);
                std::multiset<std::string> mb2(b.begin(), b.end());
                std::string missing;
                for (auto& t : a) {
                    auto it = mb2.find(t);
                    if (it == mb2.end()) { missing = t; break; }
                    mb2.erase(it);
                }
                CString evidence = !missing.empty()
                    ? (L"placeholder " + CString(CA2W(missing.c_str(), CP_UTF8)) + L" missing")
                    : L"placeholder mismatch";
                Row row{ e.msgctxt, e.msgid, res };
                row.flag = Row::Flag::Placeholder; row.evidence = evidence;
                placeholder.push_back(row);
                break;   // one row per cell even if rule_format returns >1 finding
            }
            int idc = validate::accelerator_count(e.msgid), strc = validate::accelerator_count(e.msgstr);
            if (idc > strc) {
                Row row{ e.msgctxt, e.msgid, res };
                row.flag = Row::Flag::Accelerator;
                row.evidence = L"missing '&' accelerator (English has one)";
                accel.push_back(row);
            }
        }
    }
    // -- dialog-scoped duplicate accelerator (RES_DIALOGS only) --
    {
        std::map<long long, std::vector<validate::AcceleratorEntry>> byDialog;
        std::map<std::pair<long long, std::string>, std::string> msgidByCtx;   // (dialog,ctx)->msgid
        for (const auto& r : Idx().dialogs()) {
            if (!r.control) continue;   // captions aren't accelerator-bearing controls
            if (auto* e = m_po[RES_DIALOGS].find(r.msgctxt, r.msgid); e && !e->msgstr.empty()) {
                byDialog[r.dialog].push_back({ r.msgctxt, e->msgstr });
                msgidByCtx[{ r.dialog, r.msgctxt }] = r.msgid;
            }
        }
        for (auto& [dlg, entries] : byDialog)
            for (const auto& f : validate::rule_duplicate_accelerator(entries)) {
                auto it = msgidByCtx.find({ dlg, f.ctx });
                if (it == msgidByCtx.end() || dismissed(f.ctx, it->second)) continue;
                Row row{ f.ctx, it->second, RES_DIALOGS };
                row.flag = Row::Flag::Accelerator;
                row.evidence = CString(CA2W(f.msg.c_str(), CP_UTF8));
                row.flagDialog = dlg;
                accel.push_back(row);
            }
    }
    // -- Options tree category consistency (RES_STRINGS only): a property-page title (IDD_PPAGE* etc.)
    // is a STRING-table entry whose msgctxt is the BARE dialog symbol and whose msgid/msgstr use "::"
    // to separate the Options tree CATEGORY from the page name ("Player::General"). Every page sharing
    // an English category must translate that prefix IDENTICALLY, or the built player's Options tree
    // splits into multiple branches for the same category. --
    {
        auto isBareDialogSymbol = [](const std::string& ctx) {
            if (ctx.rfind("IDD_", 0) != 0 || ctx.size() <= 4) return false;
            for (size_t i = 4; i < ctx.size(); ++i) {
                char c = ctx[i];
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
            }
            return true;
        };
        struct CatEntry { std::string msgctxt, msgid, englishCat, gotCat; };
        std::vector<CatEntry> withCat;   // msgstr HAS "::" -- category successfully split off
        for (const auto& e : m_po[RES_STRINGS].entries) {
            if (e.msgstr.empty() || dismissed(e.msgctxt, e.msgid)) continue;
            if (!isBareDialogSymbol(e.msgctxt)) continue;
            size_t sep = e.msgid.find("::");
            if (sep == std::string::npos) continue;   // not a page-title entry
            std::string englishCat = e.msgid.substr(0, sep);
            size_t gotSep = e.msgstr.find("::");
            if (gotSep == std::string::npos) {
                Row row{ e.msgctxt, e.msgid, RES_STRINGS };
                row.flag = Row::Flag::Category;
                row.evidence.Format(L"the \"::\" category separator is missing (the Options tree needs "
                                     L"\"%s::<page>\")", (LPCWSTR)CString(CA2W(englishCat.c_str(), CP_UTF8)));
                category.push_back(row);
                continue;
            }
            withCat.push_back({ e.msgctxt, e.msgid, englishCat, e.msgstr.substr(0, gotSep) });
        }
        // Group by English category; flag every entry in a group whose translated categories disagree.
        std::map<std::string, std::vector<size_t>> byEnglishCat;   // englishCat -> indices into withCat
        for (size_t i = 0; i < withCat.size(); ++i) byEnglishCat[withCat[i].englishCat].push_back(i);
        for (const auto& [englishCat, idxs] : byEnglishCat) {
            std::vector<std::string> variants;   // distinct translated categories, first-seen order
            for (size_t i : idxs) {
                const std::string& got = withCat[i].gotCat;
                if (std::find(variants.begin(), variants.end(), got) == variants.end())
                    variants.push_back(got);
            }
            if (variants.size() <= 1) continue;   // this language agrees -- nothing to flag
            CString variantList;
            for (size_t v = 0; v < variants.size(); ++v) {
                if (v) variantList += L" / ";
                variantList += L"\"" + CString(CA2W(variants[v].c_str(), CP_UTF8)) + L"\"";
            }
            CString evidence;
            evidence.Format(L"category \"%s\" is translated %zu different ways here: %s",
                             (LPCWSTR)CString(CA2W(englishCat.c_str(), CP_UTF8)), variants.size(),
                             (LPCWSTR)variantList);
            for (size_t i : idxs) {
                Row row{ withCat[i].msgctxt, withCat[i].msgid, RES_STRINGS };
                row.flag = Row::Flag::Category;
                row.evidence = evidence;
                category.push_back(row);
            }
        }
    }
    // -- fit, from cache (may be empty/not-yet-computed; EnsureFitScan already kicked off elsewhere —
    //    this function must not itself block or start scans) --
    if (auto it = m_fitCache.find(langKey); it != m_fitCache.end()) {
        for (const auto& f : it->second) {
            if (dismissed(f.msgctxt, f.msgid)) continue;
            Row row{ f.msgctxt, f.msgid, RES_DIALOGS };
            row.flag = f.kind; row.evidence = f.evidence;
            row.flagDialog = f.dialog; row.flagControlId = f.controlId;
            (f.kind == Row::Flag::FitHard ? hard : tight).push_back(row);
        }
        // Most-severe-first: overflowPx isn't threaded onto Row, but every hard-fit evidence string
        // embeds it ("overflows by 179px" / "group overflows by 46px (…)") — extract and sort
        // numerically descending (a lexical sort would rank "9px" above "179px").
        auto overflowPx = [](const CString& ev) -> int {
            int i = ev.Find(L"overflows by ");
            return i >= 0 ? _wtoi(ev.Mid(i + 13)) : 0;
        };
        std::sort(hard.begin(), hard.end(), [&](const Row& a, const Row& b) {
            int pa = overflowPx(a.evidence), pb = overflowPx(b.evidence);
            return pa != pb ? pa > pb : a.evidence < b.evidence;   // px desc, then stable-by-text
        });
    }
    m_rows.clear();   // PopulateList already cleared it, but be defensive: this can also be called
                      // directly from CommitEdit (targeted refresh), not just via PopulateList
    for (auto& r : hard) m_rows.push_back(std::move(r));
    for (auto& r : placeholder) m_rows.push_back(std::move(r));
    for (auto& r : accel) m_rows.push_back(std::move(r));
    for (auto& r : category) m_rows.push_back(std::move(r));
    for (auto& r : tight) m_rows.push_back(std::move(r));
    m_curRow = -1;
    m_list.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    m_list.SetItemCountEx((int)m_rows.size(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    m_list.EnsureVisible(0, FALSE);
    m_list.Invalidate();
    bool scanning = m_fitScanRunning && m_fitCache.find(langKey) == m_fitCache.end();
    size_t dismissedCount = 0;
    for (const auto& t : m_dismissed) if (std::get<0>(t) == langKey) ++dismissedCount;
    CString s;
    s.Format(L"Review: %zu hard overflow(s), %zu placeholder issue(s), %zu accelerator issue(s), "
             L"%zu category issue(s), %zu tight fit(s)%s. Dismissed: %zu.%s",
             hard.size(), placeholder.size(), accel.size(), category.size(), tight.size(),
             scanning ? L" (fit scan in progress)" : L"",
             dismissedCount,
             scanning ? L" Computing fit\x2026" : L"");
    SetStatus(s);
}

// M2: the "Generate AI suggestions" worklist -- every empty cell (Flag::None) across all 3 resources,
// PLUS every non-empty cell CellFlag() flags (a validation-failing ACCEPTED cell, per the plan §6d
// carve-out). Mirrors PopulateList's RES_UNTRANS scan for the "empty" half; CellFlag for the rest.
// Deliberately excludes the dialog-scoped duplicate-accelerator check (validate::rule_duplicate_
// accelerator) -- out of scope for M2's per-cell worklist (the M1 Review tab still surfaces it; this
// worklist just doesn't auto-generate fixes for it, to keep the per-cell generation shape simple).
std::vector<MainFrame::Row> MainFrame::BuildGenWorklist() {
    return BuildGenWorklistCore(m_po, m_fitCache.count(std::wstring(m_lang)) ? &m_fitCache[std::wstring(m_lang)] : nullptr);
}

// M4: static twin -- see the declaration in MainFrame.h for why (the ALL-languages batch worker builds
// a worklist per-language off the UI thread, from its OWN po snapshot + freshly-computed fit flags,
// never m_po/m_fitCache[m_lang]).
std::vector<MainFrame::Row> MainFrame::BuildGenWorklistCore(const mpctrans::PoFile poByRes[RES_COUNT],
                                                             const std::vector<ReviewFlag>* fitFlags) {
    std::vector<Row> out;
    for (int res = 0; res < RES_COUNT; ++res) {
        for (const auto& e : poByRes[res].entries) {
            if (e.msgid.empty()) continue;
            if (e.msgstr.empty()) { out.push_back(Row{ e.msgctxt, e.msgid, res }); continue; }
            CString evidence; long long fd = -1, fc = -1;
            Row::Flag flag = CellFlagCore(e.msgctxt, e.msgid, res, e.msgstr, evidence, fd, fc, fitFlags);
            if (flag == Row::Flag::None) continue;
            Row row{ e.msgctxt, e.msgid, res };
            row.flag = flag; row.evidence = evidence; row.flagDialog = fd; row.flagControlId = fc;
            out.push_back(row);
        }
    }
    return out;
}

// "Dismiss" — Review tab only: hide the selected flagged row for this language until the string or
// its translation changes again (a fresh CommitEdit re-adds it if the underlying issue is still there).
void MainFrame::OnDismissClicked() {
    if (m_curRow < 0 || m_curRow >= (int)m_rows.size() || m_tabs.GetCurSel() != RES_REVIEW) return;
    const Row& row = m_rows[m_curRow];
    m_dismissed.insert({ std::wstring(m_lang), row.msgctxt, row.msgid });
    PopulateList();   // rebuilds from PopulateReviewRows(), which filters m_dismissed
    m_edit.ClearString();
}

void MainFrame::OnSubmitPrCmd() { SubmitPr(); }

// Approach C: render the Dialogs surface from a live .rc (parsed + emitted) instead of the pinned
// bundle DLL, so upstream layout changes appear with no rebuild. For automation, $MPCTRANS_DEMO_RC
// points straight at the .rc (resource.h assumed beside it); otherwise a file picker is shown.
void MainFrame::OnRenderFromRc() {
    CString rc, dir;
    char* env = nullptr; size_t n = 0; _dupenv_s(&env, &n, "MPCTRANS_DEMO_RC");
    if (env && *env) { rc = CString(CA2W(env, CP_UTF8)); dir = rc.Left(rc.ReverseFind(L'\\')); }
    free(env);
    if (rc.IsEmpty()) {
        CFileDialog fd(TRUE, L"rc", nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                       L"Resource script (mpc-hc.rc)|*.rc|All files|*.*||", this);
        if (fd.DoModal() != IDOK) return;
        rc = fd.GetPathName(); dir = rc.Left(rc.ReverseFind(L'\\'));
    }
    CString h = dir + L"\\resource.h";
    if (!exists(h)) { MessageBox(L"resource.h must sit next to the .rc:\n" + h, L"Render from RC",
                                 MB_ICONERROR); return; }
    if (!m_preview.SetRcSource(rc, h)) { MessageBox(L"Could not parse " + rc, L"Render from RC",
                                                    MB_ICONERROR); return; }
    // Derive the control-index from the same parse so the list / click-to-edit / substitution pick
    // up new controls too (keep the pinned menu records — RC menus aren't parsed yet).
    m_rcIndex = ControlIndex::from_records(rc_dialog_records(m_preview.RcDialogs()), m_index.menus());
    m_useRcIndex = true;
    if (m_tabs.GetCurSel() != RES_DIALOGS) { m_tabs.SetCurSel(RES_DIALOGS); Layout(); }
    long long keep = m_curDialog;
    { char* d = nullptr; size_t dn = 0; _dupenv_s(&d, &dn, "MPCTRANS_DEMO_DLG");   // demo: jump to it
      if (d && *d) keep = _atoi64(d); free(d); }
    PopulateDialogCombo();
    for (int i = 0; i < (int)m_dlgIds.size(); ++i)
        if (m_dlgIds[i] == keep) { m_dlgCombo.SetCurSel(i); m_curDialog = keep; break; }
    PopulateList();
    RenderCurrentDialog();
    m_edit.ClearString();
    SetStatus(L"Preview from live RC (Approach C) — new controls now appear in the list and are "
              L"editable; their translation shows if the fetched .po already has it.");
}

void MainFrame::OnRenderFromBundle() {
    m_preview.ClearRcSource();
    m_useRcIndex = false;
    long long keep = m_curDialog;
    PopulateDialogCombo();
    for (int i = 0; i < (int)m_dlgIds.size(); ++i)
        if (m_dlgIds[i] == keep) { m_dlgCombo.SetCurSel(i); m_curDialog = keep; break; }
    PopulateList();
    RenderCurrentDialog();
    m_edit.ClearString();
    SetStatus(L"Preview from the bundled DLL (pinned @ " +
              (m_index.upstream_sha.empty() ? CString(L"bundle")
               : CString(CA2W(m_index.upstream_sha.substr(0, 8).c_str(), CP_UTF8))) + L").");
}

// Automation hook (no menu entry): select the dialog id in $MPCTRANS_DEMO_DLG and render it.
void MainFrame::OnDemoSelect() {
    char* env = nullptr; size_t n = 0; _dupenv_s(&env, &n, "MPCTRANS_DEMO_DLG");
    if (!env) return;
    long long id = _atoi64(env); free(env);
    for (int i = 0; i < (int)m_dlgIds.size(); ++i)
        if (m_dlgIds[i] == id) { m_dlgCombo.SetCurSel(i); m_curDialog = id; break; }
    PopulateList();
    RenderCurrentDialog();
    m_edit.ClearString();
}

void MainFrame::SubmitPr() {
    static const char* kRes[RES_COUNT] = { "dialogs", "menus", "strings" };
    size_t total = m_dirty[0].size() + m_dirty[1].size() + m_dirty[2].size();
    if (!m_checkedOut || total == 0) { MessageBox(L"No pending edits.", L"Submit PR"); return; }

    // hard-block on validation errors (warnings are the translator's call)
    for (int r = 0; r < RES_COUNT; ++r)
        for (const auto& key : m_dirty[r])
            if (auto* e = m_po[r].find(key.first, key.second))
                for (const auto& f : validate::check_entry(e->msgctxt, e->msgid, e->msgstr))
                    if (f.sev == validate::Severity::Error) {
                        MessageBox(L"Validation ERROR in " + CString(CA2W(key.first.c_str(), CP_UTF8)) +
                                   L":\n" + CString(CA2W(f.msg.c_str(), CP_UTF8)), L"Submit PR",
                                   MB_ICONERROR);
                        return;
                    }

    try {
        CWaitCursor wait;
        auto tok = github::load_token();
        if (!tok) {
            github::DeviceCode dc = github::request_device_code(config::DEVICE_SCOPE);
            github::Token t = github::poll_for_token(dc, [this](const github::DeviceCode& d) {
                ::ShellExecuteW(nullptr, L"open", CA2W(d.verification_uri.c_str(), CP_UTF8),
                                nullptr, nullptr, SW_SHOWNORMAL);
                MessageBox(L"A browser was opened at " +
                           CString(CA2W(d.verification_uri.c_str(), CP_UTF8)) +
                           L"\n\nEnter this code to authorize the Studio:\n\n        " +
                           CString(CA2W(d.user_code.c_str(), CP_UTF8)) +
                           L"\n\nClick OK AFTER you have authorized.", L"GitHub sign-in");
            });
            github::store_token(t);
            tok = t;
        }
        std::string translator = github::whoami(*tok);
        std::string lang = CW2A(m_lang, CP_UTF8);

        std::vector<github::FileEdit> files;
        for (int r = 0; r < RES_COUNT; ++r) {
            if (m_dirty[r].empty()) continue;
            std::string repo_path = std::string(config::PO_DIR) + "/mpc-hc." + lang + "." +
                                    kRes[r] + ".po";
            std::string latest = github::fetch_latest(*tok, repo_path);   // splice onto CURRENT upstream
            std::vector<PoEntry> edits;
            for (const auto& key : m_dirty[r])
                if (auto* e = m_po[r].find(key.first, key.second)) edits.push_back(*e);
            std::string spliced = PoFile::splice(latest, edits, translator);
            // Only submit files that actually differ from current upstream. splice() is byte-exact for
            // unchanged entries, so spliced==latest means these edits produce no real change (e.g. a
            // value that already matches upstream, or a key that's absent from this language's .po and
            // got dropped) -- skip it rather than commit an empty diff.
            if (spliced != latest) files.push_back({ repo_path, spliced });
        }
        if (files.empty()) {
            MessageBox(L"Your pending edits don't differ from the current upstream translations, so "
                       L"there's nothing to submit.\n\nThis happens when a cell was applied blank, "
                       L"already matches upstream, or the string isn't present in this language's .po "
                       L"file. No branch or pull request was created.",
                       L"Submit PR", MB_ICONINFORMATION);
            return;
        }
        std::string title = "Update " + lang + " translations (Translation Studio)";
        std::string url = github::open_translation_pr(*tok, lang, files, title);
        // The edits are committed to a branch on your fork; GitHub's "Open a pull request" compare
        // page is opened so you review the diff and click "Create pull request". Local drafts are
        // deliberately KEPT (not cleared) -- the PR isn't created until you click Create, so nothing
        // is lost if you don't; the branch also persists on your fork. Re-submitting makes a new
        // timestamped branch. Edits clear naturally once they land in upstream.
        SetStatus(L"Review your changes on GitHub, then click “Create pull request”.");
        ::ShellExecuteW(nullptr, L"open", CA2W(url.c_str(), CP_UTF8), nullptr, nullptr, SW_SHOWNORMAL);
        MessageBox(L"Your edits were pushed to a branch and GitHub's “Open a pull request” "
                   L"page was opened in your browser.\n\nReview the diff there and click "
                   L"“Create pull request” to submit — nothing is submitted until you do.",
                   L"Submit PR", MB_ICONINFORMATION);
    } catch (const std::exception& ex) {
        MessageBox(L"Submit failed:\n" + CString(CA2W(ex.what(), CP_UTF8)), L"Submit PR",
                   MB_ICONERROR);
    }
}

// "Suggest fix…" — the research panel's correction flow (see SuggestFixDlg + mpctrans::corrections).
// GitHub auth mirrors SubmitPr's device-flow path exactly (same load_token/request_device_code/
// poll_for_token/store_token sequence); the network submit itself runs on a worker thread (like
// StartPrefetch) so the UI stays responsive, handing the result back via WM_APP_SUGGESTFIX_DONE.
void MainFrame::OnSuggestFixClicked() {
    if (m_curRow < 0 || m_curRow >= (int)m_rows.size() || !m_haveCurInfo) return;
    if (m_suggestFixThread.joinable()) return;   // a submit is already in flight
    const Row row = m_rows[m_curRow];            // copy: m_curRow may move before the worker finishes

    CString msgctxt = CString(CA2W(row.msgctxt.c_str(), CP_UTF8));
    CString english = CString(CA2W(row.msgid.c_str(), CP_UTF8));
    CString meaning = CString(CA2W(m_curInfo.semantic_purpose.c_str(), CP_UTF8));
    CString function = CString(CA2W(m_curInfo.functional_purpose.c_str(), CP_UTF8));

    SuggestFixDlg dlg(this, msgctxt, english, meaning, function);
    if (dlg.DoModal() != IDOK) return;
    std::vector<corrections::Correction> changed = dlg.Changed();
    if (changed.empty()) return;   // dlg already told the user ("No changes to submit") and stayed open

    try {
        auto tok = github::load_token();
        if (!tok) {
            github::DeviceCode dc = github::request_device_code(config::DEVICE_SCOPE);
            github::Token t = github::poll_for_token(dc, [this](const github::DeviceCode& d) {
                ::ShellExecuteW(nullptr, L"open", CA2W(d.verification_uri.c_str(), CP_UTF8),
                                nullptr, nullptr, SW_SHOWNORMAL);
                MessageBox(L"A browser was opened at " +
                           CString(CA2W(d.verification_uri.c_str(), CP_UTF8)) +
                           L"\n\nEnter this code to authorize the Studio:\n\n        " +
                           CString(CA2W(d.user_code.c_str(), CP_UTF8)) +
                           L"\n\nClick OK AFTER you have authorized.", L"GitHub sign-in");
            });
            github::store_token(t);
            tok = t;
        }
        std::string author = github::whoami(*tok);

        // Apply the session overlay immediately -- the panel reflects the fix without waiting on the
        // PR (see m_researchOverrides) -- then re-render the research panel for the current row.
        auto& ov = m_researchOverrides[{ row.msgctxt, row.msgid }];
        for (const auto& c : changed) {
            if (c.field == corrections::Field::SemanticPurpose) ov.semantic_purpose = c.new_text;
            else ov.functional_purpose = c.new_text;
        }
        if (m_curRow >= 0 && m_curRow < (int)m_rows.size() && m_rows[m_curRow].msgctxt == row.msgctxt &&
            m_rows[m_curRow].msgid == row.msgid)
            SelectRow(m_curRow);

        SetStatus(L"Submitting research correction\x2026");
        m_btnSuggestFix.EnableWindow(FALSE);
        HWND hwnd = GetSafeHwnd();
        github::Token tokCopy = *tok;
        auto* work = new std::vector<corrections::Correction>(std::move(changed));
        m_suggestFixThread = std::thread([hwnd, tokCopy, author, work]() {
            auto* res = new SuggestFixResult;
            try {
                res->url = corrections::submit_research_correction(tokCopy, author, *work);
                res->ok = true;
            } catch (const std::exception& ex) {
                res->ok = false; res->error = ex.what();
            }
            delete work;
            if (!::PostMessage(hwnd, WM_APP_SUGGESTFIX_DONE, (WPARAM)res, 0)) delete res;
        });
    } catch (const std::exception& ex) {
        MessageBox(L"Suggest fix failed:\n" + CString(CA2W(ex.what(), CP_UTF8)), L"Suggest a correction",
                  MB_ICONERROR);
    }
}

// UI thread: report the background PR submit's outcome (see OnSuggestFixClicked) — same presentation
// as SubmitPr's success/error path.
LRESULT MainFrame::OnSuggestFixDone(WPARAM wp, LPARAM) {
    std::unique_ptr<SuggestFixResult> res((SuggestFixResult*)wp);
    if (m_suggestFixThread.joinable()) m_suggestFixThread.join();
    if (m_btnSuggestFix.GetSafeHwnd()) m_btnSuggestFix.EnableWindow(m_haveCurInfo);
    if (res->ok) {
        // A branch with the correction was created on your fork; open GitHub's "Open a pull request"
        // compare page so you review and click "Create pull request" (nothing is PR'd until you do).
        SetStatus(L"Review your correction on GitHub, then click “Create pull request”.");
        ::ShellExecuteW(nullptr, L"open", CA2W(res->url.c_str(), CP_UTF8), nullptr, nullptr, SW_SHOWNORMAL);
        MessageBox(L"A branch with your correction was pushed and GitHub's “Open a pull request” "
                   L"page was opened.\n\nReview it and click “Create pull request” to submit.",
                   L"Suggest a correction", MB_ICONINFORMATION);
    } else {
        SetStatus(L"Correction submit failed.");
        MessageBox(L"Suggest fix failed:\n" + CString(CA2W(res->error.c_str(), CP_UTF8)),
                  L"Suggest a correction", MB_ICONERROR);
    }
    return 0;
}

// ---------- On-demand "Suggest with AI" (EditPanel's IDC_EP_AI_SUGGEST button) ----------

// Cred target for a provider's stored API key -- the 4 distinct targets noted in ai_client.h's header
// comment (one per provider id: claude/gpt/glm/openrouter). Provider ids are plain ASCII.
static std::wstring AiCredTarget(const std::string& provider_id) {
    return std::wstring(L"mpc-hc-translation-studio/ai/") + std::wstring(provider_id.begin(), provider_id.end());
}

// A short table of English names for the ISO codes this project ships language packs for (there's no
// existing lookup for this anywhere in the codebase -- the language combo only ever holds raw codes).
// Falls back to the code itself when not listed, which degrades gracefully rather than failing.
static CString LanguageEnglishName(const CString& code) {
    static const std::map<CString, const wchar_t*> kNames = {
        { L"ar", L"Arabic" }, { L"be", L"Belarusian" }, { L"bg", L"Bulgarian" }, { L"ca", L"Catalan" },
        { L"cs", L"Czech" }, { L"da", L"Danish" }, { L"de", L"German" }, { L"el", L"Greek" },
        { L"en", L"English" }, { L"es", L"Spanish" }, { L"et", L"Estonian" }, { L"eu", L"Basque" },
        { L"fa", L"Persian" }, { L"fi", L"Finnish" }, { L"fr", L"French" }, { L"gl", L"Galician" },
        { L"he", L"Hebrew" }, { L"hi", L"Hindi" }, { L"hr", L"Croatian" }, { L"hu", L"Hungarian" },
        { L"id", L"Indonesian" }, { L"it", L"Italian" }, { L"ja", L"Japanese" }, { L"ka", L"Georgian" },
        { L"ko", L"Korean" }, { L"lt", L"Lithuanian" }, { L"lv", L"Latvian" }, { L"mk", L"Macedonian" },
        { L"nl", L"Dutch" }, { L"no", L"Norwegian" }, { L"pl", L"Polish" }, { L"pt", L"Portuguese" },
        { L"pt-br", L"Portuguese (Brazil)" }, { L"ro", L"Romanian" }, { L"ru", L"Russian" },
        { L"sk", L"Slovak" }, { L"sl", L"Slovenian" }, { L"sq", L"Albanian" }, { L"sr", L"Serbian" },
        { L"sv", L"Swedish" }, { L"th", L"Thai" }, { L"tr", L"Turkish" }, { L"uk", L"Ukrainian" },
        { L"vi", L"Vietnamese" }, { L"zh-cn", L"Chinese (Simplified)" }, { L"zh-tw", L"Chinese (Traditional)" },
    };
    auto it = kNames.find(code);
    return it != kNames.end() ? CString(it->second) : code;
}

// "IDS_INFOBAR_LOCATION" -> "IDS_INFOBAR_" (the first two underscore-delimited tokens). Used to group
// a loose (non-dialog) string with its same-family siblings when it has no dialog of its own to pull
// already-translated pairs from -- confirmed against real upstream msgctxt values (mpc-hc.*.strings.po
// uses this ALLCAPS_UNDERSCORE convention throughout, e.g. IDS_AUTOPLAY_PLAYVIDEO / IDS_CMD_*), NOT a
// dotted convention. Falls back to the whole msgctxt when there's no second underscore to group by.
static std::string MsgctxtFamilyPrefix(const std::string& ctx) {
    size_t u1 = ctx.find('_');
    if (u1 == std::string::npos) return ctx;
    size_t u2 = ctx.find('_', u1 + 1);
    return u2 == std::string::npos ? ctx : ctx.substr(0, u2 + 1);
}

// v3-ported system prompt (ported from an earlier research prototype's build_prompt()) -- rules
// re-ordered so the Meaning line (in the USER prompt, see BuildAiUserPrompt) is
// authoritative over reference/example grounding, plus Studio-specific invariants v3's Python harness
// didn't need (the JSON-response contract; command-line switch tokens; tab preservation). The literal
// full-width CJK punctuation glyphs below are UTF-8 source chars (the project compiles with /utf-8).
static std::string AiSystemPrompt() {
    return
        "You localize UI strings for MPC-HC, a Windows media player. Respond with ONLY a JSON "
        "object of the exact shape {\"translation\": \"...\"} and nothing else -- no markdown code "
        "fences, no commentary before or after it.\n"
        "RULES:\n"
        "(1) A 'Meaning' line, when present, gives the exact in-app sense and is AUTHORITATIVE: "
        "your translation MUST match it. Same-dialog/family examples are grounding only and may "
        "reflect the WRONG sense -- when they conflict with the Meaning, follow the Meaning.\n"
        "(2) Be MAXIMALLY CONCISE: the shortest natural wording a native speaker accepts in a "
        "cramped UI control. Do not add words the English omits.\n"
        "(3) Match the English source's punctuation style exactly -- ASCII/half-width punctuation "
        "( : , . ( ) % - ) and the same trailing colon/period as the source; never substitute "
        "full-width CJK punctuation ( ： ， 。 （ ） ) even when translating "
        "into a CJK language.\n"
        "(4) Preserve every printf-style format specifier (%s, %d, %ld, %.2f, %%, %1, %i64d, ...) "
        "exactly as written and in the same order -- they are substituted at runtime and must "
        "survive verbatim.\n"
        "(5) Preserve the '&' accelerator/mnemonic convention: a single '&' before a letter marks "
        "that letter as the keyboard mnemonic -- choose a sensible, non-conflicting mnemonic letter "
        "from the TRANSLATED text (it need not match the English letter), keeping exactly one '&' "
        "in the output; a literal '&&' in the source is an escaped ampersand and must stay '&&' "
        "verbatim.\n"
        "(6) Preserve leading/trailing whitespace, embedded tab characters (column alignment), and "
        "line breaks exactly as they appear -- do not add, remove, or move them.\n"
        "(7) Do not translate command-line switch tokens: in strings shaped \"/switch\\tdescription\", "
        "the text before the first tab is a literal flag and must be copied unchanged; only the "
        "description after the tab is translated.\n"
        "(8) Match the terminology and phrasing already used in the same-dialog/family examples "
        "provided, so vocabulary stays consistent with the rest of the UI, EXCEPT where the Meaning "
        "(rule 1) says otherwise.\n"
        "If the user message notes this translation is being fixed for a specific problem (e.g. it "
        "overflows its control, or is missing a placeholder), that fix instruction is also "
        "mandatory and takes priority over rule (2)'s general concision guidance where they'd "
        "conflict.";
}

// Assembles the user prompt for `row`: target language, context, research fields, up to 8
// already-translated pairs from the same dialog (or, for a loose string, the same msgctxt family),
// and fidelity-ranked cross-language sibling translations -- everything OnSuggestAiClicked needs to
// hand the model besides the system prompt. (v3.2: the reference-match translation pool that
// used to ride alongside these is no longer included -- see kAiPromptVersion's comment.) Bounded to
// roughly 4KB: same-dialog/family pairs and sibling lines are collected first, then -- if the
// assembled prompt would exceed the budget -- the LONGEST remaining lines are dropped first
// (deterministic: sorted by length descending, removed until it fits), so a handful of short, cheap
// examples survive rather than the whole section being cut for one long outlier.
//
// This is a thin wrapper: it gathers `this` members into explicit locals and hands off to
// BuildAiUserPromptCore (static, no `this`) -- see that function's declaration in MainFrame.h for why
// (the M2 background generation worker needs the IDENTICAL v3 prompt-assembly logic off the UI
// thread, after snapshotting these same inputs by value).
CString MainFrame::BuildAiUserPrompt(const Row& row) {
    CString langCode = m_lang;
    CString langName = LanguageEnglishName(langCode);
    std::vector<CString> siblings = BuildSiblingLines(row, m_lang);
    return BuildAiUserPromptCore(langCode, langName, row, m_haveCurInfo, m_curInfo, m_curHint, siblings, Idx(), m_po);
}

// "pt_BR"/"zh_CN" (source-DB convention: underscore, mixed case) -> "pt-br"/"zh-cn" (Studio
// convention: hyphen, lowercase ASCII) so a `lab` language_fidelity code can be compared against
// m_lang / m_sessionPo keys. Byte-level ASCII transform only (same posture as
// confidence::normalize_for_agreement).
static std::wstring NormalizeLangCode(const std::string& lab) {
    std::wstring out(CA2W(lab.c_str(), CP_UTF8));
    for (wchar_t& c : out) {
        if (c == L'_') c = L'-';
        else if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
    }
    return out;
}

// Now a thin formatter over BuildSiblingPairs (below) -- pure extract-method refactor, the walk/lookup
// logic itself didn't move. Verified byte-identical output by inspection: the loop bound (3), the
// target-language exclusion, the m_sessionPo/PoEntry lookup, and the "%s (%s): \"%s\"" line shape are
// all unchanged, just split across the two functions instead of inlined in one.
std::vector<CString> MainFrame::BuildSiblingLines(const Row& row, const CString& targetLang) const {
    std::vector<CString> lines;
    for (const auto& p : BuildSiblingPairs(row, targetLang))
        lines.push_back(p.first + L" (" + LanguageEnglishName(p.first) + L"): \"" + p.second + L"\"");
    return lines;
}

// AI-prep export (JSONL): see the declaration in MainFrame.h for why this is factored out of
// BuildSiblingLines separately from that function's own formatting. Identical walk to the pre-refactor
// BuildSiblingLines body: up to 3 fidelity-ranked languages (m_fidelity, already sorted desc), skip the
// target language itself, skip languages not prefetched this session (m_sessionPo miss), skip an empty
// translation, else emit (normalized code, msgstr) raw -- no display formatting here.
std::vector<std::pair<CString, CString>> MainFrame::BuildSiblingPairs(const Row& row, const CString& targetLang) const {
    std::vector<std::pair<CString, CString>> pairs;
    std::wstring target(targetLang);
    for (wchar_t& c : target) if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';   // normalize case only
    for (const auto& lf : m_fidelity) {
        if (pairs.size() >= 3) break;
        std::wstring code = NormalizeLangCode(lf.first);
        if (code == target) continue;
        auto it = m_sessionPo.find(code);
        if (it == m_sessionPo.end()) continue;   // not prefetched this session -- graceful skip
        const PoEntry* e = it->second[row.res].find(row.msgctxt, row.msgid);
        if (!e || e->msgstr.empty()) continue;
        CString codeCs(code.c_str());
        pairs.push_back({ codeCs, CString(CA2W(e->msgstr.c_str(), CP_UTF8)) });
    }
    return pairs;
}

// v3.3: per-role gloss for the "Translator hints:" block's Role line -- Studio-authored context for
// the 8 `role` values the hint generator can emit (see string_hints' schema comment in enrichment.h),
// not part of the mined data itself. Unknown/empty role -> nullptr (no parenthetical).
static const wchar_t* RoleGloss(const std::string& role) {
    if (role == "command")     return L"render as an action, not a noun";
    if (role == "label")       return L"a static label, not an action";
    if (role == "option")      return L"an option/setting name";
    if (role == "title")       return L"a window or dialog title";
    if (role == "status")      return L"a transient status message";
    if (role == "format-name") return L"the name of a media/file format";
    if (role == "unit")        return L"a unit of measurement";
    if (role == "value")       return L"a literal value or number";
    return nullptr;
}

static CString JoinUtf8Tokens(const std::vector<std::string>& tokens) {
    CString out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) out += L", ";
        out += CString(CA2W(tokens[i].c_str(), CP_UTF8));
    }
    return out;
}

// v3.3: renders `hint` (see mpctrans::CoreEnrichment::hint_for) as a compact block for
// BuildAiUserPromptCore -- only non-empty fields are emitted, one line each. Returns an empty CString
// if the hint has nothing worth showing (every field empty), so the caller's blank append is a no-op.
static CString BuildTranslatorHintsBlock(const mpctrans::StringHint& hint) {
    CString body;
    if (!hint.role.empty()) {
        body += L"- Role: " + CString(CA2W(hint.role.c_str(), CP_UTF8));
        if (const wchar_t* gloss = RoleGloss(hint.role)) body += L" (" + CString(gloss) + L")";
        body += L"\r\n";
    }
    if (!hint.keep_verbatim.empty())
        body += L"- Keep exactly: " + JoinUtf8Tokens(hint.keep_verbatim) + L"\r\n";
    if (!hint.soft_verbatim.empty())
        body += L"- May keep or localize: " + JoinUtf8Tokens(hint.soft_verbatim) + L"\r\n";
    if (!hint.referent.empty())
        body += L"- Refers to: " + CString(CA2W(hint.referent.c_str(), CP_UTF8)) + L"\r\n";
    if (!hint.agreement.empty() && hint.agreement != "n/a")
        body += L"- Agreement: " + CString(CA2W(hint.agreement.c_str(), CP_UTF8)) + L"\r\n";
    if (!hint.notes.empty())
        body += L"- Note: " + CString(CA2W(hint.notes.c_str(), CP_UTF8)) + L"\r\n";
    if (body.IsEmpty()) return body;
    return L"\r\nTranslator hints:\r\n" + body;
}

CString MainFrame::BuildAiUserPromptCore(const CString& langCode, const CString& langName, const Row& row,
                                         bool haveInfo, const StringInfo& info,
                                         const std::optional<mpctrans::StringHint>& hint,
                                         const std::vector<CString>& siblingLines,
                                         const ControlIndex& idx, const PoFile poByRes[RES_COUNT]) {
    CString msgctxt = CString(CA2W(row.msgctxt.c_str(), CP_UTF8));
    CString english = CString(CA2W(row.msgid.c_str(), CP_UTF8));

    CString header;
    header += L"Target language: " + langCode + L" (" + langName + L")\r\n";
    header += L"Context (msgctxt): " + msgctxt + L"\r\n";
    header += L"English source:\r\n" + english + L"\r\n";
    if (haveInfo) {
        auto add = [&](const wchar_t* label, const std::string& v) {
            if (!v.empty()) header += CString(label) + CString(CA2W(v.c_str(), CP_UTF8)) + L"\r\n";
        };
        // Same fields EditPanel::ShowString's `info` panel already shows for this string. Meaning is
        // deliberately NOT here -- it's appended LAST, marked authoritative (see below), the v3-ported
        // structural finding: reference/example grounding should be read BEFORE the authoritative sense.
        add(L"Where: ", info.ui_location);
        add(L"Type: ", info.ui_type);
    }

    std::vector<CString> pairLines;
    long long dlg = -1;
    for (const auto& r : idx.dialogs())
        if (r.msgctxt == row.msgctxt && r.msgid == row.msgid) { dlg = r.dialog; break; }
    if (dlg >= 0) {
        for (const auto& r : idx.dialogs()) {
            if (r.dialog != dlg || (r.msgctxt == row.msgctxt && r.msgid == row.msgid)) continue;
            if (const PoEntry* e = poByRes[RES_DIALOGS].find(r.msgctxt, r.msgid))
                if (!e->msgstr.empty())
                    pairLines.push_back(L"\"" + CString(CA2W(r.msgid.c_str(), CP_UTF8)) + L"\" -> \"" +
                                        CString(CA2W(e->msgstr.c_str(), CP_UTF8)) + L"\"");
            if (pairLines.size() >= 8) break;
        }
    } else {
        std::string family = MsgctxtFamilyPrefix(row.msgctxt);
        for (const auto& e : poByRes[row.res].entries) {
            if (e.msgctxt == row.msgctxt || e.msgstr.empty()) continue;
            if (MsgctxtFamilyPrefix(e.msgctxt) != family) continue;
            pairLines.push_back(L"\"" + CString(CA2W(e.msgid.c_str(), CP_UTF8)) + L"\" -> \"" +
                                CString(CA2W(e.msgstr.c_str(), CP_UTF8)) + L"\"");
            if (pairLines.size() >= 8) break;
        }
    }

    // ~4KB budget: drop the longest remaining pair/sibling line first until the assembled prompt
    // fits (or every extra line has been dropped). See the function comment for the rationale.
    const int kBudget = 4096;
    std::vector<bool> keepPair(pairLines.size(), true), keepSib(siblingLines.size(), true);
    struct Item { int pool; size_t idx; int len; };   // pool 0 = pairs, 1 = siblings
    std::vector<Item> items;
    for (size_t i = 0; i < pairLines.size(); ++i) items.push_back({ 0, i, pairLines[i].GetLength() });
    for (size_t i = 0; i < siblingLines.size(); ++i) items.push_back({ 1, i, siblingLines[i].GetLength() });
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.len > b.len; });
    auto assembledLen = [&]() {
        int total = header.GetLength();
        for (size_t i = 0; i < pairLines.size(); ++i) if (keepPair[i]) total += pairLines[i].GetLength() + 2;
        for (size_t i = 0; i < siblingLines.size(); ++i) if (keepSib[i]) total += siblingLines[i].GetLength() + 2;
        return total;
    };
    size_t next = 0;
    while (assembledLen() > kBudget && next < items.size()) {
        const Item& it = items[next++];
        (it.pool == 0 ? keepPair[it.idx] : keepSib[it.idx]) = false;
    }

    CString prompt = header;
    bool anyPair = false;
    for (size_t i = 0; i < pairLines.size(); ++i) if (keepPair[i]) { anyPair = true; break; }
    if (anyPair) {
        // Same-dialog/family same-language grounding (terminology consistency); fidelity-ranked
        // CROSS-language siblings (M3) ride in separately below via siblingLines.
        prompt += L"\r\nAlready-translated strings from the same " + CString(dlg >= 0 ? L"dialog" : L"family") +
                 L" -- grounding only, may be the WRONG sense; the MEANING below is authoritative:\r\n";
        for (size_t i = 0; i < pairLines.size(); ++i) if (keepPair[i]) prompt += pairLines[i] + L"\r\n";
    }
    bool anySib = false;
    for (size_t i = 0; i < siblingLines.size(); ++i) if (keepSib[i]) { anySib = true; break; }
    if (anySib) {
        prompt += L"\r\nSIBLING TRANSLATIONS (same string, high-fidelity languages -- grounding only, "
                 L"target language may legitimately differ; the MEANING below is authoritative):\r\n";
        for (size_t i = 0; i < siblingLines.size(); ++i) if (keepSib[i]) prompt += siblingLines[i] + L"\r\n";
    }
    if (haveInfo && !info.semantic_purpose.empty())
        prompt += L"\r\n>>> MEANING (authoritative -- translate to match THIS sense): " +
                  CString(CA2W(info.semantic_purpose.c_str(), CP_UTF8)) + L"\r\n";
    if (hint) prompt += BuildTranslatorHintsBlock(*hint);
    switch (row.flag) {
        case Row::Flag::FitHard:
        case Row::Flag::FitTight:
            prompt += L"\r\n>>> FIX REQUIRED: the current translation overflows its control -- " +
                      row.evidence + L". Produce a SHORTER translation that fits, preserving the "
                      L"meaning above.\r\n";
            break;
        case Row::Flag::Placeholder:
            prompt += L"\r\n>>> FIX REQUIRED: " + row.evidence + L". Produce a corrected translation "
                      L"that preserves every placeholder from the English source exactly.\r\n";
            break;
        case Row::Flag::Accelerator:
            prompt += L"\r\n>>> FIX REQUIRED: " + row.evidence + L". Produce a corrected translation "
                      L"with a non-conflicting accelerator letter.\r\n";
            break;
        case Row::Flag::Category:
            prompt += L"\r\n>>> FIX REQUIRED: " + row.evidence + L". Use exactly the SAME translation for "
                      L"the category prefix as the other pages in this category, keeping the \"::\" "
                      L"separator so the Options tree stays a single branch.\r\n";
            break;
        default:
            break;
    }
    prompt += L"\r\nRespond with only the JSON object described in the system prompt.";
    return prompt;
}

// ---------- "Export AI-prep context" (JSONL): dump the SAME context BuildAiUserPromptCore assembles,
// so a translator can hand it to an external AI tool. See the feature's task brief for the exact
// DECIDED SCOPE: English + Meaning (authoritative) + Function + ui_type/ui_location + fidelity-ranked
// sibling translations + available px + the current translation -- deliberately EXCLUDING private
// source data that isn't distributed with this repo (licensing). `glossary` is always `[]` (see
// AiPrepContext's comment in MainFrame.h for why). (v3.3: BuildAiUserPromptCore's own generated user
// prompt may additionally carry a "Translator hints:" block right after the Meaning line when
// mpctrans::CoreEnrichment::hint_for() has data for the string -- that hint layer is NOT part of this
// export's DECIDED SCOPE and is not reflected in AiPrepContext/ToJsonLine below.) ----------

// Serializes one AiPrepContext to a compact JSON line (nlohmann::json handles all the UTF-8 escaping --
// no hand-rolled string escaping here, per the task's explicit instruction).
std::string MainFrame::ToJsonLine(const AiPrepContext& ctx) {
    nlohmann::json j;
    j["msgctxt"] = ctx.msgctxt;
    j["msgid"] = ctx.msgid;
    j["current"] = ctx.current;
    j["meaning"] = ctx.meaning;
    j["function"] = ctx.function;
    j["ui_type"] = ctx.ui_type;
    j["ui_location"] = ctx.ui_location;
    nlohmann::json siblings = nlohmann::json::object();
    for (const auto& p : ctx.siblings) siblings[p.first] = p.second;
    j["siblings"] = siblings;
    j["glossary"] = nlohmann::json::array();   // always empty -- no glossary reader exists (see above)
    j["available_px"] = ctx.available_px ? nlohmann::json(*ctx.available_px) : nlohmann::json(nullptr);
    return j.dump();
}

// See the declaration in MainFrame.h for the threading contract (pure, no `this`, safe off the UI
// thread). CW2A/CP_UTF8 for the CString halves of `siblings` -- everything else here is already a
// UTF-8 std::string (PoEntry::msgstr, StringInfo's fields), so it goes to the struct untouched, per the
// task's own note that most of this data needs no CString round-trip at all.
MainFrame::AiPrepContext MainFrame::BuildAiPrepContext(const Row& row, const std::string& current,
                                                        bool haveInfo, const StringInfo& info,
                                                        const std::vector<std::pair<CString, CString>>& siblings,
                                                        std::optional<int> availablePx) {
    AiPrepContext ctx;
    ctx.msgctxt = row.msgctxt;
    ctx.msgid = row.msgid;
    ctx.current = current;
    if (haveInfo) {
        ctx.meaning = info.semantic_purpose;
        ctx.function = info.functional_purpose;
        ctx.ui_type = info.ui_type;
        ctx.ui_location = info.ui_location;
    }
    for (const auto& p : siblings)
        ctx.siblings.push_back({ std::string(CW2A(p.first, CP_UTF8)), std::string(CW2A(p.second, CP_UTF8)) });
    ctx.available_px = availablePx;
    return ctx;
}

// See the declaration in MainFrame.h for the full rationale (mirrors EnsureFitScan's off-screen
// never-shown WS_POPUP host technique, but keeps EVERY measurement instead of only overflowing ones).
std::map<std::pair<std::string, std::string>, int> MainFrame::MeasureAvailablePx(
        const std::set<long long>& dialogIds, const ControlIndex& idx, const PoFile& dialogsPo,
        const CString& neutralDll, bool useRc, const std::vector<RcDialog>& rcDialogs) {
    std::map<std::pair<std::string, std::string>, int> out;
    CWnd host;
    host.CreateEx(0, AfxRegisterWndClass(0, nullptr, nullptr, nullptr), L"",
                 WS_POPUP, 0, 0, 10, 10, nullptr, nullptr);
    LivePreview lp;
    lp.LoadNeutralDll(neutralDll);
    if (useRc && !rcDialogs.empty()) lp.SetRcDialogs(rcDialogs);
    for (long long dialogId : dialogIds) {
        HWND dlg = lp.RenderDialog(dialogId, &host, idx, dialogsPo);
        if (!dlg) continue;
        for (const auto& m : lp.MeasureFit(dlg, dialogId, idx)) {
            // Group-aware selection, identical to AppendFitFlags' own -- a chained control's individual
            // availablePx is misleading; the group's summed width is what actually constrains the text.
            int avail = m.grouped ? m.groupAvailablePx : m.availablePx;
            if (avail <= 0) continue;   // "not measurable" -- available_px stays null for this cell
            out[{ m.msgctxt, m.msgid }] = avail;
        }
        lp.DestroyPreview();
    }
    return out;
}

// UI-thread only -- see the declaration in MainFrame.h (mirrors BuildGenContext's rationale, leaner
// shape: no LangPack/refs, since the reference corpus is out of scope for this export).
std::map<std::pair<std::string, std::string>, MainFrame::ExportCtx>
MainFrame::BuildExportContext(const CString& langCode, const std::vector<Row>& worklist) {
    std::map<std::pair<std::string, std::string>, ExportCtx> ctxByKey;
    for (const auto& row : worklist) {
        auto key = std::make_pair(row.msgctxt, row.msgid);
        if (ctxByKey.count(key)) continue;
        ExportCtx ec;
        if (m_haveCore) {
            if (auto si = m_core.by_key(row.msgctxt, row.msgid)) {
                ec.info = *si; ec.haveInfo = true;
                auto ov = m_researchOverrides.find(key);
                if (ov != m_researchOverrides.end()) {
                    if (ov->second.semantic_purpose) ec.info.semantic_purpose = *ov->second.semantic_purpose;
                    if (ov->second.functional_purpose) ec.info.functional_purpose = *ov->second.functional_purpose;
                }
            }
        }
        ec.siblings = BuildSiblingPairs(row, langCode);   // fidelity-ranked cross-language grounding
        ctxByKey.emplace(std::move(key), std::move(ec));
    }
    return ctxByKey;
}

// Filesystem-safe default filename stem from a msgctxt (an IDS_.../IDC_...-style C symbol -- already
// filesystem-safe in practice per this codebase's naming convention, but defensively strip the reserved
// Windows filename characters anyway rather than assume it).
static CString SanitizeForFilename(const CString& s) {
    static const wchar_t* kReserved = L"\\/:*?\"<>|";
    CString out = s;
    for (int i = 0; i < out.GetLength(); ++i)
        if (wcschr(kReserved, out[i])) out.SetAt(i, L'_');
    return out;
}

// "<name>.jsonl" -> "<name>.prompt.md" (the companion file sits right next to the JSONL export);
// anything else just gets ".prompt.md" appended, per the task's fallback rule.
static CString PromptSpecPathFor(const CString& jsonlPath) {
    if (jsonlPath.Right(6).CompareNoCase(L".jsonl") == 0)
        return jsonlPath.Left(jsonlPath.GetLength() - 6) + L".prompt.md";
    return jsonlPath + L".prompt.md";
}

// Companion `.prompt.md` file emitted alongside every AI-prep export: a short "how to use" note, plus
// the EXACT v3.1 SYSTEM prompt text -- via AiSystemPrompt() itself, never retyped/paraphrased, so this
// file can never drift from the real prompt the Studio's own "Suggest with AI" feature sends.
static std::string BuildAiPrepPromptSpec() {
    std::ostringstream out;
    out <<
        "# AI-prep context \xe2\x80\x94 how to use\n\n"
        "Each line of the companion `.jsonl` file is one string's translation context: the English "
        "source, the authoritative Meaning + Function (from this project's semantic research \xe2\x80\x94 "
        "not the raw UI category), `ui_type`/`ui_location`, up to 3 fidelity-ranked SIBLING "
        "translations (the same string in tr/ro/pl/fr/ja/de \xe2\x80\x94 whichever were loaded this "
        "session \xe2\x80\x94 as grounding, not ground truth), an always-empty `glossary` array (no "
        "curated glossary is available to this export \xe2\x80\x94 see below), an `available_px` width "
        "(a number, or `null` when the string has no on-screen control \xe2\x80\x94 e.g. a menu item, "
        "or a loose string like an OSD/status-bar message), and the `current` translation (empty for "
        "an untranslated/void cell).\n\n"
        "Build your translation prompt the same way MPC-HC Translation Studio's own \"Suggest with AI\" "
        "feature does:\n"
        "- **Meaning is AUTHORITATIVE** \xe2\x80\x94 if a sibling translation (or anything else) "
        "conflicts with it, follow the Meaning.\n"
        "- **Be maximally concise** \xe2\x80\x94 the shortest natural wording a native speaker accepts "
        "in a cramped UI control; if `available_px` is set, the translation must fit that width when "
        "rendered in the target script's font.\n"
        "- **Preserve every printf-style format specifier** (`%s`, `%d`, `%ld`, `%.2f`, `%%`, `%1`, "
        "`%i64d`, ...) exactly, in the same order.\n"
        "- **Preserve the `&` accelerator/mnemonic convention** \xe2\x80\x94 exactly one `&` in the "
        "output, a sensible non-conflicting letter chosen from the TRANSLATED text; a literal `&&` in "
        "the source stays `&&` verbatim.\n"
        "- **Preserve leading/trailing whitespace, embedded tabs, and line breaks** exactly as they "
        "appear.\n"
        "- **Match the English source's punctuation style** (ASCII/half-width, never full-width CJK "
        "punctuation) and the terminology already used in the siblings, except where the Meaning says "
        "otherwise.\n\n"
        "`glossary` is always `[]` in this export: MPC-HC Translation Studio has no glossary reader "
        "today \xe2\x80\x94 no curated glossary/termbase is bundled or wired into any export. If you "
        "have your own glossary/termbase, apply it yourself alongside this context.\n\n"
        "The full v3.1 SYSTEM prompt this project's own AI-suggestion feature sends \xe2\x80\x94 "
        "verbatim, so this file can never drift from the real prompt:\n\n"
        "```\n" << AiSystemPrompt() << "\n```\n";
    return out.str();
}

// File > "Export AI-prep context for this string..." -- dumps the SAME structured context the
// on-demand "Suggest with AI" path already builds for the CURRENTLY SELECTED row (English + Meaning +
// Function + ui_type/ui_location via m_curInfo/m_haveCurInfo, already resolved by SelectRow including
// any accepted research correction -- cheaper than re-querying m_core, same data per the task's own
// note) plus fidelity-ranked siblings (BuildSiblingPairs) and available_px (a single-dialog,
// synchronous MeasureAvailablePx call -- fast, so no worker thread needed here) as one JSONL line, plus
// a companion `.prompt.md`. Guard mirrors OnSuggestAiClicked's existing convention.
void MainFrame::OnExportAiContextString() {
    if (m_curRow < 0 || m_curRow >= (int)m_rows.size()) return;
    const Row row = m_rows[m_curRow];   // copy: cheap, keeps the rest of this function self-contained

    // available_px: only strings that live in a rendered dialog have an "available width" -- menu
    // items, command-line help, and other loose strings have none, so DialogForString returns -1 for
    // them and this stays std::nullopt (-> JSON null, never a missing key -- see ToJsonLine).
    std::optional<int> availablePx;
    long long dlg = DialogForString(row.msgctxt, row.msgid);
    if (dlg >= 0) {
        std::set<long long> ids{ dlg };
        auto measured = MeasureAvailablePx(ids, Idx(), m_po[RES_DIALOGS], m_bundle.neutral_dll,
                                           m_preview.UsingRc(), m_preview.RcDialogs());
        auto it = measured.find({ row.msgctxt, row.msgid });
        if (it != measured.end()) availablePx = it->second;
    }

    const PoFile* po = PoFor(row.msgctxt, row.msgid);
    const PoEntry* e = po ? po->find(row.msgctxt, row.msgid) : nullptr;
    std::string current = e ? e->msgstr : std::string();   // "" for a void cell, per the schema

    AiPrepContext ctx = BuildAiPrepContext(row, current, m_haveCurInfo, m_curInfo,
                                           BuildSiblingPairs(row, m_lang), availablePx);
    std::string jsonlLine = ToJsonLine(ctx) + "\n";

    // Automation escape hatch, same convention as OnRenderFromRc's $MPCTRANS_DEMO_RC -- when set, use
    // the path directly and skip the (untestable-in-CI) common file-save dialog.
    CString path;
    char* envPath = nullptr; size_t envLen = 0;
    _dupenv_s(&envPath, &envLen, "MPCTRANS_EXPORT_PATH");
    if (envPath && *envPath) path = CString(CA2W(envPath, CP_UTF8));
    free(envPath);
    if (path.IsEmpty()) {
        CString defName = SanitizeForFilename(CString(CA2W(row.msgctxt.c_str(), CP_UTF8))) + L".ai-context.jsonl";
        CFileDialog fd(FALSE, L"jsonl", defName, OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY,
                       L"JSON Lines (*.jsonl)|*.jsonl|All files|*.*||", this);
        if (fd.DoModal() != IDOK) return;
        path = fd.GetPathName();
    }

    write_file_raw(path, jsonlLine);
    write_file_raw(PromptSpecPathFor(path), BuildAiPrepPromptSpec());
    SetStatus(L"Exported AI-prep context -> " + path);
}

// File > "Export AI-prep context for current &language..." -- same per-cell context as
// OnExportAiContextString, for EVERY translatable cell in the current language's 3 resources (worklist
// shape mirrors BuildGenWorklistCore but WITHOUT its empty/flagged filtering -- the export wants ALL
// translatable cells, not just voids + validation failures), or just the untranslated subset per the
// confirmation dialog. ALWAYS runs on a worker thread: the whole-language available_px pass renders and
// measures every dialog in the bundle (~50), the one genuinely non-trivial cost here -- everything else
// is pure in-memory po/enrichment lookups. Threading mirrors OnGenerateAiSuggestions's snapshot-then-
// background-worker shape (see its comment for why nothing here calls back into the UI thread
// synchronously): every input the worker needs is captured BY VALUE before the thread starts, including
// ctxByKey (BuildExportContext -- UI-thread-only for the same m_core/m_researchOverrides/m_sessionPo
// reasons as BuildGenContext).
void MainFrame::OnExportAiContextLanguage() {
    if (m_exportRunning) {
        MessageBox(L"An AI-prep context export is already running \x2014 try again once it finishes "
                  L"(see the status bar).", L"Export AI-prep context", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!m_checkedOut) return;   // no language loaded yet

    std::vector<Row> allRows;
    long long untranslatedCount = 0, totalCount = 0;
    for (int res = 0; res < RES_COUNT; ++res) {
        for (const auto& e : m_po[res].entries) {
            if (e.msgid.empty()) continue;
            allRows.push_back(Row{ e.msgctxt, e.msgid, res });
            ++totalCount;
            if (e.msgstr.empty()) ++untranslatedCount;
        }
    }
    if (allRows.empty()) {
        MessageBox(L"Nothing to export: no translatable cells for this language.",
                  L"Export AI-prep context", MB_OK | MB_ICONINFORMATION);
        return;
    }

    CString msg;
    msg.Format(L"Export AI-prep context for '%s'?\r\n\r\n"
              L"Yes = untranslated only (~%lld cells)\r\nNo = all cells (%lld cells)\r\nCancel = abort",
              (LPCWSTR)m_lang, untranslatedCount, totalCount);
    int choice = MessageBox(msg, L"Export AI-prep context", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (choice == IDCANCEL) return;
    std::vector<Row> worklist;
    if (choice == IDYES) {
        for (const auto& row : allRows) {
            const PoEntry* e = m_po[row.res].find(row.msgctxt, row.msgid);
            if (e && e->msgstr.empty()) worklist.push_back(row);
        }
    } else {
        worklist = allRows;
    }
    if (worklist.empty()) {
        MessageBox(L"Nothing to export: no untranslated cells for this language.",
                  L"Export AI-prep context", MB_OK | MB_ICONINFORMATION);
        return;
    }

    CString path;
    char* envPath = nullptr; size_t envLen = 0;
    _dupenv_s(&envPath, &envLen, "MPCTRANS_EXPORT_PATH");
    if (envPath && *envPath) path = CString(CA2W(envPath, CP_UTF8));
    free(envPath);
    if (path.IsEmpty()) {
        CString defName = L"mpc-hc." + m_lang + L".ai-context.jsonl";
        CFileDialog fd(FALSE, L"jsonl", defName, OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY,
                       L"JSON Lines (*.jsonl)|*.jsonl|All files|*.*||", this);
        if (fd.DoModal() != IDOK) return;
        path = fd.GetPathName();
    }

    // ---- snapshot everything the worker needs, by VALUE, on the UI thread ----
    auto ctxByKey = BuildExportContext(m_lang, worklist);   // UI-thread only (m_core/m_researchOverrides)
    ControlIndex idxCopy = Idx();
    std::array<PoFile, RES_COUNT> poCopy = { m_po[0], m_po[1], m_po[2] };
    std::vector<RcDialog> rcCopy = m_preview.RcDialogs();
    bool useRcCopy = m_preview.UsingRc();
    CString neutralDll = m_bundle.neutral_dll;
    CString langCode = m_lang;
    std::set<long long> dialogIds;
    for (const auto& r : idxCopy.dialogs()) dialogIds.insert(r.dialog);

    m_exportRunning = true;
    m_downloading = true;   // reuse the same progress-bar strip StartPrefetch/EnsureFitScan use
    m_progress.SetRange32(0, (int)worklist.size());
    m_progress.SetPos(0);
    m_progress.ShowWindow(SW_SHOW);
    Layout();
    CString startMsg;
    startMsg.Format(L"Exporting AI-prep context for '%s': 0/%zu\x2026", (LPCWSTR)langCode, worklist.size());
    SetStatus(startMsg);

    if (m_exportThread.joinable()) m_exportThread.join();   // a PREVIOUS run finished but wasn't joined yet
    HWND hwnd = GetSafeHwnd();
    m_exportThread = std::thread([hwnd, worklist, ctxByKey, idxCopy, poCopy, rcCopy, useRcCopy,
                                  neutralDll, langCode, path, dialogIds]() {
        auto* result = new ExportResult;
        result->lang = langCode; result->path = path;

        std::map<std::pair<std::string, std::string>, int> availByKey =
            MeasureAvailablePx(dialogIds, idxCopy, poCopy[RES_DIALOGS], neutralDll, useRcCopy, rcCopy);

        std::string buf;
        buf.reserve(worklist.size() * 128);
        ExportCtx emptyCtx;
        int total = (int)worklist.size();
        int done = 0;
        for (const auto& row : worklist) {
            const PoEntry* e = poCopy[row.res].find(row.msgctxt, row.msgid);
            std::string current = e ? e->msgstr : std::string();

            auto ctxIt = ctxByKey.find({ row.msgctxt, row.msgid });
            const ExportCtx& ec = ctxIt != ctxByKey.end() ? ctxIt->second : emptyCtx;

            std::optional<int> avail;
            auto avIt = availByKey.find({ row.msgctxt, row.msgid });
            if (avIt != availByKey.end()) avail = avIt->second;

            AiPrepContext ctx = BuildAiPrepContext(row, current, ec.haveInfo, ec.info, ec.siblings, avail);
            buf += ToJsonLine(ctx);
            buf += "\n";

            ++done;
            if (done % 50 == 0 || done == total)   // throttled: avoid flooding the UI-thread message queue
                ::PostMessage(hwnd, WM_APP_EXPORT_PROGRESS, (WPARAM)done, (LPARAM)total);
        }

        write_file_raw(path, buf);
        write_file_raw(PromptSpecPathFor(path), BuildAiPrepPromptSpec());
        result->count = total;
        if (!::PostMessage(hwnd, WM_APP_EXPORT_DONE, (WPARAM)result, 0)) delete result;
    });
}

// A worker tick -- advance the status bar + progress bar. Wording deliberately contains the literal
// substring "Exporting" + the fraction (greppable by automation), matching OnGenProgress's convention.
LRESULT MainFrame::OnExportProgress(WPARAM done, LPARAM total) {
    CString s;
    s.Format(L"Exporting AI-prep context for '%s': %d/%d\x2026", (LPCWSTR)m_lang, (int)done, (int)total);
    SetStatus(s);
    if (m_progress.GetSafeHwnd()) m_progress.SetPos((int)done);
    return 0;
}

// UI thread: the whole-language export finished -- restore the progress strip, report a summary.
LRESULT MainFrame::OnExportDone(WPARAM wp, LPARAM) {
    std::unique_ptr<ExportResult> res((ExportResult*)wp);
    if (m_exportThread.joinable()) m_exportThread.join();
    m_exportRunning = false;
    m_downloading = false;
    if (m_progress.GetSafeHwnd()) m_progress.ShowWindow(SW_HIDE);
    Layout();
    CString s;
    s.Format(L"Exported %d AI-prep context line(s) for '%s' -> %s",
             res->count, (LPCWSTR)res->lang, (LPCWSTR)res->path);
    SetStatus(s);
    return 0;
}

// ---------- "Check for data updates" (see mpctrans::data_update) ----------

// File menu -> fetch dist/data-manifest.json from the STUDIO_* repo, diff it against the bundle's
// on-disk core-enrichment.sqlite / control-index.json / per-language packs, and download+verify+
// atomically swap in whatever differs -- all off the UI thread. Mirrors OnSuggestFixClicked's shape:
// resolve inputs on the UI thread, hand them to the worker BY VALUE, report back via a single
// WM_APP_*_DONE message. No confirmation prompt before downloading -- these are small data files
// (KB-MB), not the app itself, and apply() never touches a good local file with unverified bytes.
void MainFrame::OnCheckDataUpdate() {
    if (m_dataUpdateThread.joinable()) {
        MessageBox(L"A data update check is already running.", L"Check for data updates",
                  MB_ICONINFORMATION);
        return;
    }
    if (!m_bundle.ok) {
        MessageBox(L"Bundle not located -- can't check for data updates.", L"Check for data updates",
                  MB_ICONERROR);
        return;
    }
    SetStatus(L"Checking for data updates\x2026");
    HWND hwnd = GetSafeHwnd();
    CString indexJson = m_bundle.index_json, coreSqlite = m_bundle.core_sqlite, langDir = m_bundle.lang_dir;
    m_dataUpdateThread = std::thread([hwnd, indexJson, coreSqlite, langDir]() {
        // repo_path (as listed in dist/data-manifest.json) -> the actual on-disk path THIS bundle
        // reads -- mirrors Bundle::Locate's release ("<bundle>/core-enrichment.sqlite", ".../lang/
        // <code>.sqlite") and dev-checkout ("dist/core-enrichment.sqlite", "enrichment/lang/<code>.
        // sqlite") layouts; either way m_bundle already resolved the right root, so this is just a
        // repo_path -> filename mapping. A manifest entry this build doesn't recognize maps to ""
        // and is skipped by data_update::check/apply.
        auto localPathFor = [&](const std::string& repo_path) -> std::string {
            CString path;
            if (repo_path == "dist/core-enrichment.sqlite") path = coreSqlite;
            else if (repo_path == "dist/control-index.json") path = indexJson;
            else if (repo_path.rfind("enrichment/lang/", 0) == 0 && !langDir.IsEmpty()) {
                std::string file = repo_path.substr(std::string("enrichment/lang/").size());
                path = langDir + L"\\" + CString(CA2W(file.c_str(), CP_UTF8));
            }
            if (path.IsEmpty()) return {};
            return std::string(CW2A(path, CP_UTF8));
        };

        auto* res = new DataUpdateResult;
        try {
            data_update::UpdatePlan plan = data_update::check(localPathFor);
            res->remoteVersion = CString(CA2W(plan.remote_version.c_str(), CP_UTF8));
            res->localVersion  = CString(CA2W(plan.local_version.c_str(), CP_UTF8));
            res->ok = true;
            res->upToDate = plan.changed.empty();
            if (!res->upToDate) {
                data_update::ApplyResult ar = data_update::apply(plan, localPathFor, nullptr);
                res->updated = ar.updated;
                res->failed  = ar.failed;
                for (const auto& e : ar.errors) res->errors.push_back(CString(CA2W(e.c_str(), CP_UTF8)));
            }
        } catch (const std::exception& ex) {
            res->ok = false;
            res->error = CString(CA2W(ex.what(), CP_UTF8));
        }
        if (!::PostMessage(hwnd, WM_APP_DATA_UPDATE_DONE, (WPARAM)res, 0)) delete res;
    });
}

// UI thread: report the background check/apply's outcome (see OnCheckDataUpdate). A successful apply
// never touches live in-memory state (m_index/m_core/m_pack and everything derived from them --
// dialog/menu trees, HTREEITEM maps, the owner-draw HMENUs) -- reloading THAT mid-session is the same
// scope of work as a language switch and risks leaving stale cross-references, so this just asks for a
// restart rather than attempting a live reload (acceptable per the feature's own scope: a data refresh,
// not a hot-reload).
LRESULT MainFrame::OnDataUpdateDone(WPARAM wp, LPARAM) {
    std::unique_ptr<DataUpdateResult> res((DataUpdateResult*)wp);
    if (m_dataUpdateThread.joinable()) m_dataUpdateThread.join();

    if (!res->ok) {
        SetStatus(L"Data update check failed.");
        MessageBox(L"Check for data updates failed:\n" + res->error, L"Check for data updates", MB_ICONERROR);
        return 0;
    }
    if (res->upToDate) {
        SetStatus(L"Data is up to date (version " + res->remoteVersion + L").");
        MessageBox(L"The bundled data (enrichment, control index, language packs) is already up to date.\n\n"
                  L"Version: " + res->remoteVersion, L"Check for data updates", MB_ICONINFORMATION);
        return 0;
    }

    CString msg;
    msg.Format(L"%d data file(s) updated to version %s.", res->updated, (LPCWSTR)res->remoteVersion);
    if (res->failed > 0) {
        CString fail;
        fail.Format(L"\n\n%d file(s) FAILED and were left untouched:", res->failed);
        msg += fail;
        for (const CString& e : res->errors) msg += L"\n  - " + e;
    }
    if (res->updated > 0) {
        msg += L"\n\nRestart the Studio to pick up the new data.";
        SetStatus(L"Data updated to version " + res->remoteVersion + L" -- restart to apply.");
    } else {
        SetStatus(L"Data update failed.");
    }
    MessageBox(msg, L"Check for data updates", res->failed > 0 ? MB_ICONWARNING : MB_ICONINFORMATION);
    return 0;
}

// Mirrors OnSuggestFixClicked's shape: GitHub-style config check (here: an AI provider's stored API
// key rather than a GitHub token) -> background HTTP call on a worker thread -> WM_APP_AI_SUGGEST_DONE.
// A session cache (keyed msgctxt/msgid/target-language) skips the network entirely on a cache hit.
void MainFrame::OnSuggestAiClicked() {
    if (m_curRow < 0 || m_curRow >= (int)m_rows.size()) return;
    if (m_aiSuggestThread.joinable()) return;   // a request is already in flight
    const Row row = m_rows[m_curRow];           // copy: m_curRow may move before the worker finishes

    CWinApp* app = AfxGetApp();
    std::string providerId = std::string(CW2A(
        app ? app->GetProfileString(L"AI", L"Provider", L"claude") : CString(L"claude"), CP_UTF8));
    const ProviderInfo* provider = find_provider(providerId);
    if (!provider) { providerId = "claude"; provider = find_provider(providerId); }
    if (!provider) return;   // the built-in table always has "claude"; defensive only

    std::wstring credTarget = AiCredTarget(providerId);
    auto tok = github::load_token(credTarget.c_str());
    if (!tok) {
        AiSettingsDlg dlg(this);
        dlg.DoModal();   // Save persists provider/model + (optionally) the key; Cancel changes nothing
        providerId = std::string(CW2A(
            app ? app->GetProfileString(L"AI", L"Provider", L"claude") : CString(L"claude"), CP_UTF8));
        provider = find_provider(providerId);
        if (!provider) return;
        credTarget = AiCredTarget(providerId);
        tok = github::load_token(credTarget.c_str());
        if (!tok) return;   // still no key configured -- abort quietly, no error dialog
    }

    CString modelCs = app ? app->GetProfileString(L"AI", L"Model", L"") : CString();
    std::string model = !modelCs.IsEmpty() ? std::string(CW2A(modelCs, CP_UTF8)) : provider->default_model;
    CString providerLabel = CString(providerId.c_str()) + L"/" + CString(model.c_str());

    CString msgctxt = CString(CA2W(row.msgctxt.c_str(), CP_UTF8));
    CString msgid = CString(CA2W(row.msgid.c_str(), CP_UTF8));
    CString langCode = m_lang;
    auto cacheKey = std::make_tuple(msgctxt, msgid, langCode);

    auto cached = m_aiSuggestCache.find(cacheKey);
    if (cached != m_aiSuggestCache.end()) {
        m_edit.SetAiSuggestion(cached->second);
        m_edit.ShowAiSuggestionValidation(validate::check_entry(
            row.msgctxt, row.msgid, std::string(CW2A(cached->second, CP_UTF8))));
        // On-demand suggestions are a single model call -- no cross-model vote/back-translation
        // score to show, and never flagged "Review recommended" (no confidence::classify inputs).
        m_edit.SetAiDetails(providerLabel, cached->second, CString(), CString(),
                            false, 0.0, false, 0.0, false);
        return;
    }

    m_edit.SetAiSuggestRequesting();

    AiConfig cfg{ providerId, model };
    std::string apiKey = tok->access_token;
    std::string sys = AiSystemPrompt();

    // Lazy translator-hint fallback: if this string has no shipped LLM hint (e.g. a brand-new upstream
    // string, or a build whose LLM hints weren't filled), fill it with the user's own key. A hint we've
    // cached before is applied NOW so it grounds this very suggestion; an uncached one is generated in a
    // detached background call and cached for next time -- we never delay the suggestion the user asked
    // for on a second network round-trip. Failures are silent (the suggestion just proceeds hint-less).
    if (!m_curHint || m_curHint->role.empty()) {
        if (auto ch = suggestion_store::get_cached_hint(row.msgctxt, row.msgid); ch && !ch->role.empty()) {
            mpctrans::StringHint h;
            if (m_curHint) h = *m_curHint;      // preserve any deterministic keep_verbatim/parallel fields
            h.role = ch->role; h.form = ch->form; h.referent = ch->referent;
            h.agreement = ch->agreement; h.notes = ch->notes; h.source = "app:" + ch->model;
            m_curHint = h;
        } else if (m_haveCurInfo && !m_curInfo.semantic_purpose.empty()) {
            AiConfig hcfg = cfg; std::string hkey = apiKey, hmodel = model;
            std::string mc = row.msgctxt, mi = row.msgid, sem = m_curInfo.semantic_purpose,
                        fun = m_curInfo.functional_purpose, loc = m_curInfo.ui_location;
            std::thread([hcfg, hkey, hmodel, mc, mi, sem, fun, loc]() {
                if (auto g = generate_hint(hcfg, hkey, mi, sem, fun, loc)) {
                    suggestion_store::HintRow hr{ g->role, g->form, g->referent, g->agreement, g->notes, hmodel };
                    suggestion_store::put_cached_hint(mc, mi, hr);
                }
            }).detach();   // fire-and-forget cache fill; touches only the mutex-guarded store + WinHTTP
        }
    }

    std::string usr = std::string(CW2A(BuildAiUserPrompt(row), CP_UTF8));
    HWND hwnd = GetSafeHwnd();

    m_aiSuggestThread = std::thread([hwnd, cfg, apiKey, sys, usr, msgctxt, msgid, langCode, providerLabel]() {
        auto* res = new AiSuggestResult;
        res->msgctxt = msgctxt; res->msgid = msgid; res->lang = langCode; res->providerLabel = providerLabel;
        try {
            std::string raw = suggest_translation(cfg, apiKey, sys, usr);
            res->text = CString(CA2W(extract_translation_json(raw).c_str(), CP_UTF8));
            res->ok = true;
        } catch (const std::exception& ex) {
            res->ok = false; res->error = ex.what();
        } catch (...) {
            res->ok = false; res->error = "unknown error";
        }
        if (!::PostMessage(hwnd, WM_APP_AI_SUGGEST_DONE, (WPARAM)res, 0)) delete res;
    });
}

// UI thread: report the background AI HTTP call's outcome (see OnSuggestAiClicked). Cache the result
// unconditionally on success (it's still useful if the translator comes back to this exact string),
// but drop the UI update silently if the selection or target language has since moved on -- an AI
// suggestion arriving for a string the translator isn't looking at anymore would be confusing, not
// helpful.
LRESULT MainFrame::OnAiSuggestDone(WPARAM wp, LPARAM) {
    std::unique_ptr<AiSuggestResult> res((AiSuggestResult*)wp);
    if (m_aiSuggestThread.joinable()) m_aiSuggestThread.join();

    if (res->ok) m_aiSuggestCache[{ res->msgctxt, res->msgid, res->lang }] = res->text;

    bool stale = true;
    if (m_curRow >= 0 && m_curRow < (int)m_rows.size()) {
        const Row& row = m_rows[m_curRow];
        stale = !(CString(CA2W(row.msgctxt.c_str(), CP_UTF8)) == res->msgctxt &&
                 CString(CA2W(row.msgid.c_str(), CP_UTF8)) == res->msgid && m_lang == res->lang);
    }
    if (stale) return 0;

    if (res->ok) {
        // Re-measure a fit fix against its control before offering it. Only the INDIVIDUAL control is
        // re-measured even for a grouped flag -- re-running a group's joint resize behavior (re-running
        // ApplyWidgetPairs with the hypothetical new text) is out of scope for M1.
        CString overflowNote;
        if (m_curRowFlag == Row::Flag::FitHard || m_curRowFlag == Row::Flag::FitTight) {
            const Row& row = m_rows[m_curRow];
            if (row.flagControlId >= 0 && m_preview.CurrentDlg()) {
                HWND ctrl = ::GetDlgItem(m_preview.CurrentDlg(), (int)row.flagControlId);
                LivePreview::TextFit fit = m_preview.MeasureControlText(ctrl, res->text);
                if (fit.measured && fit.renderedPx > fit.availablePx)
                    overflowNote.Format(L"Suggested text still overflows by %dpx", fit.renderedPx - fit.availablePx);
            }
        }
        m_edit.SetAiSuggestion(res->text);
        m_edit.ShowAiSuggestionValidation(validate::check_entry(
            std::string(CW2A(res->msgctxt, CP_UTF8)), std::string(CW2A(res->msgid, CP_UTF8)),
            std::string(CW2A(res->text, CP_UTF8))));
        if (!overflowNote.IsEmpty()) m_edit.AppendValidationLine(overflowNote);
        // On-demand suggestions are a single model call -- no cross-model vote/back-translation
        // score to show, and never flagged "Review recommended" (no confidence::classify inputs).
        m_edit.SetAiDetails(res->providerLabel, res->text, CString(), CString(),
                            false, 0.0, false, 0.0, false);
    } else {
        CString shortMsg = CString(CA2W(res->error.c_str(), CP_UTF8));
        if (shortMsg.GetLength() > 140) shortMsg = shortMsg.Left(137) + L"...";
        m_edit.SetAiSuggestError(shortMsg);
    }
    return 0;
}

// File > "AI provider…" -- opens the same settings modal OnSuggestAiClicked pops automatically when
// no key is configured yet.
void MainFrame::OnFileAiProvider() {
    AiSettingsDlg dlg(this);
    dlg.DoModal();
}

// Shared by OnGenerateAiSuggestions and OnGenerateAiSuggestionsAll (M4): resolve the configured AI
// provider/model, same flow OnSuggestAiClicked uses -- auto-opening AiSettingsDlg if no key is stored
// yet (Save persists provider/model + optionally the key; Cancel changes nothing). Returns false (with
// nothing else touched -- no side effects beyond reading profile strings + github::load_token) if still
// unconfigured after the dialog closes, same as both callers' previous inline "abort quietly" behavior.
bool MainFrame::ResolveAiProvider(std::string& providerId, std::string& model, std::string& apiKey) {
    CWinApp* app = AfxGetApp();
    providerId = std::string(CW2A(
        app ? app->GetProfileString(L"AI", L"Provider", L"claude") : CString(L"claude"), CP_UTF8));
    const ProviderInfo* provider = find_provider(providerId);
    if (!provider) { providerId = "claude"; provider = find_provider(providerId); }
    if (!provider) return false;   // the built-in table always has "claude"; defensive only

    std::wstring credTarget = AiCredTarget(providerId);
    auto tok = github::load_token(credTarget.c_str());
    if (!tok) {
        AiSettingsDlg dlg(this);
        dlg.DoModal();   // Save persists provider/model + (optionally) the key; Cancel changes nothing
        providerId = std::string(CW2A(
            app ? app->GetProfileString(L"AI", L"Provider", L"claude") : CString(L"claude"), CP_UTF8));
        provider = find_provider(providerId);
        if (!provider) return false;
        credTarget = AiCredTarget(providerId);
        tok = github::load_token(credTarget.c_str());
        if (!tok) return false;   // still no key configured -- abort quietly, same as the on-demand path
    }
    CString modelCs = app ? app->GetProfileString(L"AI", L"Model", L"") : CString();
    model = !modelCs.IsEmpty() ? std::string(CW2A(modelCs, CP_UTF8)) : provider->default_model;
    apiKey = tok->access_token;
    return true;
}

// UI-thread only: m_sessionPo is a plain std::map the UI thread also writes (LoadLanguage/
// OnPrefetchDone) -- see the declaration in MainFrame.h for the full rationale. Session-cache hit,
// else the bundled disk cache (m_bundle.po_dir), NEVER the network -- mirrors LoadLanguage's
// fromGithub=false branch, reusing its own free-function helpers (exists/read_file_lf/PoFile::parse_bytes)
// directly rather than duplicating their logic.
std::optional<std::array<mpctrans::PoFile, MainFrame::RES_COUNT>>
MainFrame::LoadLanguagePoNoNetwork(const CString& code) {
    static const char* kRes[RES_COUNT] = { "dialogs", "menus", "strings" };
    std::wstring skey(code);
    if (auto it = m_sessionPo.find(skey); it != m_sessionPo.end()) return it->second;
    if (m_bundle.po_dir.IsEmpty()) return std::nullopt;
    std::string lang = CW2A(code, CP_UTF8);
    std::array<mpctrans::PoFile, RES_COUNT> out;
    for (int r = 0; r < RES_COUNT; ++r) {
        std::string name = "mpc-hc." + lang + "." + std::string(kRes[r]) + ".po";
        CString cached = m_bundle.po_dir + L"\\" + CString(CA2W(name.c_str(), CP_UTF8));
        if (!exists(cached)) return std::nullopt;
        out[r] = mpctrans::PoFile::parse_bytes(read_file_lf(cached));
    }
    return out;
}

// UI-thread only (CoreEnrichment::by_key is SQLITE_THREADSAFE=0). See the declaration in MainFrame.h
// for the full rationale (shared by the per-language and ALL-languages paths so they can't drift).
// v3.2: no longer opens a LangPack here -- the reference corpus it used to populate
// PromptCtx::refs with is out of scope for the generated prompt now (see kAiPromptVersion's comment);
// the References PANEL's own independent m_pack.refs_for lookup in SelectRow is untouched.
std::map<std::pair<std::string, std::string>, MainFrame::PromptCtx>
MainFrame::BuildGenContext(const CString& langCode, const std::vector<Row>& worklist) {
    std::map<std::pair<std::string, std::string>, PromptCtx> ctxByKey;
    for (const auto& row : worklist) {
        auto key = std::make_pair(row.msgctxt, row.msgid);
        if (ctxByKey.count(key)) continue;
        PromptCtx pc;
        if (m_haveCore) {
            if (auto si = m_core.by_key(row.msgctxt, row.msgid)) {
                pc.info = *si; pc.haveInfo = true;
                auto ov = m_researchOverrides.find(key);
                if (ov != m_researchOverrides.end()) {
                    if (ov->second.semantic_purpose) pc.info.semantic_purpose = *ov->second.semantic_purpose;
                    if (ov->second.functional_purpose) pc.info.functional_purpose = *ov->second.functional_purpose;
                }
                pc.hint = m_core.hint_for(pc.info.id);   // v3.3: translator hint layer (nullopt-safe)
            }
        }
        pc.siblings = BuildSiblingLines(row, langCode);   // M3: fidelity-ranked cross-language grounding
        ctxByKey.emplace(std::move(key), std::move(pc));
    }
    return ctxByKey;
}

// ---------- M2: bulk "Generate AI suggestions" (enrichment-grounded, all empty + validation-failing cells) ----------

// File > "Generate AI suggestions…" -- confirms scope (language / provider-model / counts) then
// launches a background worker that walks BuildGenWorklist() sequentially, ONE ai_client::
// suggest_translation call at a time (no batching -- batching degrades context per the plan), gates
// each result (JSON-extract retry-once-then-skip, validate::check_entry, fit-for-dialog-strings-only),
// and persists via suggestion_store::put(). Re-invoking while a run is already in flight is the
// required cancel affordance (MB_YESNO "cancel?").
//
// Threading: the worker below NEVER touches a live MainFrame member after launch and NEVER calls back
// into the UI thread synchronously. Everything BuildAiUserPromptCore needs (the ControlIndex, the 3
// PoFiles, and each worklist cell's StringInfo -- resolved from m_core HERE, on the UI thread, since
// that sqlite3 build is ALSO SQLITE_THREADSAFE=0) is snapshotted by value before
// the thread starts, mirroring EnsureFitScan's proven off-UI-thread pattern. A synchronous per-cell
// SendMessage callback into the UI thread was considered and rejected: a worker blocked mid-
// SendMessage while OnDestroy's thread::join() blocks the very UI thread that would need to service
// it is a real deadlock (SendMessage across threads requires the target thread to be pumping
// messages, which it can't do while join()ed) -- snapshotting sidesteps that class of bug entirely.
void MainFrame::OnGenerateAiSuggestions() {
    if (m_genRunning) {
        CString msg;
        msg.Format(L"Generation in progress (%d/%d) — cancel?", m_genDone, m_genTotal);
        if (MessageBox(msg, L"Generate AI suggestions", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            m_genCancel = true;
            JoinGenThreadPumping(m_genThread);   // see JoinGenThreadPumping's comment (m_genThread may
                                                  // be the M4 batch worker, mid per-language handoff)
            m_genRunning = false;
            m_downloading = false;
            if (m_progress.GetSafeHwnd()) m_progress.ShowWindow(SW_HIDE);
            Layout();
            SetStatus(L"Generation cancelled.");
        }
        return;
    }
    if (!m_checkedOut) return;   // no language loaded yet
    // Both generation commands are disabled while a fit scan is running for the current language --
    // it might race the fit cache (BuildGenWorklist/CellFlag read m_fitCache[m_lang] on the UI thread
    // right after this, and EnsureFitScan's worker installs into it asynchronously).
    if (m_fitScanRunning) {
        MessageBox(L"A fit scan is already running for the current language — try again once it "
                  L"finishes (see the status bar).", L"Generate AI suggestions", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::string providerId, model, apiKey;
    if (!ResolveAiProvider(providerId, model, apiKey)) return;   // still unconfigured -- abort quietly

    // M3: pick the cross-model vote provider — first configured provider of a DIFFERENT lineage.
    std::vector<std::pair<std::string, bool>> avail;
    for (const auto& p : list_providers())
        avail.push_back({ p.id, github::load_token(AiCredTarget(p.id).c_str()).has_value() });
    auto votePick = pick_vote_provider(providerId, model, avail);
    std::optional<AiConfig> voteCfg; std::string voteKey; CString voteLabel;
    if (votePick) {
        if (auto vt = github::load_token(AiCredTarget(votePick->provider_id).c_str())) {
            voteCfg = AiConfig{ votePick->provider_id, votePick->model };
            voteKey = vt->access_token;
            voteLabel = CString(votePick->provider_id.c_str()) + L"/" + CString(votePick->model.c_str());
        }
    }

    // The confirmation counts need m_fitCache populated for this language (fit-flagged dialog cells
    // are part of "validation-flagged"); never block the UI thread waiting for the scan.
    if (!m_fitCache.count(std::wstring(m_lang))) {
        EnsureFitScan();
        MessageBox(L"Fit scanning is still running for this language (see the status bar) — try "
                  L"'Generate AI suggestions' again once it finishes.",
                  L"Generate AI suggestions", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::vector<Row> worklist = BuildGenWorklist();
    if (worklist.empty()) {
        MessageBox(L"Nothing to generate: no empty or validation-flagged cells for this language.",
                  L"Generate AI suggestions", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // M4 skip/resume rule: drop every cell that already has a CURRENT (same model + prompt_version)
    // primary suggestion stored -- re-invoking a generation run must never re-bill those.
    CString providerLabel = CString(providerId.c_str()) + L"/" + CString(model.c_str());
    std::string langUtf8 = std::string(CW2A(m_lang, CP_UTF8));
    std::string fullModel = providerId + "/" + model;
    auto already = suggestion_store::primary_keys(langUtf8, fullModel, kAiPromptVersion);
    long long alreadySuggestedCount = 0;
    worklist.erase(std::remove_if(worklist.begin(), worklist.end(), [&](const Row& r) {
        if (already.count({ r.msgctxt, r.msgid })) { ++alreadySuggestedCount; return true; }
        return false;
    }), worklist.end());

    if (worklist.empty()) {
        CString m;
        if (alreadySuggestedCount > 0)
            m.Format(L"Nothing to generate: all %lld eligible cell(s) already have a current AI "
                     L"suggestion (%s, prompt %hs).", alreadySuggestedCount, (LPCWSTR)providerLabel,
                     kAiPromptVersion);
        else
            m = L"Nothing to generate: no empty or validation-flagged cells for this language.";
        MessageBox(m, L"Generate AI suggestions", MB_OK | MB_ICONINFORMATION);
        return;
    }
    long long emptyCount = 0, flaggedCount = 0;
    for (const auto& r : worklist) (r.flag == Row::Flag::None ? emptyCount : flaggedCount)++;

    long long n = (long long)worklist.size();
    long long totalCalls = n * (1 + (voteCfg ? 1 : 0) + 2);   // primary + vote(optional) + back-translation(2: translate + judge)
    CString voteLine;
    if (voteCfg) voteLine.Format(L"Cross-model vote: %s (+%lld calls)", (LPCWSTR)voteLabel, n);
    else voteLine = L"Cross-model vote: unavailable (configure a second provider key)";
    CString btLine; btLine.Format(L"Back-translation + semantic judge: +%lld calls", n * 2);
    CString msg;
    msg.Format(L"Generate AI suggestions for '%s' using %s?\r\n\r\n"
              L"%lld empty cell(s)\r\n%lld validation-flagged cell(s)\r\n"
              L"already suggested: %lld (skipped)\r\n\r\n"
              L"%s\r\n%s\r\n\r\n"
              L"Estimated AI calls: %lld",
              (LPCWSTR)m_lang, (LPCWSTR)providerLabel, emptyCount, flaggedCount, alreadySuggestedCount,
              (LPCWSTR)voteLine, (LPCWSTR)btLine, totalCalls);
    if (MessageBox(msg, L"Generate AI suggestions", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    // ---- snapshot everything the worker needs by value; it must never touch `this` again ----
    CString langCode = m_lang;
    CString langName = LanguageEnglishName(langCode);
    ControlIndex idxCopy = Idx();
    std::vector<RcDialog> rcCopy = m_preview.RcDialogs();
    bool useRcCopy = m_preview.UsingRc();
    std::array<PoFile, RES_COUNT> poCopy = { m_po[0], m_po[1], m_po[2] };
    CString neutralDll = m_bundle.neutral_dll;
    std::string sys = AiSystemPrompt();
    AiConfig cfg{ providerId, model };
    std::string providerIdCopy = providerId;   // M3: needed for the "provider/model" model-attribution string

    // Per-cell research context (m_core.by_key + m_researchOverrides), resolved ONCE here on the UI
    // thread -- CoreEnrichment's sqlite3 build is ALSO SQLITE_THREADSAFE=0 (same vendored
    // amalgamation, one compile), so the worker must never touch m_core itself; this map is its
    // thread-safe substitute. Factored into BuildGenContext (M4) so the ALL-languages batch path
    // shares the IDENTICAL logic.
    std::map<std::pair<std::string, std::string>, PromptCtx> ctxByKey = BuildGenContext(langCode, worklist);

    m_genAllMode = false;
    m_genCancel = false; m_genRunning = true; m_genDone = 0; m_genTotal = (int)worklist.size();
    m_downloading = true;   // reuse the same progress-bar strip StartPrefetch/EnsureFitScan use
    m_progress.SetRange32(0, m_genTotal);
    m_progress.SetPos(0);
    m_progress.ShowWindow(SW_SHOW);
    Layout();
    CString startMsg;
    startMsg.Format(L"Generating 0/%d (%s)\x2026", m_genTotal, (LPCWSTR)langCode);
    SetStatus(startMsg);

    if (m_genThread.joinable()) m_genThread.join();   // a PREVIOUS run finished but wasn't joined yet
    HWND hwnd = GetSafeHwnd();
    std::atomic<bool>* cancel = &m_genCancel;
    m_genThread = std::thread([hwnd, cancel, cfg, apiKey, sys, worklist, langCode, langName,
                               idxCopy, rcCopy, useRcCopy, poCopy, neutralDll, ctxByKey,
                               voteCfg, voteKey, providerIdCopy]() {
        auto* result = new GenerateResult;
        result->lang = langCode;

        // Off-screen render technique (see EnsureFitScan's comment for the full rationale): a
        // never-shown WS_POPUP host so a dialog's fit-check render never flashes on screen while
        // generation runs in the background, and never disturbs whatever the live m_preview shows.
        CWnd host;
        host.CreateEx(0, AfxRegisterWndClass(0, nullptr, nullptr, nullptr), L"",
                     WS_POPUP, 0, 0, 10, 10, nullptr, nullptr);
        LivePreview lp;
        lp.LoadNeutralDll(neutralDll);
        if (useRcCopy && !rcCopy.empty()) lp.SetRcDialogs(rcCopy);

        // Precompute each row's owning dialog id (idxCopy is a plain in-memory copy -- no sqlite, safe
        // off the UI thread) and process grouped by dialog so RenderDialog (which tears down the
        // previous dialog first -- LivePreview holds only one at a time) runs at most once per dialog
        // per run instead of once per cell in it.
        struct Item { const Row* row; long long dlg; };
        std::vector<Item> items; items.reserve(worklist.size());
        for (const auto& row : worklist) {
            long long dlg = -1;
            if (row.res == RES_DIALOGS)
                for (const auto& r : idxCopy.dialogs())
                    if (r.msgctxt == row.msgctxt && r.msgid == row.msgid) { dlg = r.dialog; break; }
            items.push_back({ &row, dlg });
        }
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.dlg < b.dlg; });

        long long renderedDialog = -1; HWND curDlg = nullptr;
        int done = 0, generated = 0, skipped = 0, flaggedByGates = 0, high = 0, low = 0;
        int total = (int)items.size();
        StringInfo emptyInfo;
        std::optional<mpctrans::StringHint> emptyHint;
        std::vector<CString> emptySiblings;

        for (const auto& it : items) {
            if (cancel->load()) break;
            const Row& row = *it.row;

            auto ctxIt = ctxByKey.find({ row.msgctxt, row.msgid });
            bool haveInfo = ctxIt != ctxByKey.end() && ctxIt->second.haveInfo;
            const StringInfo& info = haveInfo ? ctxIt->second.info : emptyInfo;
            const std::optional<mpctrans::StringHint>& hint = ctxIt != ctxByKey.end() ? ctxIt->second.hint : emptyHint;
            const std::vector<CString>& siblings = ctxIt != ctxByKey.end() ? ctxIt->second.siblings : emptySiblings;

            // Gate-free step: assemble the prompt via the SAME v3 logic BuildAiUserPrompt uses for the
            // on-demand path (BuildAiUserPromptCore is static/pure -- see its declaration).
            CString userPromptCs = BuildAiUserPromptCore(langCode, langName, row, haveInfo, info, hint,
                                                         siblings, idxCopy, poCopy.data());
            std::string usr(CW2A(userPromptCs, CP_UTF8));

            // Gate 1: extract_translation_json throws on malformed JSON -- retry the WHOLE call once,
            // then skip (counted, not fatal to the run).
            std::string candidateUtf8; bool ok = false;
            for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
                try {
                    std::string raw = suggest_translation(cfg, apiKey, sys, usr);
                    candidateUtf8 = extract_translation_json(raw);
                    ok = true;
                } catch (const std::exception&) { /* retry once, then fall through to skip */ }
            }
            ++done;
            if (!ok) {
                ++skipped;
                ::PostMessage(hwnd, WM_APP_GEN_PROGRESS, (WPARAM)done, (LPARAM)total);
                continue;
            }
            CString candidate = CA2W(candidateUtf8.c_str(), CP_UTF8);

            // Gate 2: validate::check_entry -- findings recorded into the stored row's flags column.
            auto findings = validate::check_entry(row.msgctxt, row.msgid, candidateUtf8);
            std::string flagsStr;
            for (const auto& f : findings) { if (!flagsStr.empty()) flagsStr += "; "; flagsStr += f.msg; }

            // Gate 3: fit -- ONLY for dialog strings (row.res == RES_DIALOGS with a resolved dialog id).
            std::optional<int> fitsPx;
            if (row.res == RES_DIALOGS && it.dlg >= 0) {
                if (renderedDialog != it.dlg) {
                    curDlg = lp.RenderDialog(it.dlg, &host, idxCopy, poCopy[RES_DIALOGS]);
                    renderedDialog = it.dlg;
                }
                if (curDlg) {
                    long long controlId = -1;
                    for (const auto& r : idxCopy.dialogs())
                        if (r.dialog == it.dlg && r.msgctxt == row.msgctxt && r.msgid == row.msgid) {
                            controlId = r.control.value_or(-1); break;
                        }
                    if (controlId >= 0) {
                        HWND ctrl = ::GetDlgItem(curDlg, (int)controlId);
                        if (ctrl) {
                            LivePreview::TextFit fit = lp.MeasureControlText(ctrl, candidate);
                            if (fit.measured) fitsPx = std::max<int>(0, fit.renderedPx - fit.availablePx);
                        }
                    }
                }
                // controlId < 0 (or ctrl not found) -> fitsPx stays nullopt, i.e. NULL -- "couldn't be
                // measured", per spec.
            }

            // M3 pass 1: cross-model vote -- SAME system+user prompt, a different (independent-lineage)
            // provider. Single attempt; any exception just skips the vote for this cell (agreement
            // stays nullopt, never blocks the primary from being stored).
            std::optional<double> agreement;
            std::string voteTextUtf8; bool voteOk = false;
            if (voteCfg) {
                try {
                    std::string raw = suggest_translation(*voteCfg, voteKey, sys, usr);
                    voteTextUtf8 = extract_translation_json(raw);
                    voteOk = true;
                    agreement = confidence::agreement_score(candidateUtf8, voteTextUtf8);
                } catch (const std::exception&) { /* vote skipped for this cell */ }
            }

            // M3 pass 2: back-translation drift -- deliberately context-free (no system prompt beyond
            // the JSON contract, no reference/meaning grounding): drift must be measurable, not coached.
            // Uses the PRIMARY provider/model. TWO calls: back-translate to English, then an LLM
            // semantic-equivalence judge scores meaning-match against the true English msgid (replaces
            // the trigram back_translation_similarity metric -- see confidence.h's comment on
            // kBackTranslationThreshold). Single attempt each; failure -> nullopt, never blocks.
            std::optional<double> backScore;
            try {
                std::string btSys = "You translate UI strings to English. Respond with ONLY a JSON "
                    "object of the exact shape {\"translation\": \"...\"} and nothing else -- no "
                    "markdown fences, no commentary.";
                std::string btUsr = "Translate this " + std::string(CW2A(langName, CP_UTF8)) +
                    " UI string to English:\n" + candidateUtf8;
                std::string raw = suggest_translation(cfg, apiKey, btSys, btUsr);
                std::string backTextUtf8 = extract_translation_json(raw);
                backScore = semantic_equivalence_judge(cfg, apiKey, row.msgid, backTextUtf8);
            } catch (const std::exception&) { /* back-translation skipped for this cell */ }

            suggestion_store::Suggestion s;
            s.lang = std::string(CW2A(langCode, CP_UTF8));
            s.msgctxt = row.msgctxt; s.msgid = row.msgid;
            s.suggestion = candidateUtf8;
            s.model = providerIdCopy + "/" + cfg.model; s.prompt_version = kAiPromptVersion;
            s.fits_px = fitsPx; s.flags = flagsStr;
            s.role = "primary"; s.agreement = agreement; s.back_translation_score = backScore;
            suggestion_store::put(s);

            if (voteOk) {
                suggestion_store::Suggestion v;
                v.lang = s.lang; v.msgctxt = row.msgctxt; v.msgid = row.msgid;
                v.suggestion = voteTextUtf8;
                v.model = voteCfg->provider_id + "/" + voteCfg->model; v.prompt_version = kAiPromptVersion;
                v.role = "vote"; v.agreement = agreement;   // back_translation_score/fits_px left nullopt
                auto voteFindings = validate::check_entry(row.msgctxt, row.msgid, voteTextUtf8);
                std::string voteFlagsStr;
                for (const auto& f : voteFindings) { if (!voteFlagsStr.empty()) voteFlagsStr += "; "; voteFlagsStr += f.msg; }
                v.flags = voteFlagsStr;
                suggestion_store::put(v);
            }

            ++generated;
            if (!flagsStr.empty() || (fitsPx && *fitsPx > 0)) ++flaggedByGates;

            confidence::ConfidenceInputs ci{ agreement, backScore, !(fitsPx && *fitsPx > 0), !flagsStr.empty() };
            (confidence::classify(ci) == confidence::Confidence::High ? high : low)++;

            result->items.push_back({ CString(CA2W(row.msgctxt.c_str(), CP_UTF8)),
                                      CString(CA2W(row.msgid.c_str(), CP_UTF8)), candidate });

            ::PostMessage(hwnd, WM_APP_GEN_PROGRESS, (WPARAM)done, (LPARAM)total);
        }
        result->generated = generated; result->skipped = skipped; result->flaggedByGates = flaggedByGates;
        result->high = high; result->low = low;
        result->cancelled = cancel->load();
        if (!::PostMessage(hwnd, WM_APP_GEN_DONE, (WPARAM)result, 0)) delete result;
    });
}

// A worker tick -- advance the progress status + bar. Wording deliberately contains the literal
// substring "Generating" + the fraction (greppable by automation), same convention as EnsureFitScan's
// "Computing fit" progress text.
LRESULT MainFrame::OnGenProgress(WPARAM done, LPARAM total) {
    m_genDone = (int)done; m_genTotal = (int)total;
    CString s;
    s.Format(L"Generating %d/%d (%s)\x2026", m_genDone, m_genTotal, (LPCWSTR)m_lang);
    SetStatus(s);
    if (m_progress.GetSafeHwnd()) m_progress.SetPos(m_genDone);
    return 0;
}

// UI thread: install the bulk-generation result -- seed the session AI-suggestion cache for every
// cell generated this run (so the on-demand panel is instant if the translator lands on one), report
// a summary, and refresh the EditPanel if the currently-selected cell might now have a stored
// suggestion (SelectRow re-derives eligibility + re-reads the store itself, so this is just "run
// SelectRow again for the same row").
LRESULT MainFrame::OnGenerateDone(WPARAM wp, LPARAM) {
    std::unique_ptr<GenerateResult> res((GenerateResult*)wp);
    if (m_genThread.joinable()) m_genThread.join();
    m_genRunning = false;
    m_downloading = false;
    if (m_progress.GetSafeHwnd()) m_progress.ShowWindow(SW_HIDE);
    Layout();

    for (const auto& item : res->items)
        m_aiSuggestCache[{ item.msgctxt, item.msgid, res->lang }] = item.text;

    CString s;
    s.Format(L"Generated %d (high %d / low %d), skipped %d (errors), %d flagged by gates.%s",
             res->generated, res->high, res->low, res->skipped, res->flaggedByGates,
             res->cancelled ? L" (cancelled)" : L"");
    SetStatus(s);

    if (m_curRow >= 0 && m_curRow < (int)m_rows.size() && res->lang == m_lang) SelectRow(m_curRow);
    return 0;
}

// ---------- M4: bulk "Generate AI suggestions for ALL languages" ----------

// File > "Generate AI suggestions for ALL languages…" -- same shape as OnGenerateAiSuggestions (confirm
// scope, snapshot by value, background worker), extended to walk every language in m_langCombo one at a
// time. Reuses m_genThread/m_genCancel/m_genRunning (the two commands are mutually exclusive by
// construction -- the busy guard below refuses to start while either is already running); m_genAllMode
// distinguishes which one is in flight for the shared cancel-prompt/progress wording.
//
// Threading: two-phase UI-thread handoff, per language. The worker can NEVER touch m_sessionPo/m_core/
// m_pack/m_researchOverrides itself (same SQLITE_THREADSAFE=0 / plain-map-race constraints as the
// single-language path -- see LoadLanguagePoNoNetwork/BuildGenContext's comments), but a single
// up-front snapshot (like the single-language worker does for ITS one language) isn't possible here
// because the worklist for language N+1 isn't known until language N's fit scan (computed ON the
// worker thread, off the UI thread) has run. So each language does a short BLOCKING round trip to the
// UI thread instead, via PostMessage + a manual-reset-false (auto-reset) Win32 event the worker waits
// on with WaitForSingleObject(INFINITE):
//   Phase A (WM_APP_BATCH_PREP_PO / OnBatchPrepPo): "load this language's 3 .po files" --
//     LoadLanguagePoNoNetwork is UI-thread-only (reads m_sessionPo, a plain map the UI thread also
//     writes).
//   Phase B (WM_APP_BATCH_PREP_CTX / OnBatchPrepCtx): "build the research context for THIS (already
//     fit-scanned, already skip-filtered) worklist" -- BuildGenContext is UI-thread-only (m_core/
//     m_researchOverrides).
// A synchronous SendMessage was rejected for the same reason OnGenerateAiSuggestions's comment gives
// for the single-language path (SendMessage while OnDestroy's thread::join() blocks the UI thread is a
// real deadlock) -- PostMessage + an explicit event sidesteps it identically, just twice per language
// instead of once per run. The request structs live on the WORKER THREAD'S OWN STACK for the entire
// wait, so handing the UI thread a raw pointer through WPARAM is safe (it never outlives the wait).
void MainFrame::OnGenerateAiSuggestionsAll() {
    if (m_genRunning) {
        CString msg;
        if (m_genAllMode)
            msg.Format(L"ALL-languages generation in progress (language %d/%d — %s: %d/%d) — cancel?",
                      m_genAllLangIndex, m_genAllLangTotal, (LPCWSTR)m_genAllLangCode,
                      m_genAllCellDone, m_genAllCellTotal);
        else
            msg.Format(L"Generation in progress (%d/%d) — cancel?", m_genDone, m_genTotal);
        if (MessageBox(msg, L"Generate AI suggestions for ALL languages", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            m_genCancel = true;
            JoinGenThreadPumping(m_genThread);   // see JoinGenThreadPumping's comment -- this worker is
                                                  // very likely mid per-language handoff right now
            m_genRunning = false;
            m_genAllMode = false;
            m_downloading = false;
            if (m_progress.GetSafeHwnd()) m_progress.ShowWindow(SW_HIDE);
            Layout();
            SetStatus(L"Generation cancelled.");
        }
        return;
    }
    // Same guardrail as OnGenerateAiSuggestions: a fit scan for the CURRENT language might race
    // m_fitCache -- refuse to start either generation command while one is running.
    if (m_fitScanRunning) {
        MessageBox(L"A fit scan is already running for the current language — try again once it "
                  L"finishes (see the status bar).",
                  L"Generate AI suggestions for ALL languages", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (m_langCombo.GetCount() == 0) return;   // nothing to iterate

    std::string providerId, model, apiKey;
    if (!ResolveAiProvider(providerId, model, apiKey)) return;   // still unconfigured -- abort quietly

    // M3: pick the cross-model vote provider — first configured provider of a DIFFERENT lineage (same
    // as OnGenerateAiSuggestions).
    std::vector<std::pair<std::string, bool>> avail;
    for (const auto& p : list_providers())
        avail.push_back({ p.id, github::load_token(AiCredTarget(p.id).c_str()).has_value() });
    auto votePick = pick_vote_provider(providerId, model, avail);
    std::optional<AiConfig> voteCfg; std::string voteKey; CString voteLabel;
    if (votePick) {
        if (auto vt = github::load_token(AiCredTarget(votePick->provider_id).c_str())) {
            voteCfg = AiConfig{ votePick->provider_id, votePick->model };
            voteKey = vt->access_token;
            voteLabel = CString(votePick->provider_id.c_str()) + L"/" + CString(votePick->model.c_str());
        }
    }

    CString providerLabel = CString(providerId.c_str()) + L"/" + CString(model.c_str());
    std::string fullModel = providerId + "/" + model;

    // ---- Pre-flight: LOWER BOUND only -- no fit scan (too expensive for 44 languages up front); just
    // empty-cell counts per language, minus the skip-rule's already-suggested subset. ----
    struct LangPre { CString code; long long emptyCount = 0, skipCount = 0; };
    std::vector<LangPre> langsWithWork;     // languages with >0 empty cells (for the top-5 ranking)
    std::vector<CString> loadedCodes;       // EVERY loaded language (even 0-empty ones may still have
                                             // validation/fit-flagged cells -- the worker recomputes
                                             // that per language, so this snapshot must include them)
    std::vector<CString> notLoaded;
    long long totalEmpty = 0, totalSkip = 0;
    int totalLangs = m_langCombo.GetCount();
    for (int i = 0; i < totalLangs; ++i) {
        CString code; m_langCombo.GetLBText(i, code);
        auto po = LoadLanguagePoNoNetwork(code);
        if (!po) { notLoaded.push_back(code); continue; }
        loadedCodes.push_back(code);
        std::set<std::pair<std::string, std::string>> emptyKeys;
        for (int r = 0; r < RES_COUNT; ++r)
            for (const auto& e : (*po)[r].entries)
                if (!e.msgid.empty() && e.msgstr.empty()) emptyKeys.insert({ e.msgctxt, e.msgid });
        if (emptyKeys.empty()) continue;
        std::string langUtf8 = std::string(CW2A(code, CP_UTF8));
        auto already = suggestion_store::primary_keys(langUtf8, fullModel, kAiPromptVersion);
        long long skip = 0;
        for (const auto& k : emptyKeys) if (already.count(k)) ++skip;
        langsWithWork.push_back({ code, (long long)emptyKeys.size(), skip });
        totalEmpty += (long long)emptyKeys.size(); totalSkip += skip;
    }

    long long netCells = totalEmpty - totalSkip;
    if (totalEmpty == 0) {
        MessageBox(L"Nothing to generate: no untranslated cells found in any loaded language "
                  L"(languages that failed to load are skipped, not counted as empty).",
                  L"Generate AI suggestions for ALL languages", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (netCells <= 0) {
        CString m;
        m.Format(L"Nothing to generate: all %lld empty cell(s) across %zu language(s) already have a "
                 L"current AI suggestion (%s, prompt %hs). (Per-language validation/fit-flagged cells "
                 L"are still computed during a run -- try again if you expect more work there.)",
                 totalEmpty, langsWithWork.size(), (LPCWSTR)providerLabel, kAiPromptVersion);
        MessageBox(m, L"Generate AI suggestions for ALL languages", MB_OK | MB_ICONINFORMATION);
        return;
    }

    CString notLoadedLine;
    if (notLoaded.empty()) {
        notLoadedLine = L"Languages not loaded (no network attempted): 0";
    } else if (notLoaded.size() <= 6) {
        CString list;
        for (size_t i = 0; i < notLoaded.size(); ++i) { if (i) list += L", "; list += notLoaded[i]; }
        notLoadedLine.Format(L"Languages not loaded (no network attempted): %zu (%s)",
                             notLoaded.size(), (LPCWSTR)list);
    } else {
        notLoadedLine.Format(L"Languages not loaded (no network attempted): %zu", notLoaded.size());
    }

    std::vector<LangPre> top5 = langsWithWork;
    std::sort(top5.begin(), top5.end(), [](const LangPre& a, const LangPre& b) { return a.emptyCount > b.emptyCount; });
    if (top5.size() > 5) top5.resize(5);
    CString top5Line = L"Largest languages by empty-cell count:\r\n";
    for (const auto& lp : top5) {
        CString line;
        line.Format(L"  %s: %lld empty, %lld already suggested\r\n", (LPCWSTR)lp.code, lp.emptyCount, lp.skipCount);
        top5Line += line;
    }

    long long totalCalls = netCells * (1 + (voteCfg ? 1 : 0) + 2);   // primary + vote(optional) + back-translation(2: translate + judge)
    CString voteLine;
    if (voteCfg) voteLine.Format(L"Cross-model vote: %s (+%lld calls)", (LPCWSTR)voteLabel, netCells);
    else voteLine = L"Cross-model vote: unavailable (configure a second provider key)";
    CString btLine; btLine.Format(L"Back-translation + semantic judge: +%lld calls", netCells * 2);

    CString msg;
    msg.Format(L"Generate AI suggestions for ALL languages using %s?\r\n\r\n"
              L"This is a LOWER BOUND: fit/validation-flagged cells are computed per language DURING "
              L"the run and are not counted below.\r\n\r\n"
              L"%lld empty cell(s) across %zu language(s) with untranslated content\r\n"
              L"already suggested: %lld (skipped)\r\n"
              L"will attempt (net): %lld cell(s)\r\n\r\n"
              L"%s\r\n\r\n%s\r\n%s\r\n%s\r\n\r\n"
              L"Estimated AI calls (lower bound): %lld",
              (LPCWSTR)providerLabel, totalEmpty, langsWithWork.size(), totalSkip, netCells,
              (LPCWSTR)notLoadedLine, (LPCWSTR)top5Line, (LPCWSTR)voteLine, (LPCWSTR)btLine, totalCalls);
    if (MessageBox(msg, L"Generate AI suggestions for ALL languages", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    // ---- snapshot everything the worker needs ONCE, up front (language-independent things only) ----
    ControlIndex idxCopy = Idx();
    std::vector<RcDialog> rcCopy = m_preview.RcDialogs();
    bool useRcCopy = m_preview.UsingRc();
    CString neutralDll = m_bundle.neutral_dll;
    std::string sys = AiSystemPrompt();
    AiConfig cfg{ providerId, model };
    std::string providerIdCopy = providerId;

    m_genAllMode = true;
    m_genCancel = false; m_genRunning = true;
    m_genAllLangIndex = 0; m_genAllLangTotal = (int)loadedCodes.size();
    m_genAllLangCode.Empty(); m_genAllCellDone = 0; m_genAllCellTotal = 0;
    m_downloading = true;   // reuse the same progress-bar strip StartPrefetch/EnsureFitScan use
    m_progress.SetRange32(0, 1);
    m_progress.SetPos(0);
    m_progress.ShowWindow(SW_SHOW);
    Layout();
    CString startMsg;
    startMsg.Format(L"Generating (ALL languages) — 0/%d languages\x2026", m_genAllLangTotal);
    SetStatus(startMsg);

    if (m_genThread.joinable()) m_genThread.join();   // a PREVIOUS run finished but wasn't joined yet
    HWND hwnd = GetSafeHwnd();
    std::atomic<bool>* cancel = &m_genCancel;
    m_genThread = std::thread([hwnd, cancel, cfg, apiKey, sys, idxCopy, rcCopy, useRcCopy, neutralDll,
                               loadedCodes, voteCfg, voteKey, providerIdCopy, fullModel]() {
        auto* result = new BatchGenerateResult;

        // Off-screen render technique (see EnsureFitScan's comment for the full rationale): a
        // never-shown WS_POPUP host, reused across every language this run processes.
        CWnd host;
        host.CreateEx(0, AfxRegisterWndClass(0, nullptr, nullptr, nullptr), L"",
                     WS_POPUP, 0, 0, 10, 10, nullptr, nullptr);
        LivePreview lp;
        lp.LoadNeutralDll(neutralDll);
        if (useRcCopy && !rcCopy.empty()) lp.SetRcDialogs(rcCopy);

        std::set<long long> dialogIds;
        for (const auto& r : idxCopy.dialogs()) dialogIds.insert(r.dialog);

        int langTotal = (int)loadedCodes.size();
        int langIndex = 0;
        for (const auto& langCode : loadedCodes) {
            ++langIndex;
            if (cancel->load()) { result->cancelled = true; break; }
            std::string langUtf8 = std::string(CW2A(langCode, CP_UTF8));

            // ---- Phase A: this language's po, via a blocking UI-thread round trip ----
            HANDLE evA = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            BatchPrepPoRequest reqA; reqA.lang = langCode; reqA.done = evA;
            ::PostMessage(hwnd, WM_APP_BATCH_PREP_PO, (WPARAM)&reqA, 0);
            ::WaitForSingleObject(evA, INFINITE);
            ::CloseHandle(evA);
            if (!reqA.outPo) {
                BatchGenerateResult::LangSummary ls; ls.lang = langCode; ls.notLoaded = true;
                result->perLang.push_back(ls); ++result->languagesNotLoaded;
                auto* prog = new BatchProgress{ langIndex, langTotal, langCode, 0, 0 };
                if (!::PostMessage(hwnd, WM_APP_GEN_ALL_PROGRESS, (WPARAM)prog, 0)) delete prog;
                continue;
            }
            std::array<PoFile, RES_COUNT> po = *reqA.outPo;

            // ---- fit scan for THIS language, entirely on this worker thread (no UI-thread
            // involvement, no member access). Not warmed back into m_fitCache: the batch path only
            // ever generates for languages other than (or including, incidentally) m_lang, and
            // OnFitScanDone's join-m_fitThread logic wasn't written with a SECOND caller in mind --
            // this is a pure UX nicety per the plan, not a correctness requirement, so it's skipped
            // for simplicity/safety rather than reusing that handler from a different thread's message. ----
            std::vector<ReviewFlag> fitFlags;
            for (long long dialogId : dialogIds) {
                HWND dlg = lp.RenderDialog(dialogId, &host, idxCopy, po[RES_DIALOGS]);
                if (dlg) {
                    AppendFitFlags(fitFlags, dialogId, lp.MeasureFit(dlg, dialogId, idxCopy));
                    lp.DestroyPreview();
                }
            }

            std::vector<Row> worklist = BuildGenWorklistCore(po.data(), &fitFlags);

            // ---- skip rule ----
            auto already = suggestion_store::primary_keys(langUtf8, fullModel, kAiPromptVersion);
            long long alreadySuggestedCount = 0;
            worklist.erase(std::remove_if(worklist.begin(), worklist.end(), [&](const Row& r) {
                if (already.count({ r.msgctxt, r.msgid })) { ++alreadySuggestedCount; return true; }
                return false;
            }), worklist.end());

            if (worklist.empty()) {
                BatchGenerateResult::LangSummary ls;
                ls.lang = langCode; ls.alreadySuggested = (int)alreadySuggestedCount;
                result->perLang.push_back(ls);
                result->totalAlreadySuggested += (int)alreadySuggestedCount;
                auto* prog = new BatchProgress{ langIndex, langTotal, langCode, 0, 0 };
                if (!::PostMessage(hwnd, WM_APP_GEN_ALL_PROGRESS, (WPARAM)prog, 0)) delete prog;
                if (cancel->load()) { result->cancelled = true; break; }
                continue;
            }

            // ---- Phase B: ctxByKey for exactly this (filtered) worklist, via a second blocking
            // UI-thread round trip ----
            HANDLE evB = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            BatchPrepCtxRequest reqB; reqB.lang = langCode; reqB.worklist = &worklist; reqB.done = evB;
            ::PostMessage(hwnd, WM_APP_BATCH_PREP_CTX, (WPARAM)&reqB, 0);
            ::WaitForSingleObject(evB, INFINITE);
            ::CloseHandle(evB);
            auto ctxByKey = std::move(reqB.outCtxByKey);

            // ---- normal per-cell loop: IDENTICAL logic to OnGenerateAiSuggestions's worker body ----
            CString langName = LanguageEnglishName(langCode);
            struct Item { const Row* row; long long dlg; };
            std::vector<Item> items; items.reserve(worklist.size());
            for (const auto& row : worklist) {
                long long dlg = -1;
                if (row.res == RES_DIALOGS)
                    for (const auto& r : idxCopy.dialogs())
                        if (r.msgctxt == row.msgctxt && r.msgid == row.msgid) { dlg = r.dialog; break; }
                items.push_back({ &row, dlg });
            }
            std::stable_sort(items.begin(), items.end(),
                             [](const Item& a, const Item& b) { return a.dlg < b.dlg; });

            long long renderedDialog = -1; HWND curDlg = nullptr;
            int cellDone = 0, generated = 0, skipped = 0, flaggedByGates = 0, high = 0, low = 0;
            int cellTotal = (int)items.size();
            StringInfo emptyInfo;
            std::optional<mpctrans::StringHint> emptyHint;
            std::vector<CString> emptySiblings;

            for (const auto& it : items) {
                if (cancel->load()) break;
                const Row& row = *it.row;

                auto ctxIt = ctxByKey.find({ row.msgctxt, row.msgid });
                bool haveInfo = ctxIt != ctxByKey.end() && ctxIt->second.haveInfo;
                const StringInfo& info = haveInfo ? ctxIt->second.info : emptyInfo;
                const std::optional<mpctrans::StringHint>& hint = ctxIt != ctxByKey.end() ? ctxIt->second.hint : emptyHint;
                const std::vector<CString>& siblings = ctxIt != ctxByKey.end() ? ctxIt->second.siblings : emptySiblings;

                CString userPromptCs = BuildAiUserPromptCore(langCode, langName, row, haveInfo, info, hint,
                                                             siblings, idxCopy, po.data());
                std::string usr(CW2A(userPromptCs, CP_UTF8));

                std::string candidateUtf8; bool ok = false;
                for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
                    try {
                        std::string raw = suggest_translation(cfg, apiKey, sys, usr);
                        candidateUtf8 = extract_translation_json(raw);
                        ok = true;
                    } catch (const std::exception&) { /* retry once, then fall through to skip */ }
                }
                ++cellDone;
                if (!ok) {
                    ++skipped;
                    auto* prog = new BatchProgress{ langIndex, langTotal, langCode, cellDone, cellTotal };
                    if (!::PostMessage(hwnd, WM_APP_GEN_ALL_PROGRESS, (WPARAM)prog, 0)) delete prog;
                    continue;
                }
                CString candidate = CA2W(candidateUtf8.c_str(), CP_UTF8);

                auto findings = validate::check_entry(row.msgctxt, row.msgid, candidateUtf8);
                std::string flagsStr;
                for (const auto& f : findings) { if (!flagsStr.empty()) flagsStr += "; "; flagsStr += f.msg; }

                std::optional<int> fitsPx;
                if (row.res == RES_DIALOGS && it.dlg >= 0) {
                    if (renderedDialog != it.dlg) {
                        curDlg = lp.RenderDialog(it.dlg, &host, idxCopy, po[RES_DIALOGS]);
                        renderedDialog = it.dlg;
                    }
                    if (curDlg) {
                        long long controlId = -1;
                        for (const auto& r : idxCopy.dialogs())
                            if (r.dialog == it.dlg && r.msgctxt == row.msgctxt && r.msgid == row.msgid) {
                                controlId = r.control.value_or(-1); break;
                            }
                        if (controlId >= 0) {
                            HWND ctrl = ::GetDlgItem(curDlg, (int)controlId);
                            if (ctrl) {
                                LivePreview::TextFit fit = lp.MeasureControlText(ctrl, candidate);
                                if (fit.measured) fitsPx = std::max<int>(0, fit.renderedPx - fit.availablePx);
                            }
                        }
                    }
                }

                std::optional<double> agreement;
                std::string voteTextUtf8; bool voteOk = false;
                if (voteCfg) {
                    try {
                        std::string raw = suggest_translation(*voteCfg, voteKey, sys, usr);
                        voteTextUtf8 = extract_translation_json(raw);
                        voteOk = true;
                        agreement = confidence::agreement_score(candidateUtf8, voteTextUtf8);
                    } catch (const std::exception&) { /* vote skipped for this cell */ }
                }

                // M3 pass 2: back-translation drift -- see the single-language worker's identical block
                // (OnGenerateAiSuggestions) for the full rationale. TWO calls: back-translate to
                // English, then an LLM semantic-equivalence judge scores meaning-match against the
                // true English msgid (replaces the trigram back_translation_similarity metric).
                std::optional<double> backScore;
                try {
                    std::string btSys = "You translate UI strings to English. Respond with ONLY a JSON "
                        "object of the exact shape {\"translation\": \"...\"} and nothing else -- no "
                        "markdown fences, no commentary.";
                    std::string btUsr = "Translate this " + std::string(CW2A(langName, CP_UTF8)) +
                        " UI string to English:\n" + candidateUtf8;
                    std::string raw = suggest_translation(cfg, apiKey, btSys, btUsr);
                    std::string backTextUtf8 = extract_translation_json(raw);
                    backScore = semantic_equivalence_judge(cfg, apiKey, row.msgid, backTextUtf8);
                } catch (const std::exception&) { /* back-translation skipped for this cell */ }

                suggestion_store::Suggestion s;
                s.lang = langUtf8;
                s.msgctxt = row.msgctxt; s.msgid = row.msgid;
                s.suggestion = candidateUtf8;
                s.model = providerIdCopy + "/" + cfg.model; s.prompt_version = kAiPromptVersion;
                s.fits_px = fitsPx; s.flags = flagsStr;
                s.role = "primary"; s.agreement = agreement; s.back_translation_score = backScore;
                suggestion_store::put(s);

                if (voteOk) {
                    suggestion_store::Suggestion v;
                    v.lang = s.lang; v.msgctxt = row.msgctxt; v.msgid = row.msgid;
                    v.suggestion = voteTextUtf8;
                    v.model = voteCfg->provider_id + "/" + voteCfg->model; v.prompt_version = kAiPromptVersion;
                    v.role = "vote"; v.agreement = agreement;
                    auto voteFindings = validate::check_entry(row.msgctxt, row.msgid, voteTextUtf8);
                    std::string voteFlagsStr;
                    for (const auto& f : voteFindings) { if (!voteFlagsStr.empty()) voteFlagsStr += "; "; voteFlagsStr += f.msg; }
                    v.flags = voteFlagsStr;
                    suggestion_store::put(v);
                }

                ++generated;
                if (!flagsStr.empty() || (fitsPx && *fitsPx > 0)) ++flaggedByGates;

                confidence::ConfidenceInputs ci{ agreement, backScore, !(fitsPx && *fitsPx > 0), !flagsStr.empty() };
                (confidence::classify(ci) == confidence::Confidence::High ? high : low)++;

                result->items.push_back({ langCode, CString(CA2W(row.msgctxt.c_str(), CP_UTF8)),
                                          CString(CA2W(row.msgid.c_str(), CP_UTF8)), candidate });

                auto* prog = new BatchProgress{ langIndex, langTotal, langCode, cellDone, cellTotal };
                if (!::PostMessage(hwnd, WM_APP_GEN_ALL_PROGRESS, (WPARAM)prog, 0)) delete prog;
            }

            BatchGenerateResult::LangSummary ls;
            ls.lang = langCode; ls.generated = generated; ls.skipped = skipped;
            ls.flaggedByGates = flaggedByGates; ls.high = high; ls.low = low;
            ls.alreadySuggested = (int)alreadySuggestedCount;
            result->perLang.push_back(ls);
            result->totalGenerated += generated; result->totalErrors += skipped;
            result->totalAlreadySuggested += (int)alreadySuggestedCount;
            result->totalGateFlagged += flaggedByGates; result->totalHigh += high; result->totalLow += low;

            if (cancel->load()) { result->cancelled = true; break; }   // stop the OUTER language loop too
        }
        result->cancelled = result->cancelled || cancel->load();
        if (!::PostMessage(hwnd, WM_APP_GEN_ALL_DONE, (WPARAM)result, 0)) delete result;
    });
}

// Blocking-handoff handler (Phase A): the batch worker thread posted this and is blocked on
// req->done -- run LoadLanguagePoNoNetwork (UI-thread only) then wake it up. `req` lives on the
// worker's own stack for the duration of the wait, so this raw pointer is safe.
LRESULT MainFrame::OnBatchPrepPo(WPARAM wp, LPARAM) {
    auto* req = (BatchPrepPoRequest*)wp;
    req->outPo = LoadLanguagePoNoNetwork(req->lang);
    ::SetEvent(req->done);
    return 0;
}

// Blocking-handoff handler (Phase B): same shape as OnBatchPrepPo, for BuildGenContext.
LRESULT MainFrame::OnBatchPrepCtx(WPARAM wp, LPARAM) {
    auto* req = (BatchPrepCtxRequest*)wp;
    req->outCtxByKey = BuildGenContext(req->lang, *req->worklist);
    ::SetEvent(req->done);
    return 0;
}

// A worker tick -- advance the progress status + bar for the ALL-languages path. Wording deliberately
// contains "Generating (ALL languages)" (greppable by automation), mirroring OnGenProgress's convention.
LRESULT MainFrame::OnGenProgressAll(WPARAM wp, LPARAM) {
    std::unique_ptr<BatchProgress> p((BatchProgress*)wp);
    m_genAllLangIndex = p->langIndex; m_genAllLangTotal = p->langTotal;
    m_genAllLangCode = p->langCode;
    m_genAllCellDone = p->cellDone; m_genAllCellTotal = p->cellTotal;
    CString s;
    s.Format(L"Generating (ALL languages) — language %d/%d — %s: %d/%d\x2026",
             m_genAllLangIndex, m_genAllLangTotal, (LPCWSTR)m_genAllLangCode,
             m_genAllCellDone, m_genAllCellTotal);
    SetStatus(s);
    if (m_progress.GetSafeHwnd()) {
        m_progress.SetRange32(0, m_genAllCellTotal > 0 ? m_genAllCellTotal : 1);
        m_progress.SetPos(m_genAllCellDone);
    }
    return 0;
}

// UI thread: install the ALL-languages batch result -- seed the session AI-suggestion cache (each item
// carries its OWN language now, unlike the single-language GenerateResult), report a grand summary, and
// refresh the EditPanel if the currently-selected cell might now have a stored suggestion.
LRESULT MainFrame::OnGenerateAllDone(WPARAM wp, LPARAM) {
    std::unique_ptr<BatchGenerateResult> res((BatchGenerateResult*)wp);
    if (m_genThread.joinable()) m_genThread.join();
    m_genRunning = false;
    m_genAllMode = false;
    m_downloading = false;
    if (m_progress.GetSafeHwnd()) m_progress.ShowWindow(SW_HIDE);
    Layout();

    for (const auto& item : res->items)
        m_aiSuggestCache[{ item.msgctxt, item.msgid, item.lang }] = item.text;

    CString s;
    s.Format(L"All languages: generated %d (high %d / low %d), skipped %d errors, %d already "
             L"suggested, %d flagged by gates.%s Re-run to resume.",
             res->totalGenerated, res->totalHigh, res->totalLow, res->totalErrors,
             res->totalAlreadySuggested, res->totalGateFlagged, res->cancelled ? L" (cancelled)" : L"");
    SetStatus(s);

    if (m_curRow >= 0 && m_curRow < (int)m_rows.size()) SelectRow(m_curRow);
    return 0;
}
