---
title: "Building on VS 2026, VS-bundled CMake, and CMake 4.x (cJSON policy compatibility)"
date: 2026-08-01
category: build-errors
module: nanobot-manager/build
problem_type: build_error
component: tooling
severity: high
symptoms:
  - "CMake configure fails on a fresh VS 2026 install with 'Generator Visual Studio 17 2022' errors or a missing-toolset message"
  - "'cmake' is not recognized on a machine that installed C++ only via the VS installer (no standalone CMake on PATH)"
  - "CMake 4.x aborts at configure with: Compatibility with CMake < 3.5 has been removed from CMake"
  - "The same clone builds fine on a VS 2022 machine but fails on a brand-new dev box"
root_cause: incomplete_setup
resolution_type: environment_setup
related_components:
  - nanobot-manager/installer
tags: [cmake, visual-studio, vs2026, vswhere, cmake4, cmake-policy-version-minimum, fetchcontent, cjson, build, windows]
---

# Building on VS 2026, VS-bundled CMake, and CMake 4.x (cJSON policy compatibility)

## Problem

`scripts\build.bat` historically hard-coded `-G "Visual Studio 17 2022"` and assumed `cmake` was on `PATH`. On a new developer machine that installed C++ **only** through the Visual Studio 2026 installer (no standalone CMake, newest VS year), the build broke in three independent ways at once — none of them intuitive, and the first two are not even about this project's own code.

## Symptoms

- **VS 2026 generator missing:** `cmake -G "Visual Studio 17 2022"` either fails outright or warns about a missing toolset, because the 2022 generator may not be installed on a 2026-only box.
- **No `cmake` on PATH:** `'cmake' is not recognized as an internal or external command` — the VS installer's CMake lives under the VS tree, not on the system PATH.
- **CMake 4.x policy error:** on CMake 4.x, configure aborts with `Compatibility with CMake < 3.5 has been removed from CMake.` — and the offending `cmake_minimum_required(VERSION 3.0)` is **not in this project**, it is in cJSON, pulled via `FetchContent`.

## What Didn't Work

- **Bumping this project's own `cmake_minimum_required`.** The project already declares `cmake_minimum_required(VERSION 3.20)`. The policy error points at *cJSON's* `CMakeLists.txt` (`cmake_minimum_required(VERSION 3.0)`), which `FetchContent_Declare(cjson ...)` pulls at configure time. You cannot fix a transitive dependency's `cmake_minimum_required` without forking it.
- **Telling devs to install standalone CMake.** Works, but pushes a per-machine setup step that the VS installer already solved — VS ships CMake, just not on PATH.
- **Pinning to the 2022 generator.** Breaks the moment the dev only has VS 2026 installed, which is increasingly the default on `windows-latest` and new machines.

## Solution

All three land in `scripts\build.bat` (single file — `08dced1`). They are independent fixes that happen to co-occur on a fresh VS 2026 box.

### 1. Detect the VS generator from `vswhere`

Don't hard-code the generator. Ask `vswhere` for the installed VS *year* and map it:

```bat
set "CMAKE_GENERATOR=Visual Studio 17 2022"           rem default
if exist "%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe" (
    for /f "usebackq tokens=*" %%i in (
        `"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property catalog_productLineVersion`
    ) do set "VS_YEAR=%%i"
    if defined VS_YEAR (
        if "!VS_YEAR!"=="2026" set "CMAKE_GENERATOR=Visual Studio 18 2026"
        if "!VS_YEAR!"=="2022" set "CMAKE_GENERATOR=Visual Studio 17 2022"
        if "!VS_YEAR!"=="2019" set "CMAKE_GENERATOR=Visual Studio 16 2019"
    )
    rem Fallback: VS 18 path layout even without catalog year
    echo !VS_PATH! | findstr /I "\18\" >nul && set "CMAKE_GENERATOR=Visual Studio 18 2026"
)
```

Two non-obvious bits:

- **`catalog_productLineVersion`** (not `installationVersion`) is what reports the marketing year (`2026`/`2022`/`2019`). `installationVersion` returns the internal `17.x`/`18.x` number and would need a different mapping table.
- **The `\18\` path-layout fallback** covers VS 2026 builds whose `vswhere` doesn't report a catalog year (preview/odd installs). The VS 2026 install path contains `\18\` where VS 2022 contained `\17\`.

When VS 2028 ships, this table needs one more row (`set "CMAKE_GENERATOR=Visual Studio 19 2028"`) and a matching `\19\` fallback — treat it as a maintenance table, not a one-off.

### 2. Fall back to the VS-bundled CMake

Prefer `cmake` on PATH, but if `where cmake` fails, use the one VS installed:

```bat
set "CMAKE_EXE=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
    set "CMAKE_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist "!CMAKE_EXE!" (
        echo ERROR: cmake not found. Install CMake or the Visual Studio C++ CMake tools.
        exit /b 1
    )
)
```

The VS-bundled CMake path is stable across VS versions as long as `%VS_PATH%` (resolved from `vswhere -property installationPath`) is used as the base. This is the same `%VS_PATH%` already used to call `vcvarsall.bat`.

### 3. `CMAKE_POLICY_VERSION_MINIMUM=3.5` for cJSON

This is the load-bearing one. CMake 4.x removed compatibility for `cmake_minimum_required < 3.5`. cJSON v1.7.18 declares `cmake_minimum_required(VERSION 3.0)`, so its configure step (triggered by `FetchContent_MakeAvailable`) aborts under CMake 4.x. The fix is a single flag passed to *our* configure:

```bat
"!CMAKE_EXE!" -B %BUILD_DIR% -G "!CMAKE_GENERATOR!" -A x64 ^
    -DCMAKE_PREFIX_PATH="%VS_PATH%" ^
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

`CMAKE_POLICY_VERSION_MINIMUM` tells CMake "treat the policy baseline as if it were at least this version," so cJSON's `cmake_minimum_required(3.0)` no longer triggers the 4.x incompatibility abort. It applies project-wide (including `FetchContent` deps), which is exactly what we want here. It does **not** require modifying cJSON.

> The same flag is documented in `AGENTS.md`, the README Testing section, and the CI workflow — keep all four in sync if you ever change the floor.

## Why This Works

- **Generator detection removes the year assumption.** VS generators are version-coupled: the `Visual Studio 17 2022` generator needs the VS 2022 toolset, `Visual Studio 18 2026` needs VS 2026. Detecting from `vswhere` picks whichever is actually installed.
- **Bundled-CMake fallback matches how VS installs C++.** The "C++ CMake tools for Windows" VS component installs CMake under the VS tree but deliberately does not modify PATH. Probing `where cmake` then falling back to the VS path mirrors the installer's intent.
- **`CMAKE_POLICY_VERSION_MINIMUM` is the CMake-blessed escape hatch** for "a dependency I don't own declares too old a minimum." It is safer than `cmake_policy(SET CMP0xxx OLD)` per-policy, because it lifts the whole baseline rather than cherry-picking.

## Prevention

- **CI uses `windows-latest`** (currently VS 2026), so a generator/CMake regression on a new VS year fails CI before it reaches a developer. Don't pin CI to an older windows image to mask this — the whole point is that the build must track the newest VS.
- **Don't remove the `\18\` path-layout fallback** even when `catalog_productLineVersion` works on your machine — it exists for installs where it doesn't.
- **When bumping cJSON's `GIT_TAG`**, re-test that the new tag still configures under `CMAKE_POLICY_VERSION_MINIMUM=3.5` (newer cJSON may raise its own minimum; if it ever reaches 3.5, the flag becomes a no-op but is harmless).
- **When VS 2028 ships**, add the generator row + `\19\` fallback in one commit and verify on a 2028 image. The pattern is documented so this is a known maintenance task, not a surprise.

## Related Issues

- `docs/solutions/infrastructure/testing-and-ci-setup.md` — CI reuses `scripts\build.bat` rather than reimplementing the build, so these fixes flow to CI for free; also documents `fetch-depth: 1` is fine because version comes from CMake, not `git describe`.
- `nanobot-manager/scripts/build.bat` — the single file holding all three fixes.
- `nanobot-manager/CMakeLists.txt` — `FetchContent_Declare(cjson GIT_TAG v1.7.18)`; the source of the transitive `cmake_minimum_required(3.0)`.
- Commit `08dced1` — the original fix.
