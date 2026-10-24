# Concepts

Shared domain vocabulary for this project — entities, named processes, and status concepts with project-specific meaning. Seeded with core domain vocabulary, then accretes as ce-compound and ce-compound-refresh process learnings; direct edits are fine. Glossary only, not a spec or catch-all.

## Gateway Management

### Nanobot Gateway
The independently managed nanobot service that exposes health and WebUI/chat interfaces and can outlive the native manager.

The manager launches it through nanobot's background lifecycle, observes the real gateway instance recorded by the upstream runtime, and requests graceful stop through the gateway control plane rather than treating a launcher PID as the service.

### Health Check
A readiness observation that reports whether the Nanobot Gateway is serving its health endpoint; it is distinct from process identity and from the WebUI interface.

Health is sampled asynchronously and may tolerate transient failures, while process-handle signaling provides immediate death detection.

### Gateway Health Port
The Nanobot Gateway endpoint used for readiness checks and selected when the manager starts a new gateway; it is independent of the WebUI Port.

For a running gateway, the manager may observe a different effective value only through a Gateway State Record whose Gateway Process Identity has been verified; that observation does not change the next-launch preference.

### Gateway State Record
The upstream-owned rendezvous record that identifies one background Nanobot Gateway instance and preserves the information required to rediscover, validate, observe, and stop it across manager launches.

### Gateway Process Identity
The process-creation identity paired with a Gateway State Record's PID so stale state cannot target a different process after Windows reuses that PID.

### Gateway Lifecycle State
The manager's coordinated status for resolving, idle, startup, running, stopping, and terminal unavailability, used to assign ownership of asynchronous transitions and distinguish an invited stop from an unsolicited failure.

Terminal unavailability can be reached by a process-exit event or sustained failed readiness; it does not by itself prove that the process has exited.

### Managed Venv Install
The on-disk form of nanobot's official Windows one-click install: an isolated venv at `~\.nanobot\venv` (or `%NANOBOT_VENV%`) with entry points `Scripts\nanobot.exe` / `Scripts\python.exe -m nanobot`.

Unlike pip PATH installs or uv tool / pipx shims, it touches neither PATH nor the registry nor any shim directory, so it is discoverable only by probing the canonical location — which is why command resolution has a dedicated tier for it.

### WebUI Port
The Nanobot Gateway endpoint used for its web interface, Bootstrap, and Chat Session transport; it is independent of the Gateway Health Port.

The manager may override only its connection target; it does not change the gateway's configuration or listening port.

### Bootstrap
A negotiation step on the WebUI Port that obtains the connection token and route required to establish a Chat Session.

When negotiation is unavailable or does not require a token, the client may attempt a direct chat connection.

### Bootstrap Credential
A user-scoped secret that authorizes Bootstrap, distinct from the short-lived connection token returned by that negotiation.

The manager keeps it outside ordinary configuration and reactive UI state. Migration removes a legacy plaintext source only after the credential can be read back, and conflicting sources require explicit resolution rather than silent overwrite.

### Chat Session
A WebSocket attachment to the gateway's chat interface, established after Bootstrap, that carries one or more Chat Turns and may outlive any individual turn.

Connection lifecycle and Chat Turn lifecycle are separate: a connected session may be idle, processing a turn, or carrying a delayed event from an earlier turn.

### Chat Turn
The client-visible lifecycle of one accepted chat input, from transport acceptance through the evidence that permits the manager to accept another input.

Normal response turns require response activity before a terminal event can complete them, while inline commands may define a distinct acknowledgement. A terminal event without the required current-turn evidence is treated as stale rather than as proof of completion.

### Chat History
The manager-owned local sequence of user and assistant messages restored across manager launches, distinct from the gateway's server-side conversation context.

An unreadable existing history is never treated as permission to overwrite it. A destructive command keeps a process-local compensation copy until acknowledgement, but that copy neither reverses server-side effects nor survives a process crash.

### Message Bubble
A display item representing one user or assistant message in Chat History.

An assistant bubble may be built from partial response events and then normalized by a complete response; transient rendering identity is not part of durable Chat History.

## Branding & Packaging

### Nanobot Mark
The square robot-head brand glyph used for small system chrome — Windows app icon, tray, taskbar, and installer — as opposed to the wide wordmark that pairs the glyph with product lettering.

### Nanobot Wordmark
The wide brand lockup that combines the robot head with the product name; suited to README, marketing, and wide UI surfaces, not to 16–32 px Windows chrome.

### App Icon Resource
The multi-size Windows icon asset embedded into the manager executable and reused by the tray, shell properties, and installer UI so all of those surfaces share one brand mark.

## Dual-Repo & Submodule

### Core UI Submodule
The git submodule that embeds the shared native UI framework under `core-ui/` so the manager can load UIX pages and call Core UI APIs without owning the framework's history.

The parent product records only a Submodule Pin for that tree; framework commits, tags, and public API changes live in the core-ui repository and must be published there before the parent depends on them.

### Submodule Pin
The gitlink SHA the parent repository records for the Core UI Submodule checkout — the single commit object that clones and CI must resolve.

A Submodule Pin is independent of the Submodule Remote URL: retargeting where git fetches does not move the pin, and bumping the pin does not change the remote.

### Submodule Remote URL
The fetch/push location configured for the Core UI Submodule (via `.gitmodules` and the submodule's `origin`), independent of which Submodule Pin the parent currently records.

Changing only the Submodule Remote URL does not change framework content; clones still need the pin's commit object to exist at that remote.

### Dual-Repo Feature Landing
The ordered practice of shipping a Core UI capability first in the framework repository, then consuming it in the parent product by bumping the Submodule Pin and wiring manager code to the new APIs.

Landing only parent product changes while leaving framework commits unpushed, or treating `core-ui/` edits as monorepo product source, leaves other clones unable to resolve the Submodule Pin.

## Core UI Input & IME

### Custom-Drawn Text Input
A Core UI single- or multi-line text field whose caret, selection, and glyphs are painted by the framework (Direct2D) rather than hosted as a system edit control.

Because the OS does not own the caret geometry, anything that must track the caret — including Input Method composition chrome — needs explicit framework reporting; successful character commit alone does not imply that chrome is positioned correctly.

### IME Caret Position
The caret origin the framework reports to the platform input method so composition and candidate windows attach near the visible insertion point.

For a Custom-Drawn Text Input the position is derived from the same layout the control uses to draw the caret, then converted to client or screen coordinates; a measure path that is not bound to the active renderer can silently report zero text advance and place chrome at the field origin or the window corner.

### Composition Window
The platform input-method UI that shows in-progress composition (for example pinyin preedit) while the user is still choosing characters.

### Candidate Window
The platform input-method UI that lists conversion candidates; it should follow the IME Caret Position rather than defaulting to the client origin when the host is a Custom-Drawn Text Input.

## Core UI Layout & Text Measurement

### Measure Context Scope
The framework's binding of its text-measurement context to the active renderer, established only during a scoped layout pass and required for any text measurement — wrap height or caret advance — to be correct.

Layout triggered outside that scope runs with no measure context, so text measurement silently degrades: multi-line text reports a single-line height and caret geometry reports zero advance. Event callbacks must therefore only request a layout (mark it dirty) and let the next frame's scoped layout pass perform it, never invoke layout synchronously. This is the same unbound-measure hazard named under IME Caret Position.

## UI Text & Localization

### Language Table
The bilingual key-to-copy mapping the UI resolves `$t()` lookups against, assembled from one source and handed to the UI framework at startup; its Chinese and English sides must contain exactly the same keys.
*Avoid:* language blob, language strings

A key missing from the active language renders as the raw key, so zh/en parity is enforced by a unit test rather than review discipline.

### Status Key
A UI state wording key (running, stopped, starting, stopping, and their sub-lines) that is consumed through two paths at once: template `$t()` lookups and direct C++ status pushes.

Because of that dual consumption, each Status Key's wording is defined exactly once and the Language Table's status entries are generated from that definition; hand-writing status wording anywhere else creates a second source that can drift.

## Window Management

### System Tray
The Windows notification area that hosts the manager's icon when the main window is hidden, allowing the app to stay resident without a visible taskbar presence.

The user interacts with the tray icon through double-click (summon) or the right-click context menu (show / quit). Closing the main window while tray mode is active routes that close into a hide rather than an exit, so the process continues running until the user explicitly quits from the tray menu.

### Global Hotkey Summon
A registered system-wide keyboard shortcut (Ctrl+Shift+N) that brings the manager window to the foreground from any application.

Because Windows enforces a foreground-lock that silently rejects `SetForegroundWindow` from background threads, the implementation must use `AttachThreadInput` to temporarily share the input queue with the foreground thread before activating. The hotkey is registered against the window HWND after creation and unregistered before destruction; if another application already owns the key combo, registration fails silently with no crash or popup.

### System Theme
The user's chosen Windows light/dark personalization for applications, which the manager mirrors when its theme mode is set to "follow system."

Windows exposes this state only as a registry value, not through a theme API, so the manager reads it at startup before the first paint and again when the user picks the "system" option. The concrete theme must be resolved before the window is created, or the wrong theme flashes for one frame; a hard-coded theme attribute on the UI template silently overrides whatever the manager decides and must not be present.


