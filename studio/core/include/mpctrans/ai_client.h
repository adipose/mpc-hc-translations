// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

// AI-assisted translation suggestions: a thin client over a handful of hosted chat-completion
// APIs (Claude/Anthropic, OpenAI-compatible GPT/GLM/OpenRouter). Native impl: reuses
// mpctrans::github::https_request (WinHTTP) for the actual network call — see http_internal.h.
//
// This module does NOT store API keys itself; callers fetch the key (e.g. via
// mpctrans::github::load_token against a cred target like
// L"mpc-hc-translation-studio/ai/<provider_id>") and pass it in per-call.

namespace mpctrans {

// One entry in the built-in provider table (list_providers()/find_provider()).
struct ProviderInfo {
    std::string id;             // "claude" | "gpt" | "glm" | "openrouter"
    std::string display_name;   // e.g. "Claude (Anthropic)"
    std::string default_model;  // e.g. "claude-sonnet-4-5"
};

// Which provider + model to call. `model` empty -> the provider's default_model is used.
struct AiConfig {
    std::string provider_id;
    std::string model;
};

// All built-in providers, in table order (claude, gpt, glm, openrouter).
std::vector<ProviderInfo> list_providers();

// Looks up a provider by id (e.g. "claude"). Returns nullptr if unknown. The returned pointer
// refers to static storage — valid for the process lifetime, no need to copy defensively.
const ProviderInfo* find_provider(const std::string& id);

// Lineage classification for routing a cross-model "vote" call to an INDEPENDENT lineage (agreement
// between same-lineage models is not a real confidence signal). model may be empty (falls back to
// the provider's own default_model's lineage where relevant).
//   claude -> "anthropic"   gpt -> "openai"   glm -> "zai"
//   openrouter -> derived from the model string's "vendor/..." prefix ("anthropic/claude-..." ->
//     "anthropic", "openai/..." -> "openai", "z-ai/..." or "zai/..." -> "zai", any other vendor
//     prefix -> that prefix verbatim as its own lineage bucket, no prefix at all -> "openrouter").
//   unknown provider_id -> the provider_id itself (defensive fallback, should not normally happen).
std::string provider_lineage(const std::string& provider_id, const std::string& model);

// One candidate for the cross-model vote pass: a provider (built-in table id) + the model to call
// (always that provider's own default_model — there's no per-provider stored model preference today,
// only the single currently-selected provider/model pair persists in AI Settings).
struct VoteCandidate { std::string provider_id, model; };

// Picks the vote provider: the FIRST provider (in list_providers() table order: claude, gpt, glm,
// openrouter) that is (a) not `primary_provider_id`, (b) has a credential per `available` (a
// provider_id -> has-a-stored-key map, caller-resolved -- this module knows nothing about Windows
// Credential Manager / mpctrans::github, keeping it link-portable and unit-testable), and (c) has a
// DIFFERENT lineage than the primary (via provider_lineage, using each candidate's own default_model).
// Returns nullopt if no provider qualifies (vote is then skipped entirely -- agreement stays NULL,
// never blocks generation -- caller's job, not this function's).
std::optional<VoteCandidate> pick_vote_provider(const std::string& primary_provider_id,
    const std::string& primary_model, const std::vector<std::pair<std::string, bool>>& available);

// Calls the configured provider with a system + user prompt and returns the raw model text
// (the assistant's reply, unparsed). Throws std::runtime_error on:
//   - unknown provider_id
//   - HTTP/transport failure (message includes the HTTP status and a truncated response body)
//   - a response that doesn't have the expected shape for the provider's wire format
// On non-Windows builds this throws std::logic_error("ai_client: Windows only") without making
// any network call (mirrors mpctrans::github's stub pattern), so the portable CMake gate still
// links this TU without WinHTTP.
std::string suggest_translation(const AiConfig& cfg, const std::string& api_key,
                                const std::string& system_prompt, const std::string& user_prompt);

// Extracts the translation string from a model's raw text reply, which is expected to contain
// (somewhere) a JSON object shaped like {"translation": "..."}. Tolerates:
//   - a ```json ... ``` (or bare ```) fence around the object
//   - surrounding prose before/after the object (finds the first '{' .. matching last '}')
// Throws std::runtime_error — with the RAW `model_output` text included in what() — if no valid
// JSON object with a string "translation" field can be found.
std::string extract_translation_json(const std::string& model_output);

// Asks the configured provider to judge whether two short English UI strings mean the same thing
// (used by the M2/M4 generation workers as the back-translation-drift SCORER: `english_a` is the
// true English msgid, `english_b` is the model's own back-translation of its translated suggestion
// — see confidence::ConfidenceInputs::back_translation_score). Returns a score in [0,1] (0 =
// unrelated, 1 = same meaning). Throws under the same conditions as suggest_translation (unknown
// provider/transport/shape failure) or if the reply can't be parsed (see parse_semantic_score).
// Windows-only via the same #ifdef _WIN32 split as suggest_translation would require — but since
// this function only delegates to suggest_translation (already split) + parse_semantic_score
// (portable), it does not need its own #ifdef split; it's defined once, unconditionally.
double semantic_equivalence_judge(const AiConfig& cfg, const std::string& api_key,
                                  const std::string& english_a, const std::string& english_b);

// Parses a semantic-equivalence judge's raw reply, expected to contain (somewhere) a JSON object
// shaped like {"score": N}. Tolerates the same fencing/prose-wrapping as extract_translation_json
// (a ```json ... ``` or bare ``` ``` fence; surrounding prose; first '{' .. matching last '}').
// Clamps N to [0,1] (e.g. 1.5 -> 1.0, -0.3 -> 0.0). Pure/portable (no I/O) -- unit-tested directly.
// Throws std::runtime_error — with the RAW `model_output` text included in what() — if no valid
// JSON object with a numeric "score" field can be found.
double parse_semantic_score(const std::string& model_output);

// A translator hint generated on demand for a single string (the LLM half of the hint layer; the
// deterministic half — keep_verbatim/parallel groups — is computed elsewhere). Field contract matches
// bundle-build/generate_hints.py's LLM output and the string_hints columns read by CoreEnrichment.
struct GeneratedHint { std::string role, form, referent, agreement, notes; };

// Asks the configured provider for a translator hint for `msgid` (given its Meaning/Function/Location),
// using the SAME system prompt + JSON contract as the build-time generator (generate_hints.py). Used by
// the Studio's lazy fallback: a string with no shipped hint (e.g. a new upstream string) gets one at
// runtime with the user's own key, then it's cached. Returns nullopt on any HTTP/parse failure (the
// caller then just proceeds without a hint — hint generation must NEVER block a translation suggestion).
// Same Windows-only posture as suggest_translation (which it delegates to).
std::optional<GeneratedHint> generate_hint(const AiConfig& cfg, const std::string& api_key,
    const std::string& msgid, const std::string& semantic_purpose,
    const std::string& functional_purpose, const std::string& ui_location);

// ---- exposed for unit testing without a network call ----
nlohmann::json build_openai_request(const std::string& model, const std::string& system_prompt,
                                    const std::string& user_prompt);
std::string    parse_openai_response(const nlohmann::json& response);

nlohmann::json build_anthropic_request(const std::string& model, const std::string& system_prompt,
                                       const std::string& user_prompt);
std::string    parse_anthropic_response(const nlohmann::json& response);

} // namespace mpctrans
