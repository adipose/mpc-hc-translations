# core/thirdparty/ — vendored single-file dependencies

Statically linked into `libmpctrans` (Path B: one self-contained native `.exe`, no external DLLs).

Vendored (pinned):

- **`sqlite3.c` + `sqlite3.h`** — SQLite amalgamation **3.53.3** (public domain). Reads the enrichment DBs.
  https://sqlite.org/2026/sqlite-amalgamation-3530300.zip
- **`json.hpp`** — nlohmann/json single header **v3.12.0** (MIT). Parses GitHub API responses + control-index.json.
  https://github.com/nlohmann/json/releases

WinHTTP is a system library (link `winhttp.lib`) — nothing to vendor.
No libcurl / no OpenSSL (WinHTTP handles TLS).
