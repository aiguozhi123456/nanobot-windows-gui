---
title: "std::thread fails to compile when a project header shadows a UCRT header"
date: 2026-10-26
category: build-errors
module: nanobot-manager/src
problem_type: build_error
component: tooling
severity: medium
symptoms:
  - "MSVC <thread>(79) errors: C2039 \"_beginthreadex\" is not a member of the global namespace / C3861 identifier not found"
  - "The error appears only in translation units that instantiate std::thread, at the _Start template"
  - "The identical code compiles standalone (cl t.cpp) and in every other project"
root_cause: header_shadowing
resolution_type: code_change
related_components:
  - nanobot-manager/CMakeLists.txt
tags: [msvc, stl, thread, process-h, include-path-shadowing, /I, ucrt, build, windows]
---

# `std::thread` breaks when a project header shadows a UCRT header

## Context

The chat transport rewrite introduced the project's first `std::thread` usage. Compilation failed inside MSVC's own `<thread>` with `_beginthreadex` "not found" — as if `<process.h>` had never been included, even though the TU included it explicitly before `<thread>`.

## Root Cause

MSVC's `<thread>` internally does `#include <process.h>` (angle brackets) to reach `_beginthreadex`. Angle-bracket includes search **`/I` include directories before the system directories**. This project compiles with `/I src`, and `src/` contained a project header literally named `process.h` (the nanobot process-lifecycle module). So MSVC's `<thread>` resolved `<process.h>` to the *project's* header, which of course does not declare `_beginthreadex`.

The trap lay dormant for years because no translation unit used STL threading; the header name collision only matters when a standard header of the same name is pulled in transitively.

Minimal reproduction that isolates the mechanism (passes without `/I`, fails with `/I <dir-containing-process.h>`):

```cpp
#include <windows.h>
#include <thread>
int main() { std::thread t([]{}); t.join(); }
```

## Fix

Never name a project header after a UCRT/C standard header while its directory is on the include path. The header was renamed `src/process.h` → `src/nm_process.h` (the module's own API is `nm_state_*`/`nm_resolution_*`, so `nm_` follows its prefix), with the include guard updated and the three `#include "process.h"` sites (`main.cpp`, `process.c`, `tests/test_process.c`) moved over.

Explicitly including `<process.h>` before `<thread>` in the affected TU does **not** fix this — `<thread>`'s own internal `#include <process.h>` still resolves to the shadowing header.

## Verification

- `scripts\build.bat` completes (previously failed at the `std::thread` instantiation).
- `ctest -C Release` — all tests pass (`test_process` covers the renamed header's module).

## When to Apply

- Adding the first STL threading/synchronization header to a Win32 project with a flat `src/` include dir.
- Naming new headers: avoid `process.h`, `thread.h`, `time.h`, `string.h`, `stdlib.h`, `math.h`, `assert.h`, `locale.h`, `signal.h`, `setjmp.h`, `stddef.h`, `stdint.h`, `stdio.h`, `errno.h`, `fenv.h`, `float.h`, `inttypes.h`, `limits.h`, `wchar.h`, `wctype.h` — anything UCRT owns.
- Debugging "identifier not found" errors that point *inside* the STL/UCRT itself: suspect include-path shadowing before suspecting the toolchain.

## Related

- [ChatUi/ChatClient C++ class structure](../architecture-patterns/native-websocket-chat-integration.md) — the rewrite that surfaced this
- [Testing and CI setup](../infrastructure/testing-and-ci-setup.md)
