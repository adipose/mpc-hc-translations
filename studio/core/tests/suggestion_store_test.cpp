// SPDX-License-Identifier: GPL-3.0-or-later
// Round-trip checks for mpctrans::suggestion_store. This necessarily writes to the SAME real sqlite
// file the Studio.exe uses (%LOCALAPPDATA%\MPC-HC Translation Studio\ai-suggestions.sqlite) -- there's
// no LOCALAPPDATA override available (SHGetKnownFolderPath doesn't consult the env var), so this test
// uses obviously-scoped "zztest" fixtures (lang="zztest", msgctxt prefix "ZZTEST_") and cleans up
// (remove()s) its own rows at the end so repeated runs don't accumulate garbage in the real store.
#include "mpctrans/suggestion_store.h"
#include <cstdio>
#include <string>
using namespace mpctrans::suggestion_store;

int main() {
    int fail = 0;
    auto err = [&](const std::string& m) { ++fail; std::printf("  FAIL %s\n", m.c_str()); };

    const std::string lang = "zztest";
    const std::string ctxA = "ZZTEST_ALPHA";
    const std::string ctxB = "ZZTEST_BETA";
    const std::string msgid = "Hello world";

    // Clean slate: in case a previous run crashed before cleanup, clear our fixtures first.
    remove(lang, ctxA, msgid);
    remove(lang, ctxB, msgid);

    // ---- put() twice for the SAME cell with DIFFERENT models -> get() returns the newer one ----
    {
        Suggestion s1;
        s1.lang = lang; s1.msgctxt = ctxA; s1.msgid = msgid;
        s1.suggestion = "Bonjour le monde (v1)"; s1.model = "claude-sonnet-4-5"; s1.prompt_version = "v3";
        put(s1);

        Suggestion s2;
        s2.lang = lang; s2.msgctxt = ctxA; s2.msgid = msgid;
        s2.suggestion = "Bonjour le monde (v2, glm)"; s2.model = "glm-4.6"; s2.prompt_version = "v3";
        s2.fits_px = 12; s2.flags = "warning: tight fit";
        put(s2);

        auto got = get(lang, ctxA, msgid);
        if (!got) err("get after two different-model puts: expected a row, got nullopt");
        else {
            if (got->suggestion != "Bonjour le monde (v2, glm)")
                err("get after two different-model puts: expected the NEWER (glm) row, got '" + got->suggestion + "'");
            if (got->model != "glm-4.6") err("get: expected model 'glm-4.6', got '" + got->model + "'");
            if (!got->fits_px || *got->fits_px != 12) err("get: expected fits_px=12 on the newer row");
            if (got->flags != "warning: tight fit") err("get: expected the newer row's flags to survive");
            if (got->created_at.empty()) err("get: created_at should be stamped (non-empty)");
        }
        std::printf("newest-across-models: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- put() twice for the SAME (lang,msgctxt,msgid,model) key -> only the newest content survives,
    //      still exactly one logical row (not two) ----
    int failBefore = fail;
    {
        Suggestion s1;
        s1.lang = lang; s1.msgctxt = ctxB; s1.msgid = msgid;
        s1.suggestion = "first attempt"; s1.model = "claude-sonnet-4-5"; s1.prompt_version = "v3";
        put(s1);
        long long countAfterFirst = count(lang);

        Suggestion s2;
        s2.lang = lang; s2.msgctxt = ctxB; s2.msgid = msgid;
        s2.suggestion = "second attempt (replaces the first)"; s2.model = "claude-sonnet-4-5"; s2.prompt_version = "v3";
        put(s2);
        long long countAfterSecond = count(lang);

        if (countAfterSecond != countAfterFirst)
            err("same-key put twice: count(lang) changed (" + std::to_string(countAfterFirst) +
               " -> " + std::to_string(countAfterSecond) + "); expected it to stay a single logical row");

        auto got = get(lang, ctxB, msgid);
        if (!got) err("same-key put twice: expected a row, got nullopt");
        else if (got->suggestion != "second attempt (replaces the first)")
            err("same-key put twice: expected only the NEWEST content to survive, got '" + got->suggestion + "'");
    }
    std::printf("same-key-replace: %s\n", fail != failBefore ? "FAIL" : "OK");

    // ---- remove() -> get() returns nullopt afterward ----
    failBefore = fail;
    {
        remove(lang, ctxA, msgid);
        if (get(lang, ctxA, msgid)) err("remove: get() should return nullopt after remove()");
        // ctxB's row (from the previous block) should be unaffected by removing ctxA.
        if (!get(lang, ctxB, msgid)) err("remove: removing ctxA should not affect ctxB's row");
    }
    std::printf("remove: %s\n", fail != failBefore ? "FAIL" : "OK");

    // ---- count() reflects inserts/removals ----
    failBefore = fail;
    {
        remove(lang, ctxB, msgid);   // clean up the last fixture row
        long long c = count(lang);
        if (c != 0) err("count: expected 0 after removing all zztest fixtures, got " + std::to_string(c));

        Suggestion s;
        s.lang = lang; s.msgctxt = ctxA; s.msgid = msgid;
        s.suggestion = "recount check"; s.model = "claude-sonnet-4-5"; s.prompt_version = "v3";
        put(s);
        if (count(lang) != 1) err("count: expected 1 after a single put()");
        remove(lang, ctxA, msgid);   // tidy citizen: leave no zztest rows behind
        if (count(lang) != 0) err("count: expected 0 after final cleanup");
    }
    std::printf("count: %s\n", fail != failBefore ? "FAIL" : "OK");

    // ---- M3: get_all() surfaces multiple rows (primary + vote) for the same cell; get() still
    //      returns just the newest one; agreement/back_translation_score round-trip (incl. nullopt) ----
    const std::string ctxC = "ZZTEST_GAMMA";
    remove(lang, ctxC, msgid);   // clean slate
    failBefore = fail;
    {
        Suggestion primary;
        primary.lang = lang; primary.msgctxt = ctxC; primary.msgid = msgid;
        primary.suggestion = "Bonjour le monde (primary)"; primary.model = "claude-sonnet-4-5";
        primary.prompt_version = "v3"; primary.role = "primary";
        primary.agreement = 0.93; primary.back_translation_score = 0.88;
        put(primary);

        Suggestion vote;
        vote.lang = lang; vote.msgctxt = ctxC; vote.msgid = msgid;
        vote.suggestion = "Bonjour tout le monde (vote)"; vote.model = "glm-4.6";
        vote.prompt_version = "v3"; vote.role = "vote";
        vote.agreement = 0.93;   // back_translation_score left nullopt -- vote rows aren't back-translated
        put(vote);

        auto all = get_all(lang, ctxC, msgid);
        if (all.size() != 2)
            err("get_all: expected exactly 2 rows, got " + std::to_string(all.size()));
        else {
            const Suggestion* p = nullptr;
            const Suggestion* v = nullptr;
            for (const auto& r : all) {
                if (r.role == "primary") p = &r;
                else if (r.role == "vote") v = &r;
            }
            if (!p) err("get_all: no row with role='primary' found");
            else {
                if (p->model != "claude-sonnet-4-5") err("get_all: primary row has wrong model");
                if (!p->agreement || *p->agreement != 0.93) err("get_all: primary row's agreement should be 0.93");
                if (!p->back_translation_score || *p->back_translation_score != 0.88)
                    err("get_all: primary row's back_translation_score should be 0.88");
            }
            if (!v) err("get_all: no row with role='vote' found");
            else {
                if (v->model != "glm-4.6") err("get_all: vote row has wrong model");
                if (!v->agreement || *v->agreement != 0.93) err("get_all: vote row's agreement should be 0.93");
                if (v->back_translation_score.has_value())
                    err("get_all: vote row's back_translation_score should be nullopt");
            }
        }

        // get() still returns only the newest row (the vote row, put() second).
        auto newest = get(lang, ctxC, msgid);
        if (!newest) err("get: expected a row after two M3 puts, got nullopt");
        else if (newest->role != "vote" || newest->model != "glm-4.6")
            err("get: expected the newest (vote) row, got role='" + newest->role + "' model='" + newest->model + "'");
    }
    std::printf("m3-get-all: %s\n", fail != failBefore ? "FAIL" : "OK");

    remove(lang, ctxC, msgid);   // tidy citizen

    // ---- M3: agreement/back_translation_score left as std::nullopt round-trip as nullopt ----
    const std::string ctxD = "ZZTEST_DELTA";
    remove(lang, ctxD, msgid);
    failBefore = fail;
    {
        Suggestion s;
        s.lang = lang; s.msgctxt = ctxD; s.msgid = msgid;
        s.suggestion = "no confidence data"; s.model = "claude-sonnet-4-5"; s.prompt_version = "v3";
        // s.agreement / s.back_translation_score / s.role left default-constructed (nullopt/"").
        put(s);

        auto got = get(lang, ctxD, msgid);
        if (!got) err("get: expected a row for the nullopt-fields fixture");
        else {
            if (got->agreement.has_value()) err("get: agreement should round-trip as nullopt");
            if (got->back_translation_score.has_value()) err("get: back_translation_score should round-trip as nullopt");
            if (got->role != "") err("get: role should round-trip as empty string");
        }

        auto all = get_all(lang, ctxD, msgid);
        if (all.size() != 1) err("get_all: expected exactly 1 row for the nullopt-fields fixture");
        else if (all[0].agreement.has_value() || all[0].back_translation_score.has_value())
            err("get_all: nullopt fields should round-trip as nullopt");
    }
    std::printf("m3-nullopt-roundtrip: %s\n", fail != failBefore ? "FAIL" : "OK");

    remove(lang, ctxD, msgid);   // tidy citizen

    // ---- primary_keys(): the skip/resume rule (M4 batch generation) -- only role='primary' rows at
    //      EXACTLY the given (model, prompt_version) are returned; an older prompt_version's row, and
    //      a role='vote' row with no matching primary, must NOT be returned. ----
    const std::string ctxE = "ZZTEST_EPSILON_CURRENT";     // primary @ model+v3.1 (current) -- IS in the set
    const std::string ctxF = "ZZTEST_EPSILON_OLDPROMPT";   // primary @ model+v3 (older) -- NOT in the set
    const std::string ctxG = "ZZTEST_EPSILON_VOTEONLY";    // vote-only, no primary -- NOT in the set
    const std::string curModel = "claude/claude-sonnet-4-5";
    const std::string curPrompt = "v3.1";
    remove(lang, ctxE, msgid); remove(lang, ctxF, msgid); remove(lang, ctxG, msgid);
    failBefore = fail;
    {
        Suggestion primaryCurrent;
        primaryCurrent.lang = lang; primaryCurrent.msgctxt = ctxE; primaryCurrent.msgid = msgid;
        primaryCurrent.suggestion = "current primary"; primaryCurrent.model = curModel;
        primaryCurrent.prompt_version = curPrompt; primaryCurrent.role = "primary";
        put(primaryCurrent);

        Suggestion primaryOld;
        primaryOld.lang = lang; primaryOld.msgctxt = ctxF; primaryOld.msgid = msgid;
        primaryOld.suggestion = "old-prompt primary"; primaryOld.model = curModel;
        primaryOld.prompt_version = "v3"; primaryOld.role = "primary";
        put(primaryOld);

        Suggestion voteOnly;
        voteOnly.lang = lang; voteOnly.msgctxt = ctxG; voteOnly.msgid = msgid;
        voteOnly.suggestion = "a vote row with no matching primary"; voteOnly.model = curModel;
        voteOnly.prompt_version = curPrompt; voteOnly.role = "vote";
        put(voteOnly);

        auto keys = primary_keys(lang, curModel, curPrompt);
        if (!keys.count({ ctxE, msgid }))
            err("primary_keys: expected the current-prompt primary row's key to be in the set");
        if (keys.count({ ctxF, msgid }))
            err("primary_keys: an OLDER prompt_version's primary row must NOT be in the set");
        if (keys.count({ ctxG, msgid }))
            err("primary_keys: a role='vote' row (no matching primary) must NOT be in the set");
    }
    std::printf("primary-keys-skip-rule: %s\n", fail != failBefore ? "FAIL" : "OK");

    remove(lang, ctxE, msgid); remove(lang, ctxF, msgid); remove(lang, ctxG, msgid);   // tidy citizen

    std::printf("%s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
