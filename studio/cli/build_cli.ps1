# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds studio/cli/txsync_cli.exe -- the headless "refresh open Transifex-sync PR" tool -- against
# the already-built studio/x64/Release/libmpctrans.lib (build that first via studio/Studio.sln or
# bundle-build/build_local.ps1's step 3/4). No MFC, no CMake: a single `cl` invocation, same VS dev
# shell entry as bundle-build/build_local.ps1 lines 20-26.
#
#     pwsh studio/cli/build_cli.ps1
#
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$cli  = Join-Path $repo "studio\cli"
$lib  = Join-Path $repo "studio\x64\Release\libmpctrans.lib"
if (-not (Test-Path $lib)) { throw "libmpctrans.lib not found at $lib -- build studio/Studio.sln (Release|x64) first." }

Write-Host "== Enter VS developer shell (x64) =="
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ toolset was not found." }
Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

# Intermediates stay under studio/cli/obj -- never scattered into the repo root.
$obj = Join-Path $cli "obj"
New-Item -ItemType Directory -Force $obj | Out-Null

Write-Host "== Compiling txsync_cli.exe =="
# /MT (static CRT), not /MD -- libmpctrans.lib (studio/x64/Release) was built with the MFC project's
# default static-CRT Release setting; LNK2038 RuntimeLibrary mismatches result otherwise.
& cl /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
    /I (Join-Path $repo "studio\core\include") /I (Join-Path $repo "studio\core\thirdparty") `
    (Join-Path $cli "txsync_cli.cpp") `
    /Fo:"$obj\" `
    /Fe:(Join-Path $cli "txsync_cli.exe") `
    /link $lib `
    winhttp.lib shell32.lib ole32.lib bcrypt.lib crypt32.lib advapi32.lib
if ($LASTEXITCODE) { throw "cl failed ($LASTEXITCODE)" }

Write-Host ""
Write-Host "DONE -> $(Join-Path $cli 'txsync_cli.exe')"
