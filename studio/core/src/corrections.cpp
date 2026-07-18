// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpctrans/corrections.h"
#include "mpctrans/config.h"

#include <ctime>
#include <stdexcept>

#include <json.hpp>

// Portable (no #ifdef _WIN32 needed here): everything network-y goes through mpctrans::github, which
// already handles the Windows/non-Windows split. This file just builds the JSONL line + PR body and
// calls github::fetch_latest / github::open_pr.

namespace mpctrans::corrections {

using nlohmann::json;
namespace cfg = mpctrans::config;

namespace {

// "2026-07-10T14:32:07Z" (ISO 8601, UTC) — same portable gmtime_s/gmtime_r split as po.cpp's
// now_po_revision_utc and github.cpp's branch-name stamp.
std::string now_iso8601_utc() {
    std::time_t t = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &g);
    return buf;
}

} // namespace

const char* field_name(Field f) {
    return f == Field::SemanticPurpose ? "semantic_purpose" : "functional_purpose";
}

std::string build_correction_line(const std::string& msgctxt, const std::string& msgid, Field field,
                                  const std::string& text, const std::string& note,
                                  const std::string& author) {
    json j = {
        {"msgctxt", msgctxt},
        {"msgid",   msgid},
        {"field",   field_name(field)},
        {"text",    text},
        {"note",    note},
        {"author",  author},
        {"date",    now_iso8601_utc()},
    };
    return j.dump();   // compact, single line; caller appends the '\n'
}

std::string submit_research_correction(const github::Token& t, const std::string& author,
                                       const std::vector<Correction>& corrections) {
    if (corrections.empty()) throw std::runtime_error("corrections: nothing to submit");

    static const char* kPath = "data/research-corrections.jsonl";
    std::string content;
    if (auto existing = github::fetch_latest(t, cfg::STUDIO_OWNER, cfg::STUDIO_REPO,
                                             cfg::STUDIO_BRANCH, kPath))
        content = *existing;
    // else: 404 (overlay file doesn't exist upstream yet) -> start from empty content

    if (!content.empty() && content.back() != '\n') content += '\n';   // keep the file well-formed JSONL

    std::string body = "Research correction submitted from the Translation Studio.\n\n";
    for (const auto& c : corrections) {
        content += build_correction_line(c.msgctxt, c.msgid, c.field, c.new_text, c.note, author);
        content += '\n';

        body += "**" + std::string(field_name(c.field)) + "** (`" + c.msgctxt + "`)\n\n";
        body += "- old: " + (c.old_text.empty() ? std::string("*(empty)*") : c.old_text) + "\n";
        body += "- new: " + c.new_text + "\n";
        if (!c.note.empty()) body += "- why: " + c.note + "\n";
        body += "\n";
    }

    std::string title = "research: correct " + corrections.front().msgctxt;
    return github::open_pr(t, cfg::STUDIO_OWNER, cfg::STUDIO_REPO, cfg::STUDIO_BRANCH,
                           "research-fix", { github::FileEdit{kPath, content} }, title, body);
}

} // namespace mpctrans::corrections
