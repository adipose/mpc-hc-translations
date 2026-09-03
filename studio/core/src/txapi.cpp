// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/txapi.h"
#include "mpctrans/github.h"
#include "mpctrans/config.h"
#include "mpctrans/http_internal.h"

#include <cctype>
#include <map>
#include <stdexcept>
#include <tuple>

#ifdef _WIN32

#include <json.hpp>

// Transifex REST API v3 (JSON:API) client -- WinHTTP + nlohmann/json, reusing github.cpp's HTTP
// plumbing (mpctrans::github::https_request) rather than duplicating it. Token persistence just
// delegates to mpctrans::github's Credential-Manager-backed store, parameterized by our own
// CRED_TARGET (a Transifex API token is a different secret than the GitHub PAT).

namespace mpctrans::txapi {

using nlohmann::json;
namespace cfg = mpctrans::config;

void store_token(const Token& t) {
    github::store_token(github::Token{ t.access_token }, CRED_TARGET);
}
std::optional<Token> load_token() {
    auto t = github::load_token(CRED_TARGET);
    if (!t) return std::nullopt;
    return Token{ t->access_token };
}
void clear_token() {
    github::clear_token(CRED_TARGET);
}

namespace {

// attributes.strings is either null (untranslated) or {"other": "..."} -- null-safe read.
std::string strings_other(const json& strings) {
    if (!strings.is_object()) return {};
    auto it = strings.find("other");
    return (it != strings.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// True if a 4xx error body looks like it's complaining about filter[language] (the language isn't
// part of the Transifex project) rather than something else (bad token, bad resource id, ...).
bool looks_like_language_error(const std::string& body) {
    std::string lower = body;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    return lower.find("language") != std::string::npos;
}

struct Reply { int status = 0; json body; std::string raw; };

Reply do_request(const Token& t, const wchar_t* method, const std::string& url, const json* body) {
    std::vector<std::string> h = { "Accept: application/vnd.api+json",
                                   "Authorization: Bearer " + t.access_token };
    std::string b;
    if (body) { b = body->dump(); h.push_back("Content-Type: application/vnd.api+json"); }
    github::HttpResponse r = github::https_request(method, url, h, b);
    Reply rep;
    rep.status = r.status;
    rep.raw = r.body;
    rep.body = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    return rep;
}

} // namespace

std::vector<Translation> list_translations(const Token& t, int res, const std::string& lang) {
    std::vector<Translation> out;
    std::string url = std::string(cfg::TX_API) + "/resource_translations?filter[resource]=" +
                      cfg::tx_resource_id(res) + "&filter[language]=l:" + lang +
                      "&include=resource_string";

    // page[size] is NOT accepted by this endpoint -- paginate purely via links.next until absent.
    while (!url.empty()) {
        Reply rep = do_request(t, L"GET", url, nullptr);
        if (rep.status >= 400) {
            if (rep.status / 100 == 4 && looks_like_language_error(rep.raw)) return {};
            throw std::runtime_error("txapi: list_translations -> HTTP " + std::to_string(rep.status) +
                                     " (" + rep.raw.substr(0, 300) + ")");
        }
        const json& j = rep.body;
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array())
            throw std::runtime_error("txapi: list_translations -> unexpected response shape");

        // included resource_string rows, keyed by id -> (context, key, source msgid)
        std::map<std::string, std::tuple<std::string, std::string, std::string>> included;
        if (j.contains("included") && j["included"].is_array())
            for (const auto& inc : j["included"]) {
                // JSON:API type names in Transifex v3 are PLURAL ("resource_strings"); accept the
                // singular too so a future rename can't silently empty the join again.
                if (!inc.is_object()) continue;
                std::string ty = inc.value("type", std::string());
                if (ty != "resource_strings" && ty != "resource_string") continue;
                json at = inc.contains("attributes") ? inc["attributes"] : json::object();
                json strs = at.contains("strings") ? at["strings"] : json();
                auto str_or_empty = [&](const char* k) -> std::string {   // null-safe (context can be null)
                    return at.contains(k) && at[k].is_string() ? at[k].get<std::string>() : std::string();
                };
                included[inc.value("id", std::string())] = {
                    str_or_empty("context"), str_or_empty("key"), strings_other(strs) };
            }

        for (const auto& d : j["data"]) {
            if (!d.is_object()) continue;
            Translation tr;
            tr.id = d.value("id", std::string());
            json at = d.contains("attributes") ? d["attributes"] : json::object();
            json strs = at.contains("strings") ? at["strings"] : json();
            tr.value = strings_other(strs);
            json dt = at.contains("datetime_translated") ? at["datetime_translated"] : json();
            tr.datetime_translated = dt.is_string() ? dt.get<std::string>() : std::string();
            tr.reviewed = at.value("reviewed", false);

            std::string rsId;
            if (d.contains("relationships") && d["relationships"].is_object()) {
                const json& rel = d["relationships"];
                if (rel.contains("resource_string") && rel["resource_string"].is_object() &&
                    rel["resource_string"].contains("data") && rel["resource_string"]["data"].is_object())
                    rsId = rel["resource_string"]["data"].value("id", std::string());
            }
            auto it = included.find(rsId);
            if (it != included.end()) {
                tr.context = std::get<0>(it->second);
                tr.key = std::get<1>(it->second);
                tr.source = std::get<2>(it->second);
            }
            out.push_back(std::move(tr));
        }

        url.clear();
        if (j.contains("links") && j["links"].is_object()) {
            json nx = j["links"].contains("next") ? j["links"]["next"] : json();
            if (nx.is_string()) url = nx.get<std::string>();
        }
    }
    return out;
}

void patch_translation(const Token& t, const std::string& translationId, const std::string& value) {
    json body = {
        { "data", {
            { "id", translationId },
            { "type", "resource_translations" },
            { "attributes", { { "strings", { { "other", value } } } } }
        } }
    };
    Reply rep = do_request(t, L"PATCH",
        std::string(cfg::TX_API) + "/resource_translations/" + translationId, &body);
    if (rep.status >= 400)
        throw std::runtime_error("txapi: patch_translation " + translationId + " -> HTTP " +
                                 std::to_string(rep.status) + " (" + rep.raw.substr(0, 300) + ")");
}

} // namespace mpctrans::txapi

#else // !_WIN32 — keep non-Windows builds (validators/gates) linking, same pattern as github.cpp

namespace mpctrans::txapi {
void store_token(const Token&) { throw std::logic_error("txapi: Windows only"); }
std::optional<Token> load_token() { return std::nullopt; }
void clear_token() {}
std::vector<Translation> list_translations(const Token&, int, const std::string&) { throw std::logic_error("txapi: Windows only"); }
void patch_translation(const Token&, const std::string&, const std::string&) { throw std::logic_error("txapi: Windows only"); }
} // namespace mpctrans::txapi

#endif
