// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// control-index reader — maps a rendered control back to its .po entry, and vice-versa.
// Produced by bundle-build/build_index.py (numeric-keyed; text disambiguates IDC_STATIC=-1).
// This is the linchpin of live-preview substitution AND click-to-edit.
// See plan-translation-studio "Live-preview substitution — DE-RISKED".

namespace mpctrans {

struct DialogRecord {          // one dialog control (or CAPTION when control == nullopt)
    long long dialog = 0;          // IDD numeric id
    std::optional<long long> control;  // control numeric id; nullopt = dialog caption
    std::string control_sym;       // "IDC_…" | "CAPTION"
    std::string msgctxt, msgid;    // the .po key (msgid == English text)
};
struct MenuRecord {            // one menu item (command) or popup header (command == nullopt)
    std::optional<long long> command;  // command numeric id; nullopt = POPUP header
    std::string sym;               // "ID_…" | "POPUP"
    std::string msgctxt, msgid;
};

// RC string literals escape '"' by doubling it; compiled templates (and thus rendered control
// text) contain the plain quote. Both lookup sides normalize through this so text keys match.
std::string rc_text_normalize(const std::string& s);

class ControlIndex {
public:
    static ControlIndex load(const std::string& json_path);
    // Build directly from records (e.g. rc_dialog_records from a live RC) instead of the JSON,
    // so the Studio can index a pulled RC. Builds the same lookup maps as load().
    static ControlIndex from_records(std::vector<DialogRecord> dialogs, std::vector<MenuRecord> menus);
    std::string upstream_sha;      // bundle coherence anchor

    // string->control (substitution) and control->string (click) both use these lookups.
    // Match a rendered dialog control by (dialog id, control id, English caption). Text
    // disambiguates the many control==-1 (IDC_STATIC) duplicates.
    const DialogRecord* dialog_lookup(long long dialog, std::optional<long long> control,
                                      const std::string& english_text) const;
    const DialogRecord* dialog_caption(long long dialog) const;
    const MenuRecord*   menu_command(long long command) const;
    const MenuRecord*   menu_popup(const std::string& english_text) const;

    const std::vector<DialogRecord>& dialogs() const { return dialogs_; }
    const std::vector<MenuRecord>&   menus()   const { return menus_; }
private:
    void build_maps();   // called by load(); values are indices into dialogs_/menus_
    std::vector<DialogRecord> dialogs_;
    std::vector<MenuRecord>   menus_;
    std::unordered_map<std::string, size_t> dlg_by_key_;      // (dialog,control,text)
    std::unordered_map<std::string, size_t> dlg_by_id_;       // (dialog,control) when unique
    std::unordered_map<long long, size_t>   dlg_caption_;     // dialog -> CAPTION record
    std::unordered_map<long long, size_t>   menu_by_command_; // command -> record
    std::unordered_map<std::string, size_t> popup_by_text_;   // POPUP English text -> record
};

} // namespace mpctrans
