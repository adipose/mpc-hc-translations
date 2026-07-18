# SPDX-License-Identifier: GPL-3.0-or-later
#
# Local Studio-bundle build (the whole build runs on your machine -- no CI, no repo secret).
# Prereq: Visual Studio 2022+ with the "Desktop development with C++" workload AND the MFC/ATL
# component; Python with polib on PATH. Run from anywhere:
#     pwsh bundle-build/build_local.ps1
#
# Data freshness (new upstream strings, AI translations, LLM hints) is a SEPARATE step -- run
# `ANTHROPIC_API_KEY=... pwsh bundle-build/publish.ps1` first if you want the data refreshed. This
# script only COMPILES + ASSEMBLES the distributable bundle from whatever data is committed.
#
# Produces: translation-studio-bundle-<version>.zip  (Studio.exe + artifacts + lang/ + po/), where
# <version> is -Version if given (the build number, e.g. 0.21) else the upstream SHA8. The build
# number is stamped into data-manifest.json's `version` so the app can display it (window title).
param([string]$Version)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$dist = Join-Path $repo "dist"

Write-Host "== Enter VS developer shell (x64) =="
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ toolset was not found." }
Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
Set-Location $repo

Write-Host "== 1/4  control-index (deterministic, from the pinned upstream .rc) =="
python bundle-build/build_index.py `
  --rc upstream/src/mpc-hc/mpc-hc.rc --resource-h upstream/src/mpc-hc/resource.h `
  --mpcres upstream/src/mpc-hc/mpcresources --out dist/control-index.json
if ($LASTEXITCODE) { throw "build_index.py failed" }
if ($Version) { python bundle-build/make_manifest.py --version $Version }
else          { python bundle-build/make_manifest.py }

Write-Host "== 2/4  neutral resource DLL =="
& (Join-Path $repo "bundle-build\neutral-dll\build_neutral_dll.ps1")

Write-Host "== 3/4  Studio.exe (libmpctrans + MFC shell) =="
msbuild studio/Studio.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if ($LASTEXITCODE) { throw "msbuild failed" }

Write-Host "== 4/4  Assemble bundle + zip =="
$sha  = (& git -C (Join-Path $repo "upstream") rev-parse HEAD).Trim()
$sha8 = $sha.Substring(0, 8)
$bundle = Join-Path $repo "bundle"
Remove-Item -Recurse -Force $bundle -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $bundle "lang"), (Join-Path $bundle "po") | Out-Null
# Bundle::Locate expects the artifacts + lang/ + po/ beside Studio.exe.
Copy-Item (Join-Path $dist "control-index.json"), (Join-Path $dist "core-enrichment.sqlite"), `
          (Join-Path $dist "mpcresources.neutral.dll"), (Join-Path $dist "data-manifest.json") $bundle
Copy-Item (Join-Path $repo "studio\x64\Release\Studio.exe") $bundle
Copy-Item (Join-Path $repo "enrichment\lang\*.sqlite") (Join-Path $bundle "lang")
Copy-Item (Join-Path $repo "upstream\src\mpc-hc\mpcresources\PO\*.po") (Join-Path $bundle "po")
@{ upstream_sha = $sha; built = (Get-Date).ToUniversalTime().ToString("o") } |
    ConvertTo-Json | Set-Content (Join-Path $bundle "manifest.json") -Encoding utf8
$label = if ($Version) { $Version } else { $sha8 }
$zip = Join-Path $repo "translation-studio-bundle-$label.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $bundle "*") -DestinationPath $zip
Write-Host ""
Write-Host "DONE -> $zip  ($([math]::Round((Get-Item $zip).Length/1MB,1)) MB), build $label, upstream @ $sha8"
