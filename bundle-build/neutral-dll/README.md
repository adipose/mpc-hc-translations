# neutral-dll/ — build the neutral (English) resource DLL

The Studio renders live dialogs/menus by instantiating templates from a **resource-only DLL**
built from the pinned upstream English `mpc-hc.rc`. This is a **Windows/MSVC** step (runs in
`bundle-build.yml` on `windows-latest`); it is NOT runnable on Linux/WSL.

**Do not modify the submodule.** Build from `upstream/` in-place via one of:

## Option A — resource-only vcxproj (preferred)
A small `mpcresources-neutral.vcxproj` here that:
- compiles `../../upstream/src/mpc-hc/mpc-hc.rc`
- with include dirs: `../../upstream/src/mpc-hc` (for `resource.h`, `res/…`) + the Windows/MFC SDK
- links `/DLL /NOENTRY /MACHINE:x64` → `dist/mpcresources.neutral.dll`
- Configuration mirrors the existing `upstream/src/mpc-hc/mpcresources/mpcresources.vcxproj`
  (which already builds per-language resource DLLs) but points at the English RC.

## Option B — direct rc.exe + link (thin wrapper)
```powershell
# build_neutral_dll.ps1  (sketch — wire real SDK paths in CI)
$rc  = "mpc-hc.rc"; $inc = "..\..\upstream\src\mpc-hc"
rc.exe /nologo /fo dist\neutral.res /i $inc $inc\mpc-hc.rc
link.exe /DLL /NOENTRY /MACHINE:X64 /OUT:dist\mpcresources.neutral.dll dist\neutral.res
```

## Notes
- Only dialog + menu templates are needed; bitmaps/icons are irrelevant to translation (ok if included).
- `mpc-hc.rc` `#include`s `resource.h`, `afxres.h`, `res/untranslatable.rc2`, `res/mpc-hc.rc2` — all present in the submodule.
- Output `dist/mpcresources.neutral.dll` is a bundle artifact (gitignored); packaged into the Release.

**Status: DONE (Option B)** — `build_neutral_dll.ps1` (run from a VS Developer shell). Needs
`/i upstream\include` (version.h) and `/d NO_VERSION_REV_NEEDED` (skips generated version_rev.h,
same define upstream's mpcresources projects use). Verified by the `neutral_dll` ctest gate:
every control-index record resolves against the compiled templates (19/19 captions, 469/469
controls). Remaining: un-stub the `bundle-build.yml` step.
