// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <afxwin.h>
#include <functional>
#include <vector>
#include "mpctrans/enrichment.h"
#include "mpctrans/validate.h"

// Edit panel for the currently-selected string. Shows (per plan):
//   English source · current translation (.po) · AI suggestion (one-click Accept) ·
//   reference matches · semantic/functional context · inline validation · overflow.
// Editing fires validate::check_entry() for live warnings. MainFrame owns the selection and
// receives commits via OnCommit (it knows which (msgctxt,msgid) is active).
class EditPanel : public CWnd {
public:
    BOOL CreatePanel(CWnd* parent, UINT id);
    void ShowString(const CString& msgctxt, const CString& english, const CString& current,
                    const std::vector<mpctrans::AiSuggestion>&   ai,
                    const std::vector<mpctrans::ReferenceMatch>& refs,
                    const mpctrans::StringInfo* info);
    void ClearString();
    void ShowOverflowWarning(bool overflowing);      // appended to the validation strip
    // The Review Queue's flag context for the selected row (e.g. "REVIEW: overflows by 12px"); ""
    // hides it. Shown as a prominent line in the validation strip (prepended, like the overflow note).
    void SetFlagNote(const CString& note);
    CString GetEdited() const;                       // the translator's new msgstr

    // fired when the user commits an edit (Apply / Accept-AI); MainFrame adds it to m_dirty
    std::function<void(const CString& /*msgstr*/)> OnCommit;
    // fired when the user clicks "Suggest with AI" (only while m_active and not already in flight);
    // MainFrame owns the API key / provider config and the background HTTP call.
    std::function<void()> OnSuggestAi;

    // On-demand AI suggestion (distinct from the bundled `ai` vector shown by ShowString): MainFrame
    // drives these three states across the request's lifecycle. The label carries ONLY the suggested
    // text -- no model/confidence metadata (that lives in the validation strip via SetAiDetails below).
    void SetAiSuggestRequesting();                                   // request just started
    void SetAiSuggestion(const CString& text);                        // request succeeded
    void SetAiSuggestError(const CString& shortMessage);              // request failed
    // Appends validate::check_entry findings for the AI suggestion under the existing validation
    // text (does not replace it — the next edit re-runs RunInlineValidation and supersedes this).
    void ShowAiSuggestionValidation(const std::vector<mpctrans::validate::Finding>& findings);
    // M3: appends `line` to the validation strip (replacing a lone "(no findings)" placeholder, else
    // appending on a new line) — used e.g. for a "still overflows" recheck note. Persists until the
    // next edit re-runs RunInlineValidation (same lifetime as ShowAiSuggestionValidation's appended
    // findings).
    void AppendValidationLine(const CString& line);
    // Appends the AI-suggestion detail block to the validation strip, AFTER whatever warnings are
    // already there (RunInlineValidation's findings + ShowAiSuggestionValidation + any
    // AppendValidationLine notes) — never replaces them. This is where all the AI metadata that used
    // to clutter m_lblAi now lives, in plain language:
    //   Review recommended                              <- only when lowConfidence
    //   Primary (<primaryModel>): <primaryText>
    //   Alternate (<altModel>): <altText>                <- only when altText is non-empty AND
    //                                                        differs (trimmed) from primaryText
    //   Agreement <0.NN> · Back-translation <0.NN>       <- omits either term when its has* flag is
    //                                                        false; omits the whole line when both are
    // hasAgreement/agreement and hasBackScore/backScore are the CString-friendly stand-in for
    // std::optional<double> (matches how suggestion_store::Suggestion's optional scores get unpacked
    // at the call site). lowConfidence must come from mpctrans::confidence::classify() — the single
    // source of truth shared with the bulk-generation summary — never re-derived here.
    void SetAiDetails(const CString& primaryModel, const CString& primaryText,
                      const CString& altModel, const CString& altText,
                      bool hasAgreement, double agreement,
                      bool hasBackScore, double backScore,
                      bool lowConfidence);

protected:
    BOOL PreTranslateMessage(MSG* pMsg) override;   // Tab navigation + Ctrl+A (no dialog manager here)
    afx_msg int  OnCreate(LPCREATESTRUCT);
    afx_msg void OnSize(UINT, int, int);
    afx_msg HBRUSH OnCtlColor(CDC*, CWnd*, UINT);
    afx_msg BOOL OnEraseBkgnd(CDC*);
    afx_msg void OnApply();
    afx_msg void OnAcceptAi();
    afx_msg void OnRefDblClk();        // double-clicked reference -> insert into the edit (no commit)
    afx_msg void OnTranslationChanged();
    afx_msg void OnSuggestAiClicked();
    DECLARE_MESSAGE_MAP()

private:
    void RunInlineValidation();   // validate::check_entry -> warnings under the edit box
    void Layout();
    int  S(int v) const { return ::MulDiv(v, m_dpi, 96); }   // 96-DPI-logical px -> device px
    int  m_dpi = 96;

    CStatic m_ctx, m_lblEnglish, m_lblCurrent, m_lblAi, m_lblRefs, m_info;
    CEdit   m_english, m_translation, m_validation;
    CButton m_btnApply, m_btnAcceptAi, m_btnAiSuggest;
    CListBox m_refs;
    std::vector<CString> m_refTexts;   // raw reference msgstr per row (no "source: " prefix)
    CFont   m_font;
    CString m_msgctxt, m_msgid, m_aiText, m_overflowNote, m_flagNote;
    bool m_active = false;
    bool m_aiRequestInFlight = false;   // guards against double-clicks while a suggestion is in flight
};
