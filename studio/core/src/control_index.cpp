// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/control_index.h"

#include <fstream>
#include <stdexcept>

#include <json.hpp>

// control-index.json (bundle-build/build_index.py, schema 1) -> dialogs_/menus_ + lookup maps.
// Records are keyed by NUMERIC ids; English text disambiguates the many control==-1 (IDC_STATIC)
// duplicates. Map values are indices into the record vectors (stable after load()).

namespace mpctrans {

using nlohmann::json;

std::string rc_text_normalize(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        o += s[i];
        if (s[i] == '"' && i + 1 < s.size() && s[i + 1] == '"') ++i;   // "" -> "
    }
    return o;
}

// (dialog, control-or-CAPTION, text) — control==nullopt encoded distinctly from any numeric id.
static std::string dlg_key(long long dialog, const std::optional<long long>& control,
                           const std::string& text) {
    return std::to_string(dialog) + '|' + (control ? std::to_string(*control) : "CAP") + '|' + text;
}
static std::string dlg_key_notext(long long dialog, const std::optional<long long>& control) {
    return std::to_string(dialog) + '|' + (control ? std::to_string(*control) : "CAP");
}

ControlIndex ControlIndex::load(const std::string& json_path) {
    std::ifstream f(json_path, std::ios::binary);
    if (!f) throw std::runtime_error("ControlIndex::load: cannot open " + json_path);
    json j = json::parse(f);   // throws json::parse_error on malformed input
    if (j.value("schema", 0) != 1)
        throw std::runtime_error("ControlIndex::load: unsupported schema in " + json_path);

    ControlIndex ci;
    if (j.contains("upstream_sha") && j["upstream_sha"].is_string())
        ci.upstream_sha = j["upstream_sha"].get<std::string>();

    for (const auto& d : j.at("dialogs")) {
        DialogRecord r;
        r.dialog = d.at("dialog").get<long long>();
        if (!d.at("control").is_null()) r.control = d["control"].get<long long>();
        r.control_sym = d.at("control_sym").get<std::string>();
        r.msgctxt     = d.at("msgctxt").get<std::string>();
        r.msgid       = d.at("msgid").get<std::string>();
        ci.dialogs_.push_back(std::move(r));
    }
    for (const auto& m : j.at("menus")) {
        MenuRecord r;
        if (!m.at("command").is_null()) r.command = m["command"].get<long long>();
        r.sym     = m.at("sym").get<std::string>();
        r.msgctxt = m.at("msgctxt").get<std::string>();
        r.msgid   = m.at("msgid").get<std::string>();
        ci.menus_.push_back(std::move(r));
    }
    ci.build_maps();
    return ci;
}

ControlIndex ControlIndex::from_records(std::vector<DialogRecord> dialogs,
                                        std::vector<MenuRecord> menus) {
    ControlIndex ci;
    ci.dialogs_ = std::move(dialogs);
    ci.menus_ = std::move(menus);
    ci.build_maps();
    return ci;
}

void ControlIndex::build_maps() {
    std::unordered_map<std::string, int> id_count;
    for (size_t i = 0; i < dialogs_.size(); ++i) {
        const auto& r = dialogs_[i];
        dlg_by_key_.emplace(dlg_key(r.dialog, r.control, rc_text_normalize(r.msgid)), i);
        ++id_count[dlg_key_notext(r.dialog, r.control)];
        if (!r.control) dlg_caption_.emplace(r.dialog, i);
    }
    // (dialog,control) without text is only a valid key where it is unique (not IDC_STATIC dupes)
    for (size_t i = 0; i < dialogs_.size(); ++i) {
        const auto& r = dialogs_[i];
        std::string k = dlg_key_notext(r.dialog, r.control);
        if (id_count[k] == 1) dlg_by_id_.emplace(std::move(k), i);
    }
    for (size_t i = 0; i < menus_.size(); ++i) {
        const auto& r = menus_[i];
        if (r.command) menu_by_command_.emplace(*r.command, i);
        else           popup_by_text_.emplace(rc_text_normalize(r.msgid), i);
    }
}

const DialogRecord* ControlIndex::dialog_lookup(long long dialog, std::optional<long long> control,
                                                const std::string& english_text) const {
    auto it = dlg_by_key_.find(dlg_key(dialog, control, rc_text_normalize(english_text)));
    if (it != dlg_by_key_.end()) return &dialogs_[it->second];
    // no text match (e.g. runtime-modified caption): fall back to the id pair when unambiguous
    auto it2 = dlg_by_id_.find(dlg_key_notext(dialog, control));
    return it2 != dlg_by_id_.end() ? &dialogs_[it2->second] : nullptr;
}
const DialogRecord* ControlIndex::dialog_caption(long long dialog) const {
    auto it = dlg_caption_.find(dialog);
    return it != dlg_caption_.end() ? &dialogs_[it->second] : nullptr;
}
const MenuRecord* ControlIndex::menu_command(long long command) const {
    auto it = menu_by_command_.find(command);
    return it != menu_by_command_.end() ? &menus_[it->second] : nullptr;
}
const MenuRecord* ControlIndex::menu_popup(const std::string& english_text) const {
    auto it = popup_by_text_.find(rc_text_normalize(english_text));
    return it != popup_by_text_.end() ? &menus_[it->second] : nullptr;
}

} // namespace mpctrans
