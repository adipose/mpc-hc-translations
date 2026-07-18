# libmpctrans + its test gates. Builds under MSVC (Windows), g++, or clang.
#   cmake -S . -B build ; cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(libmpctrans C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)   # sources are UTF-8 (matches the vcxproj builds)
endif()

add_library(mpctrans STATIC
    src/po.cpp
    src/validate.cpp
    src/github.cpp        # WinHTTP on Windows; stubs elsewhere
    src/fetch_cache.cpp    # disk cache for GitHub downloads, keyed by upstream HEAD sha (stub off-Windows)
    src/enrichment.cpp
    src/control_index.cpp
    src/rc_dialogs.cpp     # runtime RC dialog reconstruction (rc_conformance gate)
    src/corrections.cpp    # research-correction PR flow (data/research-corrections.jsonl overlay)
    src/ai_client.cpp      # AI-assisted translation suggestions (claude/gpt/glm/openrouter); stub off-Windows
    src/suggestion_store.cpp  # M2 persistent AI-suggestion store (ai-suggestions.sqlite); stub off-Windows
    src/confidence.cpp     # M3 confidence scoring (cross-model agreement + back-translation drift); portable
    src/data_update.cpp    # in-app data refresh: diff+apply dist/data-manifest.json; stub off-Windows
    thirdparty/sqlite3.c  # vendored amalgamation (see thirdparty/README.md)
)
target_include_directories(mpctrans PUBLIC include)
# PUBLIC (not PRIVATE): ai_client.h is a public header that itself includes <json.hpp> (it exposes
# nlohmann::json in build_openai_request/build_anthropic_request for unit testing), so consumers of
# mpctrans — including ai_client_test — need thirdparty/ on their include path too.
target_include_directories(mpctrans PUBLIC thirdparty)
# trim sqlite: no extensions loading (avoids dl deps), enrichment DBs are read-only
set_property(SOURCE thirdparty/sqlite3.c APPEND PROPERTY COMPILE_DEFINITIONS
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=0 SQLITE_DEFAULT_MEMSTATUS=0)
if(WIN32)
    target_link_libraries(mpctrans PUBLIC winhttp shell32 ole32 bcrypt)   # bcrypt: data_update's sha256
endif()

enable_testing()

add_executable(validate_conformance_test tests/validate_conformance_test.cpp)
target_link_libraries(validate_conformance_test PRIVATE mpctrans)
add_test(NAME validate COMMAND validate_conformance_test)

add_executable(po_roundtrip_test tests/po_roundtrip_test.cpp)
target_link_libraries(po_roundtrip_test PRIVATE mpctrans)
# THE FIDELITY GATE — needs the submodule checked out (git submodule update --init).
add_test(NAME po_roundtrip
    COMMAND po_roundtrip_test ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/mpcresources/PO)

add_executable(po_splice_test tests/po_splice_test.cpp)
target_link_libraries(po_splice_test PRIVATE mpctrans)
# THE SPLICE GATE — minimal-diff splice over every committed .po (needs the submodule too).
add_test(NAME po_splice
    COMMAND po_splice_test ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/mpcresources/PO)

add_executable(control_index_test tests/control_index_test.cpp)
target_link_libraries(control_index_test PRIVATE mpctrans)
# SKIPs (exit 0) if the gitignored artifact is missing (python bundle-build/build_index.py).
add_test(NAME control_index
    COMMAND control_index_test ${CMAKE_CURRENT_SOURCE_DIR}/../../dist/control-index.json)

if(WIN32)
    add_executable(github_smoke_test tests/github_smoke_test.cpp)
    target_link_libraries(github_smoke_test PRIVATE mpctrans)
    # Network checks SKIP cleanly when offline; the CredMan roundtrip always runs.
    add_test(NAME github_smoke COMMAND github_smoke_test)

    add_executable(neutral_dll_test tests/neutral_dll_test.cpp)
    target_link_libraries(neutral_dll_test PRIVATE mpctrans)
    # THE DRY-RUN GATE — SKIPs if the gitignored artifacts are missing
    # (bundle-build/neutral-dll/build_neutral_dll.ps1 + python bundle-build/build_index.py).
    add_test(NAME neutral_dll
        COMMAND neutral_dll_test ${CMAKE_CURRENT_SOURCE_DIR}/../../dist/mpcresources.neutral.dll
                                 ${CMAKE_CURRENT_SOURCE_DIR}/../../dist/control-index.json)

    add_executable(rc_conformance_test tests/rc_conformance_test.cpp)
    target_link_libraries(rc_conformance_test PRIVATE mpctrans)
    # THE RC-RECONSTRUCTION GATE — RcParser rebuilds every IDD_* dialog identical to
    # the compiled RT_DIALOG templates (oracle = the neutral resource DLL). SKIPs if
    # the gitignored DLL / checked-out RC are missing.
    add_test(NAME rc_conformance
        COMMAND rc_conformance_test ${CMAKE_CURRENT_SOURCE_DIR}/../../dist/mpcresources.neutral.dll
                                    ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/mpc-hc.rc
                                    ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/resource.h)

    add_executable(rc_emit_test tests/rc_emit_test.cpp)
    target_link_libraries(rc_emit_test PRIVATE mpctrans)
    # THE RC-EMIT GATE — emit_dlgtemplate(parse(rc)) reproduces the compiled RT_DIALOG
    # template structurally (walked-from-emitted == oracle DLL, incl. style). SKIPs if
    # the gitignored DLL / checked-out RC are missing.
    add_test(NAME rc_emit
        COMMAND rc_emit_test ${CMAKE_CURRENT_SOURCE_DIR}/../../dist/mpcresources.neutral.dll
                             ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/mpc-hc.rc
                             ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/resource.h)

    add_executable(rc_render_test tests/rc_render_test.cpp)
    target_link_libraries(rc_render_test PRIVATE mpctrans comctl32)
    # THE RC-RENDER GATE (end-to-end) — emit_dlgtemplate(parse(rc)) actually loads via
    # CreateDialogIndirect with NO DLL; asserts each dialog creates + child count matches.
    # Needs only the checked-out RC (no bundle artifact). SKIPs if the RC is missing.
    add_test(NAME rc_render
        COMMAND rc_render_test ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/mpc-hc.rc
                               ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/resource.h)

    add_executable(rc_index_test tests/rc_index_test.cpp)
    target_link_libraries(rc_index_test PRIVATE mpctrans)
    # THE RC-INDEX GATE — rc_dialog_records(parse(rc)) reproduces control-index.json's dialog
    # records (the translatable-control index). SKIPs if the gitignored json / RC are missing.
    add_test(NAME rc_index
        COMMAND rc_index_test ${CMAKE_CURRENT_SOURCE_DIR}/../../dist/control-index.json
                              ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/mpc-hc.rc
                              ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/resource.h)

    add_executable(rc_encoding_test tests/rc_encoding_test.cpp)
    target_link_libraries(rc_encoding_test PRIVATE mpctrans)
    # THE RC-ENCODING GATE — parse(UTF-16 file) == parse(UTF-8 as GitHub's API returns it), so the
    # Get-latest RC path (UTF-8) can't diverge from the local-file path. SKIPs if the RC is missing.
    add_test(NAME rc_encoding
        COMMAND rc_encoding_test ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/mpc-hc.rc
                                 ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/src/mpc-hc/resource.h)
endif()

add_executable(enrichment_test tests/enrichment_test.cpp)
target_link_libraries(enrichment_test PRIVATE mpctrans)
# Core sqlite (committed dist/core-enrichment.sqlite) SKIPs if missing; packs are committed too.
add_test(NAME enrichment
    COMMAND enrichment_test ${CMAKE_CURRENT_SOURCE_DIR}/../../dist/core-enrichment.sqlite
                            ${CMAKE_CURRENT_SOURCE_DIR}/../../enrichment/lang)

add_executable(ai_client_test tests/ai_client_test.cpp)
target_link_libraries(ai_client_test PRIVATE mpctrans)
# Pure JSON build/parse + extract_translation_json — no network, so it runs on any platform
# (suggest_translation itself is Windows-only and is NOT exercised here, same as github_smoke's
# split between the always-run CredMan roundtrip and the network-dependent checks).
add_test(NAME ai_client COMMAND ai_client_test)

add_executable(confidence_test tests/confidence_test.cpp)
target_link_libraries(confidence_test PRIVATE mpctrans)
add_test(NAME confidence COMMAND confidence_test)

if(WIN32)
    add_executable(suggestion_store_test tests/suggestion_store_test.cpp)
    target_link_libraries(suggestion_store_test PRIVATE mpctrans)
    # Writes to (and cleans up after itself in) the REAL per-machine %LOCALAPPDATA% store — there's no
    # LOCALAPPDATA override available (SHGetKnownFolderPath doesn't consult the env var), so this
    # can't be isolated from the real Studio.exe's file; it uses obviously-scoped "zztest" fixtures.
    add_test(NAME suggestion_store COMMAND suggestion_store_test)
endif()
