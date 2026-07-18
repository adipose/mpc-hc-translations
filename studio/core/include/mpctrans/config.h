// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
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

} // namespace mpctrans::config
