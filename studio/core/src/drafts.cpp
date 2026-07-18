// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/drafts.h"
#include <json.hpp>

using nlohmann::json;

namespace mpctrans {

std::string serialize_drafts(const Drafts& d) {
    json j = json::object();
    for (const auto& [lang, edits] : d) {
        json arr = json::array();
        for (const auto& e : edits)
            arr.push_back({ {"res", e.res}, {"ctx", e.msgctxt}, {"id", e.msgid}, {"str", e.msgstr} });
        j[lang] = std::move(arr);
    }
    return j.dump(1, '\t');
}

Drafts parse_drafts(const std::string& text) {
    Drafts d;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return d;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_array()) continue;
        auto& vec = d[it.key()];
        for (const auto& e : it.value())
            vec.push_back({ e.value("res", 0), e.value("ctx", std::string()),
                            e.value("id", std::string()), e.value("str", std::string()) });
    }
    return d;
}

}
