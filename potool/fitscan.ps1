<#
.SYNOPSIS
    Headless fit scan -- the entry point the mpc-hc-tests translations suite globs for
    (potool\fitscan.*, see that repo's suites/translations/README.md "Not here yet: the fit scan"
    and its Invoke-FitScan).

.DESCRIPTION
    Renders every IDD_* dialog parsed from an mpc-hc.rc off-screen (no MFC, no running Studio.exe,
    no neutral resource DLL) for each language's .po under -PoDir, and measures every translatable
    control against the SAME rules LivePreview::MeasureFit/MeasureComboFit use live in the Studio
    (studio/core/include/mpctrans/fit.h is the one source of truth both share). Writes a JSON report
    of every string that doesn't fit its control.

    This script is a thin forwarder to studio\cli\fitscan.exe: it resolves -RcDir/-Rc/-ResourceH into
    --rc/--resource-h, builds fitscan.exe via build_cli.ps1 if it isn't already built, forwards every
    other argument, and exits with the exe's own exit code.

    JSON SCHEMA (written to -Out):
        {
          "generated": "<ISO-8601 UTC timestamp>",
          "rc": "<path to mpc-hc.rc>",
          "po_dir": "<-PoDir>",
          "dpi": <int>,               # LOGPIXELSY of the scanning process's screen DC
          "languages": <int>,         # number of languages scanned
          "dialogs": <int>,           # number of IDD_* dialogs parsed from the RC
          "records": [
            {
              "lang": "de",
              "res": "dialogs" | "strings",   # "strings": a runtime-filled combo option (IDS_*)
              "dialog": "IDD_PPAGETHEME",     # RC dialog symbol
              "control": "IDC_...",           # control (or combo) symbol
              "msgctxt": "...", "msgid": "...", "msgstr": "...",
              "kind": "hard" | "tight",       # hard: rendered > available; tight: >= 90% of it
              "renderedPx": <int>, "availablePx": <int>, "overflowPx": <int>,
              "group": ["<peer msgctxt>", ...]   # present only for row-grouped controls
            }, ...
          ]
        }
    Records are sorted by language, then by overflowPx descending.

.PARAMETER RcDir
    An mpc-hc `src\mpc-hc` directory (mpc-hc.rc + resource.h live directly under it). Default:
    `<repo>\upstream\src\mpc-hc`. Ignored if both -Rc and -ResourceH are given explicitly.

.PARAMETER Rc
    Explicit path to mpc-hc.rc (overrides -RcDir).

.PARAMETER ResourceH
    Explicit path to resource.h (overrides -RcDir).

.PARAMETER PoDir
    Directory holding mpc-hc.<lang>.dialogs.po / .strings.po. Required.

.PARAMETER Out
    JSON report path. Default: fitscan.json (in the current directory).

.PARAMETER Lang
    One or more language codes to restrict the scan to (default: every language found under -PoDir).

.PARAMETER FailOnHard
    Forwarded to fitscan.exe --fail-on-hard: exit 2 if any "hard" (actually clipped) record exists.

.EXAMPLE
    pwsh potool\fitscan.ps1 -PoDir upstream\src\mpc-hc\mpcresources\PO -Lang pl -Out fitscan-pl.json

EXIT CODES (mirrors fitscan.exe): 0 the scan ran (records may or may not exist); 1 usage/IO error
(including this script's own build-tool-not-found case); 2 with -FailOnHard when a hard record exists.
#>
[CmdletBinding()]
param(
    [string]   $RcDir,
    [string]   $Rc,
    [string]   $ResourceH,
    [Parameter(Mandatory)]
    [string]   $PoDir,
    [string]   $Out = "fitscan.json",
    [string[]] $Lang,
    [switch]   $FailOnHard
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not $Rc -or -not $ResourceH) {
    if (-not $RcDir) { $RcDir = Join-Path $repo "upstream\src\mpc-hc" }
    if (-not $Rc)        { $Rc = Join-Path $RcDir "mpc-hc.rc" }
    if (-not $ResourceH) { $ResourceH = Join-Path $RcDir "resource.h" }
}
if (-not (Test-Path $Rc)) { Write-Error "mpc-hc.rc not found at $Rc (pass -RcDir or -Rc)"; exit 1 }
if (-not (Test-Path $ResourceH)) { Write-Error "resource.h not found at $ResourceH (pass -RcDir or -ResourceH)"; exit 1 }
if (-not (Test-Path $PoDir)) { Write-Error "-PoDir not found: $PoDir"; exit 1 }

$cliDir = Join-Path $PSScriptRoot "..\studio\cli"
$exe = Join-Path $cliDir "fitscan.exe"
if (-not (Test-Path $exe)) {
    Write-Host "fitscan.exe not found -- building it via build_cli.ps1 ..."
    $build = Join-Path $cliDir "build_cli.ps1"
    if (-not (Test-Path $build)) { Write-Error "build_cli.ps1 not found at $build"; exit 1 }
    & pwsh -NoProfile -File $build
    if ($LASTEXITCODE) { Write-Error "build_cli.ps1 failed ($LASTEXITCODE)"; exit 1 }
    if (-not (Test-Path $exe)) { Write-Error "fitscan.exe still missing after build_cli.ps1 ($exe)"; exit 1 }
}

$argsList = @("--rc", $Rc, "--resource-h", $ResourceH, "--po-dir", $PoDir, "--out", $Out)
if ($Lang) { $argsList += @("--lang", ($Lang -join ",")) }
if ($FailOnHard) { $argsList += "--fail-on-hard" }

& $exe @argsList
exit $LASTEXITCODE
