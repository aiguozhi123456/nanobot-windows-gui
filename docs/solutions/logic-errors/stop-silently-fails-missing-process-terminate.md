---
title: "Gateway stop silently fails — tracked handle opened without PROCESS_TERMINATE"
date: 2026-10-25
category: logic-errors
module: nanobot-manager/process
problem_type: logic_error
component: service_object
symptoms:
  - "Manager UI flips back to idle after Stop, but the gateway process is still running"
  - "Tier-1 stop escalation fails with ERROR_ACCESS_DENIED when TerminateProcess is called on the tracked handle"
  - "Gateways latched via nanobot_attach_running cannot be killed through the tracked handle because that handle also lacks PROCESS_TERMINATE"
  - "WaitForSingleObject on the tracked handle after the failed TerminateProcess merely times out while the stop flow still reports success"
  - "A stale PID-reuse guard (g_state.create_ft not cleared with the handle) makes a later state-file-pid stop refuse TerminateProcess with a false \"pid reuse suspected\" log"
root_cause: missing_permission
resolution_type: code_fix
severity: high
related_components:
  - "nanobot-manager/main"
tags:
  - openprocess
  - terminateprocess
  - process-terminate
  - access-rights
  - stop
  - handle
  - gateway-lifecycle
---

# Gateway stop silently fails — tracked handle opened without PROCESS_TERMINATE

## Problem

Clicking Stop in the manager reports success and the UI returns to idle, but the nanobot gateway process keeps running — still bound to its port and answering `/health` — because the tracked process handle could not actually be terminated.

## Symptoms

- Stop appears to succeed (status flips to idle) but the gateway is still alive: `GET http://localhost:{port}/health` keeps returning 200 after the stop flow completes.
- The stop takes a suspicious extra ~3 seconds with no error reported — that is `WaitForSingleObject(v.handle, 3000)` timing out on a handle that will never signal.
- No stop error appears in `nanobot-manager.log`; `TerminateProcess` returned FALSE with `ERROR_ACCESS_DENIED`, but its return value was never checked or logged.
- A manager relaunch that attached to an already-running gateway (`nanobot_attach_running`) produced an equally un-killable tracked handle — same silent failure.

## What Didn't Work

The original design requested `PROCESS_TERMINATE` conditionally — "only request the right when we expect to need it". In `nanobot_start` the access mask was:

```c
DWORD process_access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;
if (gs.port != health_port)          /* only on port mismatch */
    process_access |= PROCESS_TERMINATE;
h = OpenProcess(process_access, FALSE, gs.pid);
```

and in `nanobot_attach_running` it was requested never:

```c
HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                       FALSE, gs.pid);
```

This failed for two compounding reasons:

1. **The need cannot be predicted from the port comparison.** The mask was derived from whether the state file's port differed from the configured health port — a heuristic about *when the handle was opened*, not about *what the lifecycle will ever do with it*. But `nanobot_stop`'s tier-1 escalation runs `TerminateProcess(v.handle, 0)` on every tracked handle, port-matching or not. In the common port-matching case the published handle had no `PROCESS_TERMINATE`, so the tier-1 hard kill failed with `ERROR_ACCESS_DENIED`.
2. **The access denial was silently converted into a false success.** The stop flow ignored both results:

```c
TerminateProcess(v.handle, 0);          /* FALSE + ERROR_ACCESS_DENIED — unchecked */
WaitForSingleObject(v.handle, 3000);    /* WAIT_TIMEOUT after 3s — unchecked */
```

Flow continued to cleanup and reported success. Two independent bugs cooperated: the wrong mask caused the denial, and the unchecked results hid it. (The same silent pattern existed in all three tier-1 call sites — the escalation path, the confirm-or-escalate path, and the no-CLI fallback inside `nanobot_stop`.)

## Solution

Request `PROCESS_TERMINATE` unconditionally on the lifecycle-owned handle in both paths that publish it.

**`nanobot_start` (process.c) — before:**

```c
DWORD process_access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;
if (gs.port != health_port)
    process_access |= PROCESS_TERMINATE;
h = OpenProcess(process_access, FALSE, gs.pid);
```

**after:**

```c
/* PROCESS_TERMINATE is ALWAYS required: nanobot_stop's tier-1 hard
 * kill runs TerminateProcess on this published handle in the common
 * (port-matching) case too — without it Stop fails with
 * ERROR_ACCESS_DENIED while the UI still flips to IDLE. */
DWORD process_access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                       PROCESS_TERMINATE;
h = OpenProcess(process_access, FALSE, gs.pid);
```

**`nanobot_attach_running` (process.c) — before:**

```c
HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                       FALSE, gs.pid);
```

**after:**

```c
/* 3. OpenProcess with the rights the wait + query + stop paths need. */
HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                       PROCESS_TERMINATE, FALSE, gs.pid);
```

The comment on the `nanobot_start` mask is the load-bearing part: it states that `PROCESS_TERMINATE` is required by `nanobot_stop`'s tier-1 hard kill, which runs on the handle published here (and on the attach path) — so nobody "optimizes" it back to conditional.

Related same-session fix (same symptom family): the PID-reuse guard `g_state.create_ft` is now zeroed whenever the handle is cleared (`nm_state_clear()` and `nanobot_stop`'s cleanup path). Previously a stale creation time survived the handle close, so a later stop that adopted a state-file pid passed the OLD creation time as the expected identity, `kill_process_tree` logged "pid reuse suspected" and skipped `TerminateProcess` — again reporting success while the gateway survived. `nanobot_detect_running_port()` was also tightened to only suppress the configured-port probe when the state branch actually verified that port (`state_probed` flag).

Verified: `scripts\build.bat` rebuild OK; `ctest -C Release` 8/8 pass (`test_process` covers the state-machine and guard logic around these paths).

## Why This Works

- **OpenProcess access rights are fixed at open time.** A handle's granted access mask cannot be elevated later on the same handle — there is no "add PROCESS_TERMINATE to this handle" API. If the right was not requested when the handle was opened, the only remedy is closing it and reopening with a different mask. That is why the fix must live at the two open sites, not in `nanobot_stop`.
- **Every consumer of the published handle must be known when the mask is chosen.** `g_state.handle` is module state consumed by liveness watch, uptime, and stop — three call sites, one mask. The lifecycle owns stop, and stop's tier-1 escalation uses `TerminateProcess` on *every* tracked handle, so the mask must include every right the lifecycle will ever need. A mask derived from the current call path's *prediction* of need ("port mismatch → we might kill") instead of the state's *complete* consumer set is guaranteed to be wrong in the case the prediction misses.
- **Requesting `PROCESS_TERMINATE` up front is not a privilege-escalation risk here.** The manager is a per-user, non-elevated process; the gateway is its own child (or a same-user process latched via the state file). `OpenProcess` with `PROCESS_TERMINATE` succeeds for any process the user's token could already kill via Task Manager; requesting the right on a handle the process would request on the kill path anyway only moves the grant earlier. The minimal-rights principle still applies — but at the level of *not* requesting rights the lifecycle genuinely never uses (e.g. `PROCESS_VM_READ`, `PROCESS_CREATE_THREAD`), not at the level of rationing a right a first-class consumer demonstrably needs.

## Prevention

1. **Derive the access mask from ALL consumers of shared state, not the current call path.** When publishing a long-lived handle into guarded module state (here `g_state.handle`), enumerate everything that will ever read that field — watch, uptime, stop escalation — and union their required rights into the single mask at open time. A comment at the open site naming the consumer that forces each right (as the fix does) keeps future edits from re-narrowing it.
2. **Check and log `TerminateProcess`/`WaitForSingleObject` results in stop flows** so access denial cannot masquerade as success (recommended follow-up). The tier-1 call sites in `nanobot_stop` still ignore both return values; logging `GetLastError()` on FALSE and distinguishing `WAIT_TIMEOUT` from `WAIT_OBJECT_0` before cleanup would have surfaced this bug on day one and would catch any future mask regression.
3. **The stop-correctness test family in `tests/test_process.c` guards the pid-reuse guard behavior** — the zeroing of `g_state.create_ft` on handle clear, so a stale creation time can no longer make a later stop skip the kill while reporting success. Extend this family when touching stop-flow identity or handle-lifetime logic.

## Related Issues

- `docs/solutions/architecture-patterns/state-driven-gateway-lifecycle.md` — the state-machine lifecycle (start/attach/stop tiers) whose published handle is the subject of this fix.
- `docs/solutions/runtime-errors/uptime-from-process-creation-time.md` — the PID-reuse guard built on process creation time, whose stale-value variant produced the same "stop reports success, gateway survives" symptom.
