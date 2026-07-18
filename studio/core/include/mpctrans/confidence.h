// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>

// Pure confidence scoring for M3: cross-model
// agreement (NOT self-reported confidence -- self-reported confidence does not correlate with
// correctness) + back-translation drift. Everything here is a pure string/number transform, no I/O,
// no network, no sqlite -- unit-tested directly, and shared verbatim between the M2 bulk-generation
// worker (computes these scores) and the EditPanel/status-line display (must classify identically, or
// "high confidence" in the summary and "LOW CONFIDENCE" on a cell could contradict each other).

namespace mpctrans::confidence {

// Normalizes text for AGREEMENT/back-translation comparison: strips '&' mnemonic markers (both a
// bare '&' before a letter and an escaped '&&' collapse to nothing -- which letter a translator/model
// picked as the accelerator is irrelevant to whether two translations AGREE on the wording),
// collapses runs of ASCII whitespace (space/tab/CR/LF) to a single space, trims leading/trailing
// whitespace, and casefolds ASCII letters (non-ASCII/multibyte UTF-8 bytes pass through unchanged --
// this is a byte-level ASCII casefold, not full Unicode case folding; acceptable approximation, same
// posture as validate::accelerator_letter's byte-level note).
std::string normalize_for_agreement(const std::string& s);

// Character-trigram Dice-coefficient similarity in [0,1] over the RAW strings passed in (callers
// normalize first if they want normalized comparison -- kept separate so back_translation_similarity
// can normalize+mask independently). Special cases: two identical strings (including both empty) ->
// 1.0; exactly one empty -> 0.0; both non-empty but shorter than 3 chars -> 1.0 if equal else 0.0
// (too short to form a trigram); otherwise 2*|shared trigrams (multiset intersection)| / (|trigrams
// A| + |trigrams B|).
double trigram_similarity(const std::string& a, const std::string& b);

// agreement_score(a, b): 1.0 if normalize_for_agreement(a) == normalize_for_agreement(b) exactly,
// else trigram_similarity of the two normalized forms. In [0,1].
double agreement_score(const std::string& a, const std::string& b);

// Masks every printf-style specifier validate::format_specs() finds in `s` to a coarse placeholder
// token so specifier CHOICE (a real bug caught elsewhere, by validate::rule_format) doesn't dominate
// a similarity score meant to measure PROSE drift: numeric/pointer conversions (diouxXeEfFgGaA) ->
// "%D", string-ish conversions (csp@) -> "%S", a bare "%%" (literal percent, not a placeholder) is
// left untouched, a positional wrapper like "%1!s!" is classified by its inner conversion letter
// (s -> %S, d -> %D, etc). Replaces left-to-right, non-overlapping (format_specs already returns
// non-overlapping matches in source order -- rebuild the string by walking `s` and swapping each
// matched span for its token, copying everything in between unchanged).
std::string mask_placeholders(const std::string& s);

// DEPRECATED as the M2/M4 generation worker's back-translation SCORER: superseded by
// mpctrans::semantic_equivalence_judge (ai_client.h), an LLM-judged meaning-equivalence score, which
// is more tolerant of legitimate synonym/word-order/register drift than this trigram string metric.
// Kept in place (and still covered by confidence_test.cpp) since it's a reasonable general-purpose
// string-similarity primitive and other callers may still want it -- just not the generation worker.
//
// Back-translation drift score: mask_placeholders both strings, then normalize_for_agreement both,
// then trigram_similarity. `original` = the true English msgid; `back_translation` = the model's own
// EN reinterpretation of its translated suggestion (see the M2 worker for how this is produced).
double back_translation_similarity(const std::string& original, const std::string& back_translation);

// Inputs to the ONE shared confidence rule (the M3
// spec): agreement/back_translation_score are nullopt when that pass didn't run (single-provider —
// no vote available; or the back-translation call itself failed) -- a nullopt reads as "no signal
// AGAINST", not as a failure, per the settled decision that a missing pass must never itself force
// low confidence (only a pass that RAN and disagreed does).
struct ConfidenceInputs {
    std::optional<double> agreement;
    std::optional<double> back_translation_score;
    bool fits = true;            // false = the candidate overflows its control (a measured hard/soft gate failure)
    bool has_gate_flags = false; // true = validate::check_entry (or equivalent) produced >=1 finding
};

enum class Confidence { High, Low };

// Threshold for ConfidenceInputs::back_translation_score in classify() below. The score itself now
// comes from mpctrans::semantic_equivalence_judge (ai_client.h) -- an LLM judge asked whether the
// true English source and the model's back-translation of its own suggestion convey the same
// meaning -- rather than the back_translation_similarity() string metric above (still available, but
// no longer what the generation worker calls). confidence.cpp/.h stay pure either way: this header
// only documents where the score comes from, it never calls ai_client itself.
constexpr double kBackTranslationThreshold = 0.7;

// high = (agreement >= 0.85 OR agreement is nullopt)
//    AND (back_translation_score >= kBackTranslationThreshold [0.7] OR back_translation_score is nullopt)
//    AND fits AND !has_gate_flags
// low  = otherwise. This is the SINGLE source of truth -- both the bulk-generation summary line and
// the EditPanel's per-cell label/validation-strip note must call this, never re-derive the thresholds.
Confidence classify(const ConfidenceInputs& in);

} // namespace mpctrans::confidence
