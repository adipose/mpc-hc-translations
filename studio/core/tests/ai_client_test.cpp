// SPDX-License-Identifier: GPL-3.0-or-later
// Pure JSON build/parse + extract_translation_json checks for mpctrans::ai_client — no network,
// so (unlike github_smoke_test) this runs the same on every platform. suggest_translation() itself
// (the WinHTTP call) is Windows-only and is exercised manually from the Studio, not here — same
// split as github_smoke_test's CredMan-roundtrip-vs-network checks.
#include "mpctrans/ai_client.h"
#include <cstdio>
#include <string>
using namespace mpctrans;
using nlohmann::json;

int main() {
    int fail = 0;
    auto err = [&](const std::string& m) { ++fail; std::printf("  FAIL %s\n", m.c_str()); };

    // ---- provider table ----
    {
        auto providers = list_providers();
        if (providers.size() != 4) err("list_providers: expected 4 providers, got " + std::to_string(providers.size()));

        const ProviderInfo* claude = find_provider("claude");
        if (!claude || claude->default_model != "claude-sonnet-4-5")
            err("find_provider(claude): missing or wrong default_model");

        const ProviderInfo* gpt = find_provider("gpt");
        if (!gpt || gpt->default_model != "gpt-5.1")
            err("find_provider(gpt): missing or wrong default_model");

        const ProviderInfo* glm = find_provider("glm");
        if (!glm || glm->default_model != "glm-4.6")
            err("find_provider(glm): missing or wrong default_model");

        const ProviderInfo* openrouter = find_provider("openrouter");
        if (!openrouter || openrouter->default_model != "anthropic/claude-sonnet-4.5")
            err("find_provider(openrouter): missing or wrong default_model");

        if (find_provider("nonexistent") != nullptr) err("find_provider: unknown id should return nullptr");

        std::printf("provider table: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- build_openai_request ----
    {
        json req = build_openai_request("gpt-5.1", "you are a translator", "translate 'hello'");
        if (req.value("model", "") != "gpt-5.1") err("build_openai_request: model field wrong");
        if (!req.contains("messages") || !req["messages"].is_array() || req["messages"].size() != 2)
            err("build_openai_request: expected a 2-element messages array");
        else {
            if (req["messages"][0].value("role", "") != "system" ||
                req["messages"][0].value("content", "") != "you are a translator")
                err("build_openai_request: messages[0] should be the system message");
            if (req["messages"][1].value("role", "") != "user" ||
                req["messages"][1].value("content", "") != "translate 'hello'")
                err("build_openai_request: messages[1] should be the user message");
        }
        if (!req.contains("temperature") || req["temperature"].get<double>() != 0.2)
            err("build_openai_request: temperature should be 0.2");
        std::printf("build_openai_request: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- parse_openai_response ----
    {
        json resp = json::parse(R"({"choices":[{"message":{"role":"assistant","content":"Bonjour"}}]})");
        std::string text = parse_openai_response(resp);
        if (text != "Bonjour") err("parse_openai_response: expected 'Bonjour', got '" + text + "'");

        bool threw = false;
        try { parse_openai_response(json::parse(R"({"nope":true})")); }
        catch (const std::runtime_error&) { threw = true; }
        if (!threw) err("parse_openai_response: should throw on malformed response");

        std::printf("parse_openai_response: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- build_anthropic_request ----
    {
        json req = build_anthropic_request("claude-sonnet-4-5", "you are a translator", "translate 'hello'");
        if (req.value("model", "") != "claude-sonnet-4-5") err("build_anthropic_request: model field wrong");
        if (req.value("system", "") != "you are a translator") err("build_anthropic_request: system field wrong");
        if (!req.contains("max_tokens") || req["max_tokens"].get<int>() != 1024)
            err("build_anthropic_request: max_tokens should be 1024");
        if (!req.contains("messages") || !req["messages"].is_array() || req["messages"].size() != 1)
            err("build_anthropic_request: expected a 1-element messages array (user only)");
        else if (req["messages"][0].value("role", "") != "user" ||
                req["messages"][0].value("content", "") != "translate 'hello'")
            err("build_anthropic_request: messages[0] should be the user message");
        std::printf("build_anthropic_request: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- parse_anthropic_response ----
    {
        json resp = json::parse(R"({"content":[{"type":"text","text":"Bonjour"}]})");
        std::string text = parse_anthropic_response(resp);
        if (text != "Bonjour") err("parse_anthropic_response: expected 'Bonjour', got '" + text + "'");

        bool threw = false;
        try { parse_anthropic_response(json::parse(R"({"nope":true})")); }
        catch (const std::runtime_error&) { threw = true; }
        if (!threw) err("parse_anthropic_response: should throw on malformed response");

        std::printf("parse_anthropic_response: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- extract_translation_json ----
    {
        // unfenced
        std::string a = extract_translation_json(R"({"translation":"Bonjour le monde"})");
        if (a != "Bonjour le monde") err("extract_translation_json (unfenced): got '" + a + "'");

        // fenced with a json language tag
        std::string b = extract_translation_json("```json\n{\"translation\":\"Hallo Welt\"}\n```");
        if (b != "Hallo Welt") err("extract_translation_json (fenced json): got '" + b + "'");

        // bare fence, no language tag
        std::string c = extract_translation_json("```\n{\"translation\":\"Hola mundo\"}\n```");
        if (c != "Hola mundo") err("extract_translation_json (bare fence): got '" + c + "'");

        // prose-wrapped, no fence
        std::string d = extract_translation_json(
            "Sure! Here's the translation:\n{\"translation\": \"Ciao mondo\"}\nLet me know if you need more.");
        if (d != "Ciao mondo") err("extract_translation_json (prose-wrapped): got '" + d + "'");

        // prose + fence together
        std::string e = extract_translation_json(
            "Here you go:\n```json\n{\"translation\": \"Ola mundo\"}\n```\nHope that helps!");
        if (e != "Ola mundo") err("extract_translation_json (prose+fence): got '" + e + "'");

        // garbage input should throw, and the exception should include the raw text
        bool threw = false;
        const std::string garbage = "I'm not going to give you JSON, sorry about that.";
        try {
            extract_translation_json(garbage);
        } catch (const std::runtime_error& ex) {
            threw = true;
            std::string what = ex.what();
            if (what.find(garbage) == std::string::npos)
                err("extract_translation_json: exception message should contain the raw model output");
        }
        if (!threw) err("extract_translation_json: should throw on garbage input");

        // valid JSON but missing the translation field should also throw
        bool threw2 = false;
        try { extract_translation_json(R"({"foo":"bar"})"); }
        catch (const std::runtime_error&) { threw2 = true; }
        if (!threw2) err("extract_translation_json: should throw when 'translation' field is missing");

        std::printf("extract_translation_json: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- parse_semantic_score ----
    {
        // plain JSON
        double a = parse_semantic_score(R"({"score":0.8})");
        if (a != 0.8) err("parse_semantic_score (plain): expected 0.8, got " + std::to_string(a));

        // fenced JSON
        double b = parse_semantic_score("```json\n{\"score\": 0.5}\n```");
        if (b != 0.5) err("parse_semantic_score (fenced): expected 0.5, got " + std::to_string(b));

        // bare fence, no language tag
        double c = parse_semantic_score("```\n{\"score\": 0.25}\n```");
        if (c != 0.25) err("parse_semantic_score (bare fence): expected 0.25, got " + std::to_string(c));

        // prose-wrapped, no fence
        double d = parse_semantic_score("Sure! Here's my judgment:\n{\"score\": 0.9}\nHope that helps!");
        if (d != 0.9) err("parse_semantic_score (prose-wrapped): expected 0.9, got " + std::to_string(d));

        // out-of-range clamping: above 1.0
        double e = parse_semantic_score(R"({"score":1.5})");
        if (e != 1.0) err("parse_semantic_score (clamp high): expected 1.0, got " + std::to_string(e));

        // out-of-range clamping: below 0.0
        double f = parse_semantic_score(R"({"score":-0.3})");
        if (f != 0.0) err("parse_semantic_score (clamp low): expected 0.0, got " + std::to_string(f));

        // garbage input should throw, and the exception should include the raw text
        bool threw = false;
        const std::string garbage = "I refuse to give you a score.";
        try {
            parse_semantic_score(garbage);
        } catch (const std::runtime_error& ex) {
            threw = true;
            std::string what = ex.what();
            if (what.find(garbage) == std::string::npos)
                err("parse_semantic_score: exception message should contain the raw model output");
        }
        if (!threw) err("parse_semantic_score: should throw on garbage input");

        // valid JSON but missing the score field should also throw
        bool threw2 = false;
        try { parse_semantic_score(R"({"foo":"bar"})"); }
        catch (const std::runtime_error&) { threw2 = true; }
        if (!threw2) err("parse_semantic_score: should throw when 'score' field is missing");

        std::printf("parse_semantic_score: %s\n", fail ? "FAIL" : "OK");
    }

    // ---- provider_lineage / pick_vote_provider ----
    {
        if (provider_lineage("claude", "") != "anthropic") err("provider_lineage(claude): expected 'anthropic'");
        if (provider_lineage("gpt", "") != "openai") err("provider_lineage(gpt): expected 'openai'");
        if (provider_lineage("glm", "") != "zai") err("provider_lineage(glm): expected 'zai'");

        if (provider_lineage("openrouter", "anthropic/claude-sonnet-4.5") != "anthropic")
            err("provider_lineage(openrouter, anthropic/...): expected 'anthropic'");
        if (provider_lineage("openrouter", "openai/gpt-5.1") != "openai")
            err("provider_lineage(openrouter, openai/...): expected 'openai'");
        if (provider_lineage("openrouter", "some-weird-vendor/foo") != "some-weird-vendor")
            err("provider_lineage(openrouter, some-weird-vendor/...): expected 'some-weird-vendor'");
        if (provider_lineage("openrouter", "") != "openrouter")
            err("provider_lineage(openrouter, ''): expected 'openrouter' (no slash)");

        auto none = pick_vote_provider("claude", "claude-sonnet-4-5",
            {{"claude", true}, {"gpt", false}, {"glm", false}, {"openrouter", false}});
        if (none.has_value()) err("pick_vote_provider: expected nullopt when only primary has a key");

        auto pickedGpt = pick_vote_provider("claude", "claude-sonnet-4-5",
            {{"claude", true}, {"gpt", true}, {"glm", false}, {"openrouter", false}});
        if (!pickedGpt || pickedGpt->provider_id != "gpt")
            err("pick_vote_provider: expected 'gpt' to be picked");

        // openrouter's own default_model is "anthropic/claude-sonnet-4.5" -> lineage "anthropic",
        // same as claude's -> skipped, so no candidate qualifies.
        auto sameLineage = pick_vote_provider("claude", "claude-sonnet-4-5",
            {{"claude", true}, {"openrouter", true}});
        if (sameLineage.has_value())
            err("pick_vote_provider: expected nullopt when openrouter's default model shares claude's lineage");

        auto pickedGlm = pick_vote_provider("gpt", "gpt-5.1", {{"gpt", true}, {"glm", true}});
        if (!pickedGlm || pickedGlm->provider_id != "glm")
            err("pick_vote_provider: expected 'glm' to be picked");

        std::printf("provider_lineage / pick_vote_provider: %s\n", fail ? "FAIL" : "OK");
    }

    std::printf("%s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
