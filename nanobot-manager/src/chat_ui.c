#include "chat_ui.h"
#include "chat.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UiPage g_page = 0;
static int (*g_is_zh)(void) = NULL;
static int (*g_health_fn)(void) = NULL;
static int g_webui_port = 8765;

static cJSON* g_messages = NULL;
static int g_msg_next_id = 1;
static int g_chat_streaming = 0;

static void page_set_utf8(const char* name, const char* utf8) {
    if (!g_page || !name) return;
    wchar_t w[1024];
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, NULL, 0);
    if (len <= 0) return;
    if (len > 1024) len = 1024;
    MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, w, len);
    w[len - 1] = L'\0';
    ui_page_set_text(g_page, name, w);
}

static void ensure_messages(void) {
    if (!g_messages) g_messages = cJSON_CreateArray();
}

static void push_messages_ui(void) {
    if (!g_page) return;
    ensure_messages();
    char* json = cJSON_PrintUnformatted(g_messages);
    if (json) {
        ui_page_set_json(g_page, "messages", json);
        free(json);
    }
}

void chat_ui_bind(UiPage page, int (*is_zh_fn)(void)) {
    g_page = page;
    g_is_zh = is_zh_fn;
}

void chat_ui_set_gateway(int (*health_fn)(void), int webui_port) {
    g_health_fn = health_fn;
    g_webui_port = (webui_port > 0 && webui_port <= 65535) ? webui_port : 8765;
}

void chat_ui_status(const char* utf8) {
    page_set_utf8("chatStatus", utf8 ? utf8 : "");
}

void chat_ui_status_i18n(const char* zh, const char* en) {
    int zh_on = g_is_zh ? g_is_zh() : 1;
    chat_ui_status(zh_on ? zh : en);
}

static void append_msg(const char* role, const char* text) {
    ensure_messages();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", g_msg_next_id++);
    cJSON_AddStringToObject(o, "role", role ? role : "assistant");
    cJSON_AddStringToObject(o, "text", text ? text : "");
    cJSON_AddItemToArray(g_messages, o);
    push_messages_ui();
}

static void begin_assistant(void) {
    append_msg("assistant", "");
    g_chat_streaming = 1;
}

static void append_delta(const char* chunk) {
    if (!chunk || !chunk[0]) return;
    ensure_messages();
    int n = cJSON_GetArraySize(g_messages);
    if (!g_chat_streaming || n <= 0) {
        begin_assistant();
        n = cJSON_GetArraySize(g_messages);
    }
    cJSON* last = cJSON_GetArrayItem(g_messages, n - 1);
    if (!last) return;
    cJSON* t = cJSON_GetObjectItem(last, "text");
    const char* old = (t && cJSON_IsString(t)) ? t->valuestring : "";
    size_t len = strlen(old) + strlen(chunk) + 1;
    char* merged = (char*)malloc(len);
    if (!merged) return;
    snprintf(merged, len, "%s%s", old, chunk);
    cJSON_ReplaceItemInObject(last, "text", cJSON_CreateString(merged));
    free(merged);
    push_messages_ui();
}

static void set_last_assistant(const char* full) {
    ensure_messages();
    int n = cJSON_GetArraySize(g_messages);
    if (g_chat_streaming && n > 0) {
        cJSON* last = cJSON_GetArrayItem(g_messages, n - 1);
        if (last)
            cJSON_ReplaceItemInObject(last, "text", cJSON_CreateString(full ? full : ""));
        push_messages_ui();
    } else {
        append_msg("assistant", full ? full : "");
    }
    g_chat_streaming = 0;
}

void chat_ui_reset_idle(void) {
    if (g_messages) {
        cJSON_Delete(g_messages);
        g_messages = NULL;
    }
    g_messages = cJSON_CreateArray();
    g_msg_next_id = 1;
    g_chat_streaming = 0;
    if (g_page) {
        ui_page_set_json(g_page, "messages", "[]");
        ui_page_set_text(g_page, "draft", L"");
        ui_page_set_bool(g_page, "canSend", 1);
    }
    chat_ui_status_i18n(
        "\xe5\x90\xaf\xe5\x8a\xa8 nanobot \xe5\x90\x8e\xe5\x8d\xb3\xe5\x8f\xaf\xe8\x81\x8a\xe5\xa4\xa9",
        "Start nanobot to chat");
}

void chat_ui_shutdown(void) {
    if (g_messages) {
        cJSON_Delete(g_messages);
        g_messages = NULL;
    }
    g_page = 0;
    g_is_zh = NULL;
    g_health_fn = NULL;
}

void chat_ui_on_gateway_stopped(void) {
    g_chat_streaming = 0;
    chat_ui_status_i18n(
        "\xe5\x90\xaf\xe5\x8a\xa8 nanobot \xe5\x90\x8e\xe5\x8d\xb3\xe5\x8f\xaf\xe8\x81\x8a\xe5\xa4\xa9",
        "Start nanobot to chat");
    if (g_page)
        ui_page_set_bool(g_page, "canSend", 1);
}

void chat_ui_on_event(WPARAM kind, char* detail) {
    switch ((int)kind) {
    case CHAT_EVT_CONNECTED:
        chat_ui_status_i18n(
            "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5\xe2\x80\xa6",
            "Connected\xe2\x80\xa6");
        /* Prefer waiting for READY; still allow send if chat_is_ready (legacy). */
        if (g_page)
            ui_page_set_bool(g_page, "canSend", chat_is_ready() ? 1 : 0);
        break;
    case CHAT_EVT_READY:
        chat_ui_status_i18n("\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5", "Connected");
        if (g_page)
            ui_page_set_bool(g_page, "canSend", 1);
        break;
    case CHAT_EVT_DISCONNECTED:
    case CHAT_EVT_DISCONNECT_DONE:
        g_chat_streaming = 0;
        chat_ui_status_i18n("\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5", "Disconnected");
        if (g_page)
            ui_page_set_bool(g_page, "canSend", 1);
        break;
    case CHAT_EVT_DELTA:
        if (detail) append_delta(detail);
        break;
    case CHAT_EVT_MESSAGE:
        if (detail) set_last_assistant(detail);
        else g_chat_streaming = 0;
        break;
    case CHAT_EVT_STREAM_END:
        g_chat_streaming = 0;
        break;
    case CHAT_EVT_ERROR:
        g_chat_streaming = 0;
        if (detail && detail[0])
            chat_ui_status(detail);
        else
            chat_ui_status_i18n(
                "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5",
                "Connection failed");
        if (g_page)
            ui_page_set_bool(g_page, "canSend", 1);
        break;
    default:
        break;
    }
    free(detail);
}

void chat_ui_on_send(int health_running) {
    if (!g_page) return;

    char* raw = ui_page_get_json(g_page, "draft");
    if (!raw) return;
    cJSON* j = cJSON_Parse(raw);
    ui_page_free(raw);
    const char* text = NULL;
    if (j && cJSON_IsString(j))
        text = j->valuestring;
    if (!text || !text[0]) {
        cJSON_Delete(j);
        return;
    }

    if (!health_running) {
        chat_ui_status_i18n(
            "\xe8\xaf\xb7\xe5\x85\x88\xe5\x90\xaf\xe5\x8a\xa8 nanobot",
            "Start nanobot first");
        cJSON_Delete(j);
        return;
    }

    if (!chat_is_ready()) {
        chat_ui_status_i18n(
            "\xe8\xbf\x9e\xe6\x8e\xa5\xe4\xb8\xad\xe2\x80\xa6\xe8\xbf\x9e\xe6\x8e\xa5\xe5\x90\x8e\xe8\xaf\xb7\xe5\x86\x8d\xe6\xac\xa1\xe5\x8f\x91\xe9\x80\x81",
            "Connecting\xe2\x80\xa6 send again when connected");
        chat_connect();
        cJSON_Delete(j);
        return;
    }

    append_msg("user", text);
    ui_page_set_text(g_page, "draft", L"");
    g_chat_streaming = 0;
    if (chat_send(text) != 0) {
        chat_ui_status_i18n(
            "\xe5\x8f\x91\xe9\x80\x81\xe5\xa4\xb1\xe8\xb4\xa5",
            "Send failed");
    }
    cJSON_Delete(j);
}

static void on_send_click(UiWidget w, void* ud) {
    (void)w;
    (void)ud;
    int running = g_health_fn ? g_health_fn() : 0;
    chat_ui_on_send(running);
}

void chat_ui_mount_send(UiPage p, UiWidget w, void* ud) {
    (void)p;
    (void)ud;
    ui_widget_on_click(w, on_send_click, NULL);
}
