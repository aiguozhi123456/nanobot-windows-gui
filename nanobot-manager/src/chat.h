#ifndef CHAT_H
#define CHAT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds for HWND message WPARAM */
enum {
    CHAT_EVT_CONNECTED = 1,   /* socket up; chat_id may still be empty */
    CHAT_EVT_DISCONNECTED = 2,
    CHAT_EVT_DELTA = 3,       /* LPARAM: malloc'd UTF-8 text chunk (caller frees) */
    CHAT_EVT_MESSAGE = 4,     /* LPARAM: malloc'd full assistant text (caller frees) */
    CHAT_EVT_STREAM_END = 5,
    CHAT_EVT_ERROR = 6,       /* LPARAM: malloc'd error detail (caller frees) */
    CHAT_EVT_READY = 7,       /* LPARAM: offered default chat_id; not attached */
    CHAT_EVT_DISCONNECT_DONE = 8, /* async disconnect finished */
    CHAT_EVT_TURN_END = 9,
    CHAT_EVT_ATTACHED = 10    /* LPARAM: matching attached chat_id */
};

typedef struct {
    UINT64 generation;
    char* detail;
} chat_event;

void chat_event_free(chat_event* event);
UINT64 chat_current_generation(void);

/* Initialize module (call once). hwnd receives WM_CHAT_EVENT. */
void chat_init(HWND hwnd, UINT msg_id);

/* Set WebUI/WS host port and an optional short-lived bootstrap secret. */
void chat_set_endpoint(int webui_port, const char* bootstrap_secret);

/* Connect async. Idempotent if already connected/connecting. */
int chat_connect(void);

/* Disconnect and join worker threads. */
void chat_disconnect(void);

/* Disconnect on a worker thread; posts CHAT_EVT_DISCONNECT_DONE when finished. */
void chat_disconnect_async(void);

/* Queue a disconnect followed by a connection using the latest endpoint. */
void chat_reconnect_async(void);

/* 1 if WebSocket is up (may not have chat_id yet). */
int chat_is_connected(void);

/* 1 when no socket is connected or connecting; attachment is not considered. */
int chat_needs_connect(void);

/* Select a server-offered or persisted chat id. Sends only after ATTACHED. */
int chat_attach(const char* chat_id);

/* 1 if a matching chat id has been attached and can receive messages. */
int chat_is_ready(void);

/* Send user text (UTF-8). Returns 0 on success, -1 if not ready/empty. */
int chat_send(const char* utf8_text);

/* Shutdown: null UI target, disconnect, free CS. */
void chat_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
