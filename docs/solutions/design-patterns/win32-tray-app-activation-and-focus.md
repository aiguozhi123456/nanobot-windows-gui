---
title: "Win32 tray-app window activation, global hotkey, and deferred widget focus"
date: 2026-08-01
category: design-patterns
module: nanobot-manager/window-management
problem_type: design_pattern
component: tooling
severity: medium
applies_when:
  - "Building a Win32 tray app that must summon its hidden window from a background context (tray click, global hotkey)"
  - "Using core-ui (or any message-pump framework) with a subclassed WndProc for tray/hotkey message routing"
  - "Need to focus a UIX text-input widget after programmatically switching pages where v-if remounts the DOM asynchronously"
  - "RegisterHotKey/UnregisterHotKey lifecycle management alongside window creation and destruction"
tags:
  - win32
  - setforegroundwindow
  - attachthreadinput
  - registerhotkey
  - wm-hotkey
  - subclass-proc
  - tray-icon
  - sw-restore
  - deferred-focus
  - v-if-remount
  - core-ui
---

# Win32 tray-app window activation, global hotkey, and deferred widget focus

## Context

The nanobot-manager is a native Win32/C++ app built on the `core-ui` submodule. It runs
as a system-tray app: clicking X hides the window instead of closing, and the user
summons it back via tray double-click. A later feature added a global hotkey
(Ctrl+Shift+N) to do the same, plus toggle-hide.

Three Win32 surprises surfaced during that work, each independently blocking the
feature until identified and fixed.

## Guidance

### 1. Use SW_RESTORE, not SW_SHOWNORMAL, for hidden-or-minimized windows

**Problem:** `ShowWindow(hwnd, SW_SHOWNORMAL)` only works if the window is hidden
(`SW_HIDE`). It does **not** restore a window that was minimized via `ShowWindow(hwnd,
SW_MINIMIZE)` or the user clicking the taskbar minimize button. The window stays
invisible and the user thinks the hotkey "does nothing."

**Fix:** Use `SW_RESTORE` — it restores a minimized window to its previous size and
position, and it also shows a hidden window. It is the correct call for any window
that might be in either state.

```c
/* Before */
ShowWindow(g_hwnd, SW_SHOWNORMAL);

/* After */
ShowWindow(g_hwnd, SW_RESTORE);
```

### 2. Use AttachThreadInput to bypass the foreground-lock on SetForegroundWindow

**Problem:** Windows enforces a foreground-lock: `SetForegroundWindow` is silently
ignored when called from a thread that does not own the foreground. This is the
default case for tray-click and global-hotkey activation — both originate from a
background context. The result is the same symptom as above: the window is restored
but stays behind other windows.

**Fix:** Temporarily attach the current thread's input queue to the foreground
thread's queue via `AttachThreadInput`. While attached, `SetForegroundWindow` and
`SetActiveWindow` succeed. Detach immediately after to restore isolation.

```c
HWND fg = GetForegroundWindow();
int attached = 0;
if (fg && fg != g_hwnd) {
    DWORD fg_tid = GetWindowThreadProcessId(fg, NULL);
    if (fg_tid != GetCurrentThreadId()) {
        AttachThreadInput(GetCurrentThreadId(), fg_tid, TRUE);
        attached = 1;
    }
}
SetForegroundWindow(g_hwnd);
SetActiveWindow(g_hwnd);
if (attached) {
    AttachThreadInput(GetCurrentThreadId(),
                      GetWindowThreadProcessId(GetForegroundWindow(), NULL),
                      FALSE);
}
```

### 3. Use a deferred timer for focus when UIX v-if remounts the DOM

**Problem:** When a UIX page switch changes `page` from `"settings"` to `"chat"`, the
`<div v-if="page=='chat'">` block is destroyed and recreated. `ui_widget_find_by_id`
called immediately after `ui_page_set_text(g_page, "page", L"chat")` returns `0`
because the new DOM subtree does not yet exist. The focus call silently does nothing.

**Fix:** Arm a one-shot timer (50 ms) after setting the reactive page variable. The
timer fires after core-ui's next render frame, by which point the new widget tree
is mounted. The callback finds the input by id and calls `ui_window_focus_widget`.

```c
/* In show_main_window(): */
ui_page_set_text(g_page, "page", L"chat");
if (!g_focus_chat_pending) {
    g_focus_chat_pending = 1;
    SetTimer(g_hwnd, TIMER_FOCUS_CHAT, 50, on_timer_focus_chat);
}

/* Timer callback: */
static VOID CALLBACK on_timer_focus_chat(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)id; (void)tick;
    g_focus_chat_pending = 0;
    if (!g_win || !g_page) return;
    UiWidget root = ui_page_root(g_page);
    UiWidget input = ui_widget_find_by_id(root, "chat_input");
    if (input)
        ui_window_focus_widget(g_win, input);
}
```

Always `KillTimer` for the focus timer in the shutdown path to avoid a callback into
a destroyed page.

### 4. Global hotkey: RegisterHotKey + WM_HOTKEY in the existing subclass WndProc

**Problem:** Adding a global hotkey in a core-ui app requires routing `WM_HOTKEY` into
the existing message flow. core-ui owns the message pump (`ui_run()`), so a custom
`GetMessage` loop is not an option.

**Fix:** Use the same pattern already established for `WM_TRAY_ICON`:
- Register the hotkey against the HWND obtained from `ui_window_hwnd(g_win)` after
  the window is created.
- Handle `WM_HOTKEY` in the `SetWindowSubclass` callback, alongside the existing
  tray-icon handler.
- Unregister in the shutdown path, next to `tray_remove()`.

```c
/* Registration (after g_hwnd is assigned) */
static void hotkey_register(HWND hwnd) {
    if (g_hotkey_registered || !hwnd) return;
    if (RegisterHotKey(hwnd, ID_HOTKEY_SUMMON, MOD_CONTROL | MOD_SHIFT, 'N'))
        g_hotkey_registered = 1;
}

/* In subclass_proc: */
if (msg == WM_HOTKEY && wp == ID_HOTKEY_SUMMON) {
    show_main_window();
    return 0;
}
```

If another app already owns the key combo, `RegisterHotKey` fails silently — the
toggle stays on but the hotkey does nothing. This is acceptable degradation; no
crash, no popup.

## Why This Matters

These four patterns are independently load-bearing for any Win32 tray app that needs
to reactivate its window from a non-foreground context and then hand keyboard focus
to an input widget inside a reactive UI framework. Missing any one of them produces
the same user-visible symptom ("the hotkey / tray click doesn't seem to work") with
no error message, making diagnosis slow.

## When to Apply

- Window activation from tray or hotkey in any Win32/C++ app using core-ui or a
  subclassed WndProc message pump.
- Any app with `v-if`-gated pages where focus must land on a widget after a
  programmatic page switch.
- Any tray app that registers `RegisterHotKey` — use the HWND from the window
  creation step, not `NULL`.

## Examples

### Before (broken)

```c
static void show_main_window(void) {
    if (!g_win) return;
    ui_window_show(g_win);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);   // fails for minimized windows
    SetForegroundWindow(g_hwnd);          // silently ignored from background thread
}
```

### After (working)

```c
static void show_main_window(void) {
    if (!g_win) return;
    ShowWindow(g_hwnd, SW_RESTORE);
    /* AttachThreadInput bypasses foreground-lock */
    HWND fg = GetForegroundWindow();
    int attached = 0;
    if (fg && fg != g_hwnd) {
        DWORD fg_tid = GetWindowThreadProcessId(fg, NULL);
        if (fg_tid != GetCurrentThreadId()) {
            AttachThreadInput(GetCurrentThreadId(), fg_tid, TRUE);
            attached = 1;
        }
    }
    SetForegroundWindow(g_hwnd);
    SetActiveWindow(g_hwnd);
    if (attached) {
        AttachThreadInput(GetCurrentThreadId(),
                          GetWindowThreadProcessId(GetForegroundWindow(), NULL),
                          FALSE);
    }
    ui_window_show(g_win);
    ui_page_set_text(g_page, "page", L"chat");
    if (!g_focus_chat_pending) {
        g_focus_chat_pending = 1;
        SetTimer(g_hwnd, TIMER_FOCUS_CHAT, 50, on_timer_focus_chat);
    }
}
```

## Related

- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — reading UIX input
  values from C (different concern: value retrieval, not focus or window activation)
