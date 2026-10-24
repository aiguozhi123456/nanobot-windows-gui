---
title: "Logging pipeline: one module, levels, and file-redirection child capture"
date: 2026-10-24
last_updated: 2026-10-24
category: architecture-patterns
module: nanobot-manager/logger
problem_type: architecture_pattern
component: tooling
severity: low
applies_when:
  - "Adding log lines anywhere in the manager (use nm_log, not local helpers)"
  - "Capturing nanobot.exe gateway stdout/stderr for startup-failure diagnosis"
  - "Changing log verbosity at runtime (config.json log_level / settings page)"
  - "Debugging why a gateway start failed (read nanobot-gateway.log)"
tags: [logging, logger, nm_log, log_level, child-process, stdio-redirection, rotation, thread-safety, win32, c]
---

# Logging pipeline: one module, levels, and file-redirection child capture

## Context

Before this pipeline the manager had two copy-pasted static helpers —
`autostart_log()` in `main.cpp` (8 call sites) and `process_log()` in
`process.c` (45 call sites). Each helper reopened its file for every single
line, neither had a level system or a module tag, neither was thread-safe
(worker threads and thread-pool callbacks interleave writes), and only the
autostart file had size rotation. Worse, `nanobot_start()` spawned the
gateway launcher with `bInheritHandles=FALSE` and no std handles, so
nanobot's own output — import errors, dependency tracebacks, the single most
valuable signal when a start fails — was silently discarded.

The unified pipeline (all verified: MSVC build, CTest 8/8 including the new
suite, manual run confirming file creation, line format, and config-driven
level switching) replaced both helpers with one module (`src/logger.c` /
`src/logger.h`), gave every line a level and module, made writes
thread-safe and crash-proof, and redirected the child's stdout/stderr into a
log file beside the exe.

## Guidance

### One module, one file

`src/logger.c` is the single sink. `nm_log(level, module, fmt, ...)` writes

```
[YYYY-MM-DD HH:MM:SS] [LEVEL] [module] message
```

lines to `nanobot-manager.log` beside the exe (UTF-8, no BOM). Key
properties:

- A static-init `SRWLOCK` (`SRWLOCK_INIT`, no constructor needed — same
  pattern as the probe cache in `process.c`) guards a stay-open `FILE*`.
  No per-line open/close.
- Every line is `fflush`-ed, so even a hard exit loses nothing already
  emitted.
- Level filtering happens before formatting; the filter is re-checked under
  the exclusive lock so a concurrent `logger_set_level` can't race the write.
- On any file failure the logger degrades to a **no-op**: `nm_log` before
  `logger_init`, after `logger_shutdown`, or with an unopenable path simply
  returns. Logging must never block or crash the app.
- Rotation keeps the old autostart policy: above 512 KiB (overridable via
  `logger_set_max_bytes`, mainly a test hook) the file is renamed to `.bak`
  (`MoveFileExW`, `MOVEFILE_REPLACE_EXISTING`) and reopened fresh — one
  generation only. `logger_init` resumes the existing file's byte count so
  the threshold survives restarts.
- The old files (`nanobot-manager-autostart.log`,
  `nanobot-manager-process.log`) are no longer written but deliberately
  left on disk for users.

API surface (`logger.h`): `logger_init(dir, level)` with `dir == NULL` →
beside the exe (the `dir` parameter exists for testability), `logger_shutdown`,
`logger_set_level` (effective on the next `nm_log` call),
`logger_level_from_string` (`"debug"`/`"info"`/`"warn"`/`"error"`, plus
`"warning"` → WARN, case-insensitive, anything else → INFO default) and
`logger_level_to_string`, `logger_set_max_bytes`.

### Startup wiring

In `WinMain`, `logger_init(NULL, NM_LOG_INFO)` runs immediately after
`nm_state_init()` — **before** the early-out maintenance modes, the
single-instance check, and the `--autostart` path — so every exit path is
logged. After `config_load` succeeds,
`logger_set_level(logger_level_from_string(g_cfg.log_level))` applies the
user's preference; `logger_shutdown` runs after UI cleanup. If init fails
the logger is a no-op and startup proceeds regardless.

### Migration and level classification policy

The 53 helper call sites were converted mechanically to
`nm_log(..., "autostart"/"process", ...)` calls, but each got a deliberate
level under one policy:

| Situation | Level |
|---|---|
| State transitions, successes, resolutions | INFO |
| Failures, timeouts, unexpected exits, suspected PID reuse | WARN |
| CreateProcess/OpenProcess hard failures, fatal config errors | ERROR |
| Per-candidate probe enumeration, handle-watch chatter | DEBUG |

Apply the same table to new call sites. The default INFO level keeps the
log readable; DEBUG is the "show me everything you tried" tier for
candidate resolution and watch internals, which is exactly the detail you
need when diagnosing "why won't it find nanobot" but never want by default.

### Runtime level control

`config.json` gained a `log_level` string field (`debug|info|warn|error`,
default `"info"`). Parse validation rejects invalid values and keeps the
default — a typo in someone's hand-edited config must not silently disable
logging. The settings page exposes it as four immediate-apply buttons
styled like the existing language buttons, driven by a reactive `logLevel`
data field; the handler sets the logger level, saves the config, pushes the
page field, and logs the change itself (see Examples). The
`setting_log_level` i18n key exists in both zh/en tables (`tests/test_i18n.c`
enforces parity).

### Child capture is file redirection, never a pipe

`nanobot_start()` now redirects the launcher's stdout/stderr into
`nanobot-gateway.log` beside the exe (same 512 KiB `.bak` rotation). This is
deliberately a **file handle**, not a pipe:

- The gateway outlives the manager. With a pipe, once the manager exits no
  thread reads it; a full pipe would block (or break) the gateway's writes —
  a manager crash could hang the very service being managed.
- The handle is opened with `FILE_SHARE_READ | FILE_SHARE_DELETE` so a later
  rotation rename isn't blocked by a still-running gateway holding an
  inherited write handle. It is seeked to END so appends accumulate across
  manager restarts.
- Inheritance uses `STARTUPINFOEXW` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`
  listing exactly that one handle (the same pattern as the capability
  probe), so `bInheritHandles=TRUE` leaks nothing else into the child. The
  original `CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP` flags are kept, with
  `EXTENDED_STARTUPINFO_PRESENT` added when the attribute list is present.
- If anything fails (file open, attribute allocation, `UpdateProcThreadAttribute`),
  the spawn proceeds unredirected — capture is best-effort and must never
  fail the start.

**Known boundary:** `gateway --background` launches a short-lived launcher
that forks the real gateway (grandchild). Launcher-side diagnostics
(dependency errors, tracebacks) are always captured; the forked gateway's
ongoing output arrives only insofar as nanobot propagates inherited handles
through its own fork. Don't promise gateway-forever logging from the manager
side.

## Why This Matters

- **One file to attach to a bug report.** Before, diagnosing meant asking
  the user for two files (or three, counting the gateway void) with no
  levels and interleaved races. Now `[LEVEL] [module]` lines from all
  threads land in `nanobot-manager.log` in order.
- **The most valuable failure signal was going to the void.** When the
  gateway fails to start, the reason (missing dependency, Python traceback)
  is printed by the child, not the manager. File redirection captures it
  without coupling the child's lifetime to the manager's.
- **Logging can't make things worse.** The no-op degradation rule means a
  read-only install directory or a locked file turns logging off, never
  turns the app off.
- **Verbosity is a user setting, not a rebuild.** `log_level` in
  `config.json` plus immediate-apply settings buttons let a supporter say
  "switch to debug, reproduce, switch back" without a debug binary.

## When to Apply

- Adding log lines anywhere in the manager: use `nm_log` with the right
  level per the classification table and a stable module tag — never a new
  local helper.
- Capturing a child process's stdout/stderr where the child may outlive the
  parent: file redirection with `FILE_SHARE_DELETE`, one-handle inheritance
  list, degrade-to-unredirected on failure.
- Adding any new config-driven runtime knob: validate at parse (reject
  unknown values, keep defaults), expose it in settings with
  immediate-apply, keep zh/en i18n keys in parity.
- Writing Win32 log/caches guarded by SRWLOCK with static initialization.
- Debugging "gateway won't start": read `nanobot-gateway.log` (child
  diagnostics) alongside `nanobot-manager.log` (manager decisions).

## Examples

**Before — the per-module helper (git history, `process.c`):** reopened the
file per line, no level, no module, no lock.

```c
static void process_log(const char* fmt, ...) {
    wchar_t exe_dir[MAX_PATH];
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    /* ... build %s\nanobot-manager-process.log, timestamp prefix ... */
    FILE* f = _wfopen(log_path, L"a");
    if (f) {
        fputs(prefix, f);
        fputs(body, f);
        fputc('\n', f);
        fclose(f);                      /* open+close per line, per call site */
    }
}
/* call sites had no level: */
process_log("watch_handle: armed one-shot wait");
```

**After — one call, classified and tagged:**

```c
nm_log(NM_LOG_DEBUG, "process", "watch_handle: armed one-shot wait");
nm_log(NM_LOG_WARN,  "process", "nanobot_start: launcher exited code=%d (continuing)", (int)dwExit);
```

**The SRWLOCK + stay-open `FILE*` write path** (`src/logger.c`): cheap
shared-lock filter check, format outside the lock, exclusive lock only for
the write (with the filter re-checked and rotation applied there):

```c
AcquireSRWLockShared(&g_lock);
if (!g_file || level < g_level) { ReleaseSRWLockShared(&g_lock); return; }
ReleaseSRWLockShared(&g_lock);

/* ... GetLocalTime + vsnprintf + snprintf the full line ... */

AcquireSRWLockExclusive(&g_lock);
if (g_file && level >= g_level) {          /* re-check: level may have moved */
    if (g_bytes > g_max_bytes)
        rotate_locked();                    /* close -> rename .bak -> reopen */
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);                     /* survive hard exits */
        g_bytes += (unsigned long)strlen(line);
    }
}
ReleaseSRWLockExclusive(&g_lock);
```

**The one-handle inheritance spawn** (`src/process.c`,
`open_gateway_log_handle` + `nanobot_start`): the file handle is opened
inheritable with `FILE_SHARE_READ | FILE_SHARE_DELETE`, then only that
handle is listed for inheritance:

```c
SECURITY_ATTRIBUTES sa;
sa.nLength = sizeof(sa);
sa.bInheritHandle = TRUE;             /* the child inherits the write end */
HANDLE h = CreateFileW(path, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_DELETE, &sa,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
SetFilePointer(h, 0, NULL, FILE_END); /* append across manager restarts */

/* ... in nanobot_start: */
siex.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
siex.StartupInfo.hStdOutput = gateway_log;
siex.StartupInfo.hStdError  = gateway_log;
HANDLE inheritable_handles[1] = { gateway_log };
UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                          inheritable_handles, sizeof(inheritable_handles), NULL, NULL);
siex.lpAttributeList = attributes;
/* CreateProcessW with bInheritHandles = lpAttributeList ? TRUE : FALSE,
 * CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
 *   (+ EXTENDED_STARTUPINFO_PRESENT when the list is set) */
```

**The settings handler** (`src/main.cpp`): parse → normalize → set logger →
persist → push the reactive field → log the change:

```c
static void apply_log_level(const char* level) {
    nm_log_level parsed = logger_level_from_string(level);
    strncpy(g_cfg.log_level, logger_level_to_string(parsed),
            sizeof(g_cfg.log_level) - 1);
    g_cfg.log_level[sizeof(g_cfg.log_level) - 1] = '\0';
    logger_set_level(parsed);
    config_save(&g_cfg);
    page_set("logLevel", g_cfg.log_level);
    nm_log(NM_LOG_INFO, "app", "log level set to %s", g_cfg.log_level);
}
```

**Rotation test excerpt** (`tests/test_logger.c`): the invariant is
*nothing is lost* — lines in the active log plus lines in `.bak` must equal
lines written, with the threshold shrunk via the test hook:

```c
logger_set_max_bytes(500);                 /* ~6 lines of ~90 bytes */
for (int i = 0; i < total; i++)
    nm_log(NM_LOG_INFO, "rot", "line-%02d padding padding padding padding", i);

/* ... */
TEST_CHECK(n_bak > 0);                     /* rotation happened */
TEST_CHECK(lines_log + lines_bak == (size_t)total); /* nothing lost */
TEST_CHECK(n_log > 0);                     /* active log still writable */
logger_set_max_bytes(512 * 1024);          /* restore default for others */
```

The suite also covers level-string parsing (including `"warning"` and
bogus/NULL → INFO), no-op-before-init, line format + filtering with a
runtime level switch, and a 4-thread × 50-line concurrency check (exact
line count plus per-thread spot checks). `tests/test_config.c` covers
`log_level` parse validation, serialize round-trip, and defaults.

**Pitfall — a missing include surfaced as LNK2019, not a compile error.**
`logger.c` initially omitted `#include <stdlib.h>`. MSVC then treated
`_countof` as an undeclared identifier → implicit function declaration
(compiles, with a warning), and the failure appeared only at link time as
`LNK2019: unresolved external symbol _countof`. In this codebase `_countof`
arrives via `stdlib.h`; when a new file hits an unresolved-external error
for something macro-like, check the include set (warning C4013 "implicit
declaration" is the tell) before suspecting the build config.

## See also

- `architecture-patterns/state-driven-gateway-lifecycle.md` — the
  launcher-vs-real-gateway spawn model this doc's child capture builds on
- `architecture-patterns/config-system.md` — config.json field patterns the
  `log_level` addition follows
- `design-patterns/rejected-console-ctrl-break-shutdown.md` — why
  `CREATE_NO_WINDOW` children must never be assumed to share a console or
  pipe relationship with the manager
