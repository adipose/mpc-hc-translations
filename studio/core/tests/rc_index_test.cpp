// SPDX-License-Identifier: GPL-3.0-or-later
// THE RC-INDEX GATE: prove rc_dialog_records(parse(rc)) reproduces the dialog records in
// dist/control-index.json — which bundle-build/build_index.py produced from the SAME pinned
// RC (via upstream's TranslationDataRC). The translatable-control index thus stays current
// with a pulled RC without re-running python. Compares as a MULTISET of the 4-tuple
// (dialog, control, msgctxt, msgid); control_sym is omitted (it is encoded in msgctxt).
// SKIPs cleanly (exit 0) when the gitignored JSON / RC are missing.
//   Usage: rc_index_test [CONTROL_INDEX_JSON] [RC] [RESOURCE_H]
#include "mpctrans/rc_dialogs.h"
#include "mpctrans/control_index.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using mpctrans::DialogRecord;
using mpctrans::RcDialog;
using mpctrans::RcParser;
using mpctrans::rc_dialog_records;

static std::string read_file(const std::string& path, bool binary) {
    std::ifstream f(path, binary ? std::ios::binary : std::ios::in);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Canonical multiset key for one record (control nullopt -> "null", matching JSON's null).
static std::string rec_key(const DialogRecord& r) {
    return std::to_string(r.dialog) + "|" +
           (r.control ? std::to_string(*r.control) : std::string("null")) + "|" +
           r.msgctxt + "|" + r.msgid;
}

int main(int argc, char** argv) {
    std::string json_path = argc > 1 ? argv[1] : "dist/control-index.json";
    std::string rc        = argc > 2 ? argv[2] : "upstream/src/mpc-hc/mpc-hc.rc";
    std::string rh        = argc > 3 ? argv[3] : "upstream/src/mpc-hc/resource.h";
    for (const auto& p : {json_path, rc})
        if (!std::filesystem::exists(p)) { std::printf("SKIP: %s not found\n", p.c_str()); return 0; }

    // RC-derived records: parse + reproduce build_index's dialog-record rules.
    std::vector<RcDialog> parsed = RcParser::parse(read_file(rc, true), read_file(rh, false));
    std::vector<DialogRecord> rc_recs = rc_dialog_records(parsed);

    // JSON records: the oracle (load through the public reader).
    mpctrans::ControlIndex ci = mpctrans::ControlIndex::load(json_path);
    const std::vector<DialogRecord>& json_recs = ci.dialogs();

    std::vector<std::string> rc_keys, json_keys;
    rc_keys.reserve(rc_recs.size());
    json_keys.reserve(json_recs.size());
    for (const auto& r : rc_recs)  rc_keys.push_back(rec_key(r));
    for (const auto& r : json_recs) json_keys.push_back(rec_key(r));
    std::sort(rc_keys.begin(), rc_keys.end());
    std::sort(json_keys.begin(), json_keys.end());

    // Multiset difference both ways (set_difference on sorted ranges respects multiplicity).
    std::vector<std::string> only_json, only_rc;
    std::set_difference(json_keys.begin(), json_keys.end(),
                        rc_keys.begin(), rc_keys.end(), std::back_inserter(only_json));
    std::set_difference(rc_keys.begin(), rc_keys.end(),
                        json_keys.begin(), json_keys.end(), std::back_inserter(only_rc));
    size_t matched = rc_keys.size() - only_rc.size();   // == json_keys.size() - only_json.size()

    std::printf("rc-derived: %zu | json: %zu | matched: %zu | only-in-json: %zu | only-in-rc: %zu\n",
                rc_recs.size(), json_recs.size(), matched, only_json.size(), only_rc.size());
    int shown = 0;
    for (const auto& k : only_json) {
        if (shown >= 10) break;
        std::printf("  only-in-json: %s\n", k.c_str()); ++shown;
    }
    shown = 0;
    for (const auto& k : only_rc) {
        if (shown >= 10) break;
        std::printf("  only-in-rc:   %s\n", k.c_str()); ++shown;
    }

    bool ok = only_json.empty() && only_rc.empty();
    std::printf("%s\n", ok ? "RC-INDEX GATE GREEN" : "FAIL");
    return ok ? 0 : 1;
}
