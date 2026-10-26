---
title: "Measure gateway uptime from the process's creation time, not from GUI detection time"
date: 2026-08-01
last_updated: 2026-10-27
category: runtime-errors
module: nanobot-manager/process
problem_type: runtime_error
component: service_object
severity: medium
symptoms:
  - "Uptime reads near 00:00 for a gateway the manager attached to rather than started, even though the gateway has been running for hours"
  - "Uptime resets to 00:00 whenever the manager is closed and reopened, because detection time is recomputed on each attach"
  - "After a manager restart, a long-running gateway shows a tiny uptime that climbs from there, misleading the user about true gateway lifetime"
root_cause: async_timing
resolution_type: code_fix
related_components:
  - nanobot-manager/main
tags: [uptime, getprocesstimes, openprocess, process-identity, gettickcount, cross-process, win32, c]
---

# Measure gateway uptime from the process's creation time, not from GUI detection time

## Problem

The dashboard's gateway uptime counter was derived from the moment the **manager** detected the gateway (`GetTickCount64()` at detection time stored as the start tick), not from when the **gateway process** actually started. That is wrong whenever the manager attaches to an already-running gateway — the autostart path, the "close and reopen the manager" path, and the "gateway started elsewhere" path all attached to a long-running process and then displayed a near-zero uptime that climbed from the *attach* moment.

The fix measures the real gateway lifetime from the process's own creation time via `GetProcessTimes`, so uptime is correct regardless of whether the manager or something else started the gateway.

## Symptoms

- A gateway that has been running for 3 hours shows `00:00` (or a few seconds) immediately after the manager launches and attaches to it.
- Closing and reopening the manager resets the displayed uptime to `00:00`, even though the gateway never stopped.
- The "PID" and "Uptime" widgets disagree with reality: PID matches the real gateway process, but Uptime behaves as if the process just started.

## What Didn't Work

- **Storing detection-time `GetTickCount64()` as the start tick.** Correct only for the "manager started the gateway" path, because there the detection moment is ~the creation moment. It silently lies on every attach path — which is most of them once autostart and tray-resident usage are common.
- **Persisting the start tick across manager restarts.** Would fix the "reopen resets it" symptom but not the fundamental one (a first-time attach to a pre-existing gateway would still show a fabricated near-zero uptime). It also adds a persistence/ownership question that doesn't need to exist, because the process already remembers when it started.

## Solution

Read the gateway process's creation time from the OS and reconstruct the start tick as `now - process_uptime`:

```c
/* src/process.c */
int nanobot_get_process_uptime_seconds(int pid, unsigned long long* out_seconds) {
    if (pid <= 0 || !out_seconds) return 0;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);  /* fallback */
    if (!h)
        return 0;

    FILETIME create_time, exit_time, kernel_time, user_time, now_time;
    BOOL ok = GetProcessTimes(h, &create_time, &exit_time, &kernel_time, &user_time);
    CloseHandle(h);
    if (!ok)
        return 0;

    GetSystemTimeAsFileTime(&now_time);
    unsigned long long created = filetime_to_ull(create_time);
    unsigned long long now     = filetime_to_ull(now_time);
    *out_seconds = now > created ? (now - created) / 10000000ULL : 0;
    return 1;
}
```

The caller (`src/main.cpp`, `start_uptime_timer`) uses it to anchor the wall-clock start tick the periodic timer ticks against:

```c
static void start_uptime_timer(int pid) {
    ULONGLONG now = GetTickCount64();
    unsigned long long process_uptime = 0;
    if (nanobot_get_process_uptime_seconds(pid, &process_uptime))
        nm_state_set_start_tick(now - process_uptime * 1000ULL);  /* back-date to real start */
    else
        nm_state_set_start_tick(now);                              /* fallback: detect-time */
    /* ...SetTimer(TIMER_UPTIME, 1000, on_timer_uptime)... */
}
```

The 1-second `on_timer_uptime` then displays `(GetTickCount64() - start_tick) / 1000` formatted as `H:MM:SS` / `MM:SS`. Because `start_tick` is now back-dated to the real process creation moment, the display is correct on every path.

### Why `PROCESS_QUERY_LIMITED_INFORMATION` first

Open with `PROCESS_QUERY_LIMITED_INFORMATION` and fall back to `PROCESS_QUERY_INFORMATION`. The limited right is grantable on more processes (including some the manager didn't spawn) and is the least-privilege choice for a read-only time query; the broader right is the fallback for cases where the limited right isn't granted. `GetProcessTimes` works with either.

### `filetime_to_ull` — `FILETIME` is not a 64-bit int

`FILETIME` is a `DWORD dwHighDateTime`/`dwLowDateTime` pair. Cast it to a `ULONGLONG` *as a 100-ns interval* via `filetime_to_ull` before doing arithmetic, then divide by `10000000ULL` to get seconds. Treating a `FILETIME*` as `ULONGLONG*` directly is the classic alignment/strict-aliasing footgun; the helper does it safely.

## Why This Works

- **The process's creation time is the source of truth for its lifetime.** `GetProcessTimes` reports the kernel's record of when the process was created, which is exactly "how long has this gateway been running" — independent of who started it or when any observer first noticed it.
- **Reconstructing `start_tick = now - process_uptime*1000`** keeps the existing tick-based display code unchanged. The timer still does `(GetTickCount64() - start_tick)/1000`; only the *anchor* of `start_tick` is corrected. This is a one-site fix, not a re-architecture of the uptime widget.
- **The fallback to detect-time** (`else nm_state_set_start_tick(now)`) handles the rare case where the manager can't open the process for query (permissions, race where the pid already died). Degradation is graceful: the old (wrong-on-attach) behavior, never a crash.

## Prevention

- **When displaying any "age" of an external process, source it from that process, not from your observation of it.** This applies to uptime, "time since connected," "session age," etc. The detection-time pattern is the recurring trap; reach for `GetProcessTimes` (or the equivalent on the platform) first.
- **Pair this with the PID-reuse guard** used elsewhere in `process.c` (`kill_process_tree`'s `expected_create` check): the creation `FILETIME` is also the identity that proves a PID hasn't been recycled by the OS. The same `GetProcessTimes` read serves both "how old" and "is it still the same process" questions.
- **Test the attach path, not just the start path.** A unit/integration test that starts a gateway, *then* launches the manager to attach to it, would have caught this immediately. The start-only path looks fine because detection time ≈ creation time there.

## Related Issues

- `docs/solutions/architecture-patterns/state-driven-gateway-lifecycle.md` — the broader gateway-identity model (Gateway State Record, Gateway Process Identity via `FILETIME`); this uptime fix is one consumer of the same `GetProcessTimes` creation-time read that underpins PID-reuse protection.
- Commit `9572c05` — the original fix (`fix(uptime): measure from gateway process start time, not GUI detection time`).
- `nanobot-manager/src/process.c:520` — `nanobot_get_process_uptime_seconds`.
- `nanobot-manager/src/main.cpp:379` / `400` — `update_uptime` / `start_uptime_timer` consumers (`on_timer_uptime`, main.cpp:397, now also drives the chat UI tick).
