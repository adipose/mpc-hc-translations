// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

// Internal: shared WinHTTP plumbing used by mpctrans::github (github.cpp) and re-used by
// mpctrans (ai_client.cpp) so the provider HTTP calls don't duplicate the request/response
// machinery (timeouts, session reuse, header handling). Not part of the public per-module API —
// callers outside libmpctrans should go through github.h / ai_client.h instead.

namespace mpctrans::github {

struct HttpResponse { int status = 0; std::string body; };

// UTF-8 -> UTF-16, used when handing strings to the wide WinHTTP APIs.
std::wstring widen(const std::string& s);

#ifdef _WIN32
// One HTTPS request. headers are "Key: Value" lines; body is sent verbatim. If `reuseSession` is
// non-null it is used (and left open) so repeated requests reuse WinHTTP's pooled keep-alive TLS
// connection — one handshake amortized over many fetches; otherwise a session is opened per call.
// Throws std::runtime_error on a bad URL or a failed WinHTTP call (message includes the URL and,
// for send/receive failures, the WinHTTP error code).
HttpResponse https_request(const wchar_t* method, const std::string& url,
                           const std::vector<std::string>& headers, const std::string& body,
                           HINTERNET reuseSession = nullptr);
#else
// Non-Windows stub — mirrors github.h's "Windows only" stub pattern so ai_client.cpp (and any
// other TU that calls this) still links on non-Windows portable-gate builds.
HttpResponse https_request(const wchar_t* method, const std::string& url,
                           const std::vector<std::string>& headers, const std::string& body,
                           void* reuseSession = nullptr);
#endif

} // namespace mpctrans::github
