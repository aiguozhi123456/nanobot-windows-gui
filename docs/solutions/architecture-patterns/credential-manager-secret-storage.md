---
title: "Storing the bootstrap secret in Windows Credential Manager with migrate-then-verify-then-delete"
date: 2026-08-01
category: architecture-patterns
module: nanobot-manager/credential
problem_type: architecture_pattern
component: authentication
severity: high
related_components:
  - nanobot-manager/config
  - nanobot-manager/chat
applies_when:
  - "Storing a user-scoped secret in a native Win32 app and keeping it out of plaintext config and reactive UI state"
  - "Migrating a secret that currently lives in a plaintext JSON file into Windows Credential Manager without losing it on a failed write"
  - "Writing config.json atomically so a crash mid-save cannot corrupt or truncate the file"
  - "Removing a legacy plaintext secret under HKLM-protected paths when the app runs unelevated at boot"
tags: [credential-manager, wincred, secret-management, atomic-save, movefileex, migration, security, win32, c, config]
---

# Storing the bootstrap secret in Windows Credential Manager with migrate-then-verify-then-delete

## Context

The gateway's optional `tokenIssueSecret` — the **Bootstrap Credential** that authorizes the `GET /webui/bootstrap` handshake — used to live as a plaintext `webui_auth` string inside `config.json` next to the executable. That had two problems:

1. **Plaintext at rest on disk.** Anyone (or any process) that could read the exe-adjacent `config.json` could read the secret. For an installed build that path is under `%LOCALAPPDATA%` / `Program Files`, but for portable/adjacent installs it is wherever the user dropped the exe.
2. **It leaked into reactive UI state.** Surfacing it as an editable settings field meant the secret transited a `v-model` reactive field, the `on_apply_network` handler, and `chat_set_endpoint(port, auth)` — multiple in-memory copies with no zeroing discipline.

The manager now keeps the secret in **Windows Credential Manager** under the fixed target name `nanobot-manager/bootstrap-secret/v1` (see `src/credential.h`), treats the old `config.json` field as **legacy/migration-only** (never serialized), writes `config.json` **atomically**, and removes the old plaintext via an **elevated helper scoped to a single fixed action**.

This is the architectural record of that change — the two existing docs it touched (`config-system.md`, `core-ui-input-read-pattern.md`) describe the *pre-migration* world and have since been refreshed to link here.

## Guidance

### The secret lives in Credential Manager, not config.json

`credential.c` wraps the Win32 `Cred*W` API (`<wincred.h>`) around one fixed target string:

```c
/* src/credential.h */
#define CREDENTIAL_TARGET "nanobot-manager/bootstrap-secret/v1"

int credential_store(const char* secret, DWORD* error);   /* CredWriteW */
int credential_load(char* out, size_t cap, DWORD* error); /* CredReadW  */
int credential_delete(DWORD* error);                      /* CredDeleteW */
```

`CRED_TYPE_GENERIC` + `CRED_PERSIST_LOCAL_MACHINE`. The blob is the raw secret bytes (length 1..127). Three deliberate properties:

- **Fixed target name, hardcoded.** The target is a compile-time constant, not derived from any user/path input. This is what makes the elevated scrub helper safe (see below) — there is no caller-controlled target to redirect.
- **Per-user, not per-machine roaming.** `CRED_PERSIST_LOCAL_MACHINE` keeps it local to the Windows account; it does not roam with the profile.
- **No reflection into reactive UI state.** The manager exposes only a boolean (`hasCredential`) and a status label (`credentialStatus`) in `dashboard.uix` — never the secret itself. The "Manage/Clear" buttons call `credential_*`; they never round-trip the value through the page.

### Write-then-verify-then-rollback on store

`credential_store_for_target` does not trust `CredWriteW` alone. After writing, it **reads the credential back and compares the bytes** before reporting success; if the read-back mismatches, it rolls back to the previous value (or deletes, if there was none):

```c
/* src/credential.c — simplified */
CredWriteW(&credential, 0);                       /* write the new blob       */
if (CredReadW(target, ..., &read_back) &&
    read_back->CredentialBlobSize == length &&
    memcmp(read_back->CredentialBlob, secret, length) == 0) {
    result = 0;                                   /* verified — keep it       */
} else {
    /* rollback: restore the saved old blob, or CredDeleteW if none had existed */
}
```

The previous blob is captured (into a `malloc`'d buffer, zeroed on free) *before* the write, so rollback is always possible. The reason this matters: `CredWriteW` can report success while the credential is not yet consistently readable, and a "stored OK" that silently lost the secret would brick bootstrap auth. **Verify, don't assume.**

### Migrate-then-verify-then-delete from the legacy source

The legacy `webui_auth` in `config.json` is migrated on load, not silently overwritten. The ordering is load-bearing:

1. **Detect** the legacy plaintext secret in the adjacent `config.json` (`config_adjacent_legacy_auth_present`), gated by `config_should_scan_adjacent_legacy`.
2. **Store** it into Credential Manager via `credential_store` (write-then-verify above).
3. **Verify** it reads back from Credential Manager before touching the plaintext.
4. **Only then scrub** the plaintext from `config.json` (`config_scrub_adjacent_legacy_auth`).

A migration flag (`legacy_migration_pending` in `app_config`) owns the run: while a migration is in progress, `config_save` **refuses to run** (`return -1` with `ERROR_BUSY`) so a concurrent save cannot re-serialize the secret into plaintext mid-migration. The struct field is `legacy_webui_auth` with a comment `transient old JSON secret; never serialized`, and `config_serialize_json` deliberately omits it — confirmed by a test asserting `strstr(json, "webui_auth") == NULL`.

The Bootstrap Credential concept in `CONCEPTS.md` states the policy: *"Migration removes a legacy plaintext source only after the credential can be read back, and conflicting sources require explicit resolution rather than silent overwrite."*

### Removing the plaintext under HKLM-protected paths

The scrub step has a privilege wrinkle. When the manager launches at boot via `--autostart` and the adjacent `config.json` lives under `Program Files` (an installed build), the unelevated process **cannot delete or rewrite** that file. The manager handles this with `config_scrub_adjacent_legacy_auth` → a `runas` helper (`--scrub-adjacent-legacy-auth`):

- The helper's command line is a **single fixed token** — `--scrub-adjacent-legacy-auth` — with **no path argument**. The target path is computed inside the helper from the exe's own location (`GetModuleFileNameW`), exactly as the main process computes it. There is no way for the spawning process to redirect the scrub to a different file.
- This is why the Credential Manager target is also a hardcoded constant: both the elevated scrub and the credential storage reject caller-controlled paths by construction.

If you ever add a second scrub target, **do not parameterize this command**. Spawn a different fixed-token helper instead. The whole safety property collapses the moment the elevated command accepts a path.

### Atomic config.json save

`config_save` (`src/config.c`) never writes `config.json` in place. It writes a uniquely-named temp file, flushes, closes, then `MoveFileExW` renames it over the target:

```c
/* src/config.c — config_save, simplified */
swprintf_s(temp, ..., L"%s.%lu.%llu.%ld.tmp",
           g_active_path, GetCurrentProcessId(),
           GetTickCount64(), InterlockedIncrement(&g_temp_counter));
file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                   FILE_ATTRIBUTE_TEMPORARY, NULL);   /* CREATE_NEW: never clobber */
/* ...WriteFile loop... */
FlushFileBuffers(file);                                /* push to disk before rename */
CloseHandle(file);
MoveFileExW(temp, g_active_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
```

Five details that each prevent a corruption class:

- **`CREATE_NEW` + a PID/tick/counter suffix** ⇒ two managers (or a fast restart) never collide on the same temp name; up to 32 attempts before giving up.
- **`FlushFileBuffers` before `CloseHandle`** ⇒ the file's content is on disk before the rename, so a power loss between write and rename leaves the old `config.json` intact, not a half-flushed new one.
- **`MoveFileExW(... MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`** ⇒ the rename is atomic on the same volume (NTFS) and write-through (committed to the device, not just the cache).
- **`DeleteFileW(temp)` on every failure path** ⇒ no temp litter on disk after a failed save.
- **Refusal during migration** (`legacy_migration_pending` ⇒ `ERROR_BUSY`) ⇒ the save path cannot reintroduce the plaintext secret while migration is mid-flight.

### Zero secrets on every teardown path

The chat transport keeps a short-lived copy of the bootstrap secret for the `X-Nanobot-Auth` header; that copy is `SecureZeroMemory`'d on every teardown — disconnect, shutdown, endpoint change (`src/chat.c`). Likewise `credential.c` zeroes the wide target string, the read-back blob, and the rollback buffer on every path (including error paths). `config_free` zeroes `legacy_webui_auth` and `legacy_source_path`. The discipline: a secret's lifetime ends with `SecureZeroMemory`, never with `free` alone.

## Why This Matters

- **Plaintext-at-rest was the core risk.** Moving the secret to Credential Manager removes it from a file that gets backed up, synced, copy-pasted into a bug report, or read by another local process. Credential Manager blobs are encrypted by DPAPI and surfaced only through the `Cred*` API.
- **Migrate-verify-delete ordering prevents data loss.** Deleting the plaintext *before* confirming the credential is durable would brick bootstrap auth on a `CredWriteW` that lied about success. The verify step is the gate.
- **The elevated scrub's safety is structural, not conventional.** "Don't pass a path on the command line" is a code-review smell, not a guarantee. Here the helper literally cannot accept one, which is what makes it safe to invoke elevated at boot. Future helpers must preserve this.
- **Atomic save prevents the "config wiped on crash" class.** A truncated or empty `config.json` from a mid-save crash would reset every setting. Temp + flush + atomic rename makes that impossible on NTFS same-volume renames.
- **Keeping the secret out of reactive UI state** means there is no `v-model` field an XSS-equivalent page script or a future developer could accidentally log. The UI exposes only presence + a label.

## When to Apply

- **Storing any user-scoped secret in a native Win32 app** — Credential Manager (`CRED_TYPE_GENERIC`, fixed target) is the right home; do not put it in a JSON file or a reactive field.
- **Migrating a secret from plaintext to secure storage** — follow detect → store → verify-read-back → delete-plaintext, and gate normal saves while migration is pending.
- **Writing any JSON/config file the app depends on at startup** — use the temp-file + `FlushFileBuffers` + `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` pattern; do not truncate-and-write in place.
- **Needing to modify a file under a privileged path from an unelevated boot process** — use a fixed-token `runas` helper that computes its own target from the exe location; never pass the path as an argument.
- **Reviewing a PR that adds a new secret** — check: (a) is it in Credential Manager, not config? (b) is every in-memory copy `SecureZeroMemory`'d on teardown? (c) is the elevated path parameter-free?

## Examples

### Store with verify (credential.c, abridged)

```c
CredWriteW(&credential, 0);                                  /* write new */
PCREDENTIALW read_back = NULL;
int result = -1;
if (CredReadW(target, ..., &read_back) &&
    read_back->CredentialBlobSize == length &&
    memcmp(read_back->CredentialBlob, secret, length) == 0) {
    result = 0;                                              /* verified   */
}
/* if result != 0: restore old blob (had_old) or CredDeleteW(target) */
```

### Atomic save (config.c, abridged)

```c
CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, ...);  /* unique temp */
WriteFile(...); FlushFileBuffers(file); CloseHandle(file);
MoveFileExW(temp, g_active_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
/* on any failure: DeleteFileW(temp) */
```

### UI exposes presence, not the value (dashboard.uix)

```js
// data() — boolean + label only; the secret never enters the page
hasCredential: false,
credentialStatus: '',
```
```html
<div id="btn_manage_credential"> ... </div>   <!-- store via dialog, not v-model -->
<div id="btn_clear_credential"> ... </div>    <!-- credential_delete() -->
```

### Never serialize the legacy field (config_serialize_json omits it)

```c
/* config.h */
char legacy_webui_auth[128];   /* transient old JSON secret; never serialized */
int  legacy_migration_pending; /* while set, config_save refuses (ERROR_BUSY)  */
```

## Related

- `docs/solutions/architecture-patterns/config-system.md` — refreshed to reflect `legacy_webui_auth` is migration-only and never round-tripped.
- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — refreshed: the `webuiAuth` input example is gone; auth is now the Credential Manager buttons.
- `docs/solutions/architecture-patterns/state-driven-gateway-lifecycle.md` — the credential-migration section of the gateway-lifecycle story; this doc is the detailed counterpart.
- `docs/solutions/reference/nanobot-gateway-overview.md` — the upstream side of the bootstrap handshake (`GET /webui/bootstrap` with `X-Nanobot-Auth`).
- `CONCEPTS.md` → **Bootstrap Credential** — the vocabulary entry for this concept and its migrate/verify/conflict-resolution rules.
- `nanobot-manager/src/credential.c` / `credential.h` — the Win32 `Cred*W` wrapper with write-verify-rollback.
- `nanobot-manager/src/config.c` — `config_save` (atomic), `config_scrub_adjacent_legacy_auth`, `config_should_scan_adjacent_legacy`, the migration gate.
- `nanobot-manager/src/main.cpp` — `--scrub-adjacent-legacy-auth` helper, the credential Manage/Clear button handlers, and the migration orchestration.
