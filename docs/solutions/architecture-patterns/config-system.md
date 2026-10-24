---
title: "Config module: a testable pure-logic core behind filesystem I/O"
date: 2026-07-11
last_updated: 2026-10-24
category: architecture-patterns
module: nanobot-manager/config
problem_type: module_architecture
component: tooling
severity: low
applies_when:
  - "Reading/writing a JSON config file (cJSON) next to the executable in a Win32 app"
  - "Wanting unit tests for config parsing/validation without running beside the real exe or touching disk"
  - "Separating filesystem I/O from parse/serialize/validation logic so the logic is testable in isolation"
tags: [config, cJSON, json, testability, unit-testing, refactor, win32, c, atomic-save]
---

# Config module: a testable pure-logic core behind filesystem I/O

## Context

`nanobot-manager` keeps its own settings in a `config.json` sitting next to the executable (not to be confused with nanobot's own `~/.nanobot/config.json`). The struct (`config.h`):

```c
typedef struct {
    char* nanobot_path;
    int   autostart_nanobot;
    int   stay_in_tray;
    int   hotkey_enabled;            /* global Ctrl+Shift+N summon hotkey, default on */
    int   health_check_port;         /* default 18790 */
    int   theme_mode;                /* 0=system 1=dark 2=light */
    char  language[8];               /* "zh" / "en", default "zh" */
    char  log_level[8];              /* "debug"/"info"/"warn"/"error", default "info" */
    int   webui_port;                /* default 8765 */
    char  legacy_webui_auth[128];    /* transient old JSON secret; NEVER serialized */
    int   legacy_migration_pending;  /* while set, config_save refuses (ERROR_BUSY) */
    wchar_t legacy_source_path[MAX_PATH]; /* file containing the transient legacy secret */
} app_config;
```

> **Important — the secret is no longer a config field.** The gateway's `tokenIssueSecret` (formerly `webui_auth`) has moved to **Windows Credential Manager**. The struct keeps a `legacy_webui_auth` field for **migration only**: it is populated from an old `config.json` on load, migrated into Credential Manager (write-verify-rollback), then the plaintext is scrubbed — and `config_serialize_json` deliberately **omits** it, so the secret is never written back. A test asserts `strstr(json, "webui_auth") == NULL`. See [`credential-manager-secret-storage.md`](credential-manager-secret-storage.md) for the full migration architecture and the atomic-save pattern (`config_save` writes a temp file + `FlushFileBuffers` + `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`).

Originally `config_load` and `config_save` were monolithic: default-init, path resolution (`GetModuleFileNameA`), file open/read, `cJSON_Parse`, per-field overlay with validation, and JSON build/write were all interleaved in single functions. That meant the parse/validate/serialize logic could not be exercised without running the binary beside a real `config.json` — so it had no tests, and the module was effectively verified only by manual run.

## Guidance

### Split out a pure-logic core with no filesystem dependency

The module now exposes three functions that touch no I/O:

```c
void  config_apply_defaults(app_config* cfg);               // memset + defaults
int   config_parse_json(app_config* cfg, const char* json); // overlay parsed fields
char* config_serialize_json(const app_config* cfg);         // malloc'd JSON, caller frees
```

`config_load` / `config_save` became thin wrappers: defaults + read-file + `config_parse_json` (load), and `config_serialize_json` + write-file (save). `get_config_path()` (the only `GetModuleFileNameA` call) stays private and is never reached by tests.

This is the single load-bearing refactor for testability: pull the logic out of the I/O function so a test can call `config_apply_defaults` → `config_parse_json(json)` → assert, or `config_serialize_json` → round-trip, with zero disk access.

### Preserve the original tolerant semantics exactly

The refactor must not change behavior. The original load was deliberately lenient, and the core keeps it:

- **Invalid JSON is tolerated, not fatal.** `config_parse_json` returns `-1` when `cJSON_Parse` fails, but the caller (`config_load`) ignores the return and keeps whatever defaults were applied first. A test asserts `config_parse_json(cfg, "garbage") == -1` *and* that the defaults survived.
- **Missing file / empty file → defaults, return 0.** `config_load` returns `0` (not an error) when the file is absent or empty; only a `get_config_path` allocation failure returns `-1`.
- **Partial JSON keeps defaults for unspecified fields.** Each field is overlaid independently and only when present + well-typed.

### Encode validation as range guards per field

Validation lives inline in `config_parse_json` and silently ignores out-of-range values (keeping the prior/default value) rather than rejecting the whole file:

| Field | Rule | Out-of-range behavior |
|---|---|---|
| `health_check_port` | `> 0 && <= 65535` | ignored, default kept |
| `webui_port` | `> 0 && <= 65535` | ignored, default kept |
| `theme_mode` | `>= 0 && <= 2` | ignored, default kept |
| `language` | `char[8]`, truncated + NUL-terminated | safe truncation |
| `log_level` | one of `"debug"`/`"info"`/`"warn"`/`"error"` | ignored, prior value kept |
| `nanobot_path` | non-empty string → `_strdup`; null/empty → NULL | no leak |
| `hotkey_enabled` | bool | ignored if not boolean |

The `legacy_webui_auth[128]` field is intentionally **absent** from this table: it is a transient migration input, not a validated/saved config field. It is populated from an old `config.json` only to be migrated into Credential Manager and then scrubbed. Do not add a validation row for it, and never add it to `config_serialize_json` — a test asserts it is absent from serialized output.

Tests cover both boundaries (1 and 65535 accepted) and the out-of-range cases (0, -1, 70000 rejected). The truncation tests pass a value longer than the buffer and assert the result is exactly NUL-terminated at the cap — this is the guard against a future buffer overrun.

### Guard against double-parse leaks

`config_parse_json` frees an existing `nanobot_path` before `_strdup`-ing the new one. `config_load` runs on a freshly-defaulted struct (path == NULL) so this never fires in production, but it makes calling `config_parse_json` twice on the same struct safe — which is exactly what the round-trip test does (parse a fixture, serialize, parse again).

### The round-trip is the contract — except for legacy fields

`config_serialize_json` → `config_parse_json` must reproduce the original struct for every **serialized** field. Two round-trip tests pin this: one on a full fixture (every field overridden) and one on pure defaults. A `configs_equal` helper compares fields by value, treating `nanobot_path == NULL` and `nanobot_path == ""` as equal on both sides. If a serialized field is ever added to `app_config`, the round-trip test forces the author to wire it through parse + serialize in lockstep or the test breaks loudly.

The deliberate exception is the legacy secret: `config_serialize_json` **omits** `legacy_webui_auth`, so it does not survive a round-trip — and that is the contract. A dedicated test asserts the serialized JSON contains no `webui_auth` key, so any future change that accidentally re-serializes the secret fails the suite immediately. If a field is migration-only, exclude it from serialization on purpose and test that exclusion.

## Why This Matters

### Why extract the core instead of testing through the file

Testing `config_load` directly requires either (a) running the test exe next to a fixture file with exactly the right name in the right directory, or (b) refactoring `get_config_path` to accept an injectable path. Both couple the test to the filesystem and to `GetModuleFileNameA`. Extracting `config_parse_json` / `config_serialize_json` makes 13 cases (defaults, full parse, partial parse, invalid JSON, port/theme validation, two round-trips, two truncations, null-vs-string path, null args) run in <50 ms with no disk at all.

### Why tolerate invalid JSON

Config corruption should never prevent the app from starting with sane defaults. Treating `cJSON_Parse` failure as "keep defaults, return success" means a half-written or hand-mangled `config.json` degrades gracefully instead of crashing on startup. The `-1` return from `config_parse_json` is there for tests/diagnostics; `config_load` deliberately does not propagate it.

### Why silent range clamping instead of reject-the-file

A single bad port in an otherwise-good file should not discard the user's other settings. Per-field overlay with silent ignore localizes the damage to the one bad field. The tradeoff is that a typo (e.g. `health_check_port: 187909`) is silently corrected to the default with no user feedback — acceptable for a local single-user config.

## When to Apply

- **Any module shaped as "read file → parse → validate → struct"** — extract the parse/validate/serialize trio as a pure core and keep the I/O function as a thin wrapper. This is the generic recipe for making I/O-bound logic unit-testable.
- **Adding a field to `app_config`** — it must be wired through `config_apply_defaults` (if it has a default), `config_parse_json` (with any range guard), and `config_serialize_json`, or the round-trip test will fail.
- **Testing a registry / network / process module** — the same split applies; see `docs/solutions/infrastructure/testing-and-ci-setup.md` for the autostart registry variant (save-and-restore the real HKCU value).

## Examples

### Testable core signatures (config.h)

```c
void  config_apply_defaults(app_config* cfg);
int   config_parse_json(app_config* cfg, const char* json);
char* config_serialize_json(const app_config* cfg);
```

### config_load reduced to a thin wrapper

```c
int config_load(app_config* cfg) {
    config_apply_defaults(cfg);
    char* path = get_config_path();
    if (!path) return -1;
    FILE* f = fopen(path, "rb");
    free(path);
    if (!f) return 0;
    /* ...read whole file into buf... */
    config_parse_json(cfg, buf);   /* invalid JSON tolerated: defaults preserved */
    free(buf);
    return 0;
}
```

### A round-trip test (tests/test_config.c)

```c
static void test_roundtrip(void) {
    app_config a, b;
    config_apply_defaults(&a);
    config_parse_json(&a, FULL_JSON);
    char* json = config_serialize_json(&a);   // serialize
    config_apply_defaults(&b);
    config_parse_json(&b, json);              // parse back
    TEST_CHECK(configs_equal(&a, &b));        // must reproduce
    free(json);
    config_free(&a);
    config_free(&b);
}
```

### A truncation safety test

```c
/* language is char[8]; overlong input must be capped + NUL-terminated. */
config_parse_json(&cfg, long_language_json);
TEST_CHECK(cfg.language[sizeof(cfg.language) - 1] == '\0');
TEST_CHECK(strlen(cfg.language) == sizeof(cfg.language) - 1);
```

### The secret-never-serialized test

```c
/* config_serialize_json must NEVER emit the legacy webui_auth key. */
config_apply_defaults(&cfg);
/* (legacy_webui_auth would only be set by the migration path, never here) */
char* json = config_serialize_json(&cfg);
TEST_CHECK(strstr(json, "webui_auth") == NULL);
free(json);
```

## Related

- `nanobot-manager/src/config.c` / `config.h` — the module under test (apply_defaults / parse_json / serialize_json + atomic load/save wrappers).
- `nanobot-manager/tests/test_config.c` — unit cases covering defaults, full/partial/invalid parse, port/theme validation, round-trips, truncations, null-vs-string path, and the secret-never-serialized assertion.
- `docs/solutions/architecture-patterns/credential-manager-secret-storage.md` — why `legacy_webui_auth` is migration-only, the migrate-verify-delete flow, and the atomic `config_save` (temp + `FlushFileBuffers` + `MoveFileExW`) implementation.
- `docs/solutions/infrastructure/testing-and-ci-setup.md` — the CMake test wiring, the self-contained `nm_test.h` framework, CTest, and CI; also the autostart registry round-trip pattern.
- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — how `webui_port` / `health_check_port` (defined here) are surfaced in the settings UI via the read-on-click Apply pattern.
- `docs/solutions/architecture-patterns/logging-pipeline.md` — how the `log_level` field (defined here) drives the unified logger's runtime filtering.
