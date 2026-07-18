# SPDX-License-Identifier: GPL-3.0-or-later
# Build the neutral (English) resource-only DLL from the pinned upstream mpc-hc.rc.
# Option B from README.md: rc.exe -> link /DLL /NOENTRY. Run from a VS Developer shell
# (rc.exe/link.exe on PATH, INCLUDE set incl. atlmfc\include for afxres.h), from any cwd.
# Output: <repo>/dist/mpcresources.neutral.dll (bundle artifact, gitignored).
$ErrorActionPreference = "Stop"

$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$src  = Join-Path $repo "upstream\src\mpc-hc"
$dist = Join-Path $repo "dist"
if (-not (Test-Path (Join-Path $src "mpc-hc.rc"))) {
    throw "upstream submodule not checked out: $src (run: git submodule update --init --depth 1)"
}
foreach ($tool in "rc.exe", "link.exe") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not on PATH - run from a VS Developer shell (vcvars64.bat)"
    }
}
New-Item -ItemType Directory -Force $dist | Out-Null

$res = Join-Path $dist "neutral.res"
$dll = Join-Path $dist "mpcresources.neutral.dll"

# /i $src for resource.h + res\*.rc2; upstream\include for version.h (pulled in by
# mpc-hc.rc2); SDK/MFC headers come from the dev shell's INCLUDE.
$inc = Join-Path $repo "upstream\include"
# NO_VERSION_REV_NEEDED: skip generated build/version_rev.h (same define upstream's
# mpcresources projects use for their per-language resource DLLs).
rc.exe /nologo /fo $res /i $src /i $inc /d NO_VERSION_REV_NEEDED (Join-Path $src "mpc-hc.rc")
if ($LASTEXITCODE) { throw "rc.exe failed ($LASTEXITCODE)" }
link.exe /NOLOGO /DLL /NOENTRY /MACHINE:X64 "/OUT:$dll" $res
if ($LASTEXITCODE) { throw "link.exe failed ($LASTEXITCODE)" }

Remove-Item $res
"wrote $dll ($((Get-Item $dll).Length.ToString('N0')) bytes)"
