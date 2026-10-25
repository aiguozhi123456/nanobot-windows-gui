---
title: "Second launch pops \"already running\" — single-instance mutex outlives the window it protects"
date: 2026-10-25
category: logic-errors
module: nanobot-manager/main
problem_type: logic_error
component: single_instance
symptoms:
  - "Quitting the manager and immediately clicking the app icon again pops a MessageBox: nanobot-manager 已经在运行 / is already running — while no window can be surfaced"
  - "The relaunch exits after the popup; the user must click the icon a second time to actually start the manager"
  - "Launching a second copy while the first is still cold-starting (before its window exists) hits the same popup instead of waiting and activating the first instance"
  - "Mirror race: a relaunch landing between WM_CLOSE-accepted and window-destroyed activates the dying window, exits, and leaves nothing on screen"
root_cause: lifetime_mismatch
resolution_type: code_fix
severity: medium
related_components:
  - "nanobot-manager/main (WinMain shutdown sequence)"
tags:
  - single-instance
  - mutex
  - createMutex
  - named-event
  - findWindow
  - window-lifetime
  - shutdown-race
  - multi-process
  - activation
---

# Second launch pops "already running" — single-instance mutex outlives the window it protects

## Problem

Quit the manager (tray → Quit), then immediately click the app icon. Instead of the manager starting, a MessageBox appeared — "nanobot-manager 已经在运行。 / is already running." — and the new launch exited; the user had to click again. From the user's perspective this is a multi-process race: the dying instance and the fresh launch disagree about whether "an instance is running."

## Symptoms

- Quit → relaunch within the shutdown-cleanup window → false "already running" popup, new process exits.
- Second launch during the first instance's cold start (config read, credential migration, window creation) → same popup instead of waiting and activating the first instance once its window appears.
- Mirror race (found by stress-testing the fix): a second launch landing after WM_CLOSE was accepted but before the window was destroyed would activate the dying window and exit — leaving nothing on screen.

## Root Cause

Two lifetimes were mismatched, and the probe used the wrong authority:

- **Mutex lifetime = process lifetime.** `single_instance_begin` created `Local\nanobot-manager-single-instance` and intentionally *leaked* the owning handle, so the kernel only destroyed the name when the process fully exited — after the entire WinMain cleanup tail (`chat_shutdown` joining the chat lifecycle thread, `nm_state_clear` draining `RegisterWait`, `ui_page_destroy` tearing down QuickJS, CRT exit).
- **Window lifetime = ends at DestroyWindow**, at the *start* of that same exit sequence (`WM_CLOSE` → `DefSubclassProc` → `ui_run()` returns).

Between those two moments the mutex existed but `FindWindowW(L"UiCore_Window", L"nanobot manager")` found nothing. The second instance's logic was: mutex taken + no window → "already running" → MessageBox → exit. The same misdiagnosis applied during cold start, before the window exists.

The deeper mistake: **FindWindow (window presence) was the only surfaceability probe**, and window destruction can lag arbitrarily behind quit-commit when the UI thread is busy (observed >300 ms on a freshly started instance).

## Solution

Three coordinated changes in `main.cpp`. The "already running" MessageBox is **deleted** — product decision: a second launch never pops UI; it either surfaces the existing instance or becomes the instance.

**1. Exit side — release the mutex as soon as the UI is gone.** The owning handle is stored in `g_single_instance_mutex` and closed immediately after `ui_run()` returns, *before* the cleanup tail. Closing the only handle releases ownership and destroys the name object, so a relaunch can take over within milliseconds even though the process lives on for the (slow) cleanup. The brief overlap is safe: the log opens with `_wfopen(L"ab")` (CRT default deny-none sharing), config is not written during cleanup, and `nm_state_clear` never kills the gateway.

**2. Owner side — a named event is the surfaceability authority.** The mutex owner creates manual-reset `Local\nanobot-manager-ui-surface`, `SetEvent`s it once its window and activate route (subclass) exist, and — the load-bearing part — `ResetEvent`s it **the instant quit commits**: in `WM_CLOSE` handling that will destroy (the tray-hide branch returns earlier) and in `WM_DESTROY`. Resetting at quit-commit rather than at destruction makes the signal independent of how slow teardown runs.

**3. Launch side — activate-or-take-over retry loop.** When `CreateMutexW` reports `ERROR_ALREADY_EXISTS` (non-autostart), the second launch waits (100 ms steps, ~10 s budget, up to 4 takeover attempts) for one of three outcomes:

1. **Surfaceable owner** — event signaled AND `FindWindowW` resolves the HWND → `PostMessageW(prev, g_msg_activate)`; after a short grace, re-check event + window; confirmed → exit silently (the existing instance comes forward). An unconfirmed target was committing to quit → keep waiting.
2. **Mutex object vanished** (`OpenMutexW` fails) — the previous instance is gone → loop back and retry `CreateMutexW(NULL, TRUE, ...)`; on success this process owns the mutex and starts normally. Retrying the acquisition (rather than just proceeding) preserves the invariant that a third launch still sees exactly one owner.
3. **Budget exhausted** (a live but long-windowless instance — always stuck behind some visible dialog) → exit silently. No popup, ever.

Owners from pre-event builds create no event object; for them the probe falls back to FindWindow alone (cross-version upgrade scenario). The `--autostart` second-launch path is unchanged: silent exit before any probing.

Verified: `scripts\build.bat` rebuild OK; `ctest -C Release` 8/8; manual matrix — activate against live instance (~0.5 s exit, window surfaces), `--autostart` second launch (~60 ms silent exit), hard-kill + immediate relaunch (takeover), graceful quit + immediate relaunch ×8 stress cycles (takeover every cycle, exactly one surfaceable instance, stragglers fully drained), old-binary owner + new second launch (legacy fallback activates, ~1.4 s).

## Why This Works

- **The mutex's job ends when the UI it guards is gone.** Ownership exists to make "exactly one window" true; once the window is destroyed mid-exit, holding the name only blocks a legitimate successor. Releasing at the DestroyWindow boundary aligns the mutex lifetime with the window lifetime instead of the process lifetime.
- **An explicit signal beats inferring state from destruction.** "Window still exists" was a guess about a live instance; a manual-reset event set at window creation and reset at quit-commit is an authoritative statement of intent, immune to teardown latency in either direction (slow destroy can no longer fake "alive"; slow cleanup can no longer fake "running" via the mutex).
- **Re-acquiring on takeover keeps single-instance semantics under races.** After observing the name vanish, a plain "start anyway" would run unlocked; N waiting launches could then all start. Looping back into `CreateMutexW` means exactly one wins and the rest fall back into activate-or-wait.
- **Every state of the previous instance has an answer.** Window alive → activate. Quit committed → event reset → wait → mutex vanishes (released first thing at teardown) → take over. Cold start → wait → event set → activate. Live-but-windowless (error dialog) → silent exit; the dialog is already on screen.

## Prevention

- **When a kernel object's visibility gates a UI decision, check the *UI's own* declared state, not object existence or window destruction timing.** Three probes were tried in sequence during this fix: mutex-exists (wrong — outlives the window), FindWindow (wrong — destruction lags quit-commit), event-set-at-commit (right). The pattern generalizes: publish intent explicitly at state transitions instead of inferring it from resource lifetimes.
- **Never leak a handle whose ownership lifetime is supposed to mean something narrower than process exit.** The old comment ("intentionally leaked: the OS releases it on process exit, which is exactly the ownership lifetime we want") documented the wrong lifetime as the desired one — comments stating invariants get copied forward, so they must state the *correct* invariant.
- **Second-launch UX rule for this app: no popups, ever.** Surface the existing window, become the instance, or stay silent. `single_instance_begin` is the only place that decides this; don't reintroduce an "inform the user" dialog there.
- **Test single-instance changes with an adversarial quit→relaunch loop**, asserting "exactly one *surfaceable* instance" (windowed), not "exactly one process": a dying instance legitimately remains a process for its cleanup tail. In this codebase that tail is ~10 s because `chat_shutdown` waits `INFINITE` on the chat lifecycle thread, which first finishes a WinHTTP operation with 5 s resolve + 5 s connect timeouts — pre-existing, unrelated to the mutex protocol (the mutex is released before it), but worth its own bounded-wait fix someday.

## Related Issues

- `docs/solutions/design-patterns/win32-tray-app-activation-and-focus.md` — the activation semantics (`surface_main_window`, no focus stealing) the retry loop relies on.
- `docs/solutions/architecture-patterns/state-driven-gateway-lifecycle.md` — why manager exit deliberately leaves the gateway running (`nm_state_clear`, not `nanobot_stop`), which is what makes the quit→relaunch path a hot path in the first place.
