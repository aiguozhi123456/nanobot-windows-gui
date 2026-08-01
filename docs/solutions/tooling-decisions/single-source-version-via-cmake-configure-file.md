---
title: "Single-source version via CMake configure_file (version.h + version.nsh from one CMake var)"
date: 2026-08-01
category: tooling-decisions
module: nanobot-manager/build
problem_type: tooling_decision
component: tooling
severity: medium
related_components:
  - nanobot-manager/main
  - nanobot-manager/installer
applies_when:
  - "Embedding the app version in more than one place (C/C++ code, NSIS installer, UI footer) and wanting to bump it once"
  - "Wondering why version.h / version.nsh are not in the repo, or why a fresh checkout fails to compile with a missing version.h"
  - "Setting CI checkout depth and deciding whether git describe / git fetch --unshallow is needed for versioning"
  - "Choosing between configure_file, git describe, and a hand-edited header for version sourcing"
tags: [versioning, cmake, configure-file, version-h, version-nsh, nsis, single-source, build, c, cpp]
---

# Single-source version via CMake configure_file (version.h + version.nsh from one CMake var)

## Context

The manager's version string is consumed in three places that must never disagree:

1. **The C/C++ binary** — `main.cpp` shows it in the dashboard footer (`NANOBOT_MANAGER_VERSION` from `version.h`).
2. **The NSIS installer** — `installer/installer.nsi` uses `PRODUCT_VERSION` (from `version.nsh`) for the installer's version metadata, Add/Remove entry, and `DisplayVersion`.
3. **The build configuration** — CMake project version, cached for downstream tooling.

The trap this avoids: defining the version independently in each place. The moment someone bumps the footer but forgets the installer, the installer reports a different version than the app it installs — a classic release-artifact inconsistency that's invisible until a user files "I installed 1.4 but the app says 1.3."

The decision: define `NANOBOT_MANAGER_VERSION` exactly once in `CMakeLists.txt`, and `configure_file` it into both `version.h` (for C/C++) and `version.nsh` (for NSIS) at configure time.

## Guidance

### One CMake cache variable is the single source

```cmake
/* nanobot-manager/CMakeLists.txt */
set(NANOBOT_MANAGER_VERSION "1.5.0" CACHE STRING "Project version" FORCE)
```

`CACHE STRING ... FORCE` means it is visible to downstream consumers and re-set on every configure (so a stale cache value from a previous version doesn't survive a bump). This is the **only** place the literal version string appears in the repo. To release a new version, change this one line and reconfigure.

### `configure_file` into both consumers, from `.in` templates

Two templates live in the source tree, each with an `@NANOBOT_MANAGER_VERSION@` substitution token:

```c
/* nanobot-manager/src/version.h.in */
#ifndef VERSION_H
#define VERSION_H
#define NANOBOT_MANAGER_VERSION "@NANOBOT_MANAGER_VERSION@"
#endif
```
```nsis
/* nanobot-manager/installer/version.nsh.in */
!define PRODUCT_VERSION "@NANOBOT_MANAGER_VERSION@"
```

CMake substitutes the token and writes the result into the **build** tree (never the source tree):

```cmake
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/installer/version.nsh.in
    ${CMAKE_BINARY_DIR}/generated/version.nsh
    @ONLY
)
```

`@ONLY` restricts substitution to `@VAR@` tokens (not `${VAR}`), so the NSIS template's own `${...}` and `!define` syntax is left untouched. The build-tree output dir (`${CMAKE_BINARY_DIR}/generated/`) is added to the target's include path:

```cmake
target_include_directories(nanobot-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated     /* ← version.h resolves here */
    ...
)
```

So `#include "version.h"` in `main.cpp` finds the generated header.

### Why not `git describe`

`git describe --tags` is a common alternative (derive version from the nearest tag). It was not used here, for two reasons:

1. **It couples the version to git history depth.** CI would need `fetch-depth: 0` (or `--unshallow`) to see tags, slowing every CI run. With the CMake-source approach, `fetch-depth: 1` is sufficient — the version is a literal in a tracked file, not derived from history.
2. **It produces non-deterministic local builds.** Two checkouts of the same commit can report different `git describe` output depending on tag availability. The CMake literal is identical everywhere.

### Why `FORCE` and `CACHE` together

`CACHE STRING` makes the value inspectable (`cmake -L`), and `FORCE` re-applies it on every configure so bumping the literal in `CMakeLists.txt` actually takes effect even when a stale cached value exists in `build/CMakeCache.txt`. Without `FORCE`, a developer who bumps the version and re-runs configure without deleting the cache can see the *old* version baked in — a silent, confusing failure. `FORCE` trades a tiny bit of "users can override the cache" flexibility for "the source of truth always wins," which is the right tradeoff for a version string.

## Why This Matters

- **Three-way disagreement is the failure mode this prevents.** Installer-version ≠ app-version is the canonical "which version am I actually running?" bug, and it's invisible until after release. One source eliminates it by construction.
- **`fetch-depth: 1` in CI is the downstream payoff.** Because version is a literal, not derived from tags, CI is fast and works on shallow checkouts. Switching to `git describe` would silently require deepening the checkout.
- **The generated header is the source of the "fresh checkout won't compile" gotcha.** `version.h` does not exist in the repo — it is generated at configure time into `build/generated/`. A workflow that tries to compile without configuring first (e.g. opening `main.cpp` in an IDE before running CMake) fails with a missing-`version.h` error, which is correct but surprising if you don't know the file is generated. The fix is always "run `scripts\build.bat` (or cmake configure) first," not "create the file by hand."
- **Bump discipline is one-site.** A release becomes: edit one `set(...)` line, commit, rebuild. There is no "did I remember to update the installer?" checklist item, because there is nothing to remember.

## When to Apply

- **Any project whose version appears in 2+ artifacts** (binary, installer, about dialog, manifest, package metadata) — drive them all from one `configure_file` source. The cost is two tiny `.in` templates; the benefit is permanent version consistency.
- **Choosing a versioning mechanism for a CMake-built app** — prefer a literal-in-CMakeLists + `configure_file` over `git describe` unless you specifically need per-commit versioning (release engineering, nightly snapshots). The literal approach is simpler, deterministic, and shallow-checkout-friendly.
- **Reviewing a PR that bumps the version** — confirm it touched exactly one line (`set(NANOBOT_MANAGER_VERSION ...)`). A PR that edits `version.h` or `version.nsh` directly is editing generated files and will be overwritten on the next configure.
- **Seeing a "missing version.h" compile error** — run CMake configure first; the file is generated, not tracked.

## Examples

### The full pipeline

```cmake
# 1. single source
set(NANOBOT_MANAGER_VERSION "1.5.0" CACHE STRING "Project version" FORCE)

# 2. generate both consumers
configure_file(src/version.h.in        ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
configure_file(installer/version.nsh.in ${CMAKE_BINARY_DIR}/generated/version.nsh @ONLY)

# 3. make the generated header findable
target_include_directories(nanobot-manager PRIVATE ${CMAKE_BINARY_DIR}/generated)
```

```c
// 4. consume in C++
#include "version.h"
ui_page_set_text(g_page, "footer_version",
    /* builds the "v1.5.0" string from NANOBOT_MANAGER_VERSION */);
```

### Releasing a new version

```diff
- set(NANOBOT_MANAGER_VERSION "1.5.0" CACHE STRING "Project version" FORCE)
+ set(NANOBOT_MANAGER_VERSION "1.6.0" CACHE STRING "Project version" FORCE)
```

Then `scripts\build.bat` (regenerates `version.h`) and `scripts\package.bat` (regenerates `version.nsh` and builds the installer). Both now report `1.6.0`. No other edit.

## Related

- `docs/solutions/infrastructure/testing-and-ci-setup.md` — CI's `fetch-depth: 1` is justified by this decision (version is a literal, not from `git describe`); the build toolchain doc covers the VS/CMake detection that wraps this.
- `docs/solutions/build-errors/vs2026-bundled-cmake-and-cmake4-policy.md` — the build toolchain that runs this configure step.
- `nanobot-manager/CMakeLists.txt` — the single `set(...)` line and both `configure_file` calls.
- `nanobot-manager/src/version.h.in`, `nanobot-manager/installer/version.nsh.in` — the two templates.
- `nanobot-manager/src/main.cpp` — `#include "version.h"` and the footer rendering.
- `nanobot-manager/installer/installer.nsi` — `!include generated/version.nsh` and `PRODUCT_VERSION` usage.
- Commit `662952e` — the original refactor (`refactor: single-source version number via CMake configure_file`).
