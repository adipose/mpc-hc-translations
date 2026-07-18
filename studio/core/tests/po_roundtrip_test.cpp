// SPDX-License-Identifier: GPL-3.0-or-later
// THE FIDELITY GATE. For every committed .po: read -> LF-normalize -> parse -> reconstruct() ->
// assert == original. polib produced the committed files, so a byte match proves the native
// serializer reproduces polib exactly. Prints the first diff line per failing file.
//   Usage: po_roundtrip_test [PO_DIR]   (default: upstream/src/mpc-hc/mpcresources/PO)
#include "mpctrans/po.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
namespace fs = std::filesystem;

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary); std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string to_lf(std::string s) {
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) s = s.substr(3);
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) { if (s[i]=='\r' && i+1<s.size() && s[i+1]=='\n') continue; o += s[i]; }
    return o;
}
static void first_diff(const std::string& a, const std::string& b) {
    size_t i = 0; while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    auto lineno = [](const std::string& s, size_t p){ return 1 + std::count(s.begin(), s.begin()+std::min(p,s.size()), '\n'); };
    auto line_at = [](const std::string& s, size_t p){ size_t b=s.rfind('\n',p?p-1:0); size_t e=s.find('\n',p);
        b = (b==std::string::npos)?0:b+1; e=(e==std::string::npos)?s.size():e; return s.substr(b, e-b); };
    std::printf("    first diff @byte %zu (line %ld)\n", i, lineno(a, i));
    std::printf("      orig : %s\n", line_at(a, i).c_str());
    std::printf("      built: %s\n", line_at(b, i).c_str());
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "upstream/src/mpc-hc/mpcresources/PO";
    int files = 0, ok = 0, fail = 0;
    for (auto& p : fs::directory_iterator(dir)) {
        if (p.path().extension() != ".po") continue;
        ++files;
        std::string orig = to_lf(read_file(p.path().string()));
        std::string built = mpctrans::PoFile::parse_bytes(orig).reconstruct();
        if (built == orig) ++ok;
        else { ++fail; if (fail <= 5) { std::printf("  FAIL %s\n", p.path().filename().string().c_str()); first_diff(orig, built); } }
    }
    std::printf("\n%d/%d files reproduced byte-exactly (%d failed)\n", ok, files, fail);
    return fail ? 1 : 0;
}
