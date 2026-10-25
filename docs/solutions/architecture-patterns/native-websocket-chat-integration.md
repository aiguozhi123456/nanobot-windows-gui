---
title: Native WebSocket Chat Integration in a Win32/C++ GUI Application
date: 2026-07-11
last_updated: 2026-10-26
category: architecture-patterns
module: nanobot-manager/chat
problem_type: architecture_pattern
component: tooling
severity: medium
applies_when:
  - Integrating a streaming WebSocket client into a native Win32/C++ GUI
  - Persisting native chat history without mixing it into application settings
  - Handling inline slash commands whose completion contract differs from normal turns
  - Preventing delayed terminal events from completing the wrong request
tags: [websocket, win32, winhttp, chat, state-machine, persistence, atomic-write, core-ui]
---

# Native WebSocket Chat Integration in a Win32/C++ GUI Application

## Context

The nanobot Windows manager provides a native chat page over the gateway's WebUI WebSocket. The integration must keep blocking WinHTTP work off the UI thread, marshal streaming events into reactive UI state, preserve messages across manager restarts, and remain correct when terminal events arrive late or are omitted for inline commands.

The difficult case was `/new`. It is a slash command sent as ordinary message content, not the WebSocket `type: "new_chat"` control envelope. Without an active pending queue, upstream sends `message: "New session started."` and then `turn_end`; with an active queue, the inline-command path sends the same acknowledgement but does not publish its own `turn_end`. Treating both operations as a normal generated turn left sending permanently disabled after a successful inline reset. Conversely, unlocking immediately and blindly ignoring the next `turn_end` allowed a delayed terminal event from the cancelled turn to complete the next user request.

The durable design therefore separates transport, UI turn state, local history storage, and application wiring. It also treats completion events as valid only in states where they can answer the active operation.

## Guidance

### Keep Four Boundaries Explicit

| Boundary | Files | Responsibility |
|----------|-------|----------------|
| Transport | `chat.cpp` / `chat.hpp` (`ChatClient`, singleton `chat_client()`) | Bootstrap, WinHTTP WebSocket connect/send/receive, lifecycle threads, JSON frame parsing, window-message delivery, CAS connection state machine, `nm_log` observability |
| UI state | `chat_ui.cpp` / `chat_ui.hpp` (`ChatUi`, singleton `chat_ui()`), `chat_draft.hpp` | `std::vector<ChatMsg>` message model, streaming `ChatDraft`, turn state machine, `/new` prepare/commit/compensation, publish pipeline, i18n status keys |
| Pure decisions | `chat_protocol.cpp` / `chat_protocol.h` | Attachment resolution, generation wrapping, retry eligibility, persistence-unsafe policy, lifecycle guards |
| History storage | `chat_history.c` / `chat_history.h` | Versioned schema, bounded normalization, load/save status, invalid-file quarantine, atomic replacement |
| App wiring | `main.cpp` | Page/window setup, widget mounts, config-derived history location, gateway health and shutdown ordering |

The transport never touches widgets or history files. The storage module never depends on Core UI or WinHTTP. The UI state layer owns the policy for when a message becomes durable and which protocol event may complete the current operation.

### Express Connection Readiness as a CAS State Machine

The transport's readiness was once three volatile flags (`g_ready`, `g_attached`, `g_connecting`) whose valid combinations were only implicit. They are now one `std::atomic<ChatConnState>` (`Idle / Connecting / Connected / Attached`) moved by `compare_exchange_strong` transitions. A lost CAS — a connect completing while a disconnect lands — is one of the benign races the flag set tolerated, logged at DEBUG and swallowed, never an assertion. `g_stop` (worker cancellation) and the generation counter (stale-event guard) remain separate concerns; folding them in would couple different lifetimes.

Every transition, connect attempt, bootstrap result, and dispatched frame logs through the unified `nm_log` pipeline (module `chat`), so `log_level: debug` makes the transport observable end to end. Workers are `std::thread` members joined in the destructor; because `std::thread` has no try-join, each worker sets a `finished_` flag as its last statement and reaping joins only `joinable() && finished` threads — the same semantics the old `WaitForSingleObject(0)` reaping provided.

### Accumulate Streams in a Draft, Publish Through One Path

The message model is a `std::vector<ChatMsg{id, role, text}>`; cJSON survives only at the history-file boundary and the publish boundary. Streaming deltas append to a `ChatDraft` (amortized O(1); the previous implementation re-copied the whole accumulated text per chunk) which is NOT part of the model: it renders as a virtual trailing bubble and is never persisted mid-stream. Canonical `message`/`stream_end` text commits the draft through one `commitCanonical` path that keeps a turn to one bubble — streamed text and the server's canonical rewrite render as the same message, replacing content instead of appending a duplicate.

Every model change only sets a dirty flag. The 66 ms UI timer (`publishTick`), the scroll mount hook, and shutdown are the only flush entry points; terminal events flush at the end of their handler. Status text is key-based: `ChatUi` pushes a `chat_status_*` key into `chatStatusKey` and the template renders `$t(chatStatusKey)` (locale switches re-resolve immediately); raw server error text rides a separate `chatStatusDetail` field the template prefers when non-empty.

### Marshal Network Events to the UI Thread

The receive thread parses complete WebSocket frames and posts a custom window message. Any optional UTF-8 detail is heap-owned by the event and freed by ``ChatUi::onEvent` after processing:

```c
static void post_event(WPARAM kind, char* detail) {
    /* Snapshot the UI target under the transport lock. */
    if (!PostMessageW(hwnd, msg_id, kind, (LPARAM)detail))
        free(detail);
}

/* Window subclass, on the UI thread. */
if (msg == WM_CHAT_EVENT) {
    chat_ui().onEvent(wparam, (chat_event*)lparam);
    return 0;
}
```

This keeps all cJSON message-array mutations and Core UI calls on the window thread. Connect, receive, disconnect, and reconnect remain transport concerns.

### Persist a Normalized Local History

Chat history lives beside the active `config.json` as a separate `chat-history.json`; it is not another settings field and contains no credential or WebSocket token. The version-1 schema stores only durable message meaning:

```json
{"version":1,"messages":[{"role":"user","text":"hello"},{"role":"assistant","text":"hi"}]}
```

UI-only identifiers are regenerated after load. The reader validates the exact root/message shape, version, roles, text types, message count, per-message size, total text size, physical or escaped NULs, and file size. The writer normalizes the current UI array and retains the newest bounded suffix when a long-running conversation exceeds the persistence limits.

Use an atomic replacement sequence:

1. Serialize the normalized snapshot.
2. Create a unique temporary file in the target directory.
3. Handle short writes until the entire payload is written.
4. Call `FlushFileBuffers`, then close successfully.
5. Replace the target with `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.

An invalid history file is moved to a unique `.invalid.*` backup before a clean history may replace it. A true I/O read error instead closes a write gate: sending retries the read, and no subsequent save may overwrite an unreadable source until it is recovered, confirmed absent, or successfully quarantined.

Streaming deltas update memory and mark the snapshot dirty, but do not write on every chunk. Stable boundaries such as a sent user message, complete response, stream end, turn end, compensation, disconnect, and shutdown call `save_if_dirty`. A failed save leaves the revision dirty so a later boundary can retry. (With the `ChatDraft` model, the streamed text itself becomes durable only when it is committed — canonical text, fixation at `TURN_END`/disconnect, or teardown — so each turn lands on disk as one atomic content update.)

### Treat `/new` as a Process-Local Compensating Transition

Match the command using the same normalization contract as the upstream router: trim Python Unicode whitespace at both ends, compare ASCII command letters case-insensitively, and allow one optional bot suffix containing only ASCII letters, digits, or underscores. Thus `/new`, `/NEW`, a whitespace-wrapped `/new`, and `/new@bot` are equivalent. Inputs with parameters, internal whitespace, invalid or repeated suffixes, full-width lookalikes, malformed UTF-8, or ordinary substrings are not `/new`.

The send flow follows prepare, provisional apply, commit, and process-local compensation:

```text
prepare:           deep-copy current messages
send:              send ordinary message content "/new"
provisional apply: install the backup, clear UI/history, persist an empty snapshot
commit:            exact "New session started." acknowledgement -> discard backup
ignore old tails:  non-ACK content or terminal events while waiting
compensate:        error, disconnect, stop, or graceful shutdown
```

If `chat_send` fails synchronously, the draft and history remain untouched. If transport send succeeds, the empty local history is visible immediately while an in-memory backup remains available until acknowledgement. The acknowledgement is a control response and is not rendered as a new assistant bubble. Other content and terminal events are ignored while waiting because an active queue may still be draining the turn that `/new` cancelled; transport and lifecycle failures use the compensation copy.

This compensation is deliberately local and process-bound. It can restore the manager's view after a reported error, disconnect, gateway stop, or orderly shutdown, but it cannot undo a server-side session clear that already happened. Because the backup is not written as a separate recovery record, a process crash or power loss after the provisional empty snapshot is saved can also lose the old local history. Stronger crash consistency would require a durable pending-command journal or postponing the disk clear until acknowledgement.

### Model Turn Completion with Explicit States

Event order alone does not establish event ownership. Use states whose names describe what evidence has been observed:

```text
IDLE
  send /new success  -> NEW_WAIT_ACK
  send normal success -> NORMAL_WAIT_ACTIVITY

NEW_WAIT_ACK
  exact ACK -> commit clear, hide ACK, IDLE
  DELTA / other MESSAGE / STREAM_END / TURN_END -> ignore
  ERROR / disconnect / stop / close -> restore backup, IDLE

NORMAL_WAIT_ACTIVITY
  MESSAGE -> render complete reply, IDLE            # non-stream / inline
  DELTA / STREAM_END -> NORMAL_ACTIVE               # streaming response
  TURN_END -> ignore                                # stale terminal event

NORMAL_ACTIVE
  DELTA / MESSAGE / STREAM_END -> remain active
  TURN_END -> IDLE
```

`NEW_WAIT_ACK` ignores content and terminal events other than the exact acknowledgement because an active queue may still deliver events from the turn that `/new` cancelled. `IDLE` likewise ignores unsolicited content and terminal events, preventing tails that arrive after acknowledgement from repopulating the cleared history.

A normal request distinguishes a complete first `message` from a streaming response. A first `message` is sufficient to finish a non-stream or inline command, including commands whose inline path does not publish `turn_end`. A first `delta` or `stream_end` establishes streaming activity and advances to `NORMAL_ACTIVE`; only then may `turn_end` complete the turn. This prevents a delayed old terminal event from re-enabling send during the next streamed turn.

### Watchdog on True Silence, Not on Tool Latency

An agent turn can legitimately run minutes: MCP tools (web fetches, shell) and reasoning phases happen before the first content frame. Interim frames — `message` frames with `kind: tool_hint/progress/reasoning` and every other unhandled event (`reasoning_delta`, `goal_status`, `file_edit`, ...) — are not rendered, but each one is posted as `CHAT_EVT_ACTIVITY` and re-arms the in-flight-turn watchdog (when the turn is still waiting for its first content frame, the status bar also shows a "working" hint). The watchdog therefore means *N seconds with zero frames of any kind*, not *N seconds without a reply*: a 45 s MCP fetch that times out and is retried no longer reads as a hung gateway.

When the watchdog does fire — or the user clicks the send button, which doubles as a "stop waiting" control while a turn is open (`turnWaiting` page field drives the label/enabled bindings) — the turn aborts SOFTLY: the partial draft is fixated, the binding is marked unsafe, the status shows the timeout, but the socket stays up and sending re-enables immediately (no disconnect, no auto-connect block). A genuinely dead socket surfaces on the next send or via the receive worker; killing a healthy connection over a slow turn, as an earlier hard-timeout did, lost the eventual reply entirely. The ceiling is 10 minutes of true silence: an observed tool chain (flaky-network web fetches retried for ~4 minutes, no ws traffic at all) outlived the original 60 s and a first 90 s cut.

Current upstream behavior gives `/new` the exact acknowledgement on both dispatch paths. The ordinary path then emits `turn_end`; the active-queue inline path does not emit a command-specific terminal event. The state machine therefore commits `/new` only on its acknowledgement and ignores `turn_end` while waiting for it.

The normal-turn branch additionally depends on two verified protocol properties: events from one WebSocket are delivered in receive order, and a streamed normal turn produces `delta` or `stream_end` before its `turn_end`. If the gateway ever permits a valid `turn_end`-only normal turn, sends a complete `message` before continuing the same turn, or interleaves multiple turns on one connection, local state alone cannot prove ownership; preserve `chat_id` plus a turn/stream identifier through the transport event and correlate explicitly.

### Separate Stream Finalization from Turn Completion

`stream_end` and `turn_end` answer different questions:

- `stream_end` finalizes assistant content. Its optional `text`/`content` may contain the server's canonical final text, including rewritten media URLs, so it replaces the turn's bubble instead of appending a duplicate.
- `turn_end` says a normal active turn has finished and may re-enable sending when the state machine permits it.

The legacy `done` alias maps to two ordered UI events: `STREAM_END` first, then `TURN_END`. Content is therefore finalized before the turn becomes idle.

### Serialize Connection Teardown

Disconnect, reconnect, and shutdown share transport handles and worker threads. A lifecycle worker serializes these requests, closes the WebSocket to unblock receive, joins the receive/connect workers, and only then releases shared state. UI-facing disconnect remains asynchronous so the window thread never blocks waiting for network timeouts.

On final application shutdown:

1. Stop chat transport and null its window-message target.
2. Drain queued chat-event payloads that can no longer be delivered.
3. Compensate an unconfirmed `/new` from its in-memory backup and retry dirty history persistence.
4. Release chat UI state, then destroy the page.

## Why This Matters

### Persistence Must Fail Without Destroying Recovery Data

Treating a missing, malformed, or temporarily unreadable file as the same empty state can destroy the only recoverable copy on the next save. Separate load outcomes and gate writes after I/O errors. Atomic replacement protects the last valid snapshot from partial writes, while quarantine preserves malformed or future-version data for diagnosis.

### Completion Events Need State Preconditions

`turn_end` is authoritative for a normal active turn, but it is not a universal acknowledgement for every input. Inline commands may have their own terminal response, and delayed events remain valid protocol frames even when they are no longer valid answers to the current UI operation. State preconditions provide the missing ownership boundary when the protocol does not expose a turn identifier.

### Destructive UI Operations Need Rollback

Clearing history immediately after a successful socket write confuses transport acceptance with server success. Keeping a process-local pre-command snapshot until acknowledgement makes reported failure, disconnect, gateway stop, and graceful shutdown converge on one compensation path. It does not provide a distributed rollback or a crash-durable transaction.

### Partial and Final Assistant Text Are One Message

Rendering both streamed text and canonical final text as separate bubbles misrepresents one logical reply and can lose server-side rewrites. Updating one assistant bubble preserves the semantic relationship between partial and final events.

## When to Apply

- Native GUI clients that receive asynchronous streaming events on worker threads
- Commands whose acknowledgement differs from ordinary request completion
- Clients without a reliable request/turn identifier on every terminal event
- Local history or cache files that must survive ordinary restarts and failed atomic writes
- Reactive message lists that combine partial and final representations of one reply

## Examples

### Startup Wiring

```cpp
chat_ui().bind(g_page);
chat_ui().setGateway(health_running_thunk, g_effective_webui_port);
chat_ui().resetIdle();
chat_ui().configureHistory(config_get_active_path());
chat_client().init(hwnd, WM_CHAT_EVENT);
```

The config module remains the authority for installed-versus-portable location selection; chat history only derives a sibling path from the selected config path.

### Safe Send Ordering

```text
validate draft and recover any read error
prepare /new backup when needed
chat_send(...)
if send succeeds:
  enter the operation-specific turn state
  update local messages
  persist the dirty snapshot
```

A normal user bubble is appended only after the transport accepts the send. A `/new` reset is provisionally applied only after both backup allocation and transport send succeed.

### Verification Boundaries

`test_chat_history` covers schema and Unicode round trips; `test_chat_draft` covers the bounded streaming buffer (accumulation, empty-chunk no-op, overflow rejection, restart); upstream-compatible `/new` normalization; invalid document shapes and an overlong message; file round trips and empty replacement; physical and escaped NUL rejection; invalid-file quarantine; rolling retention by count and total serialized text; escaped-text file sizing; path derivation; missing files; and invalid-parameter error contracts.

Release verification also includes the full Windows build and CTest suite. The UI/WebSocket state machine does not yet have an automated harness; the missing inline-command completion case was found by manual testing with existing history. Real read faults, oversized-file loading, strict aggregate-load limits, quarantine failure, the UI write gate, and crash recovery are also not covered by the current unit tests. Future state-machine extraction should cover exact acknowledgement, stale content while waiting for acknowledgement, content after acknowledgement, message-only inline completion, delayed `turn_end`, `done`, disconnect/shutdown compensation, and `stream_end.text` replacement.

## Related

- [State-Driven Gateway Lifecycle Management on Windows](state-driven-gateway-lifecycle.md) — state-scoped asynchronous observations and lifecycle ownership
- [Nanobot Gateway Overview](../reference/nanobot-gateway-overview.md) — upstream WebSocket event and command reference; distinguish `/new` message content from `type: "new_chat"`
- [Testing and CI Setup](../infrastructure/testing-and-ci-setup.md) — native CTest boundaries and remaining transport/UI integration-test gap
- `core-ui/docs/uix-ai-guide.md` — reactive UI conventions used by the chat page
- [nanobot gateway](https://github.com/HKUDS/nanobot) — upstream command and WebSocket implementation
