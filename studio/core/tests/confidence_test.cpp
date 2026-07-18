// SPDX-License-Identifier: GPL-3.0-or-later
// Pure checks for mpctrans::confidence -- no I/O, no network, runs on every platform.
// Plain ASCII only in this file (no literal umlauts) to sidestep any source-encoding pitfalls
// under MSVC; ASCII substitutes like "oeffnen"/"offnen" stand in for near-match German text.
#include "mpctrans/confidence.h"
#include <cstdio>
#include <string>
using namespace mpctrans::confidence;

int main() {
    int fail = 0;
    auto err = [&](const std::string& m) { ++fail; std::printf("  FAIL %s\n", m.c_str()); };

    // ---- normalize_for_agreement ----
    {
        if (normalize_for_agreement("&Play") != normalize_for_agreement("Play"))
            err("normalize_for_agreement: '&Play' should normalize equal to 'Play'");
        if (normalize_for_agreement("a&&b") != "ab")
            err("normalize_for_agreement: 'a&&b' should normalize to 'ab'");
        if (normalize_for_agreement("  Hi   there  ") != "hi there")
            err("normalize_for_agreement: whitespace collapse/trim/casefold failed, got '" +
                normalize_for_agreement("  Hi   there  ") + "'");
        if (normalize_for_agreement("HELLO") != normalize_for_agreement("hello"))
            err("normalize_for_agreement: casefold failed");
        std::printf("normalize_for_agreement: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- trigram_similarity ----
    {
        if (trigram_similarity("hello", "hello") != 1.0)
            err("trigram_similarity: identical strings should be 1.0");
        if (trigram_similarity("", "") != 1.0)
            err("trigram_similarity: both empty should be 1.0");
        if (trigram_similarity("hello", "") != 0.0)
            err("trigram_similarity: one empty should be 0.0");
        if (trigram_similarity("ab", "ab") != 1.0)
            err("trigram_similarity: equal short strings should be 1.0");
        if (trigram_similarity("ab", "cd") != 0.0)
            err("trigram_similarity: unequal short strings should be 0.0");

        double similar = trigram_similarity("Datei oeffnen", "Datei offnen");
        if (!(similar > 0.5))
            err("trigram_similarity: near-match pair should score > 0.5, got " + std::to_string(similar));

        double different = trigram_similarity("Datei oeffnen", "Wiedergabe starten");
        if (!(different < 0.3))
            err("trigram_similarity: unrelated pair should score < 0.3, got " + std::to_string(different));

        std::printf("trigram_similarity: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- agreement_score ----
    {
        double exact = agreement_score("&Play", "Play");
        if (exact != 1.0)
            err("agreement_score: differing only by accelerator should be 1.0, got " + std::to_string(exact));

        double exact2 = agreement_score("  WIEDERGABE  ", "wiedergabe");
        if (exact2 != 1.0)
            err("agreement_score: differing only by whitespace/case should be 1.0, got " + std::to_string(exact2));

        double disagree = agreement_score("Wiedergabe", "Abspielen");
        if (!(disagree < 0.85))
            err("agreement_score: real disagreement should score well under 0.85, got " + std::to_string(disagree));

        std::printf("agreement_score: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- mask_placeholders ----
    {
        std::string a = mask_placeholders("Copied %d file(s) to %s");
        if (a != "Copied %D file(s) to %S")
            err("mask_placeholders: expected 'Copied %D file(s) to %S', got '" + a + "'");

        std::string b = mask_placeholders("100%% done");
        if (b != "100%% done")
            err("mask_placeholders: bare '%%' should be left untouched, got '" + b + "'");

        std::string c = mask_placeholders("%1!s! of %2!d!");
        if (c != "%S of %D")
            err("mask_placeholders: expected '%S of %D', got '" + c + "'");

        std::printf("mask_placeholders: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- back_translation_similarity ----
    {
        // %d and %u are BOTH numeric/pointer conversions -> both mask to "%D" (see mask_placeholders'
        // doc comment: numeric conversions diouxXeEfFgGaA all share one bucket) -- so this pair
        // differs only in specifier CHOICE, which masking should erase, leaving a high score.
        double placeholderChoice = back_translation_similarity("Copied %d files", "Copied %u files");
        if (!(placeholderChoice > 0.9))
            err("back_translation_similarity: placeholder-choice-only difference should mask to a high "
                "score, got " + std::to_string(placeholderChoice));

        double drift = back_translation_similarity("Copy files to destination", "Delete the selected item");
        if (!(drift < 0.3))
            err("back_translation_similarity: real semantic drift should score low, got " + std::to_string(drift));

        std::printf("back_translation_similarity: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- classify ----
    {
        ConfidenceInputs highBoth;
        highBoth.agreement = 0.93; highBoth.back_translation_score = 0.88;
        highBoth.fits = true; highBoth.has_gate_flags = false;
        if (classify(highBoth) != Confidence::High)
            err("classify: agreement=0.93 back=0.88 fits=true flags=false should be High");

        ConfidenceInputs highNullopt;
        highNullopt.fits = true; highNullopt.has_gate_flags = false;   // agreement/back left nullopt
        if (classify(highNullopt) != Confidence::High)
            err("classify: nullopt agreement/back (single provider) should be High");

        ConfidenceInputs lowAgreement;
        lowAgreement.agreement = 0.4; lowAgreement.back_translation_score = 0.9;
        lowAgreement.fits = true; lowAgreement.has_gate_flags = false;
        if (classify(lowAgreement) != Confidence::Low)
            err("classify: agreement=0.4 should be Low even with everything else clean");

        ConfidenceInputs lowFits;
        lowFits.agreement = 1.0; lowFits.back_translation_score = 1.0;
        lowFits.fits = false; lowFits.has_gate_flags = false;
        if (classify(lowFits) != Confidence::Low)
            err("classify: fits=false should be Low even with agreement=1.0");

        ConfidenceInputs lowFlags;
        lowFlags.agreement = 1.0; lowFlags.back_translation_score = 1.0;
        lowFlags.fits = true; lowFlags.has_gate_flags = true;
        if (classify(lowFlags) != Confidence::Low)
            err("classify: has_gate_flags=true should be Low even with everything else clean");

        ConfidenceInputs boundary;
        boundary.agreement = 0.85; boundary.back_translation_score = 0.7;
        boundary.fits = true; boundary.has_gate_flags = false;
        if (classify(boundary) != Confidence::High)
            err("classify: boundary agreement=0.85 back=0.7 should count as High (>=)");

        std::printf("classify: %s\n", fail ? "FAIL" : "OK");
    }

    std::printf("%s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
