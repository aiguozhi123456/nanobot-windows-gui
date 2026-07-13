---
title: Native WebSocket Chat Integration in a Win32/C++ GUI Application
date: 2026-07-11
last_updated: 2026-07-13
category: architecture-patterns
module: nanobot-manager/chat
problem_type: architecture_pattern
component: tooling
severity: medium
applies_when:
  - Integrating a streaming WebSocket client into a native Win32/C++ GUI
  - Separating transport logic from UI state management in a reactive UI framework
  - Running multi-agent code review on a new native feature with thread safety concerns
tags: [websocket, win32, winhttp, chat, core-ui, thread-safety, multi-agent-review, cpp17]
---

# Native WebSocket Chat Integration in a Win32/C++ GUI Application

## Context

The nanobot Windows GUI manager (`nanobot-manager/`) is a native Win32 C/C++ application built on the `core-ui` submodule framework. It manages a local Python LLM gateway (nanobot) — starting/stopping the process, health-checking via WinHTTP, and providing a dashboard UI.

The gateway exposes a WebSocket chat endpoint on its WebUI port (default 8765), but the manager had no way to chat from within the native UI. The challenge was integrating a streaming WebSocket client into a reactive UI framework (core-ui SFC pattern with `v-for`, `v-model`, ScrollView) while maintaining thread safety, clean separation of concerns, and a responsive UI during connect/disconnect operations.

No existing WebSocket, threading, or reactive-state patterns existed in the codebase to reference. The implementation was built from scratch, reviewed by a 9-agent multi-agent review panel, and hardened through two rounds of fixes.

## Guidance

### Three-Layer Architecture

The key architectural decision was separating the chat feature into three layers, each with a clear boundary:

| Layer | File | Responsibility |
|-------|------|----------------|
| **Transport** | `chat.c` / `chat.h` | WinHTTP WebSocket connect/send/receive, thread lifecycle, JSON frame parsing |
| **UI State** | `chat_ui.c` / `chat_ui.h` | Message array management, UI field updates, event dispatch, send orchestration |
| **App Wiring** | `main.cpp` | Window setup, widget mount callbacks, health check integration, i18n, lifecycle |

This separation prevents the transport layer from knowing about UI widgets, and prevents the UI state layer from knowing about WinHTTP handles. `main.cpp` wires them together via function pointers and event messages.

### Gateway Context Pattern

The UI state layer needs gateway context (health check function, port, auth secret) to validate send operations, but the click handler receives no parameters beyond `(widget, userdata)`. The solution is a **gateway context registration** pattern:

```c
/* chat_ui.h */
void chat_ui_set_gateway(int (*health_fn)(void), int webui_port, const char* webui_auth);

/* chat_ui.c — internal globals */
static int (*g_health_fn)(void) = NULL;
static int g_webui_port = 8765;
static char g_webui_auth[128] = {0};

/* The click handler reads stored context */
static void on_send_click(UiWidget w, void* ud) {
    (void)w; (void)ud;
    int running = g_health_fn ? g_health_fn() : 0;
    chat_ui_on_send(running, g_webui_port, g_webui_auth);
}
```

This lets `chat_ui.c` own the send flow entirely — reading the draft, validating health, connecting if needed, appending the user message, and calling `chat_send` — without `main.cpp` involvement in the click path.

### Thunk Adapter for Function Signatures

`chat_ui_set_gateway` expects an `int(void)` function, while live HTTP probing is owned by the asynchronous gateway lifecycle worker. A small thunk exposes the **cached lifecycle state** to the chat UI without performing network I/O on the UI thread:

```c
/* main.cpp */
static int health_running_thunk(void) {
    return nm_state_get() == NM_STATE_RUNNING ? 1 : 0;
}

/* At init: */
chat_ui_set_gateway(health_running_thunk, g_cfg.webui_port, g_cfg.webui_auth);
```

This is a zero-cost adapter that lets the UI state layer ask whether sending is currently allowed without knowing about lifecycle state, config structure, or port numbers. The worker-owned `nanobot_check_running` result is marshalled to the UI thread before this cached state changes.

### Event-Driven UI Updates via Window Messages

The transport layer communicates with the UI via `PostMessageW` with a custom window message (`WM_CHAT_EVENT`). The transport allocates detail strings with `malloc`, posts them as `LPARAM`, and the UI layer frees them:

```c
/* chat.c — transport posts events */
static void post_event(WPARAM kind, char* detail_or_null) {
    HWND hwnd;
    UINT msg;
    EnterCriticalSection(&g_cs);
    hwnd = g_hwnd;
    msg = g_msg_id;
    LeaveCriticalSection(&g_cs);
    if (!hwnd || !msg) {
        free(detail_or_null);
        return;
    }
    if (!PostMessageW(hwnd, msg, kind, (LPARAM)detail_or_null))
        free(detail_or_null);
}

/* main.cpp — single handler delegates to chat_ui */
if (msg == WM_CHAT_EVENT) {
    chat_ui_on_event(wp, (char*)lp);
    return 0;
}

/* chat_ui.c — processes all event types, frees detail */
void chat_ui_on_event(WPARAM kind, char* detail) {
    switch ((int)kind) {
    case CHAT_EVT_DELTA:
        if (detail) append_delta(detail);
        break;
    case CHAT_EVT_MESSAGE:
        if (detail) set_last_assistant(detail);
        break;
    /* ... CONNECTED, READY, DISCONNECTED, STREAM_END, ERROR ... */
    }
    free(detail);
}
```

### Async Disconnect for UI Responsiveness

Synchronous disconnect joins threads (up to 10s), which would freeze the UI. The `chat_disconnect_async` pattern runs disconnect on a worker thread and posts a `DISCONNECT_DONE` event when finished:

```c
/* chat.c */
void chat_disconnect_async(void) {
    /* CreateThread -> disconnect_worker -> chat_disconnect -> post DISCONNECT_DONE */
}

/* main.cpp — health check stopped path */
chat_disconnect_async();
chat_ui_on_gateway_stopped();
```

### Generation-Safe Thread Lifecycle

The receive thread must not close handles that belong to a newer connection. A generation counter prevents use-after-free:

```c
/* chat.c — recv_thread only closes if generation matches */
static volatile LONG g_generation = 0;

/* In recv_thread: */
LONG my_gen = g_generation;
/* ... receive loop ... */
EnterCriticalSection(&g_cs);
if (g_generation == my_gen && g_websocket) {
    WinHttpWebSocketClose(g_websocket, 1000, NULL, 0);
}
LeaveCriticalSection(&g_cs);
```

### Shutdown Ordering

The shutdown sequence prevents use-after-free on the UI page:

```c
/* main.cpp — exit path */
chat_shutdown();              /* 1. Null g_hwnd first, disconnect, free CS */

/* 2. Drain leftover WM_CHAT_EVENT payloads */
{
    MSG m;
    while (PeekMessageW(&m, hwnd, WM_CHAT_EVENT, WM_CHAT_EVENT, PM_REMOVE)) {
        free((void*)m.lParam);
    }
}

stop_uptime_timer();
tray_remove();
RemoveWindowSubclass(hwnd, subclass_proc, 0);
chat_ui_shutdown();          /* 3. Free g_messages, null UI pointers */
ui_page_destroy(g_page);     /* 4. Destroy UI page last */
```

`chat_shutdown` nulls `g_hwnd` before disconnecting, so any in-flight `post_event` calls see a null hwnd and free their detail instead of posting to a dead window.

## Why This Matters

### Why separate transport from UI state

Without separation, `main.cpp` would contain WinHTTP handle management, thread joins, JSON frame parsing, message array management, and UI field updates — all interleaved. The three-layer split means:
- Transport can be unit-tested in isolation (no UI dependency)
- UI state changes don't require touching WinHTTP code
- `main.cpp` stays focused on app-level wiring (tray, health, config, i18n)

### Why async disconnect matters

`chat_disconnect` joins the receive thread with a 10s timeout. Calling this synchronously from the health check timer would freeze the UI for up to 10s when the gateway stops. `chat_disconnect_async` moves this to a worker thread and posts `DISCONNECT_DONE` so the UI updates asynchronously.

### Why generation-safe close matters

Without generation tracking, a race occurs: connection A's recv_thread is closing its websocket, but connection B has already started and set `g_websocket` to a new handle. The recv_thread would close B's handle. The generation counter ensures recv_thread only closes handles from its own connection generation.

### Why the thunk adapter matters

Without the thunk, `chat_ui.c` would need to know about the gateway lifecycle state or process APIs, leaking `process.h` dependencies into the UI layer. The thunk keeps `chat_ui.c` dependent only on `chat.h`, `ui_core.h`, and `cJSON`.

## When to Apply

- **Integrating any streaming network client** (WebSocket, Server-Sent Events, raw TCP) into a native GUI with a message loop — use the PostMessage event pattern
- **Separating transport from reactive UI state** in any framework where UI updates are driven by external events — the three-layer split generalizes beyond Win32
- **Bridging function signature mismatches** when a callback-driven framework needs a `void`-parameter function but your API takes parameters — the thunk pattern is zero-cost and keeps dependencies clean
- **Thread lifecycle in connection-oriented code** — generation-safe close, joinable connect/recv threads, and async disconnect apply to any reconnecting client
- **Multi-agent code review on thread-safe code** — the 9-reviewer panel approach (correctness, security, reliability, adversarial, testing, maintainability, project-standards, agent-native, learnings) surfaces P0/P1 issues that single-pass review misses

## Examples

### Gateway context registration + thunk

```c
/* main.cpp */
static int health_running_thunk(void) {
    return nm_state_get() == NM_STATE_RUNNING ? 1 : 0;
}

/* At init */
chat_ui_bind(g_page, is_zh);
chat_ui_set_gateway(health_running_thunk, g_cfg.webui_port, g_cfg.webui_auth);
chat_ui_reset_idle();
```

### Send flow (chat_ui.c)

```c
void chat_ui_on_send(int health_running, int webui_port, const char* webui_auth) {
    if (!g_page) return;

    char* raw = ui_page_get_json(g_page, "draft");
    if (!raw) return;
    cJSON* j = cJSON_Parse(raw);
    ui_page_free(raw);
    const char* text = (j && cJSON_IsString(j)) ? j->valuestring : NULL;
    if (!text || !text[0]) { cJSON_Delete(j); return; }

    if (!health_running) {
        chat_ui_status_i18n("请先启动 nanobot", "Start nanobot first");
        cJSON_Delete(j);
        return;
    }

    if (!chat_is_ready()) {
        chat_ui_status_i18n("连接中…连接后请再次发送",
                           "Connecting… send again when connected");
        chat_set_endpoint(webui_port, webui_auth);
        chat_connect();
        cJSON_Delete(j);
        return;
    }

    append_msg("user", text);
    ui_page_set_text(g_page, "draft", L"");
    g_chat_streaming = 0;
    if (chat_send(text) != 0) {
        chat_ui_status_i18n("发送失败", "Send failed");
    }
    cJSON_Delete(j);
}
```

### Streaming delta accumulation (chat_ui.c)

```c
static void append_delta(const char* chunk) {
    if (!chunk || !chunk[0]) return;
    ensure_messages();
    int n = cJSON_GetArraySize(g_messages);
    if (!g_chat_streaming || n <= 0) {
        begin_assistant();  /* start a new assistant bubble */
        n = cJSON_GetArraySize(g_messages);
    }
    cJSON* last = cJSON_GetArrayItem(g_messages, n - 1);
    cJSON* t = cJSON_GetObjectItem(last, "text");
    const char* old = (t && cJSON_IsString(t)) ? t->valuestring : "";
    size_t len = strlen(old) + strlen(chunk) + 1;
    char* merged = (char*)malloc(len);
    if (!merged) return;
    snprintf(merged, len, "%s%s", old, chunk);
    cJSON_ReplaceItemInObject(last, "text", cJSON_CreateString(merged));
    free(merged);
    push_messages_ui();  /* sync to UI via ui_page_set_json */
}
```

### CRLF injection prevention (chat.c)

```c
static int secret_is_safe(const char* s) {
    if (!s) return 1;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p < 0x20)
            return 0;
    }
    return 1;
}

/* Before formatting HTTP header with the secret: */
if (!secret_is_safe(g_bootstrap_secret)) {
    /* reject — prevents header injection via webui_auth config */
}
```

### Health check integration (main.cpp)

```c
/* health_worker performs the blocking HTTP request off the UI thread and
 * posts WM_ASYNC_HEALTH_RESULT. This handler runs on the UI thread. */
static void apply_health_result(int running) {
    if (running) {
        /* ... update status to running ... */
        if (!chat_is_ready()) {
            chat_set_endpoint(g_cfg.webui_port, g_cfg.webui_auth);
            chat_connect();  /* auto-connect when gateway becomes healthy */
        }
    } else {
        /* ... update status to stopped ... */
        chat_disconnect_async();        /* non-blocking disconnect */
        chat_ui_on_gateway_stopped();   /* update UI status */
    }
}
```

The native gateway lifecycle also watches the real process handle for immediate exit events. HTTP health and process death remain separate signals: chat connection state follows the UI-thread lifecycle transition, never a synchronous health request from the send-click path.

## Related

- `AGENTS.md` — project layout, build commands, runtime behavior (updated with chat.c, webui_port, webui_auth)
- `core-ui/docs/uix-ai-guide.md` — core-ui SFC pattern reference (v-for, v-model, ScrollView)
- [State-Driven Gateway Lifecycle Management on Windows](state-driven-gateway-lifecycle.md) — asynchronous health ownership, process-exit events, state transitions, and handle-close discipline
- [nanobot gateway](https://github.com/HKUDS/nanobot) — upstream Python gateway with WebSocket chat endpoint
