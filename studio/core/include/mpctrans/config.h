// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
// libmpctrans — build-time constants for the Translation Studio.
// Portable (no MFC). Values are all public/non-secret.

namespace mpctrans::config {

// OAuth App (device flow). PUBLIC client id — safe to ship in the binary. No secret is used.
inline constexpr char OAUTH_CLIENT_ID[] = "Ov23licLoTVvrq9xr0rz";
inline constexpr char DEVICE_SCOPE[]    = "public_repo";   // coarse; alt = fine-grained PAT

// Canonical upstream repo (PRs go straight here). The bundle submodule pins a clsid2 commit.
inline constexpr char UPSTREAM_OWNER[]  = "clsid2";
inline constexpr char UPSTREAM_REPO[]   = "mpc-hc";
inline constexpr char UPSTREAM_BRANCH[] = "develop";

// THIS project (the Studio + translation bundle itself). Used by the research-correction PR flow
// (mpctrans::corrections) — a translator submits a fix to data/research-corrections.jsonl as a PR
// here, reusing the same github::open_pr machinery as the translation PRs above.
inline constexpr char STUDIO_OWNER[]  = "adipose";
inline constexpr char STUDIO_REPO[]   = "mpc-hc-translations";
inline constexpr char STUDIO_BRANCH[] = "main";

// The maintainer's Transifex staging fork of MPC-HC (a fresher Transifex snapshot than clsid2
// develop). On language load the Studio offers to fill base-empty strings from here.
inline constexpr char TRANSIFEX_OWNER[]  = "adipose";
inline constexpr char TRANSIFEX_REPO[]   = "mpc-hc";
inline constexpr char TRANSIFEX_BRANCH[] = "transifex";

// Per-language translations live as 3 per-resource files here.
inline constexpr char PO_DIR[]          = "src/mpc-hc/mpcresources/PO";
inline constexpr const char* RESOURCES[] = { "dialogs", "menus", "strings" };  // mpc-hc.<lang>.<res>.po

// GitHub device-flow endpoints (see mpctrans::github).
inline constexpr char GH_DEVICE_CODE_URL[]   = "https://github.com/login/device/code";
inline constexpr char GH_ACCESS_TOKEN_URL[]  = "https://github.com/login/oauth/access_token";
inline constexpr char GH_VERIFICATION_URL[]  = "https://github.com/login/device";
inline constexpr char GH_API[]               = "https://api.github.com";
// Raw file CDN — not under the API's 60-req/hr unauthenticated cap (used by fetch_latest).
inline constexpr char GH_RAW[]               = "https://raw.githubusercontent.com";

// Transifex REST API v3 (JSON:API) -- see mpctrans::txapi. Reverse-push (Studio -> Transifex) keeps
// upstream develop translations from being reverted by the next Transifex-DB -> transifex-branch
// sync ("Transifex wins"), by pushing the upstream value into the live DB via PATCH.
inline constexpr char TX_API[]     = "https://rest.api.transifex.com";
inline constexpr char TX_ORG[]     = "mpchc";
inline constexpr char TX_PROJECT[] = "mpchc";

// "o:mpchc:p:mpchc:r:src-mpc-hc-mpcresources-po-mpc-hc-<dialogs|menus|strings>-pot--transifex"
inline std::string tx_resource_id(int res) {
    return std::string("o:") + TX_ORG + ":p:" + TX_PROJECT +
          ":r:src-mpc-hc-mpcresources-po-mpc-hc-" + RESOURCES[res] + "-pot--transifex";
}

// Case-insensitive substrings that mark a develop commit message as a Transifex-sync commit (its
// blame date is when the sync LANDED, not when the translation was authored, and its VALUE is
// whatever Transifex held AT sync time -- possibly older than the live DB). Covers "Transifex
// updates" (the old uptransifex.sh/GUI flow), "Refresh Transifex sync (Studio CLI)", "Merge upstream
// develop + Transifex sync (Studio CLI)", and any future message that says so. See
// mpctrans::txsync::tx_reverse_push_plan.
inline const std::vector<std::string> TX_SYNC_COMMIT_MARKERS = { "transifex" };

} // namespace mpctrans::config
