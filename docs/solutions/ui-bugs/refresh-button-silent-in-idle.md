---
title: "Refresh button does nothing in IDLE — polling-state guard rejects user-initiated probes"
date: 2026-07-31
category: ui-bugs
module: nanobot-manager/main
problem_type: ui_bug
component: tooling
severity: medium
symptoms:
  - "Clicking the refresh button while the manager is in IDLE or DEAD does not update gateway status"
  - "The health probe runs but its result is not posted to the UI thread"
  - "The 15 s steady-state polling cadence eventually catches up, so the symptom is 'delayed' rather than 'broken'"
  - "Button stays visually enabled (`canRefresh = 1`), so a click that does nothing looks like a broken control"
root_cause: logic_error
resolution_type: code_fix
tags:
  - health-worker
  - btn-refresh
  - state-guard
  - first-iteration-bypass
  - core-ui
  - nanobot-manager
---

# Refresh button does nothing in IDLE — polling-state guard rejects user-initiated probes

## Problem

Clicking the new `btn_refresh` while the manager believes the gateway is `IDLE`/`DEAD` does not update the UI — the kicked `health_worker` probes once and silently exits before posting, because the polling-state guard bails on non-`STARTING`/`RUNNING`/`RESOLVING` states.

## Symptoms

- Manager shows "已停止 / Stopped" (state = `IDLE`/`DEAD`). User clicks 刷新. Nothing happens. Status stays "stopped".
- Reproducible after the user manually runs `nanobot gateway` from a terminal while the manager is open: manager still says IDLE, refresh button is enabled, click does nothing.
- The 15 s steady-state polling cadence eventually catches up — the user sees "running" without doing anything, only if they wait up to 15 s.
- The button is visibly enabled (`canRefresh = 1`), so a click that does nothing looks like a broken control rather than a no-op.

## What Didn't Work

1. **Move the state-bail check AFTER `PostMessageW`.** Would deliver results in every state, including `STOPPING`. A worker that happens to land in `STOPPING` could post a stale `running = 1` and undo an in-progress `stop_worker`. The M8 allow-list restriction to `IDLE`/`DEAD` on the first iteration only is what keeps that invariant.
2. **Bump `nm_state_set_state(NM_STATE_RESOLVING)` on click.** Passes the existing guard without touching its logic, but pollutes the state machine with a UI-driven transient and forces every UI consumer of state to ignore transient `RESOLVING` flips. Heavier than needed.
3. **Naive `kick_health_worker` from the click handler with no worker change.** Worker was spawned, gen check passed, probe completed, then bailed at the state-bail check before posting. The button fired; the UI never received anything. The M8 first-iteration bypass is what makes the click deliver.

## Solution

Two coordinated changes — the worker fix and a UI affordance so the no-op state is visible instead of looking broken.

### A. `health_worker` first-iteration bypass (`nanobot-manager/src/main.cpp`)

The order inside the worker loop is: probe → gen-bail check → state-bail check → malloc + `PostMessageW`. The state-bail check returns early when `state` is not in `{STARTING, RUNNING, RESOLVING}`, so a fresh worker kicked from `IDLE`/`DEAD` posts nothing.

BEFORE (M7):

```c
for (;;) {
    int running = nanobot_detect_running_port(configured_port, &actual_port);
    if (InterlockedCompareExchange(&g_health_gen, my_gen, my_gen) != my_gen)
        return 0;
    nm_state s = nm_state_get();
    if (s != NM_STATE_STARTING && s != NM_STATE_RUNNING && s != NM_STATE_RESOLVING)
        return 0;                                       // bail BEFORE post
    /* ...post result... */
}
```

AFTER (M8 — bypass on first iteration only, restricted to `IDLE`/`DEAD`):

```c
int first_iteration = 1;
for (;;) {
    int running = nanobot_detect_running_port(configured_port, &actual_port);
    if (InterlockedCompareExchange(&g_health_gen, my_gen, my_gen) != my_gen)
        return 0;
    nm_state s = nm_state_get();
    int allow_post;
    if (first_iteration) {
        first_iteration = 0;
        allow_post = (s == NM_STATE_IDLE || s == NM_STATE_DEAD)
            || (s == NM_STATE_STARTING || s == NM_STATE_RUNNING || s == NM_STATE_RESOLVING);
    } else {
        allow_post = (s == NM_STATE_STARTING || s == NM_STATE_RUNNING || s == NM_STATE_RESOLVING);
    }
    if (!allow_post) return 0;
    /* ...post result... */
}
```

`kick_health_worker` is unchanged: it always bumps `g_health_gen` and passes the new value to its worker. The gen check is evaluated *before* the state guard, so a superseded worker still bails at the gen check — never reaching the bypassed state guard. A user click that supersedes a boot-phase worker does not double-post: the old worker exits at the gen check, the new worker posts once, then continues polling under the standard guard.

### B. `btn_refresh` mount + click handler

```c
static void on_refresh(UiWidget w, void* ud) {
    (void)w; (void)ud;
    kick_health_worker((HWND)ui_window_hwnd(g_win));
}

static void on_btn_refresh_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_page_set_bool(g_page, "canRefresh", 1);
    ui_widget_on_click(w, on_refresh, NULL);
}

ui_page_on_widget_mount(g_page, "btn_refresh", on_btn_refresh_mount, NULL);
```

### C. `set_status` three-state `canRefresh`

```c
if (strcmp(status, "running") == 0) {
    ui_page_set_bool(g_page, "canStart", 0);
    ui_page_set_bool(g_page, "canStop", 1);
    ui_page_set_bool(g_page, "canRefresh", 1);
} else if (strcmp(status, "starting") == 0 || strcmp(status, "stopping") == 0) {
    /* M8: also disable refresh during transitions -- the worker guard
     * would no-op a click here anyway, but a disabled button is clearer. */
    ui_page_set_bool(g_page, "canStart", 0);
    ui_page_set_bool(g_page, "canStop", 0);
    ui_page_set_bool(g_page, "canRefresh", 0);
} else {
    ui_page_set_bool(g_page, "canStart", 1);
    ui_page_set_bool(g_page, "canStop", 0);
    ui_page_set_bool(g_page, "canRefresh", 1);
}
```

### D. UI (`nanobot-manager/ui/dashboard.uix`)

Reactive bool in `data()`:

```js
canRefresh: true,
```

`.btn-refresh` CSS (secondary background, `disabled` state matches other action buttons). The button markup uses the existing `.btn` base and adds a refresh-arrow SVG + the `$t('btn_refresh')` label:

```xml
<div id="btn_refresh" class="btn btn-refresh" :enabled="canRefresh">
  <svg width="12" height="12" viewBox="0 0 24 24">
    <path fill="currentColor" d="M17.65 6.35A7.95 7.95 0 0012 4a8 8 0 107.75 10h-2.1A6 6 0 1112 6c1.66 0 3.14.69 4.22 1.78L13 11h7V4z"/>
  </svg>
  <label class="btn-label">{{ $t('btn_refresh') }}</label>
</div>
```

### E. i18n

Both zh/en blobs gain one key, placed alongside `btn_start` / `btn_stop`:

```
btn_refresh=刷新
btn_refresh=Refresh
```

## Why This Works

**`IDLE`/`DEAD`-on-first-iter-only is safe.** The reason the original guard exists is to keep a worker that is mid-shutdown from posting `running = 1` and undoing `stop_worker`. A user-initiated click is the *opposite* of that scenario: it kicks a brand-new worker, not a stale one. `first_iteration` is reset on entry, so a `kick_health_worker` from a click during `IDLE` posts exactly one result (the one the user asked for) and then settles into the original allow-list.

**Subsequent-iteration behaviour is unchanged.** `first_iteration` is set to `0` on the first iteration's entry, so iterations 2..N use the original allow-list (`STARTING || RUNNING || RESOLVING`) verbatim. The M7 stale-bail invariant and the `STOPPING` guard are unaffected.

**`STOPPING` is not bypassed.** `STOPPING` is in *neither* set of the `first_iteration` allow-list. So a click during `STOPPING` still bails at the guard, the button is disabled by `set_status` during transitions, and a stale worker cannot undo an in-progress stop.

**Generation ordering preserves the M7 invariant.** The gen check is evaluated *before* the state guard, so even on the first iteration a superseded worker bails at the gen check, never reaching the bypassed state guard. A click that supersedes a boot-phase worker does not double-post.

**`canRefresh` makes the no-op visible.** During `STARTING`/`STOPPING` the button is greyed out, so the user sees that the click would not have done anything instead of thinking the control is broken. The disabled style reuses the existing `.btn-refresh:disabled` rule (matches the surrounding `.btn-*` disabled palette).

## Prevention

- **Doc comment in `health_worker` (already present).** The M8 block comment in `health_worker` names the invariant in prose: "first iteration only, restricted to IDLE/DEAD, does not bypass STOPPING". Any future change to the guard's allow-list or to `first_iteration` must keep that comment accurate.
- **Worker-level rule of thumb.** Any polling worker that gates on UI state and is also kicked by a user-initiated UI affordance needs the same `first_iteration` treatment. The rule: "if the worker can be triggered from a UI affordance, its first iteration must be permitted in states where the UI affords the trigger." The relevant kicker in this codebase is `kick_health_worker`; both call sites are `kick_resolve_worker`'s startup path (which is `RESOLVING`, already in the allow-list) and `on_refresh`. Future new kickers need an audit.
- **Test idea (positive).** Drive a script that starts the manager while the gateway is NOT running, asserts `status == "stopped"`, invokes `btn_refresh` via the same `ui_widget_on_click` path the UI uses, and asserts a `WM_ASYNC_HEALTH_RESULT` arrives within ~50 ms. This would have caught the original symptom deterministically without waiting 15 s for steady state.
- **Test idea (negative).** Click `btn_refresh` during a programmatic `STOPPING` transition (force `nm_state_set_state(NM_STATE_STOPPING)` from a test hook) and assert no `WM_ASYNC_HEALTH_RESULT` arrives. This is the invariant the bypass is designed not to break.

## Related Issues

- [`state-driven-gateway-lifecycle.md`](../architecture-patterns/state-driven-gateway-lifecycle.md) — the existing health-worker description (around the "Separate death events from health state" section) predates M8 and does not yet mention the `first_iteration` bypass or the `STOPPING`-guard interaction. That page now carries a new subsection "First-iteration bypass for user-initiated probes" describing the M8 refinement; cross-link from there and back here.
- [`nanobot-gateway-overview.md`](../reference/nanobot-gateway-overview.md) — useful for the polling cadence baseline (15 s steady state, boot-phase backoff) that this fix supplements, and for the upstream `/health` endpoint the probe targets.
- [`native-websocket-chat-integration.md`](../architecture-patterns/native-websocket-chat-integration.md) — adjacent system; chat auto-connects from `apply_health_result` when a probe flips the gateway to `RUNNING`. The M8 bypass is what makes that auto-connect reachable after a `btn_refresh` click during `IDLE`.
- [`core-ui-input-read-pattern.md`](../design-patterns/core-ui-input-read-pattern.md) — the "Wire the button through a mount hook, not a one-shot find" rule is exactly what `btn_refresh`'s `on_btn_refresh_mount` follows.