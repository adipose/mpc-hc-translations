// SPDX-License-Identifier: GPL-3.0-or-later
// THE RC-ENCODING GATE: the parser must yield identical results whether given the on-disk
// UTF-16LE mpc-hc.rc or the UTF-8 bytes GitHub's contents API returns for it (the repo stores
// the blob UTF-8 via a working-tree-encoding filter). We convert the UTF-16 file to UTF-8 here
// and assert rc_dialog_records(utf16) == rc_dialog_records(utf8), so the Get-latest RC path
// (which parses UTF-8) can't silently diverge from the local-file path. SKIPs if the RC is missing.
//   Usage: rc_encoding_test [RC] [RESOURCE_H]
#include "mpctrans/rc_dialogs.h"
#include "mpctrans/control_index.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
using namespace mpctrans;

static std::string read_file(const std::string& p, bool bin) {
    std::ifstream f(p, bin ? std::ios::binary : std::ios::in);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
// Minimal UTF-16LE (optional BOM) -> UTF-8, matching what a working-tree-encoding filter stores.
static std::string utf16le_to_utf8(const std::string& b) {
    std::string o;
    size_t i = (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) ? 2 : 0;
    auto put = [&](unsigned cp) {
        if (cp <= 0x7F) o += (char)cp;
        else if (cp <= 0x7FF) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp <= 0xFFFF) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    };
    for (; i + 1 < b.size(); i += 2) {
        unsigned u = (unsigned char)b[i] | ((unsigned char)b[i + 1] << 8);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < b.size()) {
            unsigned lo = (unsigned char)b[i + 2] | ((unsigned char)b[i + 3] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { put(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)); i += 2; continue; }
        }
        put(u);
    }
    return o;
}
static std::multiset<std::string> keys(const std::vector<DialogRecord>& v) {
    std::multiset<std::string> k;
    for (const auto& r : v)
        k.insert(std::to_string(r.dialog) + "|" + (r.control ? std::to_string(*r.control) : "null") +
                 "|" + r.msgctxt + "|" + r.msgid);
    return k;
}

int main(int argc, char** argv) {
    std::string rc = argc > 1 ? argv[1] : "upstream/src/mpc-hc/mpc-hc.rc";
    std::string rh = argc > 2 ? argv[2] : "upstream/src/mpc-hc/resource.h";
    if (!std::filesystem::exists(rc)) { std::printf("SKIP: %s not found\n", rc.c_str()); return 0; }

    std::string u16 = read_file(rc, true), h = read_file(rh, false);
    std::string u8 = utf16le_to_utf8(u16);
    if (u8.size() >= 2 && (unsigned char)u8[0] != 0xFF) { /* now looks like UTF-8 (no BOM) */ }

    auto a = rc_dialog_records(RcParser::parse(u16, h));   // on-disk UTF-16 path
    auto b = rc_dialog_records(RcParser::parse(u8,  h));   // GitHub UTF-8 path
    bool ok = a.size() == b.size() && keys(a) == keys(b);

    std::printf("utf16 records: %zu | utf8 records: %zu | %s\n", a.size(), b.size(),
                ok ? "identical" : "DIVERGED");
    std::printf("%s\n", ok ? "RC-ENCODING GATE GREEN" : "FAIL");
    return ok ? 0 : 1;
}
