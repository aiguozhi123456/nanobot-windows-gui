#include "chat.hpp"

#include "chat_protocol.h"
#include "chat_history.h"
#include "logger.h"
#include "util.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winhttp.h>
#include "cJSON.h"

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

#ifndef WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT
#define WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT 115
#endif

/* WINHTTP_WEB_SOCKET_* types/APIs are provided by winhttp.h on Win8+ SDKs. */

#define CHAT_CLOSE_STATUS WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS
#define CHAT_CLOSE_TIMEOUT_MS 2000U

#define CHAT_RECV_INITIAL_CAPACITY (16U * 1024U)
#define CHAT_RECV_MAX_WIRE_BYTES (2U * 1024U * 1024U)

#define CHAT_LOG_MODULE "chat"

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

const char* chat_conn_state_name(ChatConnState state) {
    switch (state) {
    case ChatConnState::Idle: return "idle";
    case ChatConnState::Connecting: return "connecting";
    case ChatConnState::Connected: return "connected";
    case ChatConnState::Attached: return "attached";
    default: return "?";
    }
}

ChatClient& chat_client() {
    static ChatClient instance;
    return instance;
}

/* --- state machine ------------------------------------------------------- */

bool ChatClient::transitionState(ChatConnState expected, ChatConnState desired) {
    ChatConnState current = expected;
    if (state_.compare_exchange_strong(current, desired)) {
        nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "state: %s -> %s",
               chat_conn_state_name(expected), chat_conn_state_name(desired));
        return true;
    }
    nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "state: %s -> %s lost race (now %s)",
           chat_conn_state_name(expected), chat_conn_state_name(desired),
           chat_conn_state_name(current));
    return false;
}

void ChatClient::forceStateIdle() {
    ChatConnState previous = state_.exchange(ChatConnState::Idle);
    if (previous != ChatConnState::Idle)
        nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "state: %s -> idle (forced)",
               chat_conn_state_name(previous));
}

/* --- event plumbing ------------------------------------------------------ */

uint64_t ChatClient::invalidateGeneration() {
    for (;;) {
        uint64_t current = generation_.load();
        uint64_t next = chat_protocol_next_generation(current);
        if (generation_.compare_exchange_strong(current, next))
            return next;
    }
}

void ChatClient::postEvent(WPARAM kind, uint64_t generation, char* detail_or_null) {
    HWND hwnd;
    UINT msg;
    chat_event* event = (chat_event*)malloc(sizeof(*event));
    if (!event) { free(detail_or_null); return; }
    event->generation = generation;
    event->detail = detail_or_null;
    {
        std::lock_guard<std::mutex> lock(cs_);
        hwnd = hwnd_;
        msg = msg_id_;
    }
    if (!hwnd || !msg) {
        chat_event_free(event);
        return;
    }
    if (!PostMessageW(hwnd, msg, kind, (LPARAM)event))
        chat_event_free(event);
}

/* --- handle / thread bookkeeping (cs_ held) ------------------------------ */

/* WinHttpWebSocketClose cancels a pending receive. It must run while cs_
   serializes it with sends, but it must not release the socket handle. */
void ChatClient::cancelWebsocketLocked(HINTERNET websocket) {
    if (websocket)
        WinHttpWebSocketClose(websocket, CHAT_CLOSE_STATUS, NULL, 0);
}

/* A receive worker owns its exact websocket handle until it has returned.
   Call only after that worker has been joined or observed finished. */
void ChatClient::destroyConnectionHandlesLocked() {
    if (websocket_) {
        WinHttpCloseHandle(websocket_);
        websocket_ = NULL;
    }
    if (connect_) {
        WinHttpCloseHandle(connect_);
        connect_ = NULL;
    }
    if (session_) {
        WinHttpCloseHandle(session_);
        session_ = NULL;
    }
    chat_id_[0] = '\0';
    attach_id_[0] = '\0';
}

void ChatClient::reapFinishedThreadsLocked() {
    if (recv_thread_.joinable() && recv_finished_.load()) {
        recv_thread_.join();
        /* The recv worker already cleared the state if it still owned the
         * socket; Idle here means the connection is fully down and the
         * handles can be released. */
        if (state_.load() == ChatConnState::Idle)
            destroyConnectionHandlesLocked();
    }
    if (connect_thread_.joinable() && connect_finished_.load())
        connect_thread_.join();
}

bool ChatClient::lifecycleOperationalLocked() const {
    return chat_protocol_lifecycle_operational(lifecycle_running_ ? 1 : 0,
        lifecycle_running_ ? 1 : 0, shutting_down_ ? 1 : 0) != 0;
}

/* --- bootstrap + WS upgrade ---------------------------------------------- */

int ChatClient::httpGetBootstrap(int port, const char* secret,
                                 char* token_out, size_t token_size,
                                 char* ws_path_out, size_t ws_path_size) {
    token_out[0] = '\0';
    chat_copy_str(ws_path_out, ws_path_size, "/");

    HINTERNET session = WinHttpOpen(L"nanobot-manager/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return -1;

    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!conn) {
        nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "bootstrap: WinHttpConnect failed (%lu)", GetLastError());
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
            nm_log(NM_LOG_WARN, CHAT_LOG_MODULE, "bootstrap: secret rejected (control chars)");
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
        nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "bootstrap: request failed (%lu)", GetLastError());
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
        nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "bootstrap: HTTP %lu", status);
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
        chat_copy_str(token_out, token_size, tok->valuestring);

    cJSON* wsp = cJSON_GetObjectItem(root, "ws_path");
    if (wsp && cJSON_IsString(wsp) && wsp->valuestring[0])
        chat_copy_str(ws_path_out, ws_path_size, wsp->valuestring);

    cJSON_Delete(root);
    return 0;
}

int ChatClient::wsConnect(int port, const char* path, const char* token,
                          HINTERNET* session_out, HINTERNET* connect_out,
                          HINTERNET* websocket_out) {
    /* Build the path with std::string so a long token cannot be silently
     * truncated the way a fixed snprintf buffer would. */
    std::string path_a;
    if (token && token[0]) {
        std::string token_enc = nm_percent_encode(token);
        path_a = std::string(path) + (strchr(path, '?') ? "&" : "?") +
                 "token=" + token_enc + "&client_id=nanobot-manager";
    } else {
        path_a = std::string(path) + (strchr(path, '?') ? "&" : "?") +
                 "client_id=nanobot-manager";
    }

    /* UTF-8 never expands to more UTF-16 code units than input bytes, so a
     * buffer sized from path_a cannot truncate. Fail on a zero return
     * instead of proceeding with a conversion that did not happen. */
    std::wstring path_w(path_a.size() + 1, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, path_a.c_str(), -1,
                            &path_w[0], (int)path_w.size()) == 0) {
        nm_log(NM_LOG_WARN, CHAT_LOG_MODULE,
               "ws: request path conversion failed (%lu)", GetLastError());
        return -1;
    }

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

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path_w.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "ws: upgrade option failed (%lu)", GetLastError());
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return -1;
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "ws: handshake failed (%lu)", GetLastError());
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

    {
        DWORD close_timeout = CHAT_CLOSE_TIMEOUT_MS;
        if (!WinHttpSetOption(ws, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
                &close_timeout, sizeof(close_timeout))) {
            WinHttpCloseHandle(ws);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            return -1;
        }
    }

    *session_out = session;
    *connect_out = conn;
    *websocket_out = ws;
    return 0;
}

/* --- frame dispatch (recv worker) ---------------------------------------- */

void ChatClient::handleJsonFrame(uint64_t generation, const char* json, size_t len) {
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
    nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "frame: event=%s", event);

    if (strcmp(event, "ready") == 0) {
        cJSON* cid = cJSON_GetObjectItem(root, "chat_id");
        if (cid && cJSON_IsString(cid) && cid->valuestring)
            postEvent(CHAT_EVT_READY, generation, chat_strdup(cid->valuestring));
    } else if (strcmp(event, "attached") == 0) {
        cJSON* cid = cJSON_GetObjectItem(root, "chat_id");
        int match = 0;
        char resolved_id[sizeof(attach_id_)];
        const char* attached_id = cid && cJSON_IsString(cid) ? cid->valuestring : NULL;
        {
            std::lock_guard<std::mutex> lock(cs_);
            if (chat_protocol_resolve_attached(attach_id_, attached_id, cid != NULL,
                    resolved_id, sizeof(resolved_id))) {
                chat_copy_str(chat_id_, sizeof(chat_id_), resolved_id);
                /* Connected -> Attached; a lost CAS (duplicate frame, or Idle
                 * after a racing teardown) must not suppress the event --
                 * the generation guard owns staleness. */
                transitionState(ChatConnState::Connected, ChatConnState::Attached);
                match = 1;
            }
        }
        if (match) postEvent(CHAT_EVT_ATTACHED, generation, chat_strdup(resolved_id));
    } else if (strcmp(event, "delta") == 0) {
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (!text) text = cJSON_GetObjectItem(root, "content");
        if (text && cJSON_IsString(text) && text->valuestring)
            postEvent(CHAT_EVT_DELTA, generation, chat_strdup(text->valuestring));
    } else if (strcmp(event, "message") == 0) {
        cJSON* kind = cJSON_GetObjectItem(root, "kind");
        const char* kind_name = (kind && cJSON_IsString(kind)) ? kind->valuestring : NULL;
        /* These are interim activity frames, not completed assistant turns:
         * not rendered, but they keep the turn watchdog alive (a slow tool
         * must not read as a hung gateway). */
        if (kind_name && (strcmp(kind_name, "tool_hint") == 0 ||
                          strcmp(kind_name, "progress") == 0 ||
                          strcmp(kind_name, "reasoning") == 0)) {
            postEvent(CHAT_EVT_ACTIVITY, generation, NULL);
        } else {
            cJSON* text = cJSON_GetObjectItem(root, "text");
            if (!text) text = cJSON_GetObjectItem(root, "content");
            if (text && cJSON_IsString(text) && text->valuestring)
                postEvent(CHAT_EVT_MESSAGE, generation, chat_strdup(text->valuestring));
        }
    } else if (strcmp(event, "stream_end") == 0 || strcmp(event, "done") == 0) {
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (!text) text = cJSON_GetObjectItem(root, "content");
        postEvent(CHAT_EVT_STREAM_END, generation,
            text && cJSON_IsString(text) && text->valuestring ? chat_strdup(text->valuestring) : NULL);
        if (strcmp(event, "done") == 0)
            postEvent(CHAT_EVT_TURN_END, generation, NULL);
    } else if (strcmp(event, "turn_end") == 0) {
        postEvent(CHAT_EVT_TURN_END, generation, NULL);
    } else if (strcmp(event, "error") == 0) {
        cJSON* detail = cJSON_GetObjectItem(root, "detail");
        if (!detail) detail = cJSON_GetObjectItem(root, "message");
        if (!detail) detail = cJSON_GetObjectItem(root, "error");
        if (!detail) detail = cJSON_GetObjectItem(root, "text");
        const char* msg = (detail && cJSON_IsString(detail) && detail->valuestring)
            ? detail->valuestring : "error";
        postEvent(CHAT_EVT_ERROR, generation, chat_strdup(msg));
    } else {
        /* Any other event (reasoning_delta, goal_status, file_edit,
         * session_updated, ...) is not rendered, but still proves the
         * agent is alive mid-turn. */
        postEvent(CHAT_EVT_ACTIVITY, generation, NULL);
    }

    cJSON_Delete(root);
}

/* --- workers -------------------------------------------------------------- */

void ChatClient::recvThreadMain(RecvContext context) {
    HINTERNET ws = context.websocket;
    uint64_t generation = context.generation;
    char* frame = NULL;
    size_t acc = 0;
    size_t frame_capacity = 0;
    const char* receive_error = NULL;
    int peer_closed = 0;

    try {
        while (!stop_.load()) {
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
                nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "recv: receive err=%lu", err);
                break;
            }

            if (btype == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                peer_closed = 1;
                break;
            }

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
                    handleJsonFrame(generation, frame, acc);
                    acc = 0;
                }
            }
        }
    } catch (...) {
        nm_log(NM_LOG_ERROR, CHAT_LOG_MODULE, "recv worker: unexpected exception");
        receive_error = "Receive worker failed";
    }

    free(frame);
    if (receive_error)
        postEvent(CHAT_EVT_ERROR, generation, chat_strdup(receive_error));
    if (peer_closed)
        nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "recv: peer closed connection");

    {
        std::lock_guard<std::mutex> lock(cs_);
        /* A peer close must be acknowledged. Keep the handle alive for the
           lifecycle owner, which destroys it only after this worker joined. */
        if (websocket_ == ws) {
            if (peer_closed)
                cancelWebsocketLocked(ws);
            forceStateIdle();
            chat_id_[0] = '\0';
            attach_id_[0] = '\0';
        }
    }

    /* Do NOT touch the Connecting state here -- that belongs to the connect
       worker for the duration of a connect attempt. */
    postEvent(CHAT_EVT_DISCONNECTED, generation, NULL);
    recv_finished_.store(true);   /* last statement: enables try-join reaping */
}

void ChatClient::connectThreadMain(ConnectContext context) {
    uint64_t generation = context.generation;
    int port;
    char secret[128];
    char token[512];
    char ws_path[256];
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET websocket = NULL;

    try {
        {
            std::lock_guard<std::mutex> lock(cs_);
            port = webui_port_;
            chat_copy_str(secret, sizeof(secret), bootstrap_secret_);
        }

        token[0] = '\0';
        chat_copy_str(ws_path, sizeof(ws_path), "/");

        int boot_ok = httpGetBootstrap(port, secret, token, sizeof(token),
            ws_path, sizeof(ws_path));
        nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "bootstrap: %s (port=%d)",
               boot_ok == 0 ? "ok" : "failed", port);

        if (stop_.load()) {
            SecureZeroMemory(secret, sizeof(secret));
            SecureZeroMemory(token, sizeof(token));
            transitionState(ChatConnState::Connecting, ChatConnState::Idle);
            connect_finished_.store(true);
            return;
        }

        int rc = -1;
        if (boot_ok == 0)
            rc = wsConnect(port, ws_path, token, &session, &connect, &websocket);

        /* Fallback: no token / default path (websocketRequiresToken:false) */
        if (rc != 0 && !stop_.load()) {
            nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "ws: retrying without token");
            chat_copy_str(token, sizeof(token), "");
            chat_copy_str(ws_path, sizeof(ws_path), "/");
            rc = wsConnect(port, ws_path, token, &session, &connect, &websocket);
        }

        if (rc != 0) {
            SecureZeroMemory(secret, sizeof(secret));
            SecureZeroMemory(token, sizeof(token));
            nm_log(NM_LOG_WARN, CHAT_LOG_MODULE, "connect: websocket failed (port=%d)", port);
            transitionState(ChatConnState::Connecting, ChatConnState::Idle);
            postEvent(CHAT_EVT_ERROR, generation, chat_strdup("WebSocket connect failed"));
            postEvent(CHAT_EVT_DISCONNECTED, generation, NULL);
            connect_finished_.store(true);
            return;
        }

        /* Decide under cs_, then post events after releasing it: postEvent()
         * takes cs_ itself, so calling it inside this scope would self-
         * deadlock on the non-recursive mutex. */
        bool connect_abort = false;
        char* error_detail = NULL;      /* post CHAT_EVT_ERROR first when set */
        bool post_disconnected = false; /* post CHAT_EVT_DISCONNECTED after */
        {
            std::lock_guard<std::mutex> lock(cs_);
            if (stop_.load() || recv_thread_.joinable()) {
                if (websocket) {
                    cancelWebsocketLocked(websocket);
                    WinHttpCloseHandle(websocket);
                }
                if (connect)
                    WinHttpCloseHandle(connect);
                if (session)
                    WinHttpCloseHandle(session);
                transitionState(ChatConnState::Connecting, ChatConnState::Idle);
                SecureZeroMemory(secret, sizeof(secret));
                SecureZeroMemory(token, sizeof(token));
                post_disconnected = true;
                connect_abort = true;
            } else {
                destroyConnectionHandlesLocked();
                session_ = session;
                connect_ = connect;
                websocket_ = websocket;
                /* Publish readiness before the worker can observe an immediate
                 * peer close; a lost CAS here means a teardown already won. */
                if (!transitionState(ChatConnState::Connecting, ChatConnState::Connected)) {
                    destroyConnectionHandlesLocked();
                    SecureZeroMemory(secret, sizeof(secret));
                    SecureZeroMemory(token, sizeof(token));
                    post_disconnected = true;
                    connect_abort = true;
                } else {
                    recv_finished_ = false;
                    try {
                        recv_thread_ = std::thread(&ChatClient::recvThreadMain, this,
                                                   RecvContext{websocket, generation});
                    } catch (...) {
                        destroyConnectionHandlesLocked();
                        SecureZeroMemory(secret, sizeof(secret));
                        SecureZeroMemory(token, sizeof(token));
                        forceStateIdle();
                        error_detail = chat_strdup("Failed to start receive thread");
                        post_disconnected = true;
                        connect_abort = true;
                    }
                    /* Socket is up; a READY frame must still offer a session
                     * for attachment. */
                }
            }
        }

        if (error_detail)
            postEvent(CHAT_EVT_ERROR, generation, error_detail);
        if (post_disconnected)
            postEvent(CHAT_EVT_DISCONNECTED, generation, NULL);
        if (connect_abort) {
            connect_finished_.store(true);
            return;
        }

        SecureZeroMemory(secret, sizeof(secret));
        SecureZeroMemory(token, sizeof(token));
        nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "connect: websocket up (generation=%llu)",
               (unsigned long long)generation);
        postEvent(CHAT_EVT_CONNECTED, generation, NULL);
    } catch (...) {
        SecureZeroMemory(secret, sizeof(secret));
        SecureZeroMemory(token, sizeof(token));
        nm_log(NM_LOG_ERROR, CHAT_LOG_MODULE, "connect worker: unexpected exception");
        forceStateIdle();
        postEvent(CHAT_EVT_ERROR, generation, chat_strdup("Connect worker failed"));
        postEvent(CHAT_EVT_DISCONNECTED, generation, NULL);
    }
    connect_finished_.store(true);   /* last statement: enables try-join reaping */
}

void ChatClient::lifecycleThreadMain() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(cs_);
            lifecycleCv_.wait(lock, [this] { return lifecycle_request_ != LIFECYCLE_NONE; });
        }
        for (;;) {
            LifecycleRequest request;
            {
                std::lock_guard<std::mutex> lock(cs_);
                request = lifecycle_request_;
                if (request == LIFECYCLE_NONE) {
                    lifecycle_busy_ = false;
                    break;
                }
                lifecycle_busy_ = true;
            }
            nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "lifecycle: request=%d", (int)request);
            {
                std::lock_guard<std::mutex> disconnect_lock(disconnectMutex_);
                disconnectInternal(0);
            }
            {
                std::unique_lock<std::mutex> lock(cs_);
                if (lifecycle_request_ != request)
                    continue;
                if (request == LIFECYCLE_SHUTDOWN) {
                    lifecycle_request_ = LIFECYCLE_NONE;
                    lifecycle_busy_ = false;
                    SecureZeroMemory(bootstrap_secret_, sizeof(bootstrap_secret_));
                    lock.unlock();
                    postEvent(CHAT_EVT_DISCONNECT_DONE, currentGeneration(), NULL);
                    return;
                }
                if (request == LIFECYCLE_DISCONNECT) {
                    lifecycle_request_ = LIFECYCLE_NONE;
                    lifecycle_busy_ = false;
                    SecureZeroMemory(bootstrap_secret_, sizeof(bootstrap_secret_));
                    lock.unlock();
                    postEvent(CHAT_EVT_DISCONNECT_DONE, currentGeneration(), NULL);
                    break;
                }
            }
            bool start_reconnect = false;
            {
                std::unique_lock<std::mutex> lock(cs_);
                if (lifecycle_request_ == LIFECYCLE_RECONNECT && !shutting_down_) {
                    lifecycle_request_ = LIFECYCLE_NONE;
                    lock.unlock();

                    uint64_t replacement_generation = invalidateGeneration();
                    /* Queue completion before any replacement worker can
                     * report CONNECTED or READY. */
                    postEvent(CHAT_EVT_DISCONNECT_DONE, replacement_generation, NULL);
                    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE,
                           "lifecycle: reconnecting (generation=%llu)",
                           (unsigned long long)replacement_generation);
                    {
                        std::lock_guard<std::mutex> disconnect_lock(disconnectMutex_);
                        std::lock_guard<std::mutex> lock2(cs_);
                        start_reconnect = !shutting_down_ &&
                            lifecycle_request_ == LIFECYCLE_NONE;
                        if (start_reconnect)
                            connectLocked(replacement_generation);
                        if (start_reconnect)
                            lifecycle_busy_ = false;
                    }
                    if (start_reconnect)
                        break;
                    continue;
                }
            }
        }
    }
}

/* --- teardown plumbing ---------------------------------------------------- */

void ChatClient::disconnectInternal(int invalidate_events) {
    std::thread th_recv;
    std::thread th_conn;

    if (invalidate_events)
        invalidateGeneration();
    stop_ = true;

    {
        std::lock_guard<std::mutex> lock(cs_);
        /* Clear readiness before cancelling Receive; cs_ also serializes Send. */
        forceStateIdle();
        cancelWebsocketLocked(websocket_);
        th_recv = std::move(recv_thread_);
        th_conn = std::move(connect_thread_);
    }

    if (th_recv.joinable())
        th_recv.join();
    if (th_conn.joinable())
        th_conn.join();

    {
        std::lock_guard<std::mutex> lock(cs_);
        /* The recv worker has returned, so no WinHTTP receive can use these
         * handles. */
        destroyConnectionHandlesLocked();
    }
}

void ChatClient::requestLifecycle(LifecycleRequest request) {
    if (!inited_) return;
    {
        std::lock_guard<std::mutex> lock(cs_);
        if (!lifecycle_running_ || shutting_down_)
            return;
        lifecycle_request_ = request;
    }
    lifecycleCv_.notify_one();
}

/* --- public API ----------------------------------------------------------- */

void ChatClient::init(HWND hwnd, UINT msg_id) {
    {
        std::lock_guard<std::mutex> lock(cs_);
        hwnd_ = hwnd;
        msg_id_ = msg_id;
        shutting_down_ = false;
        lifecycle_request_ = LIFECYCLE_NONE;
        lifecycle_busy_ = false;
        inited_ = true;
    }

    try {
        std::thread thread(&ChatClient::lifecycleThreadMain, this);
        std::lock_guard<std::mutex> lock(cs_);
        lifecycle_thread_ = std::move(thread);
        lifecycle_running_ = true;
    } catch (...) {
        nm_log(NM_LOG_ERROR, CHAT_LOG_MODULE, "init: failed to start lifecycle thread");
        std::lock_guard<std::mutex> lock(cs_);
        lifecycle_running_ = false;
    }
    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "init complete (msg=%u)", msg_id);
}

void ChatClient::setEndpoint(int webui_port, const char* bootstrap_secret) {
    {
        std::lock_guard<std::mutex> lock(cs_);
        webui_port = (webui_port > 0 && webui_port <= 65535) ? webui_port : 8765;
        webui_port_ = webui_port;
        SecureZeroMemory(bootstrap_secret_, sizeof(bootstrap_secret_));
        chat_copy_str(bootstrap_secret_, sizeof(bootstrap_secret_),
                      bootstrap_secret ? bootstrap_secret : "");
    }
    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "endpoint: port=%d secret=%s",
           webui_port, (bootstrap_secret && bootstrap_secret[0]) ? "set" : "none");
}

/* Start a connect. generation == 0 bumps the generation lazily AFTER the
 * early-return guards: bumping before them would orphan every event still
 * queued for a live connection and contradict the "idempotent if already
 * connected/connecting" contract. A non-zero generation is used verbatim
 * (the lifecycle reconnect path has already bumped and announced it). */
int ChatClient::connectLocked(uint64_t generation) {
    if (!lifecycleOperationalLocked())
        return -1;
    if (state_.load() != ChatConnState::Idle)
        return 0;
    if (!transitionState(ChatConnState::Idle, ChatConnState::Connecting))
        return 0;

    stop_ = false;
    connect_finished_ = false;
    if (!generation)
        generation = invalidateGeneration();

    reapFinishedThreadsLocked();
    if (connect_thread_.joinable() || recv_thread_.joinable()) {
        /* Unreachable through the public guards; fail rather than clobber a
         * live worker (assigning onto a joinable std::thread terminates). */
        nm_log(NM_LOG_ERROR, CHAT_LOG_MODULE, "connect: worker still present, aborting");
        forceStateIdle();
        return -1;
    }

    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "connect: starting (generation=%llu)",
           (unsigned long long)generation);
    try {
        connect_thread_ = std::thread(&ChatClient::connectThreadMain, this,
                                      ConnectContext{generation});
    } catch (...) {
        nm_log(NM_LOG_ERROR, CHAT_LOG_MODULE, "connect: failed to spawn worker");
        forceStateIdle();
        return -1;
    }
    return 0;
}

int ChatClient::connect() {
    if (!inited_) return -1;
    std::lock_guard<std::mutex> disconnect_lock(disconnectMutex_);
    std::lock_guard<std::mutex> lock(cs_);
    reapFinishedThreadsLocked();
    if (!lifecycleOperationalLocked() || lifecycle_busy_) {
        return -1;
    }
    if (recv_thread_.joinable() || connect_thread_.joinable()) {
        return 0;
    }
    return connectLocked(0);   /* 0: bump only if a connect really starts */
}

void ChatClient::disconnect() {
    if (!inited_) return;
    std::lock_guard<std::mutex> disconnect_lock(disconnectMutex_);
    disconnectInternal(1);
    {
        std::lock_guard<std::mutex> lock(cs_);
        SecureZeroMemory(bootstrap_secret_, sizeof(bootstrap_secret_));
    }
    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "disconnect: complete");
}

void ChatClient::disconnectAsync() {
    invalidateGeneration();
    requestLifecycle(LIFECYCLE_DISCONNECT);
    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "disconnect_async: requested");
}

void ChatClient::reconnectAsync() {
    invalidateGeneration();
    requestLifecycle(LIFECYCLE_RECONNECT);
    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "reconnect_async: requested");
}

bool ChatClient::isConnected() const {
    ChatConnState state = state_.load();
    return state == ChatConnState::Connected || state == ChatConnState::Attached;
}

/* Socket up is enough to send; typed chat_id preferred when available.
   UI should still prefer CHAT_EVT_READY for canSend when chat_id matters. */
bool ChatClient::isReady() const {
    return state_.load() == ChatConnState::Attached;
}

bool ChatClient::needsConnect() const {
    if (!inited_) return false;
    std::lock_guard<std::mutex> lock(cs_);
    return lifecycleOperationalLocked() && state_.load() == ChatConnState::Idle;
}

int ChatClient::attach(const char* chat_id) {
    cJSON* root;
    char* payload;
    HINTERNET ws;
    DWORD err;
    if (!chat_history_is_valid_chat_id(chat_id)) return -1;
    if (!inited_) return -1;
    {
        std::lock_guard<std::mutex> lock(cs_);
        if (!isConnected() || !websocket_ || stop_.load()) return -1;
        /* Re-attach demotes Attached -> Connected; a first attach is already
         * Connected and must not log a bogus "lost race". */
        if (state_.load() == ChatConnState::Attached)
            transitionState(ChatConnState::Attached, ChatConnState::Connected);
        chat_copy_str(attach_id_, sizeof(attach_id_), chat_id);
        ws = websocket_;
        root = cJSON_CreateObject();
        if (!root || !cJSON_AddStringToObject(root, "type", "attach") ||
            !cJSON_AddStringToObject(root, "chat_id", chat_id)) {
            cJSON_Delete(root);
            return -1;
        }
        payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!payload) return -1;
        /* Hold cs_ across send so disconnect cannot close the handle mid-call. */
        err = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            payload, (DWORD)strlen(payload));
        free(payload);
    }
    nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "attach: chat_id=%.32s -> %s",
           chat_id, err == ERROR_SUCCESS ? "sent" : "failed");
    return err == ERROR_SUCCESS ? 0 : -1;
}

int ChatClient::send(const char* utf8_text) {
    if (!utf8_text || !utf8_text[0]) return -1;
    if (!inited_) return -1;

    char chat_id[128];
    HINTERNET ws;
    DWORD err;

    std::lock_guard<std::mutex> lock(cs_);
    /* Disconnect clears the state before it cancels the socket under this
     * same lock. */
    if (!isConnected() || stop_.load())
        return -1;
    chat_copy_str(chat_id, sizeof(chat_id), chat_id_);
    ws = websocket_;
    if (!ws)
        return -1;

    cJSON* root = cJSON_CreateObject();
    if (!root)
        return -1;

    if (state_.load() != ChatConnState::Attached ||
        !chat_history_is_valid_chat_id(chat_id) ||
        !cJSON_AddStringToObject(root, "type", "message") ||
        !cJSON_AddStringToObject(root, "chat_id", chat_id) ||
        !cJSON_AddStringToObject(root, "content", utf8_text)) {
        cJSON_Delete(root);
        return -1;
    }

    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
        return -1;

    /* Hold cs_ across send so disconnect cannot close the handle mid-call. */
    err = WinHttpWebSocketSend(ws,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)payload, (DWORD)strlen(payload));
    free(payload);

    nm_log(NM_LOG_DEBUG, CHAT_LOG_MODULE, "send: %zu bytes -> %s",
           strlen(utf8_text), err == ERROR_SUCCESS ? "ok" : "failed");
    return (err == ERROR_SUCCESS) ? 0 : -1;
}

void ChatClient::shutdown() {
    if (!inited_) return;
    {
        std::lock_guard<std::mutex> lock(cs_);
        hwnd_ = NULL;
        msg_id_ = 0;
        shutting_down_ = true;
        lifecycle_request_ = LIFECYCLE_SHUTDOWN;
    }
    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "shutdown: requesting lifecycle stop");

    if (lifecycle_running_) {
        lifecycleCv_.notify_one();
        if (lifecycle_thread_.joinable())
            lifecycle_thread_.join();
    } else {
        std::lock_guard<std::mutex> disconnect_lock(disconnectMutex_);
        disconnectInternal(1);
    }

    {
        std::lock_guard<std::mutex> lock(cs_);
        lifecycle_running_ = false;
        SecureZeroMemory(bootstrap_secret_, sizeof(bootstrap_secret_));
    }
    inited_ = false;
    nm_log(NM_LOG_INFO, CHAT_LOG_MODULE, "shutdown: complete");
}

ChatClient::~ChatClient() {
    /* shutdown() (WM_DESTROY) owns teardown; join defensively so a missed
     * call can never terminate the process via a joinable std::thread. If
     * the lifecycle worker is still parked on lifecycleCv_.wait, it must be
     * nudged first or the join below would hang forever. requestLifecycle()
     * cannot be used here (it no-ops once !inited_ or shutting_down_), so
     * write the shutdown request directly under cs_. */
    if (lifecycle_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(cs_);
            if (lifecycle_running_ && !shutting_down_) {
                lifecycle_request_ = LIFECYCLE_SHUTDOWN;
                lifecycleCv_.notify_one();
            }
        }
        lifecycle_thread_.join();
    }
    if (recv_thread_.joinable())
        recv_thread_.join();
    if (connect_thread_.joinable())
        connect_thread_.join();
}
