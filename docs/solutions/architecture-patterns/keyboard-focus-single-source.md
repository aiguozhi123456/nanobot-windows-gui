---
title: "Keyboard focus: single-source state, window blur round-trip, mount-time focus"
date: 2026-10-25
last_updated: 2026-10-25
category: architecture-patterns
module: core-ui/ui_window + nanobot-manager/main
problem_type: architecture_pattern
component: ui
severity: medium
applies_when:
  - "Touching anything focus-related in core-ui (focusable, SetFocus, WM_SET/ KILLFOCUS, Tab traversal, caret blink)"
  - "Wiring Enter/Esc semantics on inputs in the manager (submit hooks, window-level Esc)"
  - "Focusing a widget that lives inside a v-if page (mount hooks vs timers)"
  - "Debugging 'typing goes dead after alt-tab back' or 'focus evaporates after page switch'"
tags: [focus, keyboard, ime, alt-tab, wm-killfocus, wm-setfocus, tab-navigation, mount-hooks, enter, escape, core-ui, uix]
---

# Keyboard focus: single-source state, window blur round-trip, mount-time focus

## Context

"输入框输入和快捷键焦点机制" once had three structural problems, one of them a
user-visible bug. All three shared a root cause: **focus state did not have a
single owner**.

## Problem

1. **Dual focus state in core-ui.** `Widget::focused_` (fires
   `onFocusHook`/`onBlurHook`, drives `:focus` CSS) coexisted with a *public*
   `bool focused` on `TextInputWidget`, `TextAreaWidget`, `CustomWidget` and
   `NumberBoxWidget`, manually reconciled by `dynamic_cast` chains in
   `UiWindowImpl::SetFocus`, the `WM_KILLFOCUS` handler (a full-tree walk) and
   the `OnMouseDown` NumberBox cleanup. The copies had already drifted:
   NumberBox's flag was never set *and* never cleared by `SetFocus`;
   CustomWidget's was only ever cleared.
2. **The bug the drift produced.** `WM_KILLFOCUS` cleared the widget flags but
   kept `focusedWidget_`, and there was no `WM_SETFOCUS` handler. After an
   alt-tab round-trip (or taskbar / title-bar activation) the key router still
   delivered `WM_CHAR`/`WM_KEYDOWN` to the focused input, whose guard read the
   cleared flag and returned false — **typing went dead until the user clicked
   the input again**.
3. **App-side symptom management.** The manager focused `chat_input` after a
   summon with a 50 ms one-shot timer (`TIMER_FOCUS_CHAT`) to "wait a frame"
   for the v-if remount — a guess-timing patch for a missing primitive.

Also discovered while fixing: `<window tabNavigation="true">` was parsed but
**never consumed** (Tab navigation could not be enabled at all), and
Enter/Space keyboard activation skipped the `enabled` gate the mouse path has.

## Solution

### Single source of truth (core-ui)

`Widget::focused_` is the only focus state. `SetFocused()` fans out to a new
`virtual OnFocusChanged(bool)` — subclasses react there (caret blink restart)
instead of keeping a second flag. `HasFocusedTextInput()`'s full-tree scan is
replaced by `virtual NeedsCaretBlink()` asked of `focusedWidget_` only.

**Rule: any new focus-reaction goes into an `OnFocusChanged` override, never a
parallel bool.** (`CustomWidget.focused` survives as a documented legacy
"paint bool" for C hosts; `OnFocusChanged` syncs it when the window focus
system touches the widget.)

### Suspend/restore on window blur (core-ui)

`WM_KILLFOCUS` now *suspends*: `focusedWidget_->SetFocused(false)` (carets
stop, `:focus` CSS drops) but the pointer stays. A new `WM_SETFOCUS` handler
restores it — typing survives the round-trip. Consequence: window-level
blur/refocus fires `@blur`/`@focus` on the focused widget as a pair (documented
in `core-ui/docs/uix-ai-guide.md`).

### Focus at mount time, not timer time (manager)

`ui_page_on_widget_mount` hooks re-fire on **every** v-if remount
(`PageState::DispatchMountHooks`, plus catch-up at registration), so
`on_chat_input_mount` = `chat_ui_mount_input` (submit hook; mount hooks are
one-slot-per-id, so combine in one wrapper) + `ui_window_focus_widget`. The
50 ms timer, its pending flag and its cleanup are gone. Enter-chat-by-sidebar
now also focuses the input — intended.

`show_main_window()` keeps one direct `find_by_id` + focus for the
already-on-chat case (no remount → no hook fires).

### Keyboard semantics on settings inputs (manager)

Enter = apply: `ui_widget_on_submit(port_input, on_apply_network)` — same path
as the Apply button; v-model writes back synchronously per keystroke
(`WireQuickJSModelWrite`), so the value read on Enter is current.
Esc = revert: `ui_window_on_key` receives the bubbled `VK_ESCAPE`
(`TextInputWidget::OnKeyDown` deliberately lets Esc through, build 288) and
restores both port inputs from `g_cfg` — only on the settings page.

### Tab navigation (both repos)

`<window tabNavigation="true">` is wired at last (explicit true only; unset
stays off so existing pages are unchanged), with an equivalent
`ui_window_set_tab_navigation` C API. A generic `focusable="true"` static
attribute folds clickable `<div>`s into the focus system; JS `@click` and C
`ui_widget_on_click` both land on `Widget::onClick`, so Enter/Space activates
a focused div. `CollectFocusable` already skips `!visible || !enabled`.

## Verification notes

- Automated: `scripts\build.bat` + `ctest` (8/8).
- Manual pass that actually exercises the mechanics: (1) type in chat input →
  alt-tab away and back → keep typing without clicking; (2) tray double-click /
  Ctrl+Shift+N / second-instance launch → page + focus semantics per path;
  (3) Enter sends, focus retained; (4) settings: Enter applies, Esc reverts;
  (5) Tab/Shift+Tab walks inputs → toggles → focusable divs with a visible
  ring, Enter/Space activates, disabled buttons are skipped and not
  Enter-activatable.
- Gotcha for future debugging: `NotifyWidgetDestroyed` nulls `focusedWidget_`
  when a focused widget unmounts (v-if page switch) — focus then *evaporates*
  by design; the mount hook is what re-establishes it on the next page.

## References

- `core-ui/src/ui/widget.h` — `IsFocused` / `SetFocused` / `OnFocusChanged` / `NeedsCaretBlink`
- `core-ui/src/ui/ui_window.cpp` — `WM_SETFOCUS` / `WM_KILLFOCUS` / `SetFocus` / `DispatchKeyDown`
- `core-ui/src/ui/page/page_api.cpp` — tabNavigation wiring
- `nanobot-manager/src/main.cpp` — `on_chat_input_mount` / `on_port_input_mount` / `on_window_key`
