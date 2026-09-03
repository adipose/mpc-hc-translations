// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <vector>

// Transifex REST API v3 (JSON:API) client -- used by the reverse-push flow (mpctrans::txsync's
// tx_reverse_push_plan/tx_apply_reverse_push) to push upstream-develop translations INTO the live
// Transifex DB, so a translator's PR merged into develop doesn't get reverted by the next
// Transifex-DB -> `transifex` branch sync (GitHub integration is one-way: DB -> branch; "Transifex
// wins" on conflicts). Native impl: WinHTTP (via mpctrans::github's http_internal plumbing) +
// nlohmann/json. Token stored via Windows Credential Manager/DPAPI, same mechanism as
// mpctrans::github but under its own Credential-Manager target (a GitHub PAT and a Transifex API
// token are different secrets).

namespace mpctrans::txapi {

struct Token { std::string access_token; };

// ---- token persistence (Windows Credential Manager; DPAPI-backed) ----
inline constexpr wchar_t CRED_TARGET[] = L"mpc-hc-translation-studio/transifex";
void                 store_token(const Token&);
std::optional<Token> load_token();
void                 clear_token();

// One resource_translations row, joined with its resource_string (source side).
struct Translation {
    std::string id;                  // resource_translations id -- PATCH target
    std::string context;             // resource_string.attributes.context (== .po msgctxt)
    std::string key;                 // resource_string.attributes.key
    std::string source;              // resource_string.attributes.strings.other (== .po msgid)
    std::string value;               // attributes.strings.other -- "" when untranslated (strings is null)
    std::string datetime_translated; // ISO-8601 UTC, or "" when null/untranslated
    bool reviewed = false;
};

// All translations of one resource (config::RESOURCES[res]) in one language, paginating via the
// response's links.next until exhausted. A language absent from the Transifex project comes back
// as an HTTP 4xx naming the language in its error body -- treated as "no translations", not an
// error: returns an empty vector. Any other HTTP error (bad token, bad resource id, ...) throws
// std::runtime_error with the status and response body.
std::vector<Translation> list_translations(const Token&, int res, const std::string& lang);

// PATCH one resource_translations row's value (`{"data":{"id":...,"type":"resource_translations",
// "attributes":{"strings":{"other":value}}}}`). Throws std::runtime_error (status + body) on any
// non-2xx response.
void patch_translation(const Token&, const std::string& translationId, const std::string& value);

} // namespace mpctrans::txapi
