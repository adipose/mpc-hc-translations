// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/ai_client.h"
#include "mpctrans/http_internal.h"

#include <optional>
#include <stdexcept>

using nlohmann::json;

// Portable parts (provider table, request builders/response parsers, extract_translation_json)
// build on any platform — they're pure string/JSON transforms, no networking. Only
// suggest_translation() needs an actual HTTP call, so ONLY that function is split #ifdef _WIN32,
// same shape as mpctrans::github's Windows/non-Windows split.

namespace mpctrans {

namespace {

enum class WireFormat { OpenAI, Anthropic };

struct ProviderEntry {
    std::string id, display_name, base_url, default_model;
    WireFormat format;
};

// id -> display name, base_url+path, wire format, default model. Order here is list_providers()'s
// order (claude, gpt, glm, openrouter).
const std::vector<ProviderEntry>& provider_table() {
    static const std::vector<ProviderEntry> table = {
        {"claude",     "Claude (Anthropic)",  "https://api.anthropic.com/v1/messages",           "claude-sonnet-5",                WireFormat::Anthropic},
        {"gpt",        "GPT (OpenAI)",        "https://api.openai.com/v1/chat/completions",       "gpt-5.1",                        WireFormat::OpenAI},
        {"glm",        "GLM (Z.ai)",          "https://api.z.ai/api/paas/v4/chat/completions",    "glm-4.6",                        WireFormat::OpenAI},
        {"openrouter", "OpenRouter",          "https://openrouter.ai/api/v1/chat/completions",    "anthropic/claude-sonnet-5",      WireFormat::OpenAI},
    };
    return table;
}

const ProviderEntry* find_entry(const std::string& id) {
    for (const auto& p : provider_table())
        if (p.id == id) return &p;
    return nullptr;
}

// First 300 bytes of a response body, for error messages (avoid dumping huge HTML/JSON payloads).
std::string truncated(const std::string& s, size_t n = 300) {
    return s.size() > n ? s.substr(0, n) + "...(truncated)" : s;
}

} // namespace

std::vector<ProviderInfo> list_providers() {
    std::vector<ProviderInfo> out;
    out.reserve(provider_table().size());
    for (const auto& p : provider_table())
        out.push_back(ProviderInfo{p.id, p.display_name, p.default_model});
    return out;
}

const ProviderInfo* find_provider(const std::string& id) {
    // Cache one ProviderInfo per entry in static storage so we can return a stable pointer.
    static const std::vector<ProviderInfo> infos = [] {
        std::vector<ProviderInfo> v;
        for (const auto& p : provider_table()) v.push_back(ProviderInfo{p.id, p.display_name, p.default_model});
        return v;
    }();
    for (const auto& info : infos)
        if (info.id == id) return &info;
    return nullptr;
}

std::string provider_lineage(const std::string& provider_id, const std::string& model) {
    if (provider_id == "claude") return "anthropic";
    if (provider_id == "gpt") return "openai";
    if (provider_id == "glm") return "zai";
    if (provider_id == "openrouter") {
        size_t slash = model.find('/');
        if (slash == std::string::npos) return "openrouter";
        std::string vendor = model.substr(0, slash);
        if (vendor == "anthropic") return "anthropic";
        if (vendor == "openai") return "openai";
        if (vendor == "z-ai" || vendor == "zai") return "zai";
        return vendor;
    }
    // Defensive fallback for an unknown provider_id — should not normally happen.
    return provider_id;
}

std::optional<VoteCandidate> pick_vote_provider(const std::string& primary_provider_id,
    const std::string& primary_model, const std::vector<std::pair<std::string, bool>>& available) {
    auto has_key = [&](const std::string& id) {
        for (const auto& kv : available)
            if (kv.first == id) return kv.second;
        return false;
    };

    const std::string primary_lineage = provider_lineage(primary_provider_id, primary_model);

    for (const auto& p : provider_table()) {
        if (p.id == primary_provider_id) continue;
        if (!has_key(p.id)) continue;
        if (provider_lineage(p.id, p.default_model) == primary_lineage) continue;
        return VoteCandidate{p.id, p.default_model};
    }
    return std::nullopt;
}

json build_openai_request(const std::string& model, const std::string& system_prompt,
                          const std::string& user_prompt) {
    return json{
        {"model", model},
        {"messages", json::array({
            {{"role", "system"}, {"content", system_prompt}},
            {{"role", "user"},   {"content", user_prompt}},
        })},
        {"temperature", 0.2},
    };
}

std::string parse_openai_response(const json& response) {
    try {
        return response.at("choices").at(0).at("message").at("content").get<std::string>();
    } catch (const json::exception& e) {
        throw std::runtime_error("ai_client: unexpected OpenAI-format response shape (" +
                                 std::string(e.what()) + "): " + truncated(response.dump()));
    }
}

json build_anthropic_request(const std::string& model, const std::string& system_prompt,
                             const std::string& user_prompt) {
    return json{
        {"model", model},
        {"max_tokens", 1024},
        {"system", system_prompt},
        {"messages", json::array({
            {{"role", "user"}, {"content", user_prompt}},
        })},
    };
}

std::string parse_anthropic_response(const json& response) {
    try {
        return response.at("content").at(0).at("text").get<std::string>();
    } catch (const json::exception& e) {
        throw std::runtime_error("ai_client: unexpected Anthropic-format response shape (" +
                                 std::string(e.what()) + "): " + truncated(response.dump()));
    }
}

std::string extract_translation_json(const std::string& model_output) {
    auto try_parse = [](const std::string& text) -> std::optional<std::string> {
        json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return std::nullopt;
        auto it = j.find("translation");
        if (it == j.end() || !it->is_string()) return std::nullopt;
        return it->get<std::string>();
    };

    // 1. maybe the whole reply already IS the JSON object.
    if (auto r = try_parse(model_output)) return *r;

    std::string text = model_output;

    // 2. strip a ```json ... ``` (or bare ``` ... ```) fence, if present.
    size_t fence = text.find("```");
    if (fence != std::string::npos) {
        size_t after_fence = fence + 3;
        size_t nl = text.find('\n', after_fence);
        std::string tag = (nl != std::string::npos) ? text.substr(after_fence, nl - after_fence) : "";
        bool tag_is_word = !tag.empty() &&
            tag.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos;
        size_t body_start = (nl != std::string::npos && (tag_is_word || tag.empty())) ? nl + 1 : after_fence;
        size_t closing = text.find("```", body_start);
        std::string fenced = (closing != std::string::npos) ? text.substr(body_start, closing - body_start)
                                                             : text.substr(body_start);
        if (auto r = try_parse(fenced)) return *r;
        text = fenced;   // fall through to brace-finding on the de-fenced content too
    }

    // 3. prose-wrapped: first '{' .. matching last '}'.
    size_t open = text.find('{');
    size_t close = text.rfind('}');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        if (auto r = try_parse(text.substr(open, close - open + 1))) return *r;
    }

    throw std::runtime_error(
        "ai_client: could not find a valid {\"translation\":\"...\"} JSON object in model output: " +
        model_output);
}

double parse_semantic_score(const std::string& model_output) {
    auto clamp01 = [](double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); };
    auto try_parse = [&](const std::string& text) -> std::optional<double> {
        json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return std::nullopt;
        auto it = j.find("score");
        if (it == j.end() || !it->is_number()) return std::nullopt;
        return clamp01(it->get<double>());
    };

    // 1. maybe the whole reply already IS the JSON object.
    if (auto r = try_parse(model_output)) return *r;

    std::string text = model_output;

    // 2. strip a ```json ... ``` (or bare ``` ... ```) fence, if present.
    size_t fence = text.find("```");
    if (fence != std::string::npos) {
        size_t after_fence = fence + 3;
        size_t nl = text.find('\n', after_fence);
        std::string tag = (nl != std::string::npos) ? text.substr(after_fence, nl - after_fence) : "";
        bool tag_is_word = !tag.empty() &&
            tag.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos;
        size_t body_start = (nl != std::string::npos && (tag_is_word || tag.empty())) ? nl + 1 : after_fence;
        size_t closing = text.find("```", body_start);
        std::string fenced = (closing != std::string::npos) ? text.substr(body_start, closing - body_start)
                                                             : text.substr(body_start);
        if (auto r = try_parse(fenced)) return *r;
        text = fenced;   // fall through to brace-finding on the de-fenced content too
    }

    // 3. prose-wrapped: first '{' .. matching last '}'.
    size_t open = text.find('{');
    size_t close = text.rfind('}');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        if (auto r = try_parse(text.substr(open, close - open + 1))) return *r;
    }

    throw std::runtime_error(
        "ai_client: could not find a valid {\"score\":N} JSON object in model output: " + model_output);
}

#ifdef _WIN32

std::string suggest_translation(const AiConfig& cfg, const std::string& api_key,
                                const std::string& system_prompt, const std::string& user_prompt) {
    const ProviderEntry* p = find_entry(cfg.provider_id);
    if (!p) throw std::runtime_error("ai_client: unknown provider '" + cfg.provider_id + "'");
    const std::string model = cfg.model.empty() ? p->default_model : cfg.model;

    json body;
    std::vector<std::string> headers = {"Content-Type: application/json"};
    if (p->format == WireFormat::Anthropic) {
        body = build_anthropic_request(model, system_prompt, user_prompt);
        headers.push_back("x-api-key: " + api_key);
        headers.push_back("anthropic-version: 2023-06-01");
    } else {
        body = build_openai_request(model, system_prompt, user_prompt);
        headers.push_back("Authorization: Bearer " + api_key);
    }

    github::HttpResponse r = github::https_request(L"POST", p->base_url, headers, body.dump());
    if (r.status < 200 || r.status >= 300)
        throw std::runtime_error("ai_client: " + p->id + " -> HTTP " + std::to_string(r.status) +
                                 " (" + truncated(r.body) + ")");

    json response = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (response.is_discarded())
        throw std::runtime_error("ai_client: " + p->id + " returned invalid JSON: " + truncated(r.body));

    return p->format == WireFormat::Anthropic ? parse_anthropic_response(response)
                                              : parse_openai_response(response);
}

#else // !_WIN32 — keep non-Windows builds (validators/gates) linking, mirroring github.cpp's stub.

std::string suggest_translation(const AiConfig&, const std::string&, const std::string&, const std::string&) {
    throw std::logic_error("ai_client: Windows only");
}

#endif

// Defined once (not #ifdef-split): delegates entirely to suggest_translation (already split above)
// and parse_semantic_score (portable) -- on non-Windows builds this just inherits suggest_translation's
// std::logic_error("ai_client: Windows only"), same as every other caller of that function.
double semantic_equivalence_judge(const AiConfig& cfg, const std::string& api_key,
                                  const std::string& english_a, const std::string& english_b) {
    static const char* kSys =
        "You are comparing two short English UI strings for MEANING ONLY. Ignore synonyms, word "
        "order, and register -- judge only whether they convey the same meaning. Respond with ONLY "
        "{\"score\": N} where N is 0.0 (unrelated) to 1.0 (same meaning).";
    std::string usr = "String A: " + english_a + "\nString B: " + english_b;
    std::string raw = suggest_translation(cfg, api_key, kSys, usr);
    return parse_semantic_score(raw);
}

// Lazy hint generation. Mirrors semantic_equivalence_judge's shape (delegate to suggest_translation +
// a tolerant JSON parse); the system prompt is kept verbatim in sync with bundle-build/generate_hints.py's
// LLM_SYSTEM so runtime-generated hints match build-time ones. Defined once (no #ifdef split) since it
// only calls the already-split suggest_translation. Never throws — returns nullopt on any failure.
std::optional<GeneratedHint> generate_hint(const AiConfig& cfg, const std::string& api_key,
    const std::string& msgid, const std::string& semantic_purpose,
    const std::string& functional_purpose, const std::string& ui_location) {
    static const char* kSys =
        "You add a compact TRANSLATOR HINT for a short Windows-media-player UI string, to guide "
        "localization. Respond with ONLY a JSON object: "
        "{\"role\":\"command|label|option|title|status|format-name|unit|value\","
        "\"form\":\"imperative|noun|noun-phrase|adjective|numeric-token|proper-noun\","
        "\"referent\":\"<the noun this modifies, or what each placeholder denotes; '' if none>\","
        "\"agreement\":\"<gender/number agreement guidance for gendered languages, or n/a>\","
        "\"notes\":\"<=140 chars, only non-obvious guidance (keep-untranslated, command-not-label, etc.)\"}. "
        "Base it on the English string and the provided Meaning; be terse; no prose outside the JSON.";
    std::string usr = "String: " + msgid + "\nMeaning: " + semantic_purpose +
                      "\nFunction: " + functional_purpose + "\nLocation: " + ui_location;

    std::string raw;
    try { raw = suggest_translation(cfg, api_key, kSys, usr); }
    catch (...) { return std::nullopt; }

    // Find the first parseable JSON object: whole reply, then a ``` fence, then first '{'..last '}'.
    auto str = [](const json& j, const char* k) -> std::string {
        auto it = j.find(k);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    auto try_obj = [&](const std::string& text) -> std::optional<GeneratedHint> {
        json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return std::nullopt;
        GeneratedHint h{str(j, "role"), str(j, "form"), str(j, "referent"), str(j, "agreement"), str(j, "notes")};
        if (h.role.empty() && h.referent.empty() && h.notes.empty()) return std::nullopt;  // nothing usable
        return h;
    };
    if (auto h = try_obj(raw)) return h;
    std::string text = raw;
    size_t fence = text.find("```");
    if (fence != std::string::npos) {
        size_t after = fence + 3, nl = text.find('\n', after);
        size_t body = (nl != std::string::npos) ? nl + 1 : after;
        size_t close = text.find("```", body);
        text = (close != std::string::npos) ? text.substr(body, close - body) : text.substr(body);
        if (auto h = try_obj(text)) return h;
    }
    size_t o = text.find('{'), c = text.rfind('}');
    if (o != std::string::npos && c != std::string::npos && c > o)
        if (auto h = try_obj(text.substr(o, c - o + 1))) return h;
    return std::nullopt;
}

} // namespace mpctrans
