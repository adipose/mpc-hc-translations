// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <afxwin.h>
#include <afxcmn.h>
#include <array>
#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "LivePreview.h"
#include "EditPanel.h"
#include "Theme.h"
#include "mpctrans/control_index.h"
#include "mpctrans/enrichment.h"
#include "mpctrans/po.h"
#include "mpctrans/corrections.h"
#include "mpctrans/ai_client.h"
#include "mpctrans/suggestion_store.h"
#include "mpctrans/confidence.h"
#include <tuple>

// The menu-bar preview is an owned popup that natively displays a (dark-themed) menu bar. Its menu
// items are MFT_OWNERDRAW, so Windows sends the owner-draw messages to this window — we route them
// to the shared Theme drawing. A dedicated CWnd keeps it in MFC message-map style (no subclassing).
class CMenuBarWnd : public CWnd {
protected:
    afx_msg void OnMeasureItem(int, LPMEASUREITEMSTRUCT);
    afx_msg void OnDrawItem(int, LPDRAWITEMSTRUCT);
    afx_msg BOOL OnEraseBkgnd(CDC*);
    DECLARE_MESSAGE_MAP()
};

// The preview host (behind the embedded dialog). Its own background is palette-aware so the margin
// around the dialog matches the active theme; a plain CWnd would show its stale class brush.
class CThemedHostWnd : public CWnd {
protected:
    afx_msg BOOL OnEraseBkgnd(CDC*);
    DECLARE_MESSAGE_MAP()
};

// One control in a synthesized dialog mock-up (a dialog MPC-HC builds in C++ with no RC template, so
// we can't render it live — e.g. the MPC Audio Renderer Settings dialog). Drawn by drawAudioRenderer.
struct SynthRow {
    enum Kind { Label, ComboInline, ComboFull, Check, Button, Group, Value } kind = Label;
    CString text;       // translated label / checkbox / button text / group title
    CString value;      // right-hand value shown for Value rows (dim); "" otherwise
    bool    selected = false;   // the string the editor is on — highlighted
};

// Renders a loose (non-dialog) string in a mock-up of where it appears in MPC-HC — a message box,
// an OSD overlay, the status bar, or a tooltip — so the translator sees it in context. Owner-drawn;
// which mock-up is chosen comes from the enrichment's ui_type/ui_location (see MainFrame::ShowMockup).
class CMockupWnd : public CWnd {
public:
    enum class Mode { None, MsgBox, OSD, StatusBar, Tooltip, Menu, Generic, SynthDialog };
    ~CMockupWnd() { if (m_menu) ::DestroyMenu(m_menu); }
    // A synthesized dialog: a titled, tabbed box of `rows`, with `tip` hovered below the selected row.
    void SetSynthDialog(const CString& title, std::vector<CString> tabs, int activeTab,
                        std::vector<SynthRow> rows, const CString& tip, int dpi) {
        m_mode = Mode::SynthDialog; m_title = title; m_tabs = std::move(tabs); m_activeTab = activeTab;
        m_rows = std::move(rows); m_tip = tip; m_dpi = dpi; setMenu(nullptr);
        if (GetSafeHwnd()) Invalidate();
    }
    void SetContent(Mode m, const CString& text, const CString& caption, bool error, bool accel, int dpi) {
        m_mode = m; m_text = text; m_caption = caption; m_error = error; m_accel = accel; m_dpi = dpi;
        setMenu(nullptr);
        if (GetSafeHwnd()) Invalidate();
    }
    // Menu-in-context: an owner-draw HMENU (this window takes ownership) rendered exactly like a real
    // MPC-HC menu, with the item at row `selIndex` highlighted.
    void SetMenu(HMENU menu, int selIndex, int dpi) {
        m_mode = Mode::Menu; setMenu(menu); m_menuSelIndex = selIndex; m_dpi = dpi; m_text.Empty();
        if (GetSafeHwnd()) Invalidate();
    }
protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC*);
    DECLARE_MESSAGE_MAP()
private:
    int S(int v) const { return ::MulDiv(v, m_dpi, 96); }
    void drawMsgBox(CDC&, CRect);
    void drawOSD(CDC&, CRect, const LOGFONTW&);
    void drawStatusBar(CDC&, CRect);
    void drawTooltip(CDC&, CRect);
    void drawMenu(CDC&, CRect);
    void drawGeneric(CDC&, CRect);
    void drawAudioRenderer(CDC&, CRect);   // synthesized tabbed dialog (m_rows)
    UINT prefixFlag() const { return m_accel ? 0u : DT_NOPREFIX; }   // process '&' mnemonics only where apt
    void    setMenu(HMENU m) { if (m_menu) ::DestroyMenu(m_menu); m_menu = m; }
    Mode    m_mode = Mode::None;
    CString m_text, m_caption;
    CString m_title, m_tip;            // synthesized-dialog title + hovered tooltip
    std::vector<CString> m_tabs;       // synthesized-dialog tab labels
    int     m_activeTab = 0;
    std::vector<SynthRow> m_rows;      // synthesized-dialog controls
    HMENU   m_menu = nullptr;   // owner-draw submenu rendered in Menu mode (this window owns it)
    int     m_menuSelIndex = -1;
    bool    m_error = false;
    bool    m_accel = false;   // string carries an '&' accelerator/mnemonic (menu/button/label)
    int     m_dpi = 96;
};

// Resolved bundle artifact paths. Release layout = everything beside Studio.exe; a dev checkout
// resolves into <repo>/dist + <repo>/enrichment/lang + the submodule PO dir.
struct Bundle {
    CString index_json, neutral_dll, core_sqlite, lang_dir;
    CString po_dir;         // CACHED .po at the bundle's pinned upstream SHA (instant browse); may be empty
    bool ok = false;
    static Bundle Locate();
};

// Main window. Layout:
//   [ language picker | Get latest | status ]
//   [ surface tabs: Dialogs | Menus | All strings ]
//   left: string list   center: dialog picker + LivePreview / menu tree+bar   right: EditPanel
// Selecting a language loads its 3 PoFiles instantly from the bundled cache; "Get latest" refetches
// from upstream HEAD for an accurate PR base. Also holds the bundled ControlIndex + CoreEnrichment
// and the language's enrichment pack.
class MainFrame : public CFrameWnd {
public:
    MainFrame() = default;
    BOOL LoadBundle();                              // index + core enrichment + neutral DLL
    // Load a language's 3 .po + its pack, then refresh the active surface. fromGithub=false reads
    // the bundled cache (instant, offline); fromGithub=true fetches the latest .po from GitHub.
    // refreshRc=true also (re)fetches the live mpc-hc.rc (the layout); once fetched it persists, so
    // language switches pass refreshRc=false and only swap translations.
    bool LoadLanguage(const CString& code, bool fromGithub, bool refreshRc = false);
    // Offer to fill base-empty strings from the maintainer's Transifex staging fork (see
    // config::TRANSIFEX_*). Called once per language per session, only when the base .po was just
    // fetched online — see LoadLanguage's call site for the exact guard.
    void TransifexOverlay(const CString& code);
    void SubmitPr();                                // splice edits onto latest -> github::open_translation_pr
    // (Re)position the red locator ring around the selected command-help usage-list entry. Public: the
    // m_cmdHelp scroll/resize subclass (a free WndProc in MainFrame.cpp) calls it on the frame pointer.
    void UpdateCmdRing();

protected:
    afx_msg int  OnCreate(LPCREATESTRUCT);
    afx_msg void OnSize(UINT, int, int);
    afx_msg void OnMove(int, int);
    afx_msg void OnDestroy();
    afx_msg HBRUSH OnCtlColor(CDC*, CWnd*, UINT);
    afx_msg BOOL OnEraseBkgnd(CDC*);
    afx_msg void OnMeasureItem(int, LPMEASUREITEMSTRUCT);   // dark owner-drawn menus
    afx_msg void OnDrawItem(int, LPDRAWITEMSTRUCT);
    afx_msg void OnSettingChange(UINT, LPCTSTR);            // track the OS theme in Follow-Windows mode
    afx_msg void OnNcPaint();                               // cover the light seam under the menu bar
    afx_msg BOOL OnNcActivate(BOOL);
    afx_msg void OnCheckoutClicked();      // "Get latest" button -> fetch from GitHub
    afx_msg void OnLanguageChanged();      // language combo -> instant load from the cache
    afx_msg void OnSubmitPrCmd();
    afx_msg void OnRenderFromRc();         // File > render dialogs from a live .rc (Approach C)
    afx_msg void OnRenderFromBundle();     // File > back to the pinned DLL
    afx_msg void OnViewTheme(UINT id);              // View > Theme > Dark / Light / Windows
    afx_msg void OnUpdateViewTheme(CCmdUI* pCmdUI); // radio checkmark on the active theme
    afx_msg void OnDemoSelect();           // hidden hook: jump to $MPCTRANS_DEMO_DLG (automation)
    afx_msg void OnTabChanged(NMHDR*, LRESULT*);
    afx_msg void OnDialogPicked();
    afx_msg void OnStringSelected(NMHDR*, LRESULT*);
    afx_msg void OnListCustomDraw(NMHDR*, LRESULT*);   // dark row/selection colors (MPC-style)
    afx_msg void OnListGetDispInfo(NMHDR*, LRESULT*);  // virtual list: supply cell text on demand
    afx_msg void OnMenuTreeSelChanged(NMHDR*, LRESULT*);
    afx_msg void OnMenuTreeRClick(NMHDR*, LRESULT*);
    afx_msg LRESULT OnPrefetchDone(WPARAM, LPARAM);   // background bulk-download completed
    afx_msg LRESULT OnPrefetchProgress(WPARAM, LPARAM);  // per-language download progress
    afx_msg LRESULT OnAutoSelect(WPARAM, LPARAM);        // automation: select tab + list row
    afx_msg BOOL OnCopyData(CWnd*, COPYDATASTRUCT*);     // automation: select tab + row by msgctxt
    afx_msg void OnSuggestFixClicked();                  // "Suggest fix…" — research panel correction
    afx_msg LRESULT OnSuggestFixDone(WPARAM, LPARAM);     // background PR submit completed
    afx_msg void OnSuggestAiClicked();                    // EditPanel "Suggest with AI" button
    afx_msg LRESULT OnAiSuggestDone(WPARAM, LPARAM);      // background AI HTTP call completed
    afx_msg void OnFileAiProvider();                      // File > AI provider… -> AiSettingsDlg
    afx_msg LRESULT OnFitScanProgress(WPARAM, LPARAM);    // background fit-scan progress
    afx_msg LRESULT OnFitScanDone(WPARAM, LPARAM);        // background fit-scan completed
    afx_msg void OnDismissClicked();                      // "Dismiss" — Review tab, acts on the selected row
    afx_msg void OnGenerateAiSuggestions();                // File > Generate AI suggestions… (M2 bulk generate)
    afx_msg LRESULT OnGenProgress(WPARAM, LPARAM);         // background bulk-generation progress
    afx_msg LRESULT OnGenerateDone(WPARAM, LPARAM);        // background bulk-generation completed
    afx_msg void OnGenerateAiSuggestionsAll();              // File > Generate AI suggestions for ALL languages… (M4 batch)
    afx_msg LRESULT OnGenProgressAll(WPARAM, LPARAM);       // background ALL-languages bulk-generation progress
    afx_msg LRESULT OnGenerateAllDone(WPARAM, LPARAM);      // background ALL-languages bulk-generation completed
    // Internal blocking-handoff handlers (NOT afx_msg command routing -- posted directly by the batch
    // worker thread, which blocks on a HANDLE until these run; see OnGenerateAiSuggestionsAll's comment).
    LRESULT OnBatchPrepPo(WPARAM, LPARAM);                  // batch worker -> UI thread: load a language's po (no network)
    LRESULT OnBatchPrepCtx(WPARAM, LPARAM);                 // batch worker -> UI thread: build research context for a worklist
    afx_msg void OnExportAiContextString();                 // File > Export AI-prep context for this string...
    afx_msg void OnExportAiContextLanguage();                // File > Export AI-prep context for current language...
    afx_msg LRESULT OnExportProgress(WPARAM, LPARAM);        // background whole-language AI-prep export progress
    afx_msg LRESULT OnExportDone(WPARAM, LPARAM);             // background whole-language AI-prep export completed
    afx_msg void OnCheckDataUpdate();                         // File > Check for data updates...
    afx_msg LRESULT OnDataUpdateDone(WPARAM, LPARAM);         // background data-update check/apply completed
    DECLARE_MESSAGE_MAP()

private:
    // 0..2 index the per-resource PoFiles; RES_UNTRANS is a surface tab only (all untranslated);
    // RES_REVIEW is the M1 deterministic-flags Review Queue (also a surface tab only).
    enum Res { RES_DIALOGS = 0, RES_MENUS = 1, RES_STRINGS = 2, RES_COUNT = 3, RES_UNTRANS = 3, RES_REVIEW = 4 };
    struct Row {
        std::string msgctxt, msgid; int res;
        enum class Flag { None, FitHard, Placeholder, Accelerator, FitTight } flag = Flag::None;
        CString evidence;              // "overflows by 104px", "placeholder %d missing", etc.
        long long flagDialog = -1;     // dialog id owning a fit flag (-1 = n/a)
        long long flagControlId = -1;  // control id owning a fit flag (-1 = n/a, or for non-fit flags)
    };
    // Per-language cache entry the fit worker produces and PopulateReviewRows() reads; kept separate
    // from Row because Row is transient/tab-specific while this persists across tab switches.
    struct ReviewFlag {
        std::string msgctxt, msgid;
        Row::Flag kind = Row::Flag::None;
        CString evidence;
        long long dialog = -1, controlId = -1;
        int overflowPx = 0;            // for sorting fit flags most-severe-first; 0 for non-fit
    };
    // Result of a background fit scan (see EnsureFitScan). Heap-allocated by the worker, ownership
    // passed to the UI thread via WM_APP_FIT_SCAN_DONE. A private nested type (not a free struct at
    // file scope like PrefetchResult) because it holds ReviewFlag, itself private to MainFrame.
    struct FitScanResult { std::wstring lang; std::vector<ReviewFlag> flags; };
    // Shared by the background fit-scan worker and RecomputeFitForDialog: converts raw fit
    // measurements into ReviewFlag entries (hard overflow / tight fit + evidence text) so the
    // thresholds/wording never drift between the two call sites. Static (touches no instance state)
    // so it's safe to call from the worker thread.
    static void AppendFitFlags(std::vector<ReviewFlag>& out, long long dialogId,
                               const std::vector<LivePreview::FitMeasurement>& measurements);

    void Layout();
    int  S(int v) const { return ::MulDiv(v, m_dpi, 96); }   // 96-DPI-logical px -> device px
    int  m_dpi = 96;
    void PopulateLanguages();
    void StartPrefetch();                  // background: fetch RC + every language's .po from GitHub
    void RefreshAfterLoad();               // re-render the active surface + list after a language load
    void PopulateDialogCombo();
    void PopulateMenuCombo();
    void BuildMenuTree();
    void AddMenuNodes(HMENU m, HTREEITEM parent);
    void SubstituteMenuInPlace(HMENU m);            // translate a menu's item text (recursive)
    void UpdateMenuBarPreview();                    // native (dark-themed) horizontal menu bar (main menu)
    void ApplyThemeChange(Theme::Mode m);           // switch theme live across the whole app
    void ReloadFrameMenu();                         // rebuild the File/View menu for the current theme
    void PopulateList();
    void EnsureFitScan();                 // kick off the background scan for m_lang if not cached
    void PopulateReviewRows();             // build m_rows for tab==RES_REVIEW from validate:: + m_fitCache
    void RecomputeFitForDialog(long long dialogId);  // synchronous, single-dialog re-measure (post-edit)
    void RefreshListRow(int item);
    CString RowText(int item, int col);             // (context / english / translation) for a row
    void RenderCurrentDialog();
    long long DialogForString(const std::string& ctx, const std::string& id);   // string -> its IDD
    long long TooltipDialog(const std::string& ctx, CString& ctlEnglish);        // tooltip -> its control's IDD
    bool ShowComboItem(const std::string& ctx);   // combo-item string -> its page with the dropdown expanded
    bool ShowAdvancedSetting(const std::string& ctx, const CString& descText);  // IDS_PPAGEADVANCED_* -> Advanced list row
    bool ShowAdvancedList(const std::string& selName, const CString& tooltip,   // synthesized Advanced Name/Value list
                          const std::vector<CString>& dropItems, int dropSel);
    bool ShowArsSetting(const std::string& ctx);   // IDS_ARS_* -> synthesized MPC Audio Renderer dialog
    bool ShowPlaylistMenu(const std::string& ctx); // IDS_PLAYLIST_* -> synthesized playlist context menu
    long long PrepareAdvancedEditors(const std::vector<const char*>& show);     // hide/lift Advanced inline editors
    void ShowCommandHelp(const std::string& selCtx);   // command-line strings: usage list; ring `selCtx`'s entry
    void HideCommandHelp();
    // Loose (non-dialog) strings: pick a context mock-up from the enrichment and render it.
    bool ShowMockup(const mpctrans::StringInfo* si, const std::string& ctx, const CString& text, int res);
    // Build an owner-draw HMENU for menu command `ctx`: the real submenu it lives in (translated) when
    // found in the pinned menu resources, else a single-item menu with `text`. Sets selCmd; caller owns.
    HMENU BuildMenuMockup(const std::string& ctx, const CString& text, int& selIndex);
    void HideMockup();
    void ShowResearch(const mpctrans::StringInfo* si);   // enrichment write-up under the rendering
    // On-demand "Suggest with AI" prompt assembly (see OnSuggestAiClicked in MainFrame.cpp for the
    // full flow + the ~4KB truncation rule's rationale). Thin wrapper over BuildAiUserPromptCore
    // (below) that gathers `this` members into explicit args — see that function's comment for why.
    CString BuildAiUserPrompt(const Row& row);
    // The actual (pure, thread-safe) prompt-assembly logic, factored out of BuildAiUserPrompt so the
    // M2 background generation worker (see OnGenerateAiSuggestions) can call the IDENTICAL logic off
    // the UI thread after snapshotting its inputs by value — no forking/rewriting the v3 prompt rules,
    // just parameterizing what used to be implicit `this` member reads. Static: touches no instance
    // state, safe from any thread (same rationale as AppendFitFlags above).
    // M3: `siblingLines` are fidelity-ranked cross-language grounding lines (see BuildSiblingLines) —
    // a trimming pool alongside same-dialog/family pairs, rendered in their own SIBLING TRANSLATIONS
    // section. v3.2: the reference-match translation pool that used to ride alongside these was
    // removed from the generated prompt (see kAiPromptVersion's comment) -- no `refs` param anymore.
    // v3.3: `hint`, when present, is the row's translator hint (mpctrans::CoreEnrichment::hint_for) --
    // rendered as a compact "Translator hints:" block after Meaning/Function, before the sibling
    // examples; nullopt (no `string_hints` table in this bundle, or no row for this string) omits the
    // block entirely.
    static CString BuildAiUserPromptCore(const CString& langCode, const CString& langName, const Row& row,
                                         bool haveInfo, const mpctrans::StringInfo& info,
                                         const std::optional<mpctrans::StringHint>& hint,
                                         const std::vector<CString>& siblingLines,
                                         const mpctrans::ControlIndex& idx,
                                         const mpctrans::PoFile poByRes[RES_COUNT]);
    // M3: up to 3 translations of the SAME (msgctxt,msgid) from the top fidelity-ranked languages that
    // (a) are not `targetLang` and (b) have a non-empty translation in this session's prefetched .po
    // set (m_sessionPo; the CURRENT language's live m_po is its own entry there too). Returns
    // display-ready prompt lines; empty when no fidelity ranking / no siblings loaded (graceful).
    // M4: takes the target language EXPLICITLY (rather than reading m_lang implicitly) so BuildGenContext
    // can build sibling lines for a DIFFERENT language than whatever's on screen (the ALL-languages batch
    // path); the single-language call site still just passes m_lang, so behavior there is unchanged.
    std::vector<CString> BuildSiblingLines(const Row& row, const CString& targetLang) const;
    // AI-prep export (JSONL): the SAME fidelity-ranked sibling walk as BuildSiblingLines, factored out
    // to return RAW (normalized lang code, msgstr) pairs instead of already-formatted prompt display
    // lines -- BuildSiblingLines is now a thin formatter over this (see MainFrame.cpp; a pure
    // extract-method refactor, verified by inspection to produce byte-identical BuildSiblingLines
    // output). The AI-prep exporter (OnExportAiContextString/Language) calls this directly since its
    // JSON `siblings` field is an OBJECT keyed by language code, not a formatted prompt line.
    std::vector<std::pair<CString, CString>> BuildSiblingPairs(const Row& row, const CString& targetLang) const;
    // Shared eligibility/flag logic (M2): given a cell's CURRENT non-empty msgstr, returns the single
    // deterministic flag that would route it into the Review Queue's placeholder/accelerator checks or
    // the fit cache (Placeholder > Accelerator > Fit, first match wins — Row::Flag is a single enum, so
    // BuildAiUserPrompt's fix-mode switch only ever handles one flag at a time; a cell that
    // simultaneously trips more than one rule still surfaces as (up to) several ROWS in the M1 Review
    // tab via PopulateReviewRows's own independent enumeration — that's a different, richer shape not
    // reused here). Flag::None means "passes validation, not eligible for an AI suggestion". Used by
    // BOTH BuildGenWorklist (what gets generated) and SelectRow (when a stored suggestion may show) so
    // the two never drift — do not reimplement this check a third time.
    Row::Flag CellFlag(const std::string& msgctxt, const std::string& msgid, int res,
                       const std::string& msgstr, CString& evidenceOut,
                       long long& flagDialogOut, long long& flagControlOut);
    // M4: static twin of CellFlag, factored out so the ALL-languages batch worker (off the UI thread,
    // one language at a time, none of them necessarily m_lang) can run the IDENTICAL placeholder/
    // accelerator/fit rules against a fit-flag vector it computed itself, instead of m_fitCache[m_lang].
    // `fitFlags` may be nullptr (treated as "no fit data yet" -- same as CellFlag's own m_fitCache-miss
    // case). CellFlag forwards to this with m_fitCache[m_lang] so the two can never drift.
    static Row::Flag CellFlagCore(const std::string& msgctxt, const std::string& msgid, int res,
                                  const std::string& msgstr, CString& evidenceOut,
                                  long long& flagDialogOut, long long& flagControlOut,
                                  const std::vector<ReviewFlag>* fitFlags);
    // M3: the newest stored primary suggestion for a cell + its vote sibling + the confidence class,
    // all resolved through mpctrans::confidence::classify (the single source of truth for high/low).
    // Shared between SelectRow's display and (in future) any bulk summary so they can't drift.
    struct StoredView {
        mpctrans::suggestion_store::Suggestion primary;
        std::optional<mpctrans::suggestion_store::Suggestion> vote;
        mpctrans::confidence::Confidence conf;
    };
    static std::optional<StoredView> LoadStoredView(const std::string& lang, const std::string& msgctxt,
                                                     const std::string& msgid);
    // The M2 "Generate AI suggestions" worklist: every empty cell (flag=None) across all 3 resources,
    // PLUS every non-empty cell CellFlag() flags (validation-failing accepted cells). Used both to
    // compute the confirmation dialog's counts and as the actual per-cell work order for the
    // background generation worker.
    std::vector<Row> BuildGenWorklist();
    // M4: static twin of BuildGenWorklist, factored out for the ALL-languages batch worker -- iterates
    // an explicit `poByRes` (a snapshot of some OTHER language's 3 PoFiles, not necessarily m_po) and
    // calls CellFlagCore(..., fitFlags) instead of the member CellFlag. BuildGenWorklist() forwards to
    // this with m_po/m_fitCache[m_lang] so the two can never drift.
    static std::vector<Row> BuildGenWorklistCore(const mpctrans::PoFile poByRes[RES_COUNT],
                                                  const std::vector<ReviewFlag>* fitFlags);

    // M2/M4: per-cell research context resolved from m_core/m_researchOverrides + sibling grounding --
    // everything BuildAiUserPromptCore needs besides the row itself. Hoisted to file scope in
    // MainFrame.cpp (near GenerateResult) is what the plan called for, but the type must be nameable
    // right here too (BuildGenContext's return type, below) -- a header-visible member function
    // declaration can't use a type that's only forward-declared as an incomplete std::map value_type
    // (no standard guarantee associative containers tolerate that), so this is defined as a private
    // nested type instead. Functionally identical either way; only the type's home changed.
    // v3.2: no `refs` field anymore -- the reference-match translation pool was removed from the
    // generated prompt (see kAiPromptVersion's comment); the References PANEL's own independent
    // m_pack.refs_for lookup in SelectRow is untouched.
    struct PromptCtx {
        bool haveInfo = false;
        mpctrans::StringInfo info;
        std::optional<mpctrans::StringHint> hint;   // v3.3: translator hint layer, see hint_for()
        std::vector<CString> siblings;
    };
    // UI-thread only: m_sessionPo is a plain std::map the UI thread also writes (LoadLanguage/
    // OnPrefetchDone) — a background thread must never read it directly. Session-cache hit, else the
    // bundled disk cache (m_bundle.po_dir), NEVER the network — mirrors LoadLanguage's fromGithub=false
    // branch, but returns BY VALUE instead of mutating m_po/m_lang/m_checkedOut/m_dirty/drafts/status,
    // since batch generation must never disturb whatever language the user has on screen while it runs
    // in the background. nullopt = genuinely unavailable (no session cache entry AND no bundled disk
    // cache for this language) — caller counts the language as skipped ("not loaded, no network").
    std::optional<std::array<mpctrans::PoFile, RES_COUNT>> LoadLanguagePoNoNetwork(const CString& code);
    // UI-thread only (CoreEnrichment::by_key / LangPack::open share the SQLITE_THREADSAFE=0 build).
    // Builds the ctxByKey research-context map for a SPECIFIC language's worklist — used both by the
    // existing per-language OnGenerateAiSuggestions (replacing its inline loop, so the two paths can't
    // drift) and by the ALL-LANGUAGES batch worker's per-language handoff. Opens its OWN LangPack for
    // `langCode` (m_bundle.lang_dir + langCode + ".sqlite") rather than reusing m_pack, since the batch
    // path is very often generating for a DIFFERENT language than whatever's on screen; confirmed safe
    // (see MainFrame.cpp) because LangPack is a pure read-only sqlite reader with no session state, so
    // a freshly-opened pack for the SAME file the live m_pack already points at reads identically.
    std::map<std::pair<std::string, std::string>, PromptCtx>
        BuildGenContext(const CString& langCode, const std::vector<Row>& worklist);

    // ---- "Export AI-prep context" (JSONL) — see OnExportAiContextString/Language ----
    //
    // Per-cell context resolved from m_core/m_researchOverrides + fidelity sibling grounding --
    // everything BuildAiPrepContext needs besides the row itself and available_px (which comes from a
    // separate off-screen render/measure pass, see MeasureAvailablePx below). Deliberately NOT the same
    // shape as PromptCtx (used by the AI-suggestion generation path above): the export wants RAW (lang
    // code, text) sibling pairs for the JSON `siblings` OBJECT, not PromptCtx's already-formatted prompt
    // lines, and never needs `refs` -- the reference corpus is explicitly out of scope for
    // this export (licensing; see the feature's decided scope).
    struct ExportCtx {
        bool haveInfo = false;
        mpctrans::StringInfo info;
        std::vector<std::pair<CString, CString>> siblings;
    };
    // UI-thread only (CoreEnrichment::by_key is SQLITE_THREADSAFE=0; BuildSiblingPairs reads
    // m_sessionPo, a plain map the UI thread also writes) -- same posture/rationale as BuildGenContext,
    // which this mirrors but for the export's leaner context shape (no LangPack/refs needed). Resolve
    // this BEFORE spawning the whole-language export's worker thread, exactly like BuildGenContext
    // already does for the M2/M4 generation workers.
    std::map<std::pair<std::string, std::string>, ExportCtx>
        BuildExportContext(const CString& langCode, const std::vector<Row>& worklist);

    // The exported JSON schema, one struct per translatable cell (see OnExportAiContextString's comment
    // for the exact field names/shape). `glossary` isn't a field here since it is ALWAYS an empty array
    // in this export -- no glossary reader exists anywhere in the Studio bundle today (confirmed: zero
    // "glossary" hits under studio/); a mined glossary database was evaluated and parked (see
    // data/README.md) and is not shipped to the bundle -- so ToJsonLine emits the literal "glossary": []
    // key directly rather than threading an always-empty field through this struct.
    struct AiPrepContext {
        std::string msgctxt, msgid, current, meaning, function, ui_type, ui_location;
        std::vector<std::pair<std::string, std::string>> siblings;   // (normalized lang code, msgstr), UTF-8
        std::optional<int> available_px;   // nullopt -> JSON null (no owning dialog / not measurable)
    };
    // Assembles one cell's AiPrepContext. Pure (no `this`, no I/O) -- safe to call off the UI thread
    // once its inputs are already resolved (StringInfo/siblings via BuildExportContext -- UI-thread
    // only; availablePx via MeasureAvailablePx -- thread-safe standalone). Static: same rationale as
    // BuildAiUserPromptCore being callable from a background generation worker.
    static AiPrepContext BuildAiPrepContext(const Row& row, const std::string& current, bool haveInfo,
                                            const mpctrans::StringInfo& info,
                                            const std::vector<std::pair<CString, CString>>& siblings,
                                            std::optional<int> availablePx);
    // Serializes one AiPrepContext to a compact JSON line via nlohmann::json (no pretty-print, no
    // trailing '\n' -- callers append it themselves so multiple lines concatenate into valid JSONL).
    static std::string ToJsonLine(const AiPrepContext& ctx);
    // Off-screen (never-shown WS_POPUP host) render + LivePreview::MeasureFit pass across every id in
    // `dialogIds`, returning available px for EVERY measurable cell -- unlike AppendFitFlags (Review-
    // Queue-specific, drops non-overflow measurements), this keeps ALL of them, since the export needs
    // available_px for every cell, not just the ones that overflow. Group-aware, mirroring
    // AppendFitFlags' `avail = grouped ? groupAvailablePx : availablePx` selection line. Static and
    // thread-safe by the same reasoning as EnsureFitScan's worker (its own never-shown WS_POPUP host +
    // its own LivePreview instance -- MFC's window-creation hook state is lazily thread-local, not tied
    // to AfxBeginThread) -- callable synchronously on the UI thread (per-string export: one dialog, fast)
    // or from a background worker (whole-language export: the ~50-dialog scan).
    static std::map<std::pair<std::string, std::string>, int> MeasureAvailablePx(
        const std::set<long long>& dialogIds, const mpctrans::ControlIndex& idx,
        const mpctrans::PoFile& dialogsPo, const CString& neutralDll, bool useRc,
        const std::vector<mpctrans::RcDialog>& rcDialogs);

    // Shared by OnGenerateAiSuggestions and OnGenerateAiSuggestionsAll: resolve the configured AI
    // provider/model, auto-opening AiSettingsDlg if no key is stored yet (Save persists; Cancel leaves
    // things unconfigured). Returns false (nothing else touched) if still unconfigured afterward.
    bool ResolveAiProvider(std::string& providerId, std::string& model, std::string& apiKey);
    void SelectRow(int item);
    void ReapplyHighlight();   // reposition the red locator frame after the window moves/resizes
    void OnPreviewClick(HWND ctrl);
    void CommitEdit(const CString& msgstr);
    mpctrans::PoFile* PoFor(const std::string& ctx, const std::string& id, int* res = nullptr);
    bool IsEdited(const std::string& ctx, const std::string& id) const;   // edited & pending (dirty)
    void SaveDrafts();          // persist pending edits for the current language to disk
    void ApplyDrafts();         // re-apply persisted edits after (re)loading the language
    CString DraftsPath() const;
    void SetStatus(const CString& s) { m_status.SetWindowText(s); }

    Bundle  m_bundle;
    CString m_lang;
    bool    m_fromGithub = false;
    bool    m_checkedOut = false;

    mpctrans::ControlIndex   m_index;      // pinned (from control-index.json)
    mpctrans::ControlIndex   m_rcIndex;    // derived from a live RC (Approach C); dialogs + pinned menus
    bool m_useRcIndex = false;
    const mpctrans::ControlIndex& Idx() const { return m_useRcIndex ? m_rcIndex : m_index; }
    mpctrans::CoreEnrichment m_core;  bool m_haveCore = false;
    mpctrans::LangPack       m_pack;  bool m_havePack = false;
    // M3: fidelity-ranked languages (lab code, pct — sorted desc), set once in LoadBundle. Empty when
    // the bundle's core-enrichment.sqlite has no language_fidelity table (older export) — graceful.
    std::vector<std::pair<std::string, double>> m_fidelity;
    mpctrans::PoFile         m_po[RES_COUNT];
    std::set<std::pair<std::string, std::string>> m_dirty[RES_COUNT];   // edited & pending a PR
    // in-memory cache of .po fetched from GitHub this session, keyed by language code, so
    // revisiting a language is instant. Get latest (refreshRc) overwrites the current language's entry.
    std::map<std::wstring, std::array<mpctrans::PoFile, RES_COUNT>> m_sessionPo;
    // TransifexOverlay: languages already offered this session (dedup — ask at most once per language).
    std::set<std::string> m_txHandled;
    std::thread        m_prefetchThread;   // background bulk-download of all languages
    std::atomic<bool>  m_prefetchCancel{false};

    // Review Queue (M1): per-language cache of deterministic FIT flags, computed in the background.
    // A language is considered "computed" once m_fitCache has AN ENTRY for its key (even an empty
    // vector — that means the scan ran and found nothing).
    std::map<std::wstring, std::vector<ReviewFlag>> m_fitCache;
    std::thread        m_fitThread;
    std::atomic<bool>  m_fitCancel{false};
    bool               m_fitScanRunning = false;
    int                m_fitScanDone = 0, m_fitScanTotal = 0;          // progress
    std::set<std::tuple<std::wstring, std::string, std::string>> m_dismissed;  // (lang, ctx, msgid)

    // Research corrections ("Suggest fix…", see SuggestFixDlg + mpctrans::corrections). A per-session
    // overlay so an accepted correction shows immediately in ShowResearch/SelectRow without waiting on
    // the PR to merge and a fresh bundle export (mutating m_core in place isn't worth the complexity
    // for a read-only sqlite reader — see MainFrame::SelectRow, which applies this after m_core.by_key).
    struct ResearchOverride { std::optional<std::string> semantic_purpose, functional_purpose; };
    std::map<std::pair<std::string, std::string>, ResearchOverride> m_researchOverrides;
    mpctrans::StringInfo m_curInfo;         // the selected row's StringInfo (overlay applied), for the dialog
    bool                 m_haveCurInfo = false;
    // Translator hint layer (M4): the selected row's StringHint, if this bundle has a `string_hints`
    // table and a matching row (see mpctrans::CoreEnrichment::hint_for) -- resolved alongside m_curInfo
    // in SelectRow, threaded into BuildAiUserPrompt the same way.
    std::optional<mpctrans::StringHint> m_curHint;
    std::thread          m_suggestFixThread;   // background PR submit (see OnSuggestFixClicked)

    // On-demand "Suggest with AI" (see EditPanel::OnSuggestAi / OnSuggestAiClicked). Session cache
    // keyed (msgctxt, msgid, lang-code) so re-selecting a string (or switching back to a language)
    // within this run doesn't re-spend a network call.
    std::thread m_aiSuggestThread;
    std::map<std::tuple<CString, CString, CString>, CString> m_aiSuggestCache;

    // M2 bulk "Generate AI suggestions" background worker (see OnGenerateAiSuggestions). The worker
    // never touches live MainFrame members after launch — everything it needs (worklist, ControlIndex,
    // PoFile snapshots, per-cell research context) is snapshotted by value first; see the function for
    // the full rationale (mirrors EnsureFitScan's proven off-UI-thread pattern).
    std::thread       m_genThread;
    std::atomic<bool> m_genCancel{false};
    bool              m_genRunning = false;
    int               m_genDone = 0, m_genTotal = 0;   // progress, for the re-invoke-while-running prompt

    // M4: ALL-LANGUAGES batch generation (see OnGenerateAiSuggestionsAll). Reuses m_genThread/
    // m_genCancel/m_genRunning above (the two commands are mutually exclusive by construction — the
    // busy guard refuses to start either while m_genRunning is already true) but tracks its OWN
    // cross-language progress separately so the shared "already running — cancel?" prompt and the
    // status-bar text can distinguish which command is actually in flight.
    bool       m_genAllMode = false;                          // true while m_genThread runs the ALL-languages path
    int        m_genAllLangIndex = 0, m_genAllLangTotal = 0;  // which language (1-based) / how many, ALL-mode only
    CString    m_genAllLangCode;                               // current language code being processed, ALL-mode only
    int        m_genAllCellDone = 0, m_genAllCellTotal = 0;   // current language's cell progress, ALL-mode only
    // Blocking UI-thread round-trip request the batch worker thread posts (see OnGenerateAiSuggestionsAll's
    // comment for the two-phase design): the worker owns this struct on its OWN STACK for the entire
    // WaitForSingleObject wait, so handing the UI thread a raw pointer through WPARAM is safe. Nested
    // (not file-scope) because it carries a private nested Row*/PromptCtx -- see BuildGenContext's
    // comment for why those can't be named outside the class.
    struct BatchPrepCtxRequest {
        CString lang;
        const std::vector<Row>* worklist = nullptr;
        std::map<std::pair<std::string, std::string>, PromptCtx> outCtxByKey;   // filled by OnBatchPrepCtx
        HANDLE done = nullptr;
    };

    // "Export AI-prep context" whole-language worker (see OnExportAiContextLanguage). Same posture as
    // m_genThread/m_genRunning above: everything the worker needs (worklist, ControlIndex, PoFile
    // snapshots, per-cell ExportCtx already resolved via BuildExportContext) is snapshotted by value on
    // the UI thread BEFORE the thread starts; the worker then does pure file I/O + the off-screen
    // measure pass (MeasureAvailablePx) + JSON serialization, touching no MainFrame member. No cancel
    // flag: unlike AI generation (network calls, can run for minutes), this is pure in-memory po +
    // enrichment lookups with no network — expected to finish in well under a few seconds even for the
    // largest language, so re-invoking mid-run is simply refused (see the guard in
    // OnExportAiContextLanguage) rather than offering a cancel prompt.
    std::thread m_exportThread;
    bool        m_exportRunning = false;

    // "Check for data updates" (see OnCheckDataUpdate + mpctrans::data_update): fetches
    // dist/data-manifest.json, downloads+verifies any changed artifact, and atomically replaces it on
    // disk. No cancel flag, same posture as m_suggestFixThread/m_aiSuggestThread — a handful of small
    // files at most, expected to finish quickly; OnDestroy just joins it.
    std::thread m_dataUpdateThread;

    CFont      m_font;
    CComboBox  m_langCombo, m_dlgCombo;
    CButton    m_btnCheckout;
    CButton    m_btnSuggestFix;   // "Suggest fix…" — docked at the research panel's top-right
    CButton    m_btnDismiss;      // "Dismiss" — visible only on the Review tab, acts on the selected row
    CStatic    m_status;
    CProgressCtrl m_progress;      // shown while the background download runs
    bool       m_downloading = false;
    CTabCtrl   m_tabs;
    CListCtrl  m_list;
    CThemedHostWnd m_previewHost;
    CTreeCtrl  m_menuTree;
    CEdit      m_cmdHelp;     // command-line usage list, shown over the preview for IDS_CMD_* strings
    int        m_cmdSelStart = -1, m_cmdSelEnd = -1;   // char range of the selected entry in m_cmdHelp's text (-1 = none)
    CMockupWnd m_mockup;      // context mock-up (message box / OSD / status bar / tooltip) for loose strings
    CEdit      m_research;    // the string's enrichment write-up, shown under the rendering
    CMenuBarWnd m_menuBar;   // owned popup showing a native, dark-themed translated menu bar (main menu)
    CRect      m_menuBarRect;// where the bar strip sits, in this frame's client coords (empty = hidden)
    HMENU      m_barMenu = nullptr;             // translated menu shown (natively) in the bar preview
    LivePreview m_preview;
    EditPanel  m_edit;

    std::vector<Row>       m_rows;      // list rows -> po key
    std::vector<long long> m_dlgIds;    // dialog combo index -> IDD numeric id
    std::vector<long long> m_menuIds;   // menu combo index -> MENU resource id
    std::map<HTREEITEM, std::pair<std::string, std::string>> m_menuNodeKey;  // node -> (ctx,msgid)
    std::map<HTREEITEM, HMENU> m_menuNodeSub;   // popup node -> its submenu (for the native preview)
    long long m_curDialog = -1;
    long long m_curMenu = -1;
    long long m_barMenuId = -1;   // menu currently built into the bar preview (-1 forces a rebuild)
    int       m_curRow = -1;
    Row::Flag m_curRowFlag = Row::Flag::None;   // mirrors m_rows[m_curRow].flag when tab==RES_REVIEW
    HMENU     m_trackMenu = nullptr;   // translated live menu backing the tree + right-click popup
};
