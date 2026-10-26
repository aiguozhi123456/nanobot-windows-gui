---
title: "Chat message bubbles overlap after scrolling (core-ui ScrollView relayout)"
date: 2026-07-31
last_updated: 2026-10-26
category: ui-bugs
module: core-ui / chat scroll
problem_type: ui_bug
component: tooling
symptoms:
  - "Chat bubbles overlap each other after scrolling the chat page (mouse wheel or scrollbar drag)"
  - "Multi-line assistant replies collapse: grey bubble background is one line tall while text draws many lines"
  - "Initial load renders correctly; overlap appears only after the first scroll and then persists"
  - "Single-line messages space correctly; only multi-line bubbles overlap"
  - "A message-populated chat page freezes for tens of seconds (Windows reports the app 'stopped interacting', WER Event 1002) when a full relayout lands — the /new clear, a page switch — because every layout pass re-measures every wrapped label from scratch"
root_cause: scope_issue
resolution_type: code_fix
severity: high
tags:
  - scrollview
  - layout
  - measure-context
  - wrap-label
  - text-overlap
  - core-ui
  - chat
  - hang
---

# Chat message bubbles overlap after scrolling (core-ui ScrollView relayout)

> **Status update (2026-10-26):** the wheel-path fix is now present in this fork via core-ui commit `03e6f70` — `UiWindowImpl::OnMouseWheel`'s post-handler `sv->DoLayout()` was removed (the widget-side `ApplyScrollDelta_` pure-offset path had already landed with the 1.8.0 port), and `ScrollViewWidget::DoLayout` now counts `content_->padB` in `contentHeight_`. The earlier note below claiming upstream v1.8.0 (build 285) had already superseded this fix was wrong for this fork: the v1.8.0 rebase dropped the caller-side deletion, so `ui_window.cpp` kept the out-of-scope `DoLayout()` until `03e6f70`. A second core-ui commit, `11d7bd4`, then fixed the freeze dimension (uncached per-label text measurement at 30 Hz relayout) and the long-text wrap height. The measure-scope root-cause analysis remains valid reading for any code that still calls `DoLayout()` outside `LayoutRoot()` (upstream `TabControl::OnMouseDown` and `Splitter::OnMouseMove` still do).
>
> **Status update (2026-10-24, superseded):** ~~upstream core-ui v1.8.0 (build 285) superseded this fix with a better one — mouse scrolling no longer triggers any relayout; `ScrollViewWidget::OnMouseWheel` / thumb drag now apply a pure offset via `ApplyScrollDelta_`. The fork's `RequestLayout`-routing commit (`b594ec0`) was dropped in the v1.8.0 rebase.~~ — The widget-side half of that claim was true; the caller-side deletion was lost in the rebase. See the 2026-10-26 note above.

## Problem

After scrolling the chat page, multi-line assistant message bubbles overlapped each other and became unreadable. Single-line user bubbles stayed correctly spaced; the corruption was confined to wrapped, multi-line content and only appeared once the user scrolled.

A second manifestation of the same relayout family surfaced later: with a populated message list, a full relayout — the `/new` clear, a page switch — freezes the UI thread for tens of seconds while Windows reports the app as "stopped interacting" (WER Event 1002). The thread is not deadlocked; it is busy measuring text, over and over, faster than the message pump can drain.

## Symptoms

- Chat bubbles overlap one another after scrolling (mouse wheel or scrollbar drag).
- A multi-line assistant reply (e.g. a `/status` block) collapses: the grey bubble background is only one line tall while eight lines of text are drawn, so the next bubble is laid down one line below and everything stacks.
- Initial load renders correctly; the overlap appears only after the first scroll and then persists (it does not self-correct when scrolling stops).
- Single-line messages space correctly; only multi-line bubbles overlap.
- A populated chat page freezes for tens of seconds (WER Event 1002) when a full relayout lands — the `/new` clear froze one session for 34 s; the UI thread showed ~100 % of one core while the chat page was displayed, dropping to ~0 % once the list was empty.

## What Didn't Work

- **Suspecting a render-thread race.** core-ui 1.7.0 moved render submission to a separate thread, which looked like the obvious suspect for scroll corruption. Ruled out: the display list is an immutable, by-value snapshot captured on the UI thread; the render thread has no access to scroll state, each presented frame is a full redraw from one list, so no stale-offset read is possible.
- **Suspecting fractional-DPI (150%) rounding divergence** between the clip rect and geometry. Ruled out: both the ScrollView clip and the geometry draws use `D2D1_ANTIALIAS_MODE_PER_PRIMITIVE` (no pixel snapping), and DPI is applied uniformly via `SetDpi`, so there is no quantization point in the scroll -> layout -> record -> replay path.
- **Product-side `.msg-text { display: block }`.** This makes the label stretch to the bubble's resolved width so the *width* used at measure time matches draw time. It did **not** fix the bug, because the collapse happens at the *inner* `if (g_activeMeasureContext)` guard, not the outer `width > 0` guard. On the scroll path the measure context is null regardless of display mode, so the height still collapsed to one line.
- **Misreading the trigger as "broken at rest."** The first screenshot was taken *after* scrolling, which made the corruption look static. The actual trigger is the scroll itself: initial (unscrolled) layout is correct.
- **Blaming a third-party injected module for the freeze.** The same session's event log also showed AppCrash (Event 1000) entries whose faulting module was `iMonitorH2K.i64` (c0000005 / c0000409) — a monitoring DLL injected into processes, not this codebase. Those are separate crashes; do not chase them while diagnosing a hang.
- **Believing the "32778-message history".** The startup log line `init complete (msg=32778)` is the Windows message ID `WM_CHAT_EVENT` (`WM_APP + 10` = 32778) that `ChatClient` posts chat events to — not a message count. `CHAT_HISTORY_MAX_MESSAGES` (500) trims the loaded history (`chat_history.c` load-time normalization), so "giant history" was a phantom and full-history saves were never the freeze.
- **Suspecting a lock inversion (ABBA) around send().** `ChatClient::send` holds `cs_` across `WinHttpWebSocketSend`, which made a UI-thread/worker deadlock look plausible for the `/new` freeze — but every worker-to-UI handoff goes through `postEvent`, which uses `PostMessageW` (asynchronous), so no worker ever waits on the UI thread under that lock.
- **Suspecting an O(n²) v-for unmount.** `RebuildJsLoop` does not even call `RequestLayout()` when the new list is empty (`if (len > 0)`), and an empty-list `/new` repro completed instantly twice — the synchronous clear path is cheap.
- **A 200-message live repro that did NOT hang.** The freeze needs one layout pass to exceed the message-pump window (seconds); 200 short labels never got there. The seeded 500 × ~1.5 KB page did: the UI thread burned ~100 % of one core while the page was displayed and returned to ~0 % once the list was empty.
- **Reading WER's ReportArchive.** `C:\ProgramData\Microsoft\Windows\WER\ReportArchive\...` is permission-denied unelevated. The Application event log (`Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000,1002}`) carried the exception codes, faulting modules, and hang durations instead — query it before assuming a crash dump is needed.

## Solution

The canonical fix for the overlap (routing every interactive relayout through `ui::RequestLayout()` instead of calling `DoLayout()` directly) is now implemented in this fork by core-ui `03e6f70`: the wheel handler's pure rect shift (`ApplyScrollDelta_`) stands alone, and the caller-side `sv->DoLayout()` after the handler was deleted. The same edit pattern applies to any event-handler `DoLayout()` call.

The freeze needed a second fix — core-ui `11d7bd4` — because two costs multiply:

1. **Uncached per-label measurement.** `LabelWidget::HeightForWidth` and `LabelWidget::SizeHint` create a fresh `IDWriteTextLayout` per wrapped label *on every layout pass* (via `MeasureContext::MeasureTextHeight` / `MeasureTextWidth` — one `CreateTextLayout` per call, no memoization). For long CJK text that is milliseconds per label.
2. **Frame-rate relayout from a cosmetic timer.** The 33 ms field-drift timer (`TIMER_FIELD_DRIFT` in `main.cpp`) republishes the backdrop fields via `ui_page_set_float`, which marks the whole tree dirty (`RequestLayout`), so a full-tree relayout runs ~30 times per second while the window is shown — on every page, forever.

On a page with long-text labels up to the chat display cap (200), one pass costs seconds, the UI thread never idles, and queued input waits behind the current pass.

`11d7bd4` fixes both ends of the measurement path:

```cpp
// core-ui/src/ui/controls.cpp — LabelWidget::SizeHint / HeightForWidth
// Self-validating memoization: the key is every measurement input.
size_t textHash = std::hash<std::wstring>{}(text_);
if (wrapCacheValid_ && wrapCacheW_ == width && wrapCacheFs_ == fontSize_ &&
    wrapCacheLineH_ == lineH && wrapCachePadLR_ == (padL + padR) &&
    wrapCacheBold_ == bold_ && wrapCacheMaxLines_ == maxLines_ &&
    wrapCacheTextLen_ == text_.size() && wrapCacheTextHash_ == textHash) {
    return wrapCacheH_ + padT + padB;   // no IDWriteTextLayout this pass
}
```

and the wrap height itself:

```cpp
// before: line-count heuristic — systematically short for long CJK text
int numLines = std::max(1, (int)std::ceil(textH / (fontSize_ * 1.4f)));
h = numLines * lineH;

// after: metrics.height IS the drawn height (DrawText never sets line spacing)
h = std::max(textH, lineH);
```

CJK fallback fonts push real line spacing past both 1.4×fs and the 1.3×fs line height, so the heuristic under-measured long bubbles — the same one-line-short bubble background, now from measurement arithmetic instead of a null measure context. The single-line floor keeps the CSS line-height look.

`03e6f70`'s `contentHeight_` padB counting also matters for scroll usability: children bounds never include the content widget's own bottom padding (`.chat-list`'s 64 px composer clearance), so `MaxScrollY` was short by that padding and the list tail could not scroll clear of the composer.

## Why This Works

`LabelWidget::HeightForWidth` computes a wrapped (multi-line) height only inside `if (g_activeMeasureContext)`. That global is set only by `ActiveMeasureContextScope`. The scope wraps a paint-driven layout in `LayoutRoot()` (plus, for input-measurement correctness only, a handful of input handlers such as mouse/key dispatch); no event handler ever runs a layout pass under it.

- **Initial load / any reactive update** goes through `LayoutRoot`, so the scope is alive, the measure context is non-null, and the label reports its true multi-line height. Hence the page looks correct until you scroll.
- **Scrolling** fired `OnMouseWheel`/`OnMouseDown`/`OnMouseMove`, which called `DoLayout()` directly on a call stack with **no** `ActiveMeasureContextScope`. The recursive relayout reached `HeightForWidth` with `g_activeMeasureContext == nullptr`, the inner branch was skipped, and the height collapsed to a single `lineH`.
- `ui::RequestLayout()` only marks the tree dirty and invalidates the windows; the actual relayout then happens on the next frame inside `LayoutRoot`'s scope, where the measure context is guaranteed non-null. Deferring to the next frame costs nothing perceptible (60 Hz repaint keeps scroll/drag smooth) and also coalesces multiple rapid scroll events into one layout per frame instead of one per event.

The freeze is the same hazard with a cost multiplier. Even a *correct* scoped layout is expensive when it re-measures every label from scratch, and the drift timer converts "once per interaction" into "30 times per second". With memoized measurement, a full pass on a 500-message page is milliseconds of hash comparisons instead of hundreds of `IDWriteTextLayout` creations, so the message pump keeps up. Verified on a seeded 500 × ~1.5 KB page: `/new` completes in under 2 s with the window responsive throughout (previously a 34 s hang).

The `display: block` change on `.msg-text` is harmless and complementary: on the *scoped* path it keeps the measured width equal to the drawn width for shrink-to-fit bubbles, but it is not load-bearing for this fix.

## Prevention

- **Never call `DoLayout()` from an event handler.** `DoLayout()` may only run (a) recursively from inside another `DoLayout` override, or (b) from `LayoutRoot` under `ActiveMeasureContextScope`. Everywhere else — wheel, mouse down/move, select, value-change callbacks — call `ui::RequestLayout()` and let the next frame do the layout.
- **Guard with a grep.** Audit event callbacks for direct `DoLayout()` calls (`OnMouse*`, `OnKey*`, `OnWheel*`, `OnSelect*`, change callbacks) that are not recursive-within-scope; each one is a latent wrap-text collapse.
- **Treat null measure context as a degraded mode.** Any measurement that depends on the measure context (wrap height, caret advance) silently degrades outside the scope — multi-line height collapses to one line, caret advance reports zero. If a widget measures text, assume it is only correct when laid out under the scope.
- **Keep wrap-label measure/draw width consistent** on shrink-to-fit containers (e.g. `display: block` or an explicit width) so the scoped path measures at the same width it draws.
- **Treat layout passes as frame-rate work.** Any per-widget measurement in a layout path runs at relayout frequency — and cosmetic reactive timers multiply that frequency to frame rate. Memoize text measurement keyed on its inputs (width, font size, line-height, weight, padding, text identity), checked on read so no setter hooks are needed.
- **Audit what a reactive field write costs.** `ui_page_set_float` on a drift field marks the entire tree dirty. Position-only bindings ideally should not require a measure pass at all — a subtree rect shift, the way `ApplyScrollDelta_` works, would be enough.
- **When a page feels sluggish with no visible cause, measure the UI thread's CPU while idle.** A saturated core with `Responding=True` is the signature of an over-frequent relayout; a deadlock shows `Wait` thread states with zero CPU. The two need opposite fixes.
- **Residual, not fixed:** even fully cached, the 30 Hz full-window drift repaint (three large gradients over the acrylic backdrop) still costs ~75–85 % of one core on this machine. That is the animation's own paint cost, independent of measurement; gating the drift when the backdrop is fully covered would be a separate change.

## Related Issues

- `docs/solutions/ui-bugs/core-ui-silent-rendering-traps.md` — same module's silent-rendering family; documents the ScrollView `DrawTree` override and the measure-scope silent failures.
- `docs/solutions/ui-bugs/ime-candidate-window-top-left-custom-textinput.md` — the sibling unbound-measure hazard (zero text advance) in the same code area.
- `docs/solutions/ui-bugs/absolute-positioned-backdrop-divs-swallows-clicks.md` — the same `TIMER_FIELD_DRIFT` timer on the input path, different root cause.
- `docs/solutions/architecture-patterns/translucent-window-dwm-acrylic.md` — the translucent window the drift fields paint over.
- `docs/solutions/reference/core-ui-overview.md` — framework capability overview.
