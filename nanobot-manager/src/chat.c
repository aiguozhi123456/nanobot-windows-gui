#include "chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winhttp.h>
#include "cJSON.h"

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

/* WINHTTP_WEB_SOCKET_* types/APIs are provided by winhttp.h on Win8+ SDKs. */

static CRITICAL_SECTION g_cs;
static int g_cs_inited = 0;

static HWND g_hwnd = NULL;
static UINT g_msg_id = 0;

static int g_webui_port = 8765;
static char g_bootstrap_secret[128] = {0};

static HINTERNET g_session = NULL;
static HINTERNET g_connect = NULL;
static HINTERNET g_websocket = NULL;

#define CHAT_RECV_INITIAL_CAPACITY (16U * 1024U)
#define CHAT_RECV_MAX_WIRE_BYTES (2U * 1024U * 1024U)

static HANDLE g_thread = NULL;         /* recv_thread */
static HANDLE g_connect_thread = NULL; /* connect_thread */
static volatile LONG g_stop = 0;
static volatile LONG g_ready = 0;      /* socket up */
static volatile LONG g_connecting = 0;

enum {
    CHAT_LIFECYCLE_NONE,
    CHAT_LIFECYCLE_DISCONNECT,
    CHAT_LIFECYCLE_RECONNECT,
    CHAT_LIFECYCLE_SHUTDOWN
};

static SRWLOCK g_disconnect_lock = SRWLOCK_INIT;
static HANDLE g_lifecycle_event = NULL;
static HANDLE g_lifecycle_thread = NULL;
static LONG g_lifecycle_request = CHAT_LIFECYCLE_NONE;
static int g_lifecycle_busy = 0;
static int g_shutting_down = 0;

static char g_chat_id[128] = {0};
static char g_ws_path[256] = "/";
static char g_token[512] = {0};

static int chat_connect_locked(void);
static void chat_disconnect_internal(void);

/* Always-NUL-terminated bounded copy from untrusted/gateway strings. */
static void chat_copy_str(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

/* Reject CR/LF so secrets cannot inject extra HTTP headers. */
static int secret_is_safe(const char* s) {
    if (!s) return 1;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p < 0x20)
            return 0;
    }
    return 1;
}

static char* chat_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

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

static void close_handles_unlocked(void) {
    if (g_websocket) {
        WinHttpWebSocketClose(g_websocket, 1000, NULL, 0);
        WinHttpCloseHandle(g_websocket);
        g_websocket = NULL;
    }
    if (g_connect) {
        WinHttpCloseHandle(g_connect);
        g_connect = NULL;
    }
    if (g_session) {
        WinHttpCloseHandle(g_session);
        g_session = NULL;
    }
    g_ready = 0;
    g_chat_id[0] = '\0';
    SecureZeroMemory(g_token, sizeof(g_token));
}

static void reap_finished_threads_unlocked(void) {
    if (g_thread && WaitForSingleObject(g_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_connect_thread && WaitForSingleObject(g_connect_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_connect_thread);
        g_connect_thread = NULL;
    }
}

/* Percent-encode for query values (token). */
static void url_encode(const char* in, char* out, size_t out_sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!in || out_sz == 0) return;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 4 < out_sz; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

static int http_get_bootstrap(int port, const char* secret,
                              char* token_out, size_t token_sz,
                              char* ws_path_out, size_t ws_path_sz) {
    token_out[0] = '\0';
    chat_copy_str(ws_path_out, ws_path_sz, "/");

    HINTERNET session = WinHttpOpen(L"nanobot-manager/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return -1;

    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return -1;
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/webui/bootstrap",
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    if (secret && secret[0]) {
        if (!secret_is_safe(secret)) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return -1;
        }
        wchar_t hdr[256] = {0};
        swprintf_s(hdr, _countof(hdr), L"X-Nanobot-Auth: %hs\r\n", secret);
        WinHttpAddRequestHeaders(req, hdr, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        SecureZeroMemory(hdr, sizeof(hdr));
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    DWORD status = 0;
    DWORD sz = sizeof(status);
    WinHttpQueryHeaders(req,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    char body[8192];
    size_t total = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        if (total + avail >= sizeof(body) - 1)
            avail = (DWORD)(sizeof(body) - 1 - total);
        if (avail == 0) break;
        DWORD read = 0;
        if (!WinHttpReadData(req, body + total, avail, &read) || read == 0) break;
        total += read;
    }
    body[total] = '\0';

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);

    cJSON* root = cJSON_Parse(body);
    if (!root) return -1;

    cJSON* tok = cJSON_GetObjectItem(root, "token");
    if (!tok || !cJSON_IsString(tok) || !tok->valuestring[0])
        tok = cJSON_GetObjectItem(root, "api_token");
    if (tok && cJSON_IsString(tok) && tok->valuestring[0])
        chat_copy_str(token_out, token_sz, tok->valuestring);

    cJSON* wsp = cJSON_GetObjectItem(root, "ws_path");
    if (wsp && cJSON_IsString(wsp) && wsp->valuestring[0])
        chat_copy_str(ws_path_out, ws_path_sz, wsp->valuestring);

    cJSON_Delete(root);
    return 0;
}

static int ws_connect(int port, const char* path, const char* token,
                      HINTERNET* session_out, HINTERNET* connect_out,
                      HINTERNET* websocket_out) {
    char path_a[1024];
    char token_enc[1024];
    if (token && token[0]) {
        url_encode(token, token_enc, sizeof(token_enc));
        if (strchr(path, '?'))
            snprintf(path_a, sizeof(path_a), "%s&token=%s&client_id=nanobot-manager",
                path, token_enc);
        else
            snprintf(path_a, sizeof(path_a), "%s?token=%s&client_id=nanobot-manager",
                path, token_enc);
    } else {
        if (strchr(path, '?'))
            snprintf(path_a, sizeof(path_a), "%s&client_id=nanobot-manager", path);
        else
            snprintf(path_a, sizeof(path_a), "%s?client_id=nanobot-manager", path);
    }

    wchar_t path_w[1024];
    MultiByteToWideChar(CP_UTF8, 0, path_a, -1, path_w, 1024);

    HINTERNET session = WinHttpOpen(L"nanobot-manager/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return -1;

    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

    HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return -1;
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path_w,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!ws) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    *session_out = session;
    *connect_out = conn;
    *websocket_out = ws;
    return 0;
}

static void handle_json_frame(const char* json, size_t len) {
    char* buf = (char*)malloc(len + 1);
    if (!buf) return;
    memcpy(buf, json, len);
    buf[len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON* ev = cJSON_GetObjectItem(root, "event");
    if (!ev || !cJSON_IsString(ev)) {
        /* Some gateways use "type" */
        ev = cJSON_GetObjectItem(root, "type");
    }
    if (!ev || !cJSON_IsString(ev) || !ev->valuestring) {
        cJSON_Delete(root);
        return;
    }

    const char* event = ev->valuestring;

    if (strcmp(event, "ready") == 0) {
        cJSON* cid = cJSON_GetObjectItem(root, "chat_id");
        EnterCriticalSection(&g_cs);
        if (cid && cJSON_IsString(cid) && cid->valuestring)
            chat_copy_str(g_chat_id, sizeof(g_chat_id), cid->valuestring);
        LeaveCriticalSection(&g_cs);
        /* CONNECTED was already posted when the socket came up. */
        post_event(CHAT_EVT_READY, NULL);
    } else if (strcmp(event, "delta") == 0) {
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (!text) text = cJSON_GetObjectItem(root, "content");
        if (text && cJSON_IsString(text) && text->valuestring)
            post_event(CHAT_EVT_DELTA, chat_strdup(text->valuestring));
    } else if (strcmp(event, "message") == 0) {
        cJSON* kind = cJSON_GetObjectItem(root, "kind");
        const char* kind_name = (kind && cJSON_IsString(kind)) ? kind->valuestring : NULL;
        /* These are interim activity frames, not completed assistant turns. */
        if (!kind_name || (strcmp(kind_name, "tool_hint") != 0 &&
                           strcmp(kind_name, "progress") != 0 &&
                           strcmp(kind_name, "reasoning") != 0)) {
            cJSON* text = cJSON_GetObjectItem(root, "text");
            if (!text) text = cJSON_GetObjectItem(root, "content");
            if (text && cJSON_IsString(text) && text->valuestring)
                post_event(CHAT_EVT_MESSAGE, chat_strdup(text->valuestring));
        }
    } else if (strcmp(event, "stream_end") == 0 || strcmp(event, "done") == 0) {
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (!text) text = cJSON_GetObjectItem(root, "content");
        post_event(CHAT_EVT_STREAM_END,
            text && cJSON_IsString(text) && text->valuestring ? chat_strdup(text->valuestring) : NULL);
        if (strcmp(event, "done") == 0)
            post_event(CHAT_EVT_TURN_END, NULL);
    } else if (strcmp(event, "turn_end") == 0) {
        post_event(CHAT_EVT_TURN_END, NULL);
    } else if (strcmp(event, "error") == 0) {
        cJSON* detail = cJSON_GetObjectItem(root, "detail");
        if (!detail) detail = cJSON_GetObjectItem(root, "message");
        if (!detail) detail = cJSON_GetObjectItem(root, "error");
        if (!detail) detail = cJSON_GetObjectItem(root, "text");
        const char* msg = (detail && cJSON_IsString(detail) && detail->valuestring)
            ? detail->valuestring : "error";
        post_event(CHAT_EVT_ERROR, chat_strdup(msg));
    }

    cJSON_Delete(root);
}

static DWORD WINAPI recv_thread(LPVOID param) {
    (void)param;
    char* frame = NULL;
    size_t acc = 0;
    size_t frame_capacity = 0;
    const char* receive_error = NULL;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        HINTERNET ws;
        EnterCriticalSection(&g_cs);
        ws = g_websocket;
        LeaveCriticalSection(&g_cs);
        if (!ws) break;

        if (acc == frame_capacity) {
            size_t new_capacity;
            char* grown;

            if (frame_capacity >= CHAT_RECV_MAX_WIRE_BYTES) {
                receive_error = "Incoming WebSocket message exceeds 2 MiB";
                break;
            }
            new_capacity = frame_capacity ? frame_capacity * 2 : CHAT_RECV_INITIAL_CAPACITY;
            if (new_capacity > CHAT_RECV_MAX_WIRE_BYTES || new_capacity < frame_capacity)
                new_capacity = CHAT_RECV_MAX_WIRE_BYTES;
            grown = (char*)realloc(frame, new_capacity + 1);
            if (!grown) {
                receive_error = "Out of memory receiving WebSocket message";
                break;
            }
            frame = grown;
            frame_capacity = new_capacity;
        }

        DWORD space = (DWORD)(frame_capacity - acc);

        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE btype = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        DWORD err = WinHttpWebSocketReceive(ws, frame + acc, space, &read, &btype);

        if (err != ERROR_SUCCESS) {
            break;
        }

        if (btype == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            break;

        if (btype == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            btype == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
            btype == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            btype == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            if (read > CHAT_RECV_MAX_WIRE_BYTES - acc) {
                receive_error = "Incoming WebSocket message exceeds 2 MiB";
                break;
            }
            acc += read;
            frame[acc] = '\0';

            if (btype == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                btype == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                handle_json_frame(frame, acc);
                acc = 0;
            }
        }
    }

    free(frame);
    if (receive_error)
        post_event(CHAT_EVT_ERROR, chat_strdup(receive_error));

    EnterCriticalSection(&g_cs);
    /* Only tear down if we still own the live socket (avoid closing a
       newer generation installed by a subsequent connect). */
    if (g_websocket) {
        close_handles_unlocked();
    }
    LeaveCriticalSection(&g_cs);

    InterlockedExchange(&g_ready, 0);
    /* Do NOT clear g_connecting here — that flag is owned by
       chat_connect / connect_thread for the duration of a connect attempt. */
    post_event(CHAT_EVT_DISCONNECTED, NULL);
    return 0;
}

static DWORD WINAPI connect_thread(LPVOID param) {
    (void)param;
    int port;
    char secret[128];
    char token[512];
    char ws_path[256];
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET websocket = NULL;

    EnterCriticalSection(&g_cs);
    port = g_webui_port;
    chat_copy_str(secret, sizeof(secret), g_bootstrap_secret);
    LeaveCriticalSection(&g_cs);

    token[0] = '\0';
    chat_copy_str(ws_path, sizeof(ws_path), "/");

    int boot_ok = http_get_bootstrap(port, secret, token, sizeof(token),
        ws_path, sizeof(ws_path));

    if (InterlockedCompareExchange(&g_stop, 0, 0)) {
        SecureZeroMemory(secret, sizeof(secret));
        SecureZeroMemory(token, sizeof(token));
        InterlockedExchange(&g_connecting, 0);
        return 0;
    }

    int rc = -1;
    if (boot_ok == 0)
        rc = ws_connect(port, ws_path, token, &session, &connect, &websocket);

    /* Fallback: no token / default path (websocketRequiresToken:false) */
    if (rc != 0 && !InterlockedCompareExchange(&g_stop, 0, 0)) {
        chat_copy_str(token, sizeof(token), "");
        chat_copy_str(ws_path, sizeof(ws_path), "/");
        rc = ws_connect(port, ws_path, token, &session, &connect, &websocket);
    }

    if (rc != 0) {
        SecureZeroMemory(secret, sizeof(secret));
        SecureZeroMemory(token, sizeof(token));
        InterlockedExchange(&g_connecting, 0);
        post_event(CHAT_EVT_ERROR, chat_strdup("WebSocket connect failed"));
        post_event(CHAT_EVT_DISCONNECTED, NULL);
        return 0;
    }

    EnterCriticalSection(&g_cs);
    if (InterlockedCompareExchange(&g_stop, 0, 0) || g_thread) {
        if (websocket) {
            WinHttpWebSocketClose(websocket, 1000, NULL, 0);
            WinHttpCloseHandle(websocket);
        }
        if (connect)
            WinHttpCloseHandle(connect);
        if (session)
            WinHttpCloseHandle(session);
        LeaveCriticalSection(&g_cs);
        SecureZeroMemory(secret, sizeof(secret));
        SecureZeroMemory(token, sizeof(token));
        InterlockedExchange(&g_connecting, 0);
        post_event(CHAT_EVT_DISCONNECTED, NULL);
        return 0;
    }

    close_handles_unlocked();
    g_session = session;
    g_connect = connect;
    g_websocket = websocket;
    chat_copy_str(g_ws_path, sizeof(g_ws_path), ws_path);

    HANDLE th = CreateThread(NULL, 0, recv_thread, NULL, 0, NULL);
    if (!th) {
        close_handles_unlocked();
        LeaveCriticalSection(&g_cs);
        SecureZeroMemory(secret, sizeof(secret));
        SecureZeroMemory(token, sizeof(token));
        InterlockedExchange(&g_connecting, 0);
        post_event(CHAT_EVT_ERROR, chat_strdup("Failed to start receive thread"));
        post_event(CHAT_EVT_DISCONNECTED, NULL);
        return 0;
    }

    g_thread = th;
    /* Socket is up; chat_id may still be empty until server "ready".
       chat_send uses legacy {content} until chat_id is set. */
    g_ready = 1;
    LeaveCriticalSection(&g_cs);

    InterlockedExchange(&g_connecting, 0);
    SecureZeroMemory(secret, sizeof(secret));
    SecureZeroMemory(token, sizeof(token));
    post_event(CHAT_EVT_CONNECTED, NULL);
    return 0;
}

static void chat_disconnect_internal(void) {
    HANDLE th_recv = NULL;
    HANDLE th_conn = NULL;

    InterlockedExchange(&g_stop, 1);

    EnterCriticalSection(&g_cs);
    if (g_websocket)
        WinHttpWebSocketClose(g_websocket, 1000, NULL, 0);
    close_handles_unlocked();
    th_recv = g_thread;
    g_thread = NULL;
    th_conn = g_connect_thread;
    g_connect_thread = NULL;
    LeaveCriticalSection(&g_cs);

    if (th_recv) {
        WaitForSingleObject(th_recv, INFINITE);
        CloseHandle(th_recv);
    }
    if (th_conn) {
        WaitForSingleObject(th_conn, INFINITE);
        CloseHandle(th_conn);
    }

    InterlockedExchange(&g_connecting, 0);
    InterlockedExchange(&g_ready, 0);
}

static DWORD WINAPI lifecycle_thread(LPVOID param) {
    (void)param;

    for (;;) {
        int request;

        WaitForSingleObject(g_lifecycle_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_cs);
            request = (int)g_lifecycle_request;
            if (request == CHAT_LIFECYCLE_NONE) {
                g_lifecycle_busy = 0;
                LeaveCriticalSection(&g_cs);
                break;
            }
            g_lifecycle_busy = 1;
            LeaveCriticalSection(&g_cs);

            AcquireSRWLockExclusive(&g_disconnect_lock);
            chat_disconnect_internal();
            ReleaseSRWLockExclusive(&g_disconnect_lock);

            EnterCriticalSection(&g_cs);
            if (g_lifecycle_request != request) {
                LeaveCriticalSection(&g_cs);
                continue;
            }
            if (request == CHAT_LIFECYCLE_SHUTDOWN) {
                g_lifecycle_request = CHAT_LIFECYCLE_NONE;
                g_lifecycle_busy = 0;
                SecureZeroMemory(g_bootstrap_secret, sizeof(g_bootstrap_secret));
                LeaveCriticalSection(&g_cs);
                post_event(CHAT_EVT_DISCONNECT_DONE, NULL);
                return 0;
            }
            if (request == CHAT_LIFECYCLE_DISCONNECT) {
                g_lifecycle_request = CHAT_LIFECYCLE_NONE;
                g_lifecycle_busy = 0;
                SecureZeroMemory(g_bootstrap_secret, sizeof(g_bootstrap_secret));
                LeaveCriticalSection(&g_cs);
                post_event(CHAT_EVT_DISCONNECT_DONE, NULL);
                break;
            }
            LeaveCriticalSection(&g_cs);

            EnterCriticalSection(&g_cs);
            if (g_lifecycle_request == CHAT_LIFECYCLE_RECONNECT && !g_shutting_down) {
                int start_reconnect;
                g_lifecycle_request = CHAT_LIFECYCLE_NONE;
                LeaveCriticalSection(&g_cs);

                /* Queue completion before any replacement worker can report
                   CONNECTED or READY. */
                post_event(CHAT_EVT_DISCONNECT_DONE, NULL);

                AcquireSRWLockExclusive(&g_disconnect_lock);
                EnterCriticalSection(&g_cs);
                start_reconnect = !g_shutting_down &&
                    g_lifecycle_request == CHAT_LIFECYCLE_NONE;
                if (start_reconnect)
                    chat_connect_locked();
                if (start_reconnect)
                    g_lifecycle_busy = 0;
                LeaveCriticalSection(&g_cs);
                ReleaseSRWLockExclusive(&g_disconnect_lock);
                if (start_reconnect)
                    break;
                continue;
            }
            LeaveCriticalSection(&g_cs);
        }
    }
}

static void chat_request_lifecycle(int request) {
    HANDLE event = NULL;

    if (!g_cs_inited) return;
    EnterCriticalSection(&g_cs);
    if (!g_lifecycle_thread || !g_lifecycle_event || g_shutting_down) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    g_lifecycle_request = request;
    event = g_lifecycle_event;
    LeaveCriticalSection(&g_cs);
    SetEvent(event);
}

void chat_init(HWND hwnd, UINT msg_id) {
    if (!g_cs_inited) {
        InitializeCriticalSection(&g_cs);
        g_cs_inited = 1;
    }
    EnterCriticalSection(&g_cs);
    g_hwnd = hwnd;
    g_msg_id = msg_id;
    g_shutting_down = 0;
    g_lifecycle_request = CHAT_LIFECYCLE_NONE;
    g_lifecycle_busy = 0;
    LeaveCriticalSection(&g_cs);

    g_lifecycle_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_lifecycle_event)
        return;
    g_lifecycle_thread = CreateThread(NULL, 0, lifecycle_thread, NULL, 0, NULL);
    if (!g_lifecycle_thread) {
        CloseHandle(g_lifecycle_event);
        g_lifecycle_event = NULL;
    }
}

void chat_set_endpoint(int webui_port, const char* bootstrap_secret) {
    if (!g_cs_inited) {
        InitializeCriticalSection(&g_cs);
        g_cs_inited = 1;
    }
    EnterCriticalSection(&g_cs);
    g_webui_port = (webui_port > 0 && webui_port <= 65535) ? webui_port : 8765;
    SecureZeroMemory(g_bootstrap_secret, sizeof(g_bootstrap_secret));
    if (bootstrap_secret)
        chat_copy_str(g_bootstrap_secret, sizeof(g_bootstrap_secret), bootstrap_secret);
    else
        g_bootstrap_secret[0] = '\0';
    LeaveCriticalSection(&g_cs);
}

static int chat_connect_locked(void) {
    if (InterlockedCompareExchange(&g_ready, 0, 0))
        return 0;
    if (InterlockedCompareExchange(&g_connecting, 1, 0) != 0)
        return 0;

    InterlockedExchange(&g_stop, 0);

    HANDLE th = CreateThread(NULL, 0, connect_thread, NULL, 0, NULL);
    if (!th) {
        InterlockedExchange(&g_connecting, 0);
        return -1;
    }
    reap_finished_threads_unlocked();
    g_connect_thread = th;
    return 0;
}

int chat_connect(void) {
    int result;

    if (!g_cs_inited) return -1;
    AcquireSRWLockExclusive(&g_disconnect_lock);
    EnterCriticalSection(&g_cs);
    reap_finished_threads_unlocked();
    if (g_shutting_down || g_lifecycle_busy) {
        LeaveCriticalSection(&g_cs);
        ReleaseSRWLockExclusive(&g_disconnect_lock);
        return -1;
    }
    if (g_thread || g_connect_thread) {
        LeaveCriticalSection(&g_cs);
        ReleaseSRWLockExclusive(&g_disconnect_lock);
        return 0;
    }
    result = chat_connect_locked();
    LeaveCriticalSection(&g_cs);
    ReleaseSRWLockExclusive(&g_disconnect_lock);
    return result;
}

void chat_disconnect(void) {
    if (!g_cs_inited) return;
    AcquireSRWLockExclusive(&g_disconnect_lock);
    chat_disconnect_internal();
    EnterCriticalSection(&g_cs);
    SecureZeroMemory(g_bootstrap_secret, sizeof(g_bootstrap_secret));
    LeaveCriticalSection(&g_cs);
    ReleaseSRWLockExclusive(&g_disconnect_lock);
}

void chat_disconnect_async(void) {
    chat_request_lifecycle(CHAT_LIFECYCLE_DISCONNECT);
}

void chat_reconnect_async(void) {
    chat_request_lifecycle(CHAT_LIFECYCLE_RECONNECT);
}

int chat_is_connected(void) {
    return InterlockedCompareExchange(&g_ready, 0, 0) ? 1 : 0;
}

/* Socket up is enough to send; typed chat_id preferred when available.
   UI should still prefer CHAT_EVT_READY for canSend when chat_id matters. */
int chat_is_ready(void) {
    return InterlockedCompareExchange(&g_ready, 0, 0) ? 1 : 0;
}

int chat_send(const char* utf8_text) {
    if (!utf8_text || !utf8_text[0]) return -1;
    if (!chat_is_ready()) return -1;

    char chat_id[128];
    HINTERNET ws;
    DWORD err;

    EnterCriticalSection(&g_cs);
    chat_copy_str(chat_id, sizeof(chat_id), g_chat_id);
    ws = g_websocket;
    if (!ws) {
        LeaveCriticalSection(&g_cs);
        return -1;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        LeaveCriticalSection(&g_cs);
        return -1;
    }

    if (chat_id[0]) {
        if (!cJSON_AddStringToObject(root, "type", "message") ||
            !cJSON_AddStringToObject(root, "chat_id", chat_id) ||
            !cJSON_AddStringToObject(root, "content", utf8_text)) {
            cJSON_Delete(root);
            LeaveCriticalSection(&g_cs);
            return -1;
        }
    } else if (!cJSON_AddStringToObject(root, "content", utf8_text)) {
        cJSON_Delete(root);
        LeaveCriticalSection(&g_cs);
        return -1;
    }

    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        LeaveCriticalSection(&g_cs);
        return -1;
    }

    /* Hold CS across send so disconnect cannot close the handle mid-call. */
    err = WinHttpWebSocketSend(ws,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)payload, (DWORD)strlen(payload));
    LeaveCriticalSection(&g_cs);
    free(payload);

    return (err == ERROR_SUCCESS) ? 0 : -1;
}

void chat_shutdown(void) {
    HANDLE lifecycle_thread_handle;
    HANDLE lifecycle_event;

    if (!g_cs_inited) return;
    EnterCriticalSection(&g_cs);
    g_hwnd = NULL;
    g_msg_id = 0;
    g_shutting_down = 1;
    g_lifecycle_request = CHAT_LIFECYCLE_SHUTDOWN;
    lifecycle_thread_handle = g_lifecycle_thread;
    lifecycle_event = g_lifecycle_event;
    LeaveCriticalSection(&g_cs);

    if (lifecycle_thread_handle && lifecycle_event) {
        SetEvent(lifecycle_event);
        WaitForSingleObject(lifecycle_thread_handle, INFINITE);
        CloseHandle(lifecycle_thread_handle);
        CloseHandle(lifecycle_event);
    } else {
        AcquireSRWLockExclusive(&g_disconnect_lock);
        chat_disconnect_internal();
        ReleaseSRWLockExclusive(&g_disconnect_lock);
        if (lifecycle_event)
            CloseHandle(lifecycle_event);
    }

    EnterCriticalSection(&g_cs);
    g_lifecycle_thread = NULL;
    g_lifecycle_event = NULL;
    SecureZeroMemory(g_bootstrap_secret, sizeof(g_bootstrap_secret));
    SecureZeroMemory(g_token, sizeof(g_token));
    LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&g_cs);
    g_cs_inited = 0;
}
