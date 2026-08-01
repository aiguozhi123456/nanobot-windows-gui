---
title: "Rejected approach: console CTRL_BREAK_EVENT shutdown for the gateway (decision history)"
date: 2026-08-01
category: design-patterns
module: nanobot-manager/process
problem_type: design_pattern
component: service_object
severity: high
related_components:
  - nanobot-manager/main
applies_when:
  - "Considering GenerateConsoleCtrlEvent / CTRL_BREAK_EVENT to stop a child process spawned with CREATE_NO_WINDOW or CREATE_NEW_PROCESS_GROUP"
  - "Wondering why nanobot_stop uses a gateway CLI subcommand instead of a Win32 console signal"
  - "Reviewing git history and finding commit 3553558 (CTRL_BREAK_EVENT) that no longer matches the code"
  - "Designing graceful shutdown for a detached, no-window background process"
tags: [shutdown, ctrl-break-event, generateconsolectrlevent, console-control, detached-process, decision-history, rejected-approach, win32, c]
---

# Rejected approach: console CTRL_BREAK_EVENT shutdown for the gateway (decision history)

## Context

This is a **decision-history** record, not how-to. It documents an approach that was implemented (`3553558` — *"feat: graceful nanobot shutdown via CTRL_BREAK_EVENT"*) and later **rejected and replaced** by the control-plane shutdown in `state-driven-gateway-lifecycle.md`. It exists so a future contributor who rediscovers the "obvious" Win32 console-signal route can see that it was already tried, why it failed, and what replaced it — rather than walking the same dead end again.

The current `nanobot_stop` (`src/process.c`) spawns `nanobot gateway stop --timeout 20` against the state-record rendezvous, with `TerminateProcess`/process-tree kill as the escalating fallback. There is **no** `CTRL_BREAK_EVENT` / `GenerateConsoleCtrlEvent` anywhere in the codebase today (`grep -rn "CTRL_BREAK\|GenerateConsoleCtrl" src/` returns nothing). This doc explains the gap.

## Guidance

### What the rejected approach did

The original graceful-shutdown attempt (`3553558`) reasoned: the gateway is a child process; Win32 gives us `CTRL_BREAK_EVENT` to signal a graceful shutdown to a process group; that's the "proper" alternative to a hard `TerminateProcess`. The implementation:

1. Launch the gateway with `CREATE_NEW_PROCESS_GROUP` (instead of `DETACHED_PROCESS`) so the child is the root of a group that `GenerateConsoleCtrlEvent` can target.
2. Retain the child's `HANDLE`.
3. On stop: `FreeConsole()` → `AttachConsole(child_pid)` → `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, child_pid)` → `WaitForSingleObject(handle, 5000)`, with `kill_process_tree` as the 5-second escalation.

The `FreeConsole`/`AttachConsole` pairing matters: **a process can be attached to at most one console at a time**, so the manager must free its own console before attaching to the child's to send the break. That ordering is correct and reusable — it's not why the approach was rejected.

### Why it was rejected

The fatal problem is the one-line conclusion in `state-driven-gateway-lifecycle.md`: **"`CTRL_BREAK_EVENT` is not a reliable control plane for a detached, no-window process."** Concretely:

- **The gateway runs with `CREATE_NO_WINDOW` and no console.** A process without a console has nothing for `AttachConsole` to attach to and nothing to receive the break. The signal depends on a console existing; the whole product configuration removes it.
- **`CTRL_BREAK_EVENT` is delivered to every process in a process group that shares a console**, which is the wrong granularity for a gateway that may be shared across manager launches (the manager is *not* the only thing that cares about this process). A control-plane stop via the gateway's own CLI targets exactly one instance by identity.
- **Reliability across launch shapes.** The gateway can be launched as `nanobot.exe gateway --background` or `python -m nanobot gateway --background`; whether `CTRL_BREAK_EVENT` is even reachable depends on the launcher's console handling, which the manager does not control. The control-plane approach is uniform across both launch shapes.
- **Identity.** Console-control signals a *process group*, not a *gateway instance*. The whole point of the Gateway State Record + Gateway Process Identity (PID + creation `FILETIME`) model is to target one specific instance and refuse to act on a recycled PID. Console signaling can't carry that identity.

### What replaced it

`nanobot_stop` now does control-plane-first, escalating only on failure:

1. Reconcile the target PID (prefer the tracked gateway PID; adopt a state-record PID if the manager never tracked one).
2. Spawn `nanobot gateway stop --timeout 20` (or `python -m nanobot gateway stop --timeout 20`). The state file written by `gateway --background` is the rendezvous; the CLI subcommand is nanobot's own graceful-stop contract.
3. Capture stdout and match the result tokens (`gateway stopped.`, `gateway_not_running`, `gateway_stop_timeout`, `gateway_state_stale`).
4. Escalate to `TerminateProcess` + `kill_process_tree` (with the PID-reuse guard) only if the control-plane stop fails or times out.

The deep difference: the gateway itself decides how to shut down gracefully (draining sessions, writing final state), and the manager simply *asks* it via its published CLI. Console signals try to make the manager the arbiter of a process it doesn't own.

### The reusable kernel that survived

Two pieces of the rejected approach are still good Win32 knowledge, just not the right tool *here*:

- **`FreeConsole` → `AttachConsole(pid)` → `GenerateConsoleCtrlEvent`** is the correct sequence for sending a console signal to another process's console, and the one-console-per-process constraint is real. This is the pattern to use for *console-attached* children (e.g. a foreground dev helper you spawned with a visible console).
- **`CREATE_NEW_PROCESS_GROUP`** is the right way to make a child the root of a signalable group — but only meaningful if that group has a console. For no-window children it's inert.

## Why This Matters

- **Without this record, someone will re-invent the CTRL_BREAK route.** It's the first Google result for "graceful shutdown child process Win32," and commit `3553558` shows it was once the implemented solution. A contributor "simplifying" the current `gateway stop` spawn back to a console signal would reintroduce a regression that already took one debugging cycle to escape.
- **The lifecycle doc asserts the conclusion; this doc grounds it.** `state-driven-gateway-lifecycle.md` says console control "is not a reliable control plane" in one line. That's correct but unconvincing to someone who hasn't seen it fail. This doc is the "we tried it, here's specifically why" companion.
- **The rejection is load-bearing for the identity model.** Targeting a process *group* by signal is fundamentally incompatible with targeting a *gateway instance* by PID+creation-time. The choice of control-plane stop is part of what makes the Gateway Process Identity model coherent.

## When to Apply

- **Reading `nanobot_stop` and wondering where the console signal went.** It's not an oversight; it's a documented rejection. Leave the `gateway stop` spawn as-is.
- **Designing shutdown for a detached, no-window background process.** Prefer the process's own control-plane (CLI subcommand, IPC, HTTP) over Win32 console signals. Reserve `CTRL_BREAK_EVENT` for console-attached children you spawned for interactive/dev purposes.
- **Reviewing a PR that reintroduces `CTRL_BREAK_EVENT`.** Point here, then at the lifecycle doc. The burden is on the change to show the no-window console problem is solved.
- **You genuinely need console-control shutdown elsewhere.** Then the `FreeConsole`/`AttachConsole`/`GenerateConsoleCtrlEvent` ordering above is the correct sequence — but verify the target actually owns a console.

## Examples

### What the current code does (control-plane stop, abridged)

```c
/* src/process.c — nanobot_stop */
wchar_t* cmdline = build_command_line(wide_exe,
    resolved_is_python ? L" -m nanobot gateway stop --timeout 20"
                       : L" gateway stop --timeout 20");
CreateProcessW(NULL, cmdline, ..., CREATE_NO_WINDOW, ...);   /* ask the gateway to stop itself */
WaitForSingleObject(stop_pi.hProcess, 25000);
/* match stdout tokens; escalate to TerminateProcess + kill_process_tree on failure/timeout */
```

### What NOT to reintroduce (the rejected approach)

```c
/* DO NOT DO THIS for the no-window gateway. Kept here only as decision history. */
FreeConsole();
AttachConsole(child_pid);
GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, child_pid);   /* unreliable: no console to receive it */
WaitForSingleObject(handle, 5000);
/* fallback: kill_process_tree */
```

## Related

- `docs/solutions/architecture-patterns/state-driven-gateway-lifecycle.md` — the current shutdown model; its "What Did Not Work" section is the one-line conclusion this doc grounds. Cross-link: that doc is the *what*, this doc is the *why-not-the-obvious-thing*.
- Commit `3553558` — the original `CTRL_BREAK_EVENT` implementation (`feat: graceful nanobot shutdown via CTRL_BREAK_EVENT`).
- `nanobot-manager/src/process.c` — `nanobot_stop` (~line 1690): the `gateway stop --timeout 20` spawn, stdout-token matching, and `kill_process_tree` escalation with the PID-reuse guard.
- `CONCEPTS.md` → **Gateway Process Identity** — why targeting an *instance* by PID+creation-time, not a group by signal, is the contract the stop path must honor.
