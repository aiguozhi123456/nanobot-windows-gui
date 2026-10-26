---
title: "Background memory trim: hand working-set pages back while hidden"
date: 2026-10-25
last_updated: 2026-10-27
category: architecture-patterns
module: nanobot-manager/app
problem_type: architecture_pattern
component: tooling
severity: low
applies_when:
  - "Observing high 'memory' for nanobot-manager.exe while it sits in the tray"
  - "Changing window visibility paths (hide to tray, minimize, restore)"
  - "Adding timers that tick while the window is hidden"
tags: [memory, working-set, SetProcessWorkingSetSize, tray, timers, wm_size, win32]
---

# Background memory trim: hand working-set pages back while hidden

## Context

The manager had no memory reclamation at all: hiding to tray is just
`ShowWindow(SW_HIDE)` (core-ui keeps the D2D/DComp machinery alive), nothing
ever called `SetProcessWorkingSetSize`, and hidden state still woke the UI
thread at 10 Hz — the 100 ms `TIMER_FIELD_DRIFT` tick (`DRIFT_TICK_MS = 100`)
early-returns when hidden, but the timer itself kept firing. (`TIMER_AMBIENT`
has since been deleted; the LED heartbeat it used to drive now rides the
drift tick.) For a tray-resident app that means Task Manager's memory column
never comes down after the user is done looking at the window.

## Mechanism (main.cpp)

Three cooperating pieces, all driven from window-visibility transitions:

1. **Trim on hide** — `on_window_hidden()` calls
   `SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1)`
   (the documented "remove as many pages as possible" form, equivalent to
   `EmptyWorkingSet`) and pauses the decorative tick (`TIMER_FIELD_DRIFT`)
   behind a `g_decor_paused` guard — `TIMER_AMBIENT` is deleted; the LED
   heartbeat now rides the drift tick as a true sine of `ambientGlow`
   (0.45..1.0, 2.8 s period), so pausing the one tick pauses both.
   Call sites: WM_CLOSE-to-tray branch, hotkey-hide branch, and
   `WM_SIZE/SIZE_MINIMIZED` in `subclass_proc`; plus once at startup when
   `--autostart` mounts the window unshown.
2. **Resume on show** — `on_window_shown()` re-arms the single drift timer
   (and schedules a backdrop re-kick via `backdrop_kick_schedule()`). It is
   called from `surface_main_window()` (the single show funnel) and from
   `WM_SIZE/SIZE_RESTORED|SIZE_MAXIMIZED`. Restarting is visually seamless:
   drift positions and the heartbeat phase are sines sampled from absolute
   time.
3. **Steady-state re-trim** — `TIMER_BG_TRIM` (60s) fires forever but acts
   only while `!IsWindowVisible || IsIconic`, so pages that fault back in
   while backgrounded (health poll, chat stream) are handed back on cadence
   with no start/stop management.

## What intentionally keeps running while hidden

- `TIMER_CHAT_UI` (66ms) and `TIMER_UPTIME` — a generation streaming into the
  hidden window must keep draining; stopping them would stall background
  output.
- The WebSocket connection, chat history (`g_messages`), and
  `g_new_history_backup` — disconnecting/freeing would abort streaming turns
  and break `/new` rollback semantics, for little RAM (the big consumer is
  the UI runtime itself, which the working-set trim already pages out).
- `TIMER_PULSE` needs no handling (self-kills after 3 ticks).

## Pitfalls

- The `WM_SIZE` branch in `subclass_proc` is observation only — it must fall
  through to `DefSubclassProc` or core-ui loses resize/layout handling.
- Working set ≠ commit: the trim drops the Task Manager "Memory" column but
  committed bytes stay; pages return via soft faults on demand. Verify with
  the private-working-set column, not by expecting the commit size to move.
- Don't trim on a visible-window path: trimming while shown just evicts pages
  that immediately fault back (visible stutter for zero benefit).
- When adding timers, delta-skip their outputs like the drift tick does: it
  pushes a `ui_page_set_float` only when a position moved more than 0.35 DIP
  since the last push, so a near-stationary sample doesn't buy a wasted paint
  pass (each tick still pays a full-window repaint).
