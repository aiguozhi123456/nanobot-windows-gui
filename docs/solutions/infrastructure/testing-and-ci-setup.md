---
title: "Unit-test framework, CTest wiring, and CI for a Win32 GUI app"
date: 2026-07-11
last_updated: 2026-08-01
category: infrastructure
module: nanobot-manager/tests
problem_type: tooling_setup
component: tooling
severity: medium
applies_when:
  - "Adding unit tests to a native Win32 C/C++ app that had no test suite"
  - "Wiring a self-contained C test framework into CMake + CTest"
  - "Setting up GitHub Actions CI for a Windows MSVC / CMake project with a git submodule and FetchContent deps"
  - "Testing a module that mutates the real Windows registry (HKCU) without leaving side effects"
  - "Testing a module that wraps Windows Credential Manager without touching the product credential"
tags: [unit-testing, ctest, cmake, github-actions, ci, msvc, win32, registry, credential-manager, testing]
---

# Unit-test framework, CTest wiring, and CI for a Win32 GUI app

## Context

`nanobot-manager` is a Win32 GUI app (`WinMain`, `/SUBSYSTEM:WINDOWS`) built with CMake + MSVC, the `core-ui` submodule (which pulls QuickJS-NG), and cJSON via `FetchContent`. It had **no** test suite — `AGENTS.md` stated "verification = build + manual run." Most of the code is side-effect-heavy (process spawn, WinHTTP, registry, UI), which is why it stayed untested.

This documents how a pragmatic unit-test layer + CTest + GitHub Actions CI were added: which modules are testable, the framework choice, the CMake wiring, the registry- and credential-safe test patterns, and real defects the effort surfaced.

## Guidance

### Pick the cleanly testable surface, then grow it

The suite started with two modules and has since grown to six test executables. The testability picture is now:

| Module | Testable? | Why |
|---|---|---|
| `config.c` | ✅ `test_config` | Pure JSON parse/validate/serialize once the core is extracted (see `architecture-patterns/config-system.md`). No I/O in the tested functions; also asserts the legacy secret is never serialized. |
| `autostart.c` | ✅ `test_autostart` | Three functions over `HKCU\...\Run`. Round-trip is testable against the real registry if you save/restore the original value. |
| `credential.c` | ✅ `test_credential` | Wraps Windows Credential Manager. Testable against the real Cred API using a **process-unique target name** so it never touches the product's `nanobot-manager/bootstrap-secret/v1`. |
| `process.c` | ✅ `test_process` | The pure-logic helpers (PID-reuse guards, `FILETIME` comparisons, port parsing, uptime math) are unit-testable; the network/spawn side is still exercised manually. (Originally deferred — the extractable helpers were carved out later.) |
| `chat_history.c` | ✅ `test_chat_history` | Local history persistence (append/trim/resume, size caps). File-format logic without the transport. |
| `chat_protocol.c` | ✅ `test_chat_protocol` | The WebSocket JSON frame protocol, decoded independently of WinHTTP. Compiles `chat_history.c` alongside it. |
| `chat.c` (transport) | ❌ no | WinHTTP WebSocket + connect/recv threads. The JSON frame-parsing logic now lives in `chat_protocol.c` (tested); the threaded transport remains manual. |
| `main.cpp` | ❌ no | `WinMain`, tray, UI callbacks, message loop. Integration-test territory, not unit. |

The ROI pattern: extract pure logic into a sibling file (`chat_protocol.c`, `chat_history.c`) and test *that*, leaving the I/O-heavy parent (`chat.c`) for manual verification. Six executables now, grown incrementally as logic was carved out — not a big-bang effort.

### Use a self-contained framework instead of vendoring one

The repo carries `tests/nm_test.h` — a ~70-line header providing `TEST_CHECK(cond)`, `TEST_MSG(...)`, a `TEST_LIST = { ... }` registration, and an auto-generated `main()` runner. No download, no vendored third-party file, no license notice to track.

The deliberate alternative was [acutest](https://github.com/mity/acutest) (single header, MIT, battle-tested). It was rejected for this project to keep the native repo dependency-free; `nm_test.h` covers every need here (assertions, latched failure without abort, summary, exit code). If signal/crash capture or TAP output is ever needed, swapping to acutest is a one-file change because the `TEST_LIST` API is compatible.

### The TEST_LIST macro must carry the type

The acutest-style usage is `TEST_LIST = { {name, fn}, ..., {NULL,NULL} };` at file scope. For that to compile, `TEST_LIST` must expand to a **typed** definition, not just a name:

```c
/* nm_test.h — the macro includes the type and array brackets */
#define TEST_LIST nm_test_entry nm_test_list[]
extern nm_test_entry nm_test_list[];
```

so `TEST_LIST = { ... };` becomes `nm_test_entry nm_test_list[] = { ... };`. The generated `main()` references `nm_test_list` directly (not via the macro), because in an expression context the macro would wrongly expand to a type. Getting this wrong produces a cascade of `C2040/C2078` "indirection levels differ / too many initializers" errors.

### CMake wiring: gate with BUILD_TESTING, compile module sources directly

`nanobot-manager/CMakeLists.txt`:

```cmake
option(BUILD_TESTING "Build nanobot-manager unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
```

`tests/CMakeLists.txt` builds each test as its own **console-subsystem** executable and compiles the module under test directly into it:

```cmake
function(nm_add_test name)
    add_executable(${name} ${name}.c ${ARGN})          # ARGN = module .c source(s)
    target_link_libraries(${name} PRIVATE nm_test)
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${name} PRIVATE /utf-8 /EHsc)
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE)  # console, not GUI
    add_test(NAME ${name} COMMAND ${name})
endfunction()

nm_add_test(test_config ${CMAKE_SOURCE_DIR}/src/config.c)
target_link_libraries(test_config PRIVATE cjson shell32 ole32 advapi32)

nm_add_test(test_credential ${CMAKE_SOURCE_DIR}/src/credential.c)
target_link_libraries(test_credential PRIVATE advapi32)

nm_add_test(test_process ${CMAKE_SOURCE_DIR}/src/process.c)
target_link_libraries(test_process PRIVATE cjson winhttp shell32 ole32 advapi32)

nm_add_test(test_autostart ${CMAKE_SOURCE_DIR}/src/autostart.c)
target_link_libraries(test_autostart PRIVATE advapi32)

nm_add_test(test_chat_history ${CMAKE_SOURCE_DIR}/src/chat_history.c)
target_link_libraries(test_chat_history PRIVATE cjson)

nm_add_test(test_chat_protocol ${CMAKE_SOURCE_DIR}/src/chat_protocol.c ${CMAKE_SOURCE_DIR}/src/chat_history.c)
target_link_libraries(test_chat_protocol PRIVATE cjson)
```

Two non-obvious points:

1. **Why compile `config.c` into the test, not link a library.** The main target is `add_executable(nanobot-manager WIN32 ...)` with `main.cpp`'s `WinMain`. There is no separate `nanobot-manager-lib` to link against. Compiling the single `.c` under test into each test exe avoids pulling in `WinMain` and the whole app, and keeps the test target self-contained.
2. **`WIN32_EXECUTABLE FALSE` is mandatory.** The default for this project is a GUI subsystem (no console). A test exe built as GUI would produce no capturable stdout, so CTest would see a silent "pass" even on failure. Console subsystem lets CTest capture the framework's output.

### CTest with the Visual Studio (multi-config) generator

The project builds with the VS generator (`scripts\build.bat` auto-detects VS year). Multi-config means the test binaries live under `build/tests/Release/`, so CTest must be told the config:

```bat
ctest --test-dir nanobot-manager\build -C Release --output-on-failure --no-tests=error
```

`--no-tests=error` turns "BUILD_TESTING got disabled and nothing ran" into a CI failure rather than a silent green.

### Testing the registry without side effects (autostart)

`autostart_enable/disable/is_enabled` operate on the real `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\nanobot-manager`. The test round-trips the real value but captures the original first and restores it at the end:

```c
reg_snapshot snap;
snapshot_capture(&snap);          // RegQueryValueExW → save bytes if present
autostart_disable();              // clean slate
TEST_CHECK(autostart_is_enabled() == 0);
autostart_enable("C:\\nm\\test.exe");
TEST_CHECK(autostart_is_enabled() == 1);
/* ...optionally query the value to assert it contains "--autostart"... */
autostart_disable();
TEST_CHECK(autostart_is_enabled() == 0);
snapshot_restore(&snap);          // RegSetValueExW back, or delete if originally absent
```

**Crucially, the test body never aborts on a failed `TEST_CHECK`.** `nm_test.h` latches the failure and continues, so `snapshot_restore` always runs even if an assertion failed mid-test. A framework that `assert()`-aborts on failure would leak the test value into the user's real Run key.

### Testing Credential Manager without touching the product secret (credential)

`test_credential` exercises the real `Cred*W` API, but against a **process-unique target name** (derived from PID / a counter) rather than the product target `nanobot-manager/bootstrap-secret/v1`. This mirrors the registry save/restore discipline: test the exact production code path, but never touch the real user-owned data. The test writes, reads back, and deletes its own throwaway credential on each run, and the non-aborting framework guarantees the delete runs even if an assertion fails.

The reason this matters over mocking: the `credential_store` write-verify-rollback logic (see [`credential-manager-secret-storage.md`](../architecture-patterns/credential-manager-secret-storage.md)) is the whole point — it can only be validated against the real Cred API. A mock would test nothing. Using a unique target name gives you the real-API fidelity without the risk of clobbering the user's actual bootstrap secret.

### CI: reuse scripts\build.bat, then ctest

The workflow (`.github/workflows/ci.yml`) does **not** reimplement the build. It calls the project's own `scripts\build.bat`, which is the documented build entrypoint and already handles: locating VS via `vswhere`, setting up `vcvars`, falling back to the CMake bundled with VS, picking the matching VS generator, and passing `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (cJSON's old `cmake_minimum_required`). CI then just runs CTest. This keeps CI faithful to local dev — if it builds on a developer's machine, it builds in CI.

```yaml
- uses: actions/checkout@v4
  with: { submodules: recursive, fetch-depth: 1 }
- uses: actions/cache@v4
  with: { path: nanobot-manager/build/_deps, key: "${{ runner.os }}-deps-${{ hashFiles('**/CMakeLists.txt') }}" }
- { name: Configure & build, shell: cmd, run: nanobot-manager\scripts\build.bat }
- { name: Test, shell: cmd, run: ctest --test-dir nanobot-manager\build -C Release --output-on-failure --no-tests=error }
```

`build/_deps` (cJSON + core-ui's QuickJS-NG) is the slow part of a cold configure, so it is cached keyed on the CMakeLists hash. `windows-latest` now ships VS 2026, which `build.bat` already supports (the `\\18\\` path fallback covers it). `fetch-depth: 1` is enough because `version.h` is derived from `NANOBOT_MANAGER_VERSION` in CMakeLists, not from `git describe`.

## Why This Matters

### Why a custom framework over acutest

For a native Windows repo that values self-containment, a 70-line header with no upstream dependency is easier to reason about than a vendored copy of a third-party file (tracking its version, license, updates). The test surface here is small (two modules), so the framework's job is small too. The escape hatch (swap to acutest) stays open via the compatible `TEST_LIST` API.

### Why compile module .c directly instead of a lib target

Extracting a `nanobot-manager-lib` static library just for tests would mean restructuring the main `add_executable(... WIN32 ...)` and maintaining two consumers of every source file. Compiling the one `.c` under test into each test exe is a one-line `ARGN` and keeps the production build untouched. The cost (compiling `config.c` twice) is negligible for files this small.

### Why save/restore instead of a fake registry hive

Loading a separate registry hive (`RegLoadAppKey` / a `.dat` file) is the "pure" approach but adds platform-specific setup and still exercises a different code path than production. Round-tripping the real `HKCU\...\Run` value with save/restore tests the exact production registry calls and is safe as long as cleanup is guaranteed (which the non-aborting framework ensures).

## Pitfalls encountered while standing up the tests

Two things worth recording because each is a class of mistake that recurs in C/Win32 work.

1. **Missing `<stdlib.h>` → 64-bit pointer truncation (silent corruption).** `tests/test_autostart.c` used `malloc`/`free` without including `<stdlib.h>`. MSVC implicitly declared `malloc` as returning `int`, so the `(BYTE*)malloc(size)` cast truncated the 64-bit heap pointer to 32 bits (compiler warning `C4312`: "conversion from 'int' to 'BYTE *' of greater size"). The test compiled and ran but would dereference a garbage address. Fix: `#include <stdlib.h>`. **Lesson:** in C (not C++), every standard function needs its header; missing `malloc`/`free`/`strlen` declarations are a classic 64-bit footgun. Treat `C4312` on a `malloc` cast as a missing-header bug, not noise.

2. **`autostart_disable()` is not idempotent.** It returns `-1` when the Run value is already absent (`RegDeleteValueW` fails with `ERROR_FILE_NOT_FOUND`). An initial test assumed idempotency (`disable() == 0` after the value was gone) and failed. The test now documents the real semantic (`== -1` when already absent, `is_enabled()` stays 0). **Lesson:** when writing the first tests for legacy code, assert observed behavior first; "should-be-idempotent" is a separate product decision, not something to silently smuggle in via a test.

## When to Apply

- **Stand up the first test suite for a side-effect-heavy native app** — start with the one or two pure-logic modules; don't boil the ocean on network/process code.
- **Add a new test executable** — copy the `nm_add_test(name src.c)` + `target_link_libraries` + `add_test` pattern; keep it console subsystem.
- **Add a module that touches `HKCU`** — reuse the `snapshot_capture` / `snapshot_restore` save-and-restore pattern and confirm the framework does not abort on failure.
- **Add a CI step that must match local builds** — call the project's own build script instead of re-expressing it in YAML; divergence between local and CI build commands is a recurring source of "works on my machine."

## Examples

### nm_test.h essentials

```c
#define TEST_CHECK(cond) do { if (!(cond)) { nm_check_failed = 1; \
    printf("  [FAIL] %s:%d: TEST_CHECK(%s)\n", __FILE__, __LINE__, #cond); } } while (0)
#define TEST_MSG(...) do { printf("         " __VA_ARGS__); putchar('\n'); } while (0)
#define TEST_LIST nm_test_entry nm_test_list[]
extern nm_test_entry nm_test_list[];
/* main() iterates nm_test_list, runs each, returns non-zero on any failure */
```

### Run the suite locally

```bat
cd nanobot-manager
scripts\build.bat
ctest --test-dir build -C Release --output-on-failure
```

Current suite: `test_config`, `test_credential`, `test_process`, `test_autostart`, `test_chat_history`, `test_chat_protocol` — six console executables under `tests/`, all wired through `nm_add_test`. Runs in well under a second.

## Related

- `nanobot-manager/tests/nm_test.h` — the self-contained framework.
- `nanobot-manager/tests/test_config.c` / `test_credential.c` / `test_process.c` / `test_autostart.c` / `test_chat_history.c` / `test_chat_protocol.c` — the test cases.
- `nanobot-manager/tests/CMakeLists.txt` — `nm_add_test` + per-test wiring (six executables).
- `nanobot-manager/CMakeLists.txt` — `BUILD_TESTING` gate + `enable_testing()`.
- `.github/workflows/ci.yml` — the CI workflow.
- `docs/solutions/architecture-patterns/config-system.md` — the config testability refactor that made `test_config` possible.
- `docs/solutions/architecture-patterns/credential-manager-secret-storage.md` — the credential write-verify-rollback logic that `test_credential` validates against a process-unique target.
- `docs/solutions/build-errors/vs2026-bundled-cmake-and-cmake4-policy.md` — the build toolchain (VS 2026 / bundled CMake / CMake 4.x policy) that CI reuses via `scripts\build.bat`.
- `AGENTS.md` — build commands and the verification story.
