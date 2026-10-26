---
title: "Chinese IME composition/candidate window stuck at screen top-left on core-ui TextInput"
date: 2026-07-31
last_updated: 2026-10-27
category: ui-bugs
module: core-ui / chat input
problem_type: ui_bug
component: tooling
symptoms:
  - "Chinese IME (拼音等) composition and candidate windows appear at screen top-left when typing in nanobot-manager chat input"
  - "Same mis-positioning on other core-ui custom-drawn TextInput/TextArea widgets"
  - "Characters still commit correctly via WM_CHAR; only caret-relative IME UI positioning is wrong"
root_cause: wrong_api
resolution_type: code_fix
severity: medium
tags:
  - ime
  - chinese-input
  - textinput
  - imm32
  - core-ui
  - composition-window
  - candidate-window
---

# Chinese IME composition/candidate window stuck at screen top-left on core-ui TextInput

> **Status update (2026-10-24):** as of upstream core-ui v1.8.0 this is fixed in the framework itself — `UiWindowImpl::UpdateImeCompositionPos` resolves the focused widget's caret rect under an `ActiveMeasureContextScope` and pushes it to both `ImmSetCompositionWindow` and `ImmSetCandidateWindow`. The fork's original patch (`442461d`) was absorbed and dropped in the v1.8.0 rebase. The analysis below is still the correct mental model for any custom text control.

## Problem

When typing Chinese with an IME into the nanobot-manager chat composer (`chat_input`), the IME composition and candidate windows appeared at the top-left of the screen instead of under the caret. The root cause was that core-ui’s custom-drawn `TextInputWidget` accepts text via `WM_CHAR` but never told Imm32 where the caret was.

The chat page uses `<input id="chat_input">`, which mounts as a Direct2D self-drawn `TextInputWidget`, not a Win32 `EDIT` control. Characters still arrive through the normal message path (`WM_CHAR` → `OnKeyChar`), so composition commits correctly and the typed text appears in the control. Before the fix, the codebase had no `ImmSetCompositionWindow`, `ImmSetCandidateWindow`, or `IMR_QUERYCHARPOSITION` handling. An earlier CHANGELOG entry (around L96) for `VK_PROCESSKEY` had deliberately avoided linking `imm32`, so the IME stack was never integrated with caret geometry.

Without an explicit composition/candidate position, Imm32 falls back to the default client origin `(0,0)`. For a top-level window that maps to screen coordinates near the desktop origin, that default lands the candidate UI at the screen top-left—far from the chat composer caret.

## Symptoms

- Chinese (and other Imm32) IME candidate lists and composition windows open at the screen top-left while focus is on `chat_input`.
- Committed characters still appear correctly in the chat composer; only IME chrome is misplaced.
- The same class of mis-positioning can affect other custom-drawn inputs (e.g. settings port fields) that use `TextInputWidget` / `TextAreaWidget`.
- No layout or CSS glitch is visible in `dashboard.uix`; the native control draws caret and text normally.

## What Didn't Work

- **dashboard.uix layout / CSS** — Not a positioning or overflow issue in the SFC. The input is a native Core UI widget; UIX layout does not drive Imm32 window placement.
- **chat_ui.cpp send path** — Send/stream logic is unrelated. IME positioning fails before any send; the bug is caret-to-Imm32 plumbing in core-ui.
- **Product-side `VK_RETURN` / chat wiring in main.cpp** — nanobot-manager does not implement IME caret updates. Expecting product code to handle return keys or chat focus for IME chrome was a red herring.
- **Treating “text appears correctly” as success** — `WM_CHAR` delivery proves input works; it does not prove composition/candidate windows track the caret. The prior `VK_PROCESSKEY` fix that avoided `imm32` left this gap intentional until this change.

## Solution

Fix lives in **core-ui** (submodule), not nanobot-manager product code. Implement Imm32 caret updates for focused custom text controls and link `imm32`. Parent product only bumps the Submodule Pin after the framework commit exists.

### 1. Caret position in DIP, matching draw geometry

Caret resolvers live on the text controls as one virtual that mirrors `OnDraw` layout (padding, scroll, line metrics):

- `Widget::GetCaretRect(D2D1_RECT_F*)` (core-ui `widget.h`) — base implementation returns false; `TextInputWidget`, `TextAreaWidget`, and `ChipsInputWidget` override it (`controls.h`) to report the caret as a DIP rect. The single-line input accounts for the same horizontal padding used when drawing (e.g. 11px pad) and `scrollX_` so the reported rect tracks the visible caret, not logical text origin alone; `TextAreaWidget` is the multi-line equivalent with the same draw-path geometry.

Sketch (illustrative):

```cpp
// TextInputWidget — caret DIP rect aligned with OnDraw
bool TextInputWidget::GetCaretRect(D2D1_RECT_F* out) const {
  // pad, scrollX_, measured advance up to caret — same as draw path
  // ...
}
```

### 2. Focused caret via measure-bound context (critical)

`UiWindowImpl::UpdateImeCompositionPos` resolves the focused widget's caret inside an `ActiveMeasureContextScope(measureContext_, renderer_)` — the scope binds the renderer on the `measureContext` **before** measuring text widths. An unbound `MeasureContext` returns **0** for text advances, which collapses the caret to the padding origin and reintroduces wrong Imm positions even after Imm APIs are called.

```cpp
ActiveMeasureContextScope measureScope(measureContext_, renderer_);
D2D1_RECT_F cr{};
if (!focusedWidget_->GetCaretRect(&cr)) return;
```

`IMR_QUERYCHARPOSITION` is not handled anywhere today; routing a future handler through this same path (so composition/candidate UIs and character-position queries cannot disagree) is a recommendation, not implemented behavior.

### 3. Push position into Imm32

`UpdateImeCompositionPos`:

1. `ImmGetContext(hwnd)`
2. `ImmSetCompositionWindow` with `CFS_POINT` at the caret (client coords derived from DIP × dpiScale)
3. `ImmSetCandidateWindow` with `CFS_CANDIDATEPOS` at the same point
4. `ImmReleaseContext`

### 4. When to update

| Trigger | Why |
|---------|-----|
| `WM_SETFOCUS` | Focus restored onto a text control — IME must attach to the new caret immediately |
| `WM_IME_STARTCOMPOSITION` | Composition session start |
| `WM_IME_COMPOSITION` | Composition updates |

These three are the only call sites today (`ui_window.cpp`); there is no mouse-down, key-dispatch, `WM_CHAR`, or `WM_IME_REQUEST` / `IMR_QUERYCHARPOSITION` trigger anywhere.

### 5. Link imm32

- `core-ui/CMakeLists.txt` (`UI_CORE_SYSTEM_LIBS`)
- `core-ui/cmake/UiCoreHelpers.cmake` (static consumers)

### Key files

| File | Role |
|------|------|
| `core-ui/src/ui/widget.h` | `Widget::GetCaretRect(D2D1_RECT_F*)` virtual (base) |
| `core-ui/src/ui/controls.h` | `GetCaretRect` overrides (TextInput / TextArea / ChipsInput) |
| `core-ui/src/ui/controls.cpp` | caret rect DIP matching OnDraw |
| `core-ui/src/ui/ui_window.h` | `UpdateImeCompositionPos` |
| `core-ui/src/ui/ui_window.cpp` | `ActiveMeasureContextScope`-bound caret resolution, Imm updates, IME message handlers |
| `core-ui/CMakeLists.txt` | Link `imm32` |
| `core-ui/cmake/UiCoreHelpers.cmake` | Helper link of `imm32` for consumers |

Verified fix commits: core-ui `442461d`, parent pin `df0974a`.

## Why This Works

Win32 `EDIT` controls report caret position to the IME automatically. Core UI’s Direct2D text widgets own caret geometry entirely in DIP and paint it themselves; Imm32 has no built-in knowledge of that layout. Defaulting to client `(0,0)` is therefore expected, not a fluke.

By computing the caret in the **same coordinate system as `OnDraw`** (padding, scroll, line height), resolving it through a **renderer-bound** measure context (so advances are real), and feeding that point to both `ImmSetCompositionWindow` and `ImmSetCandidateWindow`, the IME chrome tracks the visual caret on focus and composition events. (`IMR_QUERYCHARPOSITION` remains unhandled — see the note in section 2.)

Linking `imm32` is required for those APIs; the earlier decision to avoid Imm for `VK_PROCESSKEY` alone left positioning unsolved until this explicit integration.

## Prevention

- Any **new custom-drawn text control** that accepts IME input must implement caret DIP resolution and participate in the window’s Imm update path—do not assume `WM_CHAR` success means IME chrome is correct.
- Keep a **single** measure-bound caret resolver shared by Imm set-window calls — and by `IMR_QUERYCHARPOSITION` if such handling is ever added (none exists today) — so screen and client answers cannot drift.
- Always **bind the renderer on `MeasureContext` before measuring** text for IME; unbound measure returning 0 widths is a silent regression class.
- Manual test matrix: Chinese IME on **`chat_input`** (composer) and **settings port** (and similar) inputs—confirm candidate/composition windows sit under the caret, not screen top-left.
- When fixing Imm-related issues, do not stop at “characters appear”; verify composition and candidate UI placement under real CJK IME sessions.
- If product code mounts Core UI inputs, prefer fixing Imm in core-ui rather than ad-hoc Imm calls in the app—product wiring (`main.cpp`, `chat_ui.cpp`) is the wrong layer for caret geometry.

## Related Issues

- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — how C reads TextInput values (`ui_page_get_json`); different angle (read path, not IME chrome)
- `docs/solutions/architecture-patterns/dual-repo-core-ui-submodule-workflow.md` — framework fix lands in core-ui first, then parent Submodule Pin
- `docs/solutions/reference/core-ui-overview.md` — framework capability map (TextInput / input limitations)
- `docs/solutions/architecture-patterns/native-websocket-chat-integration.md` — chat page that hosts `chat_input`
