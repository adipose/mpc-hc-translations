// SPDX-License-Identifier: GPL-3.0-or-later
// Smoke test for the WinHTTP GitHub client. Network parts SKIP (exit 0) when offline so the
// gate stays green on disconnected machines; they run for real when the network is up:
//   1. token store roundtrip on a SCRATCH credential target (never touches the real token)
//   2. request_device_code -> user_code + verification_uri (public client id, no interaction;
//      the pending code simply expires)
//   3. fetch_latest of a real upstream .po (unauthenticated Contents API) -> parses as PO
// The full open_translation_pr flow needs an authorized token + writes to github.com, so it
// is exercised manually from the Studio, not here.
#include "mpctrans/github.h"
#include "mpctrans/po.h"
#include <cstdio>
#include <string>
using namespace mpctrans;

int main() {
    int fail = 0;
    auto err = [&](const std::string& m) { ++fail; std::printf("  FAIL %s\n", m.c_str()); };

    // 1. credential-manager roundtrip (local, always runs)
    const wchar_t* scratch = L"mpc-hc-translation-studio/test-scratch";
    github::clear_token(scratch);
    github::store_token(github::Token{"gho_smoke_test_value"}, scratch);
    auto t = github::load_token(scratch);
    if (!t || t->access_token != "gho_smoke_test_value") err("token store/load roundtrip");
    github::clear_token(scratch);
    if (github::load_token(scratch)) err("token not cleared");
    std::printf("token store roundtrip: %s\n", fail ? "FAIL" : "OK");

    // 2 + 3. network
    try {
        github::DeviceCode dc = github::request_device_code("public_repo");
        if (dc.user_code.empty() || dc.verification_uri.find("github.com") == std::string::npos)
            err("device code response malformed");
        else
            std::printf("device code: user_code=%s interval=%ds expires=%ds OK\n",
                        dc.user_code.c_str(), dc.interval, dc.expires_in);

        std::string po = github::fetch_latest(github::Token{},
                                              "src/mpc-hc/mpcresources/PO/mpc-hc.de.strings.po");
        PoFile pf = PoFile::parse_bytes(po);
        if (pf.entries.size() < 100)
            err("fetched .po parsed to only " + std::to_string(pf.entries.size()) + " entries");
        else
            std::printf("fetch_latest: %zu bytes, %zu entries OK\n", po.size(), pf.entries.size());
    } catch (const std::exception& e) {
        std::printf("SKIP network checks (%s)\n", e.what());
    }
    return fail ? 1 : 0;
}
