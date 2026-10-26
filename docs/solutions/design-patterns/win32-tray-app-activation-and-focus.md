---
title: "Win32 tray-app window activation, global hotkey, and deferred widget focus"
date: 2026-08-01
last_updated: 2026-10-27
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
summons it back via tray click (single or double). A later feature added a global
hotkey (Ctrl+Shift+N) to do the same, plus toggle-hide.

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
`SetActiveWindow` succeed. Detach immediately after — but detach from the thread
id you saved *before* the attach. After a successful foreground steal,
`GetForegroundWindow()` returns your own window, so a detach that re-queries the
foreground detaches from the wrong thread: a silent no-op that leaks the
input-queue attachment.

```c
HWND fg = GetForegroundWindow();
DWORD fg_tid = 0;
if (fg && fg != g_hwnd) {
    fg_tid = GetWindowThreadProcessId(fg, NULL);
    if (fg_tid != GetCurrentThreadId())
        AttachThreadInput(GetCurrentThreadId(), fg_tid, TRUE);
    else
        fg_tid = 0;   /* our own thread: nothing was attached */
}
SetForegroundWindow(g_hwnd);
SetActiveWindow(g_hwnd);
/* Detach from the thread we attached to (saved above). Querying the
 * foreground here would return our own window after a successful steal. */
if (fg_tid)
    AttachThreadInput(GetCurrentThreadId(), fg_tid, FALSE);
```

### 3. Focus a v-if-remounted input at mount time, not with a deferred timer

**Problem:** When a UIX page switch changes `page` from `"settings"` to `"chat"`, the
`<div v-if="page=='chat'">` block is destroyed and recreated. `ui_widget_find_by_id`
called immediately after `ui_page_set_text(g_page, "page", L"chat")` returns `0`
because the new DOM subtree does not yet exist. The focus call silently does nothing.

**Fix (current):** Register a widget-mount hook once, after the page exists. The
hook fires exactly when the widget mounts — the initial mount and every later v-if
remount (entering the chat page however the user got there) — so focus lands at a
deterministic moment with no frame guessing. (An earlier fix armed a 50 ms one-shot
`TIMER_FOCUS_CHAT` after the page switch and guessed the render frame; that timer
is deleted.)

```c
/* Registered after the window/page exist; replaces the old 50 ms one-shot. */
ui_page_on_widget_mount(g_page, "chat_input", on_chat_input_mount, NULL);

static void on_chat_input_mount(UiPage p, UiWidget w, void* ud) {
    chat_ui_mount_input(p, w, ud);
    if (g_win)
        ui_window_focus_widget(g_win, w);
}
```

When the page is already `"chat"` there is no remount and the hook does not fire —
find the widget and focus it directly (see the `show_main_window` example below).
The full pattern is documented in
`docs/solutions/architecture-patterns/keyboard-focus-single-source.md`.

### 4. Global hotkey: RegisterHotKey + WM_HOTKEY in the existing subclass WndProc

**Problem:** Adding a global hotkey in a core-ui app requires routing `WM_HOTKEY` into
the existing message flow. core-ui owns the message pump (`ui_run()`), so a custom
`GetMessage` loop is not an option.

**Fix:** Use the same pattern already established for `WM_TRAY_ICON`:
- Register the hotkey against the HWND obtained from `ui_window_hwnd(g_win)` after
  the window is created — but only when the `hotkey_enabled` config field is set.
  The settings-page toggle registers/unregisters live; startup consults the config.
- Handle `WM_HOTKEY` in the `SetWindowSubclass` callback, alongside the existing
  tray-icon handler. The handler is a **toggle**: hide only when the window itself
  is the foreground window (visible-but-covered or minimized means "summon").
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
    if (GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd)) {
        ui_window_hide(g_win);
        ShowWindow(g_hwnd, SW_HIDE);
        on_window_hidden();
    } else {
        show_main_window();
    }
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
    surface_main_window();   /* SW_RESTORE + AttachThreadInput + show */

    ui_page_set_text(g_page, "page", L"chat");
    chat_ui().scrollToBottom();   /* already on chat: no remount */
    UiWidget root = ui_page_root(g_page);
    UiWidget input = ui_widget_find_by_id(root, "chat_input");
    if (input)
        ui_window_focus_widget(g_win, input);
}
```

Entering the chat page remounts `chat_input` via v-if and its mount hook (section 3)
focuses it; the direct find/focus above covers the already-on-chat case where there
is no remount.

## Related

- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — reading UIX input
  values from C (different concern: value retrieval, not focus or window activation)
