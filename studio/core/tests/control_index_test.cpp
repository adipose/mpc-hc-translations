// SPDX-License-Identifier: GPL-3.0-or-later
// Load the real dist/control-index.json (regenerate: python bundle-build/build_index.py) and
// verify every record is findable through the public lookups:
//   - each CAPTION record via dialog_caption(dialog)
//   - each control record via dialog_lookup(dialog, control, msgid) -> same msgctxt
//   - each menu command via menu_command(command); each POPUP via menu_popup(msgid)
// SKIPs (exit 0) when the artifact is absent — it is gitignored, fresh clones won't have it.
//   Usage: control_index_test [JSON_PATH]
#include "mpctrans/control_index.h"
#include <cstdio>
#include <filesystem>
#include <string>
using namespace mpctrans;

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "dist/control-index.json";
    if (!std::filesystem::exists(path)) {
        std::printf("SKIP: %s not found (run: python bundle-build/build_index.py)\n", path.c_str());
        return 0;
    }
    ControlIndex ci = ControlIndex::load(path);
    int fail = 0;
    auto err = [&](const std::string& m) { if (++fail <= 10) std::printf("  FAIL %s\n", m.c_str()); };

    if (ci.upstream_sha.empty()) err("upstream_sha empty");
    if (ci.dialogs().size() < 100) err("suspiciously few dialog records");
    if (ci.menus().size() < 50) err("suspiciously few menu records");

    int captions = 0;
    for (const auto& r : ci.dialogs()) {
        if (!r.control) {
            ++captions;
            const DialogRecord* hit = ci.dialog_caption(r.dialog);
            if (!hit || hit->msgctxt != r.msgctxt) err("caption lookup failed: " + r.msgctxt);
            // caption records must also resolve through the general lookup
            hit = ci.dialog_lookup(r.dialog, std::nullopt, r.msgid);
            if (!hit || hit->msgctxt != r.msgctxt) err("caption via dialog_lookup failed: " + r.msgctxt);
        } else {
            const DialogRecord* hit = ci.dialog_lookup(r.dialog, r.control, r.msgid);
            if (!hit) { err("dialog_lookup miss: " + r.msgctxt); continue; }
            // duplicate (dialog,control,text) triples are legitimately ambiguous; require key match
            if (hit->dialog != r.dialog || hit->control != r.control || hit->msgid != r.msgid)
                err("dialog_lookup wrong record: " + r.msgctxt);
        }
    }
    if (captions == 0) err("no CAPTION records");

    // text fallback: a control that is unique by (dialog,control) resolves even with unknown text
    for (const auto& r : ci.dialogs()) {
        if (r.control && *r.control != -1) {
            const DialogRecord* hit = ci.dialog_lookup(r.dialog, r.control, "~no such caption~");
            if (hit && (hit->dialog != r.dialog || hit->control != r.control))
                err("id-fallback returned wrong record: " + r.msgctxt);
            break;   // one probe is enough
        }
    }

    int popups = 0, commands = 0;
    for (const auto& r : ci.menus()) {
        if (r.command) {
            ++commands;
            const MenuRecord* hit = ci.menu_command(*r.command);
            if (!hit || hit->command != r.command) err("menu_command miss: " + r.sym);
        } else {
            ++popups;
            const MenuRecord* hit = ci.menu_popup(r.msgid);
            if (!hit || hit->msgid != r.msgid) err("menu_popup miss: " + r.msgid);
        }
    }
    if (popups == 0) err("no POPUP records");
    if (commands == 0) err("no command records");

    // negative lookups
    if (ci.dialog_caption(999999)) err("bogus caption resolved");
    if (ci.menu_command(999999)) err("bogus command resolved");
    if (ci.menu_popup("~no such popup~")) err("bogus popup resolved");

    std::printf("%zu dialog records (%d captions), %zu menu records (%d popups): %s\n",
                ci.dialogs().size(), captions, ci.menus().size(), popups,
                fail ? "FAIL" : "all lookups OK");
    return fail ? 1 : 0;
}
