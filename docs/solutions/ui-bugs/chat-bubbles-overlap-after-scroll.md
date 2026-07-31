---
title: "Chat message bubbles overlap after scrolling (core-ui ScrollView relayout)"
date: 2026-07-31
category: ui-bugs
module: core-ui / chat scroll
problem_type: ui_bug
component: tooling
symptoms:
  - "Chat bubbles overlap each other after scrolling the chat page (mouse wheel or scrollbar drag)"
  - "Multi-line assistant replies collapse: grey bubble background is one line tall while text draws many lines"
  - "Initial load renders correctly; overlap appears only after the first scroll and then persists"
  - "Single-line messages space correctly; only multi-line bubbles overlap"
root_cause: scope_issue
resolution_type: code_fix
severity: high
tags:
  - scrollview
  - scroll
  - layout
  - measure-context
  - wrap-label
  - text-overlap
  - core-ui
  - chat
---

# Chat message bubbles overlap after scrolling (core-ui ScrollView relayout)

## Problem

After scrolling the chat page, multi-line assistant message bubbles overlapped each other and became unreadable. Single-line user bubbles stayed correctly spaced; the corruption was confined to wrapped, multi-line content and only appeared once the user scrolled.

## Symptoms

- Chat bubbles overlap one another after scrolling (mouse wheel or scrollbar drag).
- A multi-line assistant reply (e.g. a `/status` block) collapses: the grey bubble background is only one line tall while eight lines of text are drawn, so the next bubble is laid down one line below and everything stacks.
- Initial load renders correctly; the overlap appears only after the first scroll and then persists (it does not self-correct when scrolling stops).
- The bubble *background* height equals the *single-line* measured height, while the *drawn* text uses the real wrapped height — the tell-tale mismatch.

## What Didn't Work

- **Suspecting a render-thread race.** core-ui 1.7.0 moved render submission to a separate thread, which looked like the obvious suspect for scroll corruption. Ruled out: the display list is an immutable, by-value snapshot captured on the UI thread; the render thread has no access to scroll state, each presented frame is a full redraw from one list, so no stale-offset read is possible.
- **Suspecting fractional-DPI (150%) rounding divergence** between the clip rect and geometry. Ruled out: both the ScrollView clip and the geometry draws use `D2D1_ANTIALIAS_MODE_PER_PRIMITIVE` (no pixel snapping), and DPI is applied uniformly via `SetDpi`, so there is no quantization point in the scroll -> layout -> record -> replay path.
- **Product-side `.msg-text { display: block }`.** This makes the label stretch to the bubble's resolved width so the *width* used at measure time matches draw time. It did **not** fix the bug, because the collapse happens at the *inner* `if (g_activeMeasureContext)` guard, not the outer `width > 0` guard. On the scroll path the measure context is null regardless of display mode, so the height still collapsed to one line.
- **Misreading the trigger as "broken at rest."** The first screenshot was taken *after* scrolling, which made the corruption look static. The actual trigger is the scroll itself: initial (unscrolled) layout is correct.

## Solution

Route every interactive relayout through `ui::RequestLayout()` instead of calling `DoLayout()` directly, so the relayout is deferred to the next frame's scoped `LayoutRoot()` pass. The core change (ScrollView wheel handler; the same edit was applied to the wheel scrollbar-track click, the scrollbar thumb drag, TabControl select, Splitter drag, and Flyout show):

```cpp
// core-ui/src/ui/controls.cpp — ScrollViewWidget::OnMouseWheel (before)
scrollY_ -= e.delta * 0.3f;
ClampScroll();
DoLayout();            // runs OUTSIDE the measure scope
return true;

// after
scrollY_ -= e.delta * 0.3f;
ClampScroll();
ui::RequestLayout();   // mark dirty; next frame relayouts under the scope
return true;
```

Two redundant out-of-scope calls were also removed:

- `UiWindowImpl::OnMouseWheel` (`ui_window.cpp`) called `sv->DoLayout()` again right after the handler — deleted; the handler's `RequestLayout()` plus the existing `InvalidateNow()` already cover the relayout.
- `ui_debug_scroll_set` (`ui_api.cpp`) called `sv->DoLayout()` after `SetScrollY()` — deleted; `SetScrollY` already calls `RequestLayout()` (and leaving it made the debug-server `scroll`/`wheel` repro a false negative).

`ScrollViewWidget::DoLayout` itself needed no change: after these edits its only top-level entry point is the scoped `LayoutRoot`; every other call is a recursive `content_->DoLayout()` inside that scope.

## Why This Works

`LabelWidget::HeightForWidth` computes a wrapped (multi-line) height only inside `if (g_activeMeasureContext)`. That global is set solely by `ActiveMeasureContextScope`, which is constructed only inside `LayoutRoot()` — i.e. only during a paint-driven layout pass.

- **Initial load / any reactive update** goes through `LayoutRoot`, so the scope is alive, the measure context is non-null, and the label reports its true multi-line height. Hence the page looks correct until you scroll.
- **Scrolling** fired `OnMouseWheel`/`OnMouseDown`/`OnMouseMove`, which called `DoLayout()` directly on a call stack with **no** `ActiveMeasureContextScope`. The recursive relayout reached `HeightForWidth` with `g_activeMeasureContext == nullptr`, the inner branch was skipped, and the height collapsed to a single `lineH`. Because the handlers also never set the layout-dirty flag, no later scoped pass corrected it — the wrong single-line rects stayed pinned, so the overlap persisted.

`ui::RequestLayout()` only marks the tree dirty and invalidates the windows; the actual relayout then happens on the next frame inside `LayoutRoot`'s scope, where the measure context is guaranteed non-null. Deferring to the next frame costs nothing perceptible (60 Hz repaint keeps scroll/drag smooth) and also coalesces multiple rapid scroll events into one layout per frame instead of one per event.

The `display: block` change on `.msg-text` is harmless and complementary: on the *scoped* path it keeps the measured width equal to the drawn width for shrink-to-fit bubbles, but it is not load-bearing for this fix.

## Prevention

- **Never call `DoLayout()` from an event handler.** `DoLayout()` may only run (a) recursively from inside another `DoLayout` override, or (b) from `LayoutRoot` under `ActiveMeasureContextScope`. Everywhere else — wheel, mouse down/move, select, value-change callbacks — call `ui::RequestLayout()` and let the next frame do the layout.
- **Guard with a grep.** Audit event callbacks for direct `DoLayout()` calls (`OnMouse*`, `OnKey*`, `OnWheel*`, `OnSelect*`, change callbacks) that are not recursive-within-scope; each one is a latent wrap-text collapse.
- **Treat null measure context as a degraded mode.** Any measurement that depends on the measure context (wrap height, caret advance) silently degrades outside the scope — multi-line height collapses to one line, caret advance reports zero. If a widget measures text, assume it is only correct when laid out under the scope.
- **Keep wrap-label measure/draw width consistent** on shrink-to-fit containers (e.g. `display: block` or an explicit width) so the scoped path measures at the same width it draws.

## Related Issues

- `docs/solutions/ui-bugs/ime-candidate-window-top-left-custom-textinput.md` — the sibling measure-context hazard in the same codebase area: there an *unbound* measure context reported zero text advance and mis-placed IME chrome; here a *null* measure context collapsed wrapped height. Both stem from text measurement that is only valid when bound to the active renderer inside the scoped pass.
