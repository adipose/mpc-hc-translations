// SPDX-License-Identifier: GPL-3.0-or-later
// THE SPLICE GATE. For every committed .po, PoFile::splice must be minimal-diff:
//   1. no edits (or all edits dropped) -> returns the original bytes unchanged
//   2. editing ONE entry adds exactly: the new msgstr line, new PO-Revision-Date,
//      new Last-Translator, one '# Translators:' credit line — and nothing else;
//      every other entry survives byte-identically; POT-Creation-Date is untouched
//   3. an edit whose (msgctxt,msgid) is absent is silently dropped
//   4. re-splicing with the same translator does not duplicate the credit line
//   Usage: po_splice_test [PO_DIR]   (default: upstream/src/mpc-hc/mpcresources/PO)
#include "mpctrans/po.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using mpctrans::PoEntry;
using mpctrans::PoFile;

static const std::string kTranslator = "Test Translator <test@example.com>";

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary); std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string to_lf(std::string s) {
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) s = s.substr(3);
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) { if (s[i]=='\r' && i+1<s.size() && s[i+1]=='\n') continue; o += s[i]; }
    return o;
}
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out; size_t b = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\n') { out.push_back(s.substr(b, i - b)); b = i + 1; }
    return out;
}
// Raw value of a quoted header field, e.g. `"POT-Creation-Date: <value>\n"`.
static std::string header_field(const std::string& bytes, const std::string& key) {
    const std::string prefix = "\"" + key + ": ";
    size_t p = bytes.find(prefix);
    if (p == std::string::npos) return {};
    size_t start = p + prefix.size(), end = bytes.find("\\n\"", start);
    return end == std::string::npos ? std::string{} : bytes.substr(start, end - start);
}
static size_t count_occurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0; for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}
static bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
static bool entries_equal(const PoEntry& a, const PoEntry& b) {
    return a.msgctxt == b.msgctxt && a.msgid == b.msgid && a.msgstr == b.msgstr && a.comments == b.comments;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "upstream/src/mpc-hc/mpcresources/PO";
    int files = 0, ok = 0, fail = 0;
    for (auto& p : fs::directory_iterator(dir)) {
        if (p.path().extension() != ".po") continue;
        std::string name = p.path().filename().string();
        std::string orig = to_lf(read_file(p.path().string()));
        PoFile po = PoFile::parse_bytes(orig);
        if (po.entries.empty()) continue;
        ++files;
        std::vector<std::string> errs;

        // 1. no-op splice returns the original bytes
        if (PoFile::splice(orig, {}, kTranslator) != orig)
            errs.push_back("no-op splice changed the bytes");

        // 2. one-entry edit
        PoEntry ed = po.entries.front();
        ed.msgstr = "SPLICETEST";
        std::string out = PoFile::splice(orig, {ed}, kTranslator);
        PoFile po2 = PoFile::parse_bytes(out);
        if (po2.entries.size() != po.entries.size())
            errs.push_back("entry count changed");
        else
            for (size_t i = 0; i < po.entries.size(); ++i) {
                if (i == 0) {
                    PoEntry want = po.entries[0]; want.msgstr = "SPLICETEST";
                    if (!entries_equal(po2.entries[0], want)) errs.push_back("edited entry wrong");
                } else if (!entries_equal(po2.entries[i], po.entries[i])) {
                    errs.push_back("untouched entry #" + std::to_string(i) + " changed"); break;
                }
            }
        if (header_field(out, "POT-Creation-Date") != header_field(orig, "POT-Creation-Date"))
            errs.push_back("POT-Creation-Date changed");
        if (header_field(out, "Last-Translator") != kTranslator)
            errs.push_back("Last-Translator not set");
        if (orig.find("# Translators:\n") != std::string::npos &&
            count_occurrences(out, "# " + kTranslator + ",") != 1)
            errs.push_back("translator credit missing or duplicated");

        // minimal-diff: multiset line diff between orig and out
        std::map<std::string, long> delta;
        for (auto& l : split_lines(orig)) ++delta[l];
        for (auto& l : split_lines(out))  --delta[l];
        for (auto& kv : delta) {
            if (kv.second == 0) continue;
            const std::string& l = kv.first;
            if (kv.second < 0) {   // line added by splice
                bool allowed = l == "msgstr \"SPLICETEST\"" ||
                               starts_with(l, "\"PO-Revision-Date: ") ||
                               starts_with(l, "\"Last-Translator: ") ||
                               starts_with(l, "# " + kTranslator + ",");
                if (!allowed) errs.push_back("unexpected added line: " + l);
            } else {               // line removed by splice
                bool allowed = starts_with(l, "msgstr ") || starts_with(l, "\"");
                if (!allowed) errs.push_back("unexpected removed line: " + l);
            }
        }

        // 3. an absent-key edit is dropped (identical entries to the one-edit result)
        PoEntry bogus; bogus.msgctxt = "NO_SUCH_CTX"; bogus.msgid = "no such msgid"; bogus.msgstr = "x";
        PoFile po3 = PoFile::parse_bytes(PoFile::splice(orig, {ed, bogus}, kTranslator));
        bool same = po3.entries.size() == po2.entries.size();
        for (size_t i = 0; same && i < po3.entries.size(); ++i) same = entries_equal(po3.entries[i], po2.entries[i]);
        if (!same) errs.push_back("dropped edit altered entries");

        // 4. re-splice with the same translator must not duplicate the credit
        if (po.entries.size() > 1 && orig.find("# Translators:\n") != std::string::npos) {
            PoEntry ed2 = po.entries[1]; ed2.msgstr = "SPLICETEST2";
            std::string out2 = PoFile::splice(out, {ed2}, kTranslator);
            if (count_occurrences(out2, "# " + kTranslator + ",") != 1)
                errs.push_back("credit line duplicated on re-splice");
        }

        if (errs.empty()) ++ok;
        else { ++fail; if (fail <= 5) { std::printf("  FAIL %s\n", name.c_str());
                   for (auto& e : errs) std::printf("    %s\n", e.c_str()); } }
    }
    std::printf("\n%d/%d files passed splice checks (%d failed)\n", ok, files, fail);
    return fail ? 1 : 0;
}
