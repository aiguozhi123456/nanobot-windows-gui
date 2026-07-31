---
title: "State-Driven Gateway Lifecycle Management on Windows"
date: 2026-07-13
last_updated: 2026-07-31
category: architecture-patterns
module: nanobot-manager/process
problem_type: architecture_pattern
component: tooling
severity: high
applies_when:
  - "Managing a detached local service from a native Windows GUI"
  - "Resolving multiple executable or Python-module installation forms"
  - "Reconciling process liveness with asynchronous HTTP service readiness"
  - "Attaching to a service started before or outside the manager"
  - "Coordinating process handles, wait callbacks, stop workers, and UI state"
  - "Keeping health and WebUI endpoints independent while adopting a verified runtime port"
  - "Migrating a bootstrap secret out of plaintext configuration"
tags:
  - gateway-lifecycle
  - win32
  - process-management
  - gateway-state
  - async-health
  - pid-reuse
  - handle-ownership
  - control-plane
  - credential-manager
  - secret-migration
resolution_type: code_fix
---

# State-Driven Gateway Lifecycle Management on Windows

## Context

`nanobot-manager` is a native Windows supervisor for a gateway whose lifecycle is defined by the nanobot CLI, HTTP health endpoint, and runtime state file. The manager originally treated those upstream interfaces as incidental and tried to derive truth from local snapshots and Windows process mechanics.

That split authority produced several forms of drift:

- An independent installation boolean could disagree with the command that Start would actually run, especially when only `nanobot_path` was configured.
- Python resolution and WinHTTP health checks could block the UI thread.
- A single two-second post-start check could mark a slow boot as stopped, while later crashes left the UI showing Running indefinitely.
- Foreground start behavior was mixed with `gateway stop`, even though that command relies on the state record created by background mode.
- Raw process globals, wait registrations, and multiple handle-close sites created races, leaks, and double-close paths.
- `CTRL_BREAK_EVENT` was treated as graceful shutdown even though a `CREATE_NO_WINDOW` child has no compatible console relationship.
- A configured health port changed the manager's probe target without changing the gateway launch command, while the independent WebUI port was vulnerable to being treated as a derived endpoint.
- A stale state record could name a reused PID, and several `GetProcessTimes` calls passed invalid `NULL` output pointers instead of failing closed when identity was unavailable.
- The optional bootstrap secret was serialized as `webui_auth` in `config.json` and exposed through reactive UI state instead of a native credential boundary.

The durable result is a control-plane-first supervision model: use nanobot's CLI, health endpoint, and state record as the primary lifecycle contract, then use Win32 process APIs as transport, observation, identity verification, and guarded fallback. The implementation is centered in `nanobot-manager/src/process.c` and `nanobot-manager/src/process.h`, with asynchronous orchestration and UI transitions in `nanobot-manager/src/main.cpp`.

### What Did Not Work

- **Registry-wide installation discovery:** the configured path, `nanobot.exe` on `PATH`, and known Python launchers cover the product contract. Additional registry heuristics add maintenance cost without an observed need.
- **A separate installed fact:** installation is not independent state; it is the successful result of resolving a runnable command.
- **Foreground start plus `gateway stop`:** the stop command rendezvous is the state record written by `gateway --background`. Mixing the two modes makes graceful stop a no-op.
- **HTTP-only or handle-only supervision:** HTTP answers whether the service is ready; a process handle answers whether that process exited. Neither signal answers both questions.
- **Console-control shutdown:** `CTRL_BREAK_EVENT` is not a reliable control plane for a detached, no-window process.
- **Uncoordinated handle cleanup:** closing a process handle before draining its registered wait, or closing it from both the stop worker and exit callback, creates stale-handle and double-close races.
- **Assuming state-file field shapes:** upstream writes `identity` as a decimal FILETIME string, `started_at` as UTC ISO 8601 ending in `Z`, and `command` as a JSON argv array—not a shell command string.
- **Treating HTTP 200 as process identity:** health proves readiness at an endpoint, not that the listener belongs to the PID in a state record.
- **Deriving WebUI as health plus one:** the gateway health listener and WebUI/bootstrap/WebSocket listener are independent interfaces with independent configuration.
- **Cleaning a legacy secret only on Stop:** Stop is optional and does not cover first upgrade, autostart, startup failure, or users who leave the gateway running.
- **Masking a secret in reactive UI:** visual masking does not keep the value out of the page data model, snapshots, or generic diagnostics.
- **Passing `NULL` for unused `GetProcessTimes` outputs:** all four `FILETIME` outputs require valid storage; unused results belong in local variables.

## Guidance

### Make the target control plane authoritative

Define the supervision contract before choosing Win32 mechanisms:

| Question | Primary signal | Windows role |
| --- | --- | --- |
| What command can run nanobot? | Ordered CLI resolution | Validate candidates and run probes |
| Did the gateway become usable? | HTTP 200 from `GET /health` | WinHTTP transport on a worker |
| Which background gateway is managed? | Gateway state record PID and identity | `OpenProcess` and `GetProcessTimes` verification |
| Did the process exit? | Signaled process handle | `RegisterWaitForSingleObject` event delivery |
| How should it stop? | `nanobot gateway stop --timeout 20` | Wait, then guarded termination fallback |

The OS APIs support the target's contract; they do not replace it. Process enumeration, registry scanning, and guessed command lines should not become parallel sources of truth.

### Treat installation as command resolution

`nanobot_resolve_candidates` builds one ordered candidate list:

1. `app_config.nanobot_path`
2. `nanobot.exe` on `PATH`
3. `py.exe`
4. `python.exe`, `python3.exe`, and `pythonw.exe`

`nanobot_resolve` probes candidates against the capability the manager actually requires: `gateway --help` must exit successfully and contain a standalone `--background` token. This distinguishes not found, incompatible, and probe-failed outcomes before Start. A configured executable is an explicit override, so its failure does not silently select a different executable from `PATH`; successful probes are cached for the session while failures remain retryable. Every interactive and autostart entry point uses the same resolver. `kick_resolve_worker` snapshots pointer-owned configuration before off-thread work begins, and the UI derives installation state from the resolver result.

The invariant is:

```text
installed := resolver returned a usable launch candidate
```

Do not preserve a second independently computed installation fact.

### Align start, identity, and stop around the state record

Start uses the lifecycle mode whose stop semantics are supported upstream:

```text
nanobot gateway --background
python -m nanobot gateway --background
```

The configured Gateway Health Port is part of that command in both forms:

```text
nanobot gateway --background --port <health-port>
python -m nanobot gateway --background --port <health-port>
```

The process returned directly by `CreateProcess` is the short-lived background launcher, not the gateway to supervise. Wait for and close the launcher handles, read the real PID from `%USERPROFILE%\.nanobot\run\gateway.json`, and open that PID. The handle stored in guarded state must always refer to the real gateway.

Verified upstream Windows state fields have exact meanings:

- `identity` is `str((high << 32) | low)` from the process creation FILETIME.
- `started_at` is emitted by `datetime.now(UTC).isoformat().replace('+00:00', 'Z')`.
- `command` is a JSON array of argv strings.

An illustrative record is:

```json
{
  "pid": 4312,
  "identity": "134127567890123456",
  "started_at": "2026-07-13T07:45:31.123456Z",
  "command": ["C:\\Python312\\python.exe", "-m", "nanobot", "gateway", "--foreground", "--port", "18790"]
}
```

`nanobot_read_gateway_state` reconstructs the argv array when a command-line view is useful. `nanobot_detect_running_port` adopts a state-record port only when the PID is live and its creation time matches `identity`; it uses `started_at` only when `identity` is absent, while an invalid present identity fails closed. Mismatched-port startup finishes with that same strict verifier and returns `NANOBOT_START_UNVERIFIED_STATE` when the record cannot be verified.

An unverified state record is not proof that its process is safe to terminate. Startup recovery destroys a mismatched-port candidate only when the launcher completed successfully, the candidate is not the prior recorded PID, its creation time is readable, and it was created no earlier than this launcher attempt. Otherwise the manager closes only its own handle and reports the failure.

The strict identity gate is intentionally scoped. Same-port startup retains a compatibility path when identity metadata is absent or unparseable, `nanobot_attach_running` captures live creation time without comparing it to the record, and process-tree fallback discovers children before validating the root. These remain hardening follow-ups; do not generalize the mismatched-port and detection guarantees to every attach or stop path.

Stop follows the same rendezvous. `nanobot_stop` invokes `gateway stop --timeout 20`, interprets the CLI result, and confirms death through the tracked handle. `TerminateProcess` and process-tree termination are escalation paths, not the normal control plane; destructive fallback must remain bounded by the identity-validation limitations above.

### Keep health and WebUI endpoint roles separate

The manager carries three related values with different ownership:

```text
configured health port -> next gateway launch and fallback readiness probe
verified state port    -> effective health port for the current gateway
configured WebUI port  -> browser, bootstrap, and WebSocket connections
```

Changing the configured health port is disabled while lifecycle state is resolving, starting, running, or stopping because it applies to the gateway process. The WebUI port is a manager-side connection override and remains independent. Neither endpoint is derived from the other, and the effective runtime health port is never written back as the user's next-launch preference.

### Treat process identity queries as fallible

All creation-time checks supply valid storage for every `GetProcessTimes` output:

```c
FILETIME created, exited, kernel, user;
BOOL known = GetProcessTimes(process, &created, &exited, &kernel, &user);
```

Identity-query failure means unknown identity. Callers must avoid publishing verified state or entering a destructive fallback that depends on that identity; they must not substitute a zero timestamp or omit required outputs with `NULL`.

### Migrate bootstrap credentials before deleting plaintext

Non-sensitive settings remain in `config.json`, but the Bootstrap Credential lives under the fixed generic Windows Credential Manager target `nanobot-manager/bootstrap-secret/v1`. Credential writes preserve the previous value, write the replacement, read it back, and restore or delete on failed verification. The secret is absent from normal config serialization and Core UI reactive state; the settings page displays only credential status and opens a temporary native password control for replacement.

Legacy `webui_auth` is treated only as migration input. Migration follows this order:

1. Read the current credential and reject a conflict without changing either source.
2. Store and read back the legacy value when no credential exists.
3. Save the active config without `webui_auth`.
4. Remove only that key from any separate legacy JSON while preserving unknown fields.
5. Flush and atomically replace the file, then verify cleanup where elevation was required.

An adjacent Program Files config may require a `runas` helper. That helper accepts only the fixed `--scrub-adjacent-legacy-auth` action and no caller-controlled path, preventing elevation from becoming a general file-rewrite primitive. Cancellation or failed verification leaves startup blocked or cleanup pending rather than claiming the plaintext was removed.

Secrets, credential blobs, bootstrap headers, tokens, and thread-local copies are cleared with `SecureZeroMemory` on replacement, error, disconnect, and shutdown paths. The native chat layer receives a short-lived secret copy; the UI page model never owns one.

### Separate death events from health state

Process exit is an event. `nanobot_watch_handle` registers a one-shot `RegisterWaitForSingleObject` on the real gateway handle. Its callback performs no lifecycle mutation and only posts `WM_CHILD_EXITED`; the window procedure owns the transition.

Health is sampled state. `health_worker` runs `nanobot_detect_running_port` off the UI thread; that function verifies a state-record identity before probing its port and otherwise probes the configured fallback port. The worker posts `WM_ASYNC_HEALTH_RESULT`, uses boot-phase backoff before settling into a steady cadence, tolerates one transient steady-state failure, and uses a generation counter to prevent superseded workers from posting stale results.

`NM_STATE_DEAD` currently represents terminal gateway unavailability, not proof that the process handle signaled. It is entered after an unsolicited process-exit event or after two consecutive health failures while Running; a health-driven transition can therefore occur while the process still exists.

Do not poll process death as though it were health, synchronously query HTTP health from UI callbacks, or infer service readiness merely because a process handle remains unsignaled.

### First-iteration bypass for user-initiated probes

The polling-state guard in `health_worker` (`main.cpp`) returns early when state is not in `{STARTING, RUNNING, RESOLVING}`. That guard exists to keep a worker that is mid-shutdown from posting `running = 1` and undoing `stop_worker`, but it makes a user click on the refresh button during `IDLE` or `DEAD` a silent no-op — the worker probes, the gen check passes, then the state guard bails before `PostMessageW`.

Resolution (M8): the worker widens its allow-list on the **first iteration only** to also accept `IDLE`/`DEAD`. Subsequent iterations and any `STOPPING` transition still bail before post, so steady-state strictness and the in-progress-stop invariant are preserved. The click handler calls the existing `kick_health_worker`; the new gen bumps the M7 generation counter, so a click that supersedes a boot-phase worker does not double-post — the old worker exits at the gen check, the new worker posts once, then continues under the standard guard. The reactive bool `canRefresh` (`set_status` in `main.cpp`, `dashboard.uix`) is wired so the button is visibly disabled during `STARTING`/`STOPPING`, making the no-op visible instead of looking like a broken control.

Full symptom and resolution: [Refresh button does nothing in IDLE](../ui-bugs/refresh-button-silent-in-idle.md).

### Serialize lifecycle state and marshal UI updates

The lifecycle is explicit in `nm_state`: unresolved, resolving, idle, starting, running, stopping, and terminal unavailability (`DEAD`). Process state lives behind one critical section; callers use snapshots and accessors rather than raw cross-thread globals.

Workers perform blocking work and post owned results to the window:

- `resolve_worker` posts `WM_ASYNC_RESOLVE_DONE`.
- `health_worker` posts `WM_ASYNC_HEALTH_RESULT`.
- `stop_worker` posts `WM_ASYNC_STOP_DONE`.
- The wait callback posts `WM_CHILD_EXITED`.

Use `PostMessageW`, not direct UI calls or blocking sends, from workers and wait callbacks. Snapshot pointer-owned inputs before launching a worker, define payload ownership, and free the payload when posting fails because the window has gone away.

Status text follows the same ownership rule. `g_status_i18n` is the wording source for native status updates and mechanically generates the status portion of the language blob consumed by the UI, preventing native and `$t(...)` text from drifting.

### Make handle ownership atomic and centralized

Every path that releases the tracked gateway handle goes through `nm_state_close_handle`. It atomically steals the handle under the state lock, making concurrent closers observe `NULL`, then drains the wait before closing:

```c
UnregisterWaitEx(wait, INVALID_HANDLE_VALUE);
CloseHandle(process_handle);
```

`INVALID_HANDLE_VALUE` is the completion-event sentinel that makes `UnregisterWaitEx` wait for in-flight callbacks. It is not an unregister flag.

`NM_STATE_STOPPING` is also part of ownership. If `WM_CHILD_EXITED` arrives during an invited stop, the handler leaves cleanup to the stop worker. For any other death, the UI path closes through `nm_state_close_handle` and transitions to unexpected death. This prevents two threads from closing the same handle.

Manager shutdown calls `nm_state_clear`, not `nanobot_stop`; quitting the manager deliberately leaves the independently managed gateway running. On a later launch, successful health detection lets `nanobot_attach_running` read the state record, open the existing PID, and restore handle-based death observation.

## Why This Matters

Supervisors fail when several plausible observations become competing truths. A boolean can say installed while no command is runnable. HTTP can say unavailable while a process is still booting. A handle can say alive while the service is wedged. A PID can name a different process after reuse.

The state machine and signal model keep each observation scoped to the question it can answer. Control-plane-first behavior also preserves upstream semantics: background start and stop form a matched lifecycle because the state record is their rendezvous.

Moving blocking probes away from the message pump keeps the GUI responsive during Python startup, WinHTTP timeouts, and graceful stop. Centralized ownership then makes those asynchronous paths safe: one lock guards process state, one UI thread owns presentation transitions, and one helper drains and closes the watched handle.

Exact upstream-schema verification matters for the same reason. The decimal FILETIME identity, UTC start time, and argv-array command are interoperability contracts; defensive parsing should be based on upstream implementation or observed records, not field-name guesses.

Credential migration follows the same evidence rule as process adoption: establish and verify the new authoritative state before deleting the old one. That ordering avoids both silent credential loss and a false-success UI while plaintext remains on disk.

## When to Apply

Apply this pattern when a GUI or tray application supervises a separately owned CLI service and one or more of these conditions hold:

- The target exposes start/stop commands, a health endpoint, or a state record with more lifecycle meaning than a raw PID.
- Startup uses a short-lived launcher, interpreter re-exec, or delayed readiness.
- The manager must remain responsive while probes, HTTP requests, startup, or shutdown can block.
- The service can outlive and later be reattached by the manager.
- Process death and application health require different latency or failure handling.
- Several asynchronous paths can observe or close the same process handle.
- Persisted PIDs require shared stale-state and PID-reuse validation before attach or destructive fallback.
- A service exposes separate health and user-facing endpoints whose ports cannot be inferred from each other.
- A secret must move from a legacy plaintext store into an OS credential store without losing recoverability on partial failure.

Do not add the full pattern to an in-process component with no independent lifecycle, or to a child whose entire lifetime is strictly joined by one parent thread. Even there, retain the core rule: use the highest-level contract available and make ownership explicit before adding fallbacks.

## Examples

### Lifecycle flow

1. `WinMain` initializes guarded state and kicks resolution for detection or silent-tray start.
2. The resolver derives installation UI state from one runnable command result.
3. Start launches background mode, discards launcher handles, reads and validates the state record, and opens the real gateway PID.
4. The current gateway's verified state port becomes the effective health endpoint; WebUI/bootstrap continues using its independent configured port.
5. A process wait posts immediate death events while a health worker polls readiness and steady health.
6. Stop enters the stopping state, runs the upstream stop command on a worker, confirms death, and escalates only when necessary.
7. Manager exit closes only its observation handle. A future manager launch attaches to the still-running gateway.

### Verification checklist

- **Cold start:** keep the UI responsive through detection and delayed gateway readiness; report Running only after HTTP 200.
- **Slow or invalid Python:** ensure resolution and health timeouts do not block the window.
- **External kill:** terminate the real gateway PID and verify immediate exit notification, uptime stop, and chat disconnect.
- **Repeated Stop:** stop a healthy gateway repeatedly and verify no stale Running state, leaked wait, or double-close behavior.
- **Relaunch attach:** quit the manager without stopping the gateway, relaunch, and verify PID, health, chat, and death observation are restored.
- **Config-only path:** remove nanobot/Python from `PATH`, configure only `nanobot_path`, and verify detection, Start, and autostart use it.
- **No-tray autostart:** verify boot backoff, handle release, process exit, and that the gateway remains running.
- **Capability probe:** point the manager at a nanobot version without `gateway --background` and verify it reports incompatibility before launch.
- **Independent ports:** configure unrelated health and WebUI ports; verify the launch command contains the health port and bootstrap/WebSocket use only the WebUI port.
- **State-record identity:** verify the decimal identity, UTC `started_at`, and argv-array command. Confirm detection does not trust an unverified state port and mismatched-port startup rejects unverifiable state.
- **Conservative cleanup:** vary launcher result, prior PID, candidate creation availability, and creation ordering; termination is eligible only when every ownership condition holds.
- **Process-time failure:** verify an unavailable creation time does not publish verified identity or authorize cleanup. Keep every direct `GetProcessTimes` call supplied with four valid outputs.
- **Credential round trip:** store, read, replace, and delete through a process-unique test target so tests never alter the product credential.
- **Legacy migration:** verify normal serialization never emits `webui_auth`, the scrub preserves unknown JSON fields, conflicts change neither source, and malformed or inaccessible files do not report success.
- **Manual upgrade matrix:** cover no legacy secret, matching and conflicting credentials, Program Files elevation, cancelled UAC, and chat reconnect after credential replacement or deletion.
- **Build and package:** run `scripts\build.bat` and `scripts\package.bat`; verify the Release executable and NSIS installer complete cleanly.

## Related

- [nanobot Gateway Overview](../reference/nanobot-gateway-overview.md) — upstream gateway commands, endpoints, and compatibility baseline.
- [Native WebSocket Chat Integration](native-websocket-chat-integration.md) — related worker-to-UI messaging, generation, and resource-close discipline for chat transport.
- [Testing and CI Setup](../infrastructure/testing-and-ci-setup.md) — CTest wiring and the boundary between native unit coverage and manual process/WinHTTP integration verification.
- [Configuration System](config-system.md) — manager-owned non-sensitive configuration and legacy JSON migration behavior; its older plaintext `webui_auth` examples require refresh.
- [Refresh button does nothing in IDLE](../ui-bugs/refresh-button-silent-in-idle.md) — the `health_worker` M8 first-iteration bypass that lets a user click deliver a probe result from `IDLE`/`DEAD` without breaking the `STOPPING` guard.
