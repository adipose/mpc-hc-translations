// SPDX-License-Identifier: GPL-3.0-or-later
// Pure planning-logic checks for mpctrans::txsync::tx_reverse_push_plan -- no network, so this runs
// on any platform (the listTx/blameDates lambdas are hand-built fakes; the real implementations,
// mpctrans::txapi::list_translations and mpctrans::github::blame_line_dates, are Windows-only and
// exercised manually via the Studio/CLI). Covers the rules from tx_reverse_push_plan's doc comment:
//   1. Protected + DB empty                       -> push, reason "transifex empty"
//   2. Protected + DB already == upstream          -> nothing
//   3. TxWins + DB == upstream                     -> no push, include flipped false
//   4. TxWins, upstream blame date newer           -> push, include flipped false
//   5. TxWins, transifex datetime newer            -> keepTransifex, include stays true
//   6. TxWins, no date evidence either side        -> keepTransifex ("no date" reason), include stays true
//   7. language absent from Transifex              -> note, no push/keep for that group
//   8. TxWins, blame commit is a Transifex sync    -> keepTransifex (sync-commit reason), include stays
//      (message matches config::TX_SYNC_COMMIT_MARKERS) true EVEN THOUGH the blame date is newer
//   9. two decisions sharing one msgctxt with      -> each resolves against its OWN Transifex row
//      different msgids (regression for the           (join is context+source, not context alone --
//      context-only join bug)                          see tx_reverse_push_plan's byKey)
#include "mpctrans/config.h"
#include "mpctrans/github.h"
#include "mpctrans/po.h"
#include "mpctrans/txsync.h"

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace mpctrans;
using namespace mpctrans::txsync;
using mpctrans::txapi::Translation;
using mpctrans::github::LineBlame;

namespace {

TxDecision mk(const std::string& lang, int res, const std::string& ctx, const std::string& id,
             const std::string& up, const std::string& tx, TxDecision::Kind kind, bool include) {
    TxDecision d;
    d.lang = lang; d.res = res; d.msgctxt = ctx; d.msgid = id;
    d.upstreamStr = up; d.txStr = tx; d.kind = kind; d.include = include;
    return d;
}

// Finds a plan item by (lang,msgctxt,msgid) in `items`, or nullptr. msgid included so case 9 (two
// items sharing a msgctxt) can address each row distinctly.
const ReversePushItem* find_item(const std::vector<ReversePushItem>& items, const std::string& lang,
                                 const std::string& ctx, const std::string& msgid) {
    for (auto& it : items) if (it.lang == lang && it.msgctxt == ctx && it.msgid == msgid) return &it;
    return nullptr;
}

} // namespace

int main() {
    int fail = 0;
    auto err = [&](const std::string& m) { ++fail; std::printf("  FAIL %s\n", m.c_str()); };

    namespace cfg = mpctrans::config;

    // ---- fixture: upstream .po files used for the blame-line lookups in cases 4-6, 8-9. Line
    // numbers are read back via msgstr_line rather than hard-coded, so the fixtures stay correct if
    // the layout below ever changes. ----
    const std::string menusDePath = std::string(cfg::PO_DIR) + "/mpc-hc.de.menus.po";
    const std::string upstreamMenusDe =
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
        "\n"
        "msgctxt \"IDM_A\"\n"
        "msgid \"src_a\"\n"
        "msgstr \"NewVal\"\n"
        "\n"
        "msgctxt \"IDM_B\"\n"
        "msgid \"src_b\"\n"
        "msgstr \"NewValB\"\n"
        "\n"
        "msgctxt \"IDM_C\"\n"
        "msgid \"src_c\"\n"
        "msgstr \"NewValC\"\n"
        "\n"
        "msgctxt \"IDM_D\"\n"
        "msgid \"src_d\"\n"
        "msgstr \"NewValD\"\n";

    int lineA = msgstr_line(upstreamMenusDe, "IDM_A", "src_a");
    int lineB = msgstr_line(upstreamMenusDe, "IDM_B", "src_b");
    int lineC = msgstr_line(upstreamMenusDe, "IDM_C", "src_c");
    int lineD = msgstr_line(upstreamMenusDe, "IDM_D", "src_d");
    if (lineA <= 0 || lineB <= 0 || lineC <= 0 || lineD <= 0)
        err("fixture setup: msgstr_line didn't find one of IDM_A/IDM_B/IDM_C/IDM_D (" +
            std::to_string(lineA) + "/" + std::to_string(lineB) + "/" + std::to_string(lineC) + "/" +
            std::to_string(lineD) + ")");

    // sk/dialogs: two DIFFERENT strings sharing the SAME msgctxt (the real bug this regresses --
    // upstream mpc-hc's sk dialogs .po has two strings both under IDD_GOTO_DLG_IDC_STATIC).
    const std::string skDialogsPath = std::string(cfg::PO_DIR) + "/mpc-hc.sk.dialogs.po";
    const std::string upstreamSkDialogs =
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
        "\n"
        "msgctxt \"IDD_GOTO_DLG_IDC_STATIC\"\n"
        "msgid \"Frame:\"\n"
        "msgstr \"Frame\"\n"
        "\n"
        "msgctxt \"IDD_GOTO_DLG_IDC_STATIC\"\n"
        "msgid \"Time:\"\n"
        "msgstr \"Time\"\n";

    int lineFrame = msgstr_line(upstreamSkDialogs, "IDD_GOTO_DLG_IDC_STATIC", "Frame:");
    int lineTime  = msgstr_line(upstreamSkDialogs, "IDD_GOTO_DLG_IDC_STATIC", "Time:");
    if (lineFrame <= 0 || lineTime <= 0 || lineFrame == lineTime)
        err("fixture setup: msgstr_line didn't distinguish Frame:/Time: under the shared msgctxt (" +
            std::to_string(lineFrame) + "/" + std::to_string(lineTime) + ")");

    // ---- fake Transifex DB (listTx) ----
    std::map<std::pair<int, std::string>, std::vector<Translation>> txData;
    // fr/strings: IDS_A untranslated (case 1), IDS_B already == upstream (case 2)
    txData[{2, "fr"}] = {
        Translation{ "t-A", "IDS_A", "k_a", "src_a", /*value*/"", /*dt*/"", false },
        Translation{ "t-B", "IDS_B", "k_b", "src_b", /*value*/"UP_B", /*dt*/"", false },
    };
    // cs/dialogs: IDD_X DB already caught up to upstream (case 3)
    txData[{0, "cs"}] = {
        Translation{ "t-X", "IDD_X", "k_x", "src_x", /*value*/"UP_X", /*dt*/"", false },
    };
    // de/menus: TxWins conflicts (cases 4/5/6/8)
    txData[{1, "de"}] = {
        Translation{ "t-ma", "IDM_A", "k_a", "src_a", "OldTxVal",  "2024-01-01T00:00:00Z", false },
        Translation{ "t-mb", "IDM_B", "k_b", "src_b", "OldTxValB", "2025-06-01T00:00:00Z", false },
        Translation{ "t-mc", "IDM_C", "k_c", "src_c", "OldTxValC", "",                     false },
        Translation{ "t-md", "IDM_D", "k_d", "src_d", "OldTxValD", "2024-01-01T00:00:00Z", false },
    };
    // sk/dialogs: two rows sharing a msgctxt but with distinct source msgids (case 9) -- context-only
    // join would collapse these onto whichever got inserted last.
    txData[{0, "sk"}] = {
        Translation{ "t-skF", "IDD_GOTO_DLG_IDC_STATIC", "k_f", "Frame:", "OldFrame", "2024-01-01T00:00:00Z", false },
        Translation{ "t-skT", "IDD_GOTO_DLG_IDC_STATIC", "k_t", "Time:",  "OldTime",  "2025-06-01T00:00:00Z", false },
    };
    // es/strings: deliberately absent -> listTx returns {} for it (case 7)

    auto listTx = [&](int res, const std::string& lang) -> std::vector<Translation> {
        auto it = txData.find({ res, lang });
        return it == txData.end() ? std::vector<Translation>{} : it->second;
    };

    // ---- fake blame (blameDates) ----
    std::map<int, LineBlame> menusDeBlame = {
        { lineA, LineBlame{ "2025-06-01T00:00:00Z", "aaaaaaa1111111111111111111111111111111", "Fix German menu label" } },   // upstream newer than tx's 2024-01-01, ordinary commit -> case 4
        { lineB, LineBlame{ "2024-01-01T00:00:00Z", "bbbbbbb2222222222222222222222222222222", "Fix German menu label" } },   // upstream OLDER than tx's 2025-06-01 -> case 5
        // lineC deliberately has no entry -> "no date" (case 6, combined with tx's empty datetime)
        { lineD, LineBlame{ "2026-01-01T00:00:00Z", "deadbeefcafefeed0000000000000000000000", "Transifex updates" } },       // upstream newer than tx's 2024-01-01, BUT landed by a sync commit -> case 8
    };
    std::map<int, LineBlame> skDialogsBlame = {
        { lineFrame, LineBlame{ "2025-01-01T00:00:00Z", "ccccccc3333333333333333333333333333333", "Translate Frame: label" } },  // newer than tx's 2024-01-01 -> push
        { lineTime,  LineBlame{ "2024-06-01T00:00:00Z", "ddddddd4444444444444444444444444444444", "Translate Time: label" } },   // older than tx's 2025-06-01 -> keep
    };
    auto blameDates = [&](const std::string& path) -> std::map<int, LineBlame> {
        if (path == menusDePath) return menusDeBlame;
        if (path == skDialogsPath) return skDialogsBlame;
        return {};
    };

    // ---- decisions (as tx_compute would have produced them) ----
    TxSyncResult result;
    result.languages = { "fr", "cs", "de", "es", "sk" };
    result.upstreamPoBytes[menusDePath] = upstreamMenusDe;
    result.upstreamPoBytes[skDialogsPath] = upstreamSkDialogs;
    result.decisions = {
        mk("fr", 2, "IDS_A", "src_a", "UP_A",  "",           TxDecision::Protected, false),   // 1
        mk("fr", 2, "IDS_B", "src_b", "UP_B",  "",           TxDecision::Protected, false),   // 2
        mk("cs", 0, "IDD_X", "src_x", "UP_X",  "OldCsVal",   TxDecision::TxWins,    true),    // 3
        mk("de", 1, "IDM_A", "src_a", "NewVal","OldTxVal",   TxDecision::TxWins,    true),    // 4
        mk("de", 1, "IDM_B", "src_b", "NewValB","OldTxValB", TxDecision::TxWins,    true),    // 5
        mk("de", 1, "IDM_C", "src_c", "NewValC","OldTxValC", TxDecision::TxWins,    true),    // 6
        mk("es", 2, "IDS_Z", "src_z", "UP_Z",  "OldEsVal",   TxDecision::TxWins,    true),    // 7
        mk("de", 1, "IDM_D", "src_d", "NewValD","OldTxValD", TxDecision::TxWins,    true),    // 8
        mk("sk", 0, "IDD_GOTO_DLG_IDC_STATIC", "Frame:", "Frame", "OldFrame", TxDecision::TxWins, true),   // 9a
        mk("sk", 0, "IDD_GOTO_DLG_IDC_STATIC", "Time:",  "Time",  "OldTime",  TxDecision::TxWins, true),   // 9b
    };

    ReversePushPlan plan = tx_reverse_push_plan(result, listTx, blameDates);

    // ---- 1. Protected + DB empty -> push, "transifex empty" ----
    {
        const ReversePushItem* it = find_item(plan.push, "fr", "IDS_A", "src_a");
        if (!it) err("case1: expected a push item for fr/IDS_A");
        else {
            if (it->reason != "transifex empty") err("case1: reason = '" + it->reason + "'");
            if (it->upstreamStr != "UP_A") err("case1: upstreamStr = '" + it->upstreamStr + "'");
            if (it->txId != "t-A") err("case1: txId = '" + it->txId + "'");
        }
    }

    // ---- 2. Protected + DB already == upstream -> nothing ----
    {
        if (find_item(plan.push, "fr", "IDS_B", "src_b")) err("case2: fr/IDS_B should NOT be pushed");
        if (find_item(plan.keepTransifex, "fr", "IDS_B", "src_b")) err("case2: fr/IDS_B should NOT be in keepTransifex");
    }

    // ---- 3. TxWins + DB == upstream -> no push, include flipped false ----
    {
        if (find_item(plan.push, "cs", "IDD_X", "src_x")) err("case3: cs/IDD_X should NOT be pushed");
        if (find_item(plan.keepTransifex, "cs", "IDD_X", "src_x")) err("case3: cs/IDD_X should NOT be kept either");
        if (result.decisions[2].include) err("case3: cs/IDD_X include should have flipped to false");
    }

    // ---- 4. TxWins, upstream blame date newer -> push, include flipped false ----
    {
        const ReversePushItem* it = find_item(plan.push, "de", "IDM_A", "src_a");
        if (!it) err("case4: expected a push item for de/IDM_A");
        else {
            if (it->reason.rfind("upstream newer:", 0) != 0)
                err("case4: reason = '" + it->reason + "'");
            if (it->upstreamStr != "NewVal") err("case4: upstreamStr = '" + it->upstreamStr + "'");
            if (it->txId != "t-ma") err("case4: txId = '" + it->txId + "'");
        }
        if (result.decisions[3].include) err("case4: de/IDM_A include should have flipped to false");
    }

    // ---- 5. TxWins, transifex datetime newer -> keepTransifex, include stays true ----
    {
        const ReversePushItem* it = find_item(plan.keepTransifex, "de", "IDM_B", "src_b");
        if (!it) err("case5: expected a keepTransifex item for de/IDM_B");
        else if (it->reason != "transifex newer") err("case5: reason = '" + it->reason + "'");
        if (find_item(plan.push, "de", "IDM_B", "src_b")) err("case5: de/IDM_B should NOT be pushed");
        if (!result.decisions[4].include) err("case5: de/IDM_B include should stay true");
    }

    // ---- 6. TxWins, no date evidence -> keepTransifex, conservative reason, include stays true ----
    {
        const ReversePushItem* it = find_item(plan.keepTransifex, "de", "IDM_C", "src_c");
        if (!it) err("case6: expected a keepTransifex item for de/IDM_C");
        else if (it->reason != "no date -- transifex wins (conservative)")
            err("case6: reason = '" + it->reason + "'");
        if (find_item(plan.push, "de", "IDM_C", "src_c")) err("case6: de/IDM_C should NOT be pushed");
        if (!result.decisions[5].include) err("case6: de/IDM_C include should stay true");
    }

    // ---- 7. language absent from Transifex -> note, nothing pushed/kept for that group ----
    {
        bool foundNote = false;
        for (auto& n : plan.notes) if (n.find("es") != std::string::npos && n.find("not in Transifex") != std::string::npos) foundNote = true;
        if (!foundNote) err("case7: expected a note about language 'es' not being in Transifex");
        if (find_item(plan.push, "es", "IDS_Z", "src_z")) err("case7: es/IDS_Z should NOT be pushed");
        if (find_item(plan.keepTransifex, "es", "IDS_Z", "src_z")) err("case7: es/IDS_Z should NOT be kept");
    }

    // ---- 8. TxWins, blame commit is a Transifex sync -> keepTransifex EVEN THOUGH its date is
    // newer than the tx datetime; include must NOT flip (pushing would revert a translator's edit
    // that the sync commit's value predates). ----
    {
        const ReversePushItem* it = find_item(plan.keepTransifex, "de", "IDM_D", "src_d");
        if (!it) err("case8: expected a keepTransifex item for de/IDM_D");
        else {
            if (it->reason.rfind("upstream line from a Transifex-sync commit", 0) != 0)
                err("case8: reason = '" + it->reason + "'");
            if (it->reason.find("deadbee") == std::string::npos)
                err("case8: reason should include the commit sha7, got '" + it->reason + "'");
            if (it->reason.find("Transifex updates") == std::string::npos)
                err("case8: reason should include the commit message, got '" + it->reason + "'");
        }
        if (find_item(plan.push, "de", "IDM_D", "src_d")) err("case8: de/IDM_D should NOT be pushed");
        if (!result.decisions[7].include) err("case8: de/IDM_D include should stay true (sync-commit evidence doesn't count)");
    }

    // ---- 9. two decisions sharing one msgctxt with different msgids resolve to their OWN rows ----
    {
        const ReversePushItem* frame = find_item(plan.push, "sk", "IDD_GOTO_DLG_IDC_STATIC", "Frame:");
        if (!frame) err("case9: expected a push item for sk/IDD_GOTO_DLG_IDC_STATIC/Frame:");
        else {
            if (frame->txId != "t-skF") err("case9: Frame: resolved to the wrong Transifex row, txId = '" + frame->txId + "'");
            if (frame->txStr != "OldFrame") err("case9: Frame: txStr = '" + frame->txStr + "' (expected the Frame: row's value)");
            if (frame->upstreamStr != "Frame") err("case9: Frame: upstreamStr = '" + frame->upstreamStr + "'");
        }
        const ReversePushItem* time = find_item(plan.keepTransifex, "sk", "IDD_GOTO_DLG_IDC_STATIC", "Time:");
        if (!time) err("case9: expected a keepTransifex item for sk/IDD_GOTO_DLG_IDC_STATIC/Time:");
        else {
            if (time->txId != "t-skT") err("case9: Time: resolved to the wrong Transifex row, txId = '" + time->txId + "'");
            if (time->txStr != "OldTime") err("case9: Time: txStr = '" + time->txStr + "' (expected the Time: row's value)");
        }
        // cross-checks: neither row should show up under the OTHER msgid.
        if (find_item(plan.push, "sk", "IDD_GOTO_DLG_IDC_STATIC", "Time:"))
            err("case9: Time: should NOT be in push (only Frame: is)");
        if (find_item(plan.keepTransifex, "sk", "IDD_GOTO_DLG_IDC_STATIC", "Frame:"))
            err("case9: Frame: should NOT be in keepTransifex (only Time: is)");
        if (result.decisions[8].include) err("case9: Frame: include should have flipped to false (pushed)");
        if (!result.decisions[9].include) err("case9: Time: include should stay true (kept)");
    }

    // ---- overall shape: exactly the items above, nothing extra ----
    if (plan.push.size() != 3) err("expected exactly 3 push items, got " + std::to_string(plan.push.size()));
    if (plan.keepTransifex.size() != 4) err("expected exactly 4 keepTransifex items, got " + std::to_string(plan.keepTransifex.size()));

    std::printf("tx_reverse_push_plan: %s\n", fail ? "FAIL" : "OK");
    return fail ? 1 : 0;
}
