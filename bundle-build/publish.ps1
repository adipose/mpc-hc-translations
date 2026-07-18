# SPDX-License-Identifier: GPL-3.0-or-later
#
# Gated release: ONLY if the upstream string files (mpc-hc.rc / resource.h / PO/) changed since our
# pin does this re-pin the submodule, AI-fill the new strings, refresh the data, commit it, and build
# the bundle. If nothing string-related changed, it does nothing (no tokens, no build) -- unless -Force.
# The whole thing runs locally and the AI-fill goes through the Claude Code CLI (your subscription) --
# so NO API key is needed anywhere (not in the repo, not in your env). Requires `claude` installed + a
# logged-in session. (Pass -Backend api to use the Anthropic API with $env:ANTHROPIC_API_KEY instead.)
#
#   pwsh bundle-build/publish.ps1            # gated: fill + build only if RC/PO changed
#   pwsh bundle-build/publish.ps1 -Force     # build even with no string change
#
# New upstream strings are added to core-enrichment (sync_core_strings.py), given a Meaning via the
# model (generate_meanings.py), then hinted and translated -- all at build time, no lab DB required.
# THIS is the build command -- it ends by pushing and publishing a GitHub Release. (The private lab DB,
# when re-run, just yields richer Meanings; build_local.ps1 is the compile-only step this calls.)
param([switch]$Force, [double]$Threshold = 85, [string]$Model = "claude-sonnet-5",
      [ValidateSet("claude-code", "api")][string]$Backend = "claude-code")
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repo
if ($Backend -eq "claude-code") {
    if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
        throw "Claude Code CLI not found -- install it and log in, or use -Backend api with `$env:ANTHROPIC_API_KEY."
    }
} elseif (-not $env:ANTHROPIC_API_KEY) {
    throw "-Backend api needs `$env:ANTHROPIC_API_KEY set."
}

Write-Host "== Detect upstream string changes =="
git -C upstream fetch --quiet origin develop
$pin = (git -C upstream rev-parse HEAD).Trim()
$changed = git -C upstream diff --name-only $pin FETCH_HEAD -- `
    src/mpc-hc/mpc-hc.rc src/mpc-hc/resource.h src/mpc-hc/mpcresources/PO
if (-not $changed -and -not $Force) {
    Write-Host "No RC/PO changes since $($pin.Substring(0,8)) -- nothing to fill or build. (-Force to build anyway)"
    exit 0
}
if ($changed) {
    Write-Host "String files changed upstream -- re-pinning to develop HEAD:"
    $changed | ForEach-Object { "    $_" }
    git -C upstream checkout --quiet --detach FETCH_HEAD
}

# Next build number 0.1, 0.2, ... -- computed UP FRONT so it can be stamped into the manifest + bundle
# + zip name + the release tag (one coherent version everywhere). git tags can't start with a literal
# dot, so ".N" is tagged "0.N". Every build gets its OWN release; we never overwrite a tag.
$minors = @(gh release list --json tagName --jq '.[].tagName' 2>$null |
            Select-String -Pattern '^0\.(\d+)$' | ForEach-Object { [int]$_.Matches.Groups[1].Value })
$tag = "0." + ((($minors | Measure-Object -Maximum).Maximum) + 1)
Write-Host "== This build is $tag =="

Write-Host "== Data refresh (control-index, AI-fill, hints, coverage, manifest) via $Backend =="
python -m pip install --quiet polib
if ($Backend -eq "api") { python -m pip install --quiet anthropic }
python bundle-build/build_index.py `
    --rc upstream/src/mpc-hc/mpc-hc.rc --resource-h upstream/src/mpc-hc/resource.h `
    --mpcres upstream/src/mpc-hc/mpcresources --out dist/control-index.json
if ($LASTEXITCODE) { throw "build_index failed" }
python bundle-build/sync_core_strings.py    # add any new upstream strings to core-enrichment (empty Meaning)
python bundle-build/generate_meanings.py --model $Model --backend $Backend    # fill Meanings for new strings
python bundle-build/generate_hints.py --db dist/core-enrichment.sqlite --only-missing --llm --model $Model --backend $Backend
python bundle-build/ai_fill.py --threshold $Threshold --model $Model --backend $Backend
if ($LASTEXITCODE) { throw "ai_fill failed" }
python bundle-build/report_ai_coverage.py
python bundle-build/make_manifest.py --version $tag

Write-Host "== Commit the refreshed data =="
git add upstream enrichment dist/core-enrichment.sqlite dist/control-index.json dist/data-manifest.json
git diff --cached --quiet
if ($LASTEXITCODE -ne 0) {
    $up = (git -C upstream rev-parse --short HEAD).Trim()
    git commit -q -m "publish: AI-fill (>=$Threshold%) + refresh @ upstream $up

Re-pinned upstream (string files changed), regenerated control-index, AI-filled new empty cells for
near-complete languages via $Model, filled missing hints, restamped coverage + data-manifest.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
    Write-Host "   committed @ upstream $up"
} else {
    Write-Host "   no data changes to commit"
}

Write-Host "== Build the bundle $tag =="
& (Join-Path $repo "bundle-build\build_local.ps1") -Version $tag
# build_local regenerates control-index + manifest for the bundle; keep the tree matching the commit.
git checkout -- dist/control-index.json dist/data-manifest.json 2>$null

Write-Host "== Push + publish the GitHub Release =="
git push origin main
$up  = (git -C upstream rev-parse --short HEAD).Trim()
$our = (git rev-parse --short HEAD).Trim()
$zip = Get-ChildItem (Join-Path $repo "translation-studio-bundle-$tag.zip")
gh release create $tag $zip.FullName --title "Studio bundle $tag (upstream $up)" --latest `
    --notes "Local build $tag. Our commit $our, upstream $up. AI suggestions via $Backend (>=$Threshold% languages)."
Write-Host "`nPublished release $tag with $($zip.Name)."
