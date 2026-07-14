#include "chat_ui.h"
#include "chat.h"
#include "chat_history.h"
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
typedef enum {
    CHAT_TURN_IDLE,
    CHAT_TURN_NEW_WAIT_ACK,
    CHAT_TURN_NORMAL_WAIT_ACTIVITY,
    CHAT_TURN_NORMAL_ACTIVE
} chat_turn_state;
static chat_turn_state g_turn_state = CHAT_TURN_IDLE;
static unsigned long g_revision = 0;
static unsigned long g_saved_revision = 0;
static wchar_t g_history_path[MAX_PATH] = {0};
static int g_history_read_error = 0;
static cJSON* g_new_history_backup = NULL;

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
    char* json;
    if (!g_page) return;
    ensure_messages();
    json = cJSON_PrintUnformatted(g_messages);
    if (json) {
        ui_page_set_json(g_page, "messages", json);
        free(json);
    }
}

static void mark_dirty(void) {
    g_revision++;
}

static void save_failure_status(void) {
    chat_ui_status_i18n(
        "聊天记录保存失败，将在之后重试",
        "Could not save chat history; it will be retried");
}

static void save_if_dirty(void) {
    DWORD error;
    if (!g_history_path[0] || g_history_read_error || g_saved_revision == g_revision) return;
    if (chat_history_save(g_history_path, g_messages, &error) == 0)
        g_saved_revision = g_revision;
    else
        save_failure_status();
}

static void assign_ui_ids(void) {
    int count;
    g_msg_next_id = 1;
    ensure_messages();
    count = cJSON_GetArraySize(g_messages);
    for (int index = 0; index < count; index++) {
        cJSON* message = cJSON_GetArrayItem(g_messages, index);
        cJSON* id;
        if (!cJSON_IsObject(message)) continue;
        id = cJSON_CreateNumber(g_msg_next_id);
        if (!id) break;
        while (cJSON_GetObjectItemCaseSensitive(message, "id"))
            cJSON_DeleteItemFromObjectCaseSensitive(message, "id");
        if (!cJSON_AddItemToObject(message, "id", id)) {
            cJSON_Delete(id);
            break;
        }
        g_msg_next_id++;
    }
}

static void replace_history_messages(cJSON* messages) {
    cJSON_Delete(g_messages);
    g_messages = messages;
    assign_ui_ids();
    g_revision = 0;
    g_saved_revision = 0;
    push_messages_ui();
}

static void invalid_history_backed_up_status(void) {
    chat_ui_status_i18n("无效历史已备份", "Invalid chat history was backed up");
}

static int recover_history_before_send(void) {
    cJSON* loaded = NULL;
    DWORD error = ERROR_SUCCESS;
    chat_history_load_result result;
    if (!g_history_read_error) return 1;
    result = chat_history_load(g_history_path, &loaded, &error);
    if (result == CHAT_HISTORY_LOAD_OK) {
        replace_history_messages(loaded);
        g_history_read_error = 0;
        return 1;
    }
    if (result == CHAT_HISTORY_LOAD_ABSENT) {
        g_history_read_error = 0;
        return 1;
    }
    if (result == CHAT_HISTORY_LOAD_INVALID && chat_history_quarantine_invalid(g_history_path, &error) == 0) {
        replace_history_messages(cJSON_CreateArray());
        g_history_read_error = 0;
        invalid_history_backed_up_status();
        return 1;
    }
    cJSON_Delete(loaded);
    chat_ui_status_i18n("聊天记录暂时不可用，未发送消息", "Chat history is temporarily unavailable; message was not sent");
    return 0;
}

static int append_msg(const char* role, const char* text) {
    cJSON* message;
    ensure_messages();
    message = cJSON_CreateObject();
    if (!message) return 0;
    if (!cJSON_AddNumberToObject(message, "id", g_msg_next_id++) ||
        !cJSON_AddStringToObject(message, "role", role ? role : "assistant") ||
        !cJSON_AddStringToObject(message, "text", text ? text : "")) {
        cJSON_Delete(message);
        return 0;
    }
    if (!cJSON_AddItemToArray(g_messages, message)) {
        cJSON_Delete(message);
        return 0;
    }
    mark_dirty();
    push_messages_ui();
    return 1;
}

static int begin_assistant(void) {
    if (!append_msg("assistant", "")) return 0;
    g_chat_streaming = 1;
    return 1;
}

static int append_delta(const char* chunk) {
    cJSON* last;
    cJSON* text;
    const char* old;
    size_t chunk_length;
    size_t old_length;
    size_t length;
    char* merged;
    if (!chunk || !chunk[0]) return 0;
    chunk_length = strlen(chunk);
    if (chunk_length > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES) return 0;
    ensure_messages();
    if (!g_chat_streaming || cJSON_GetArraySize(g_messages) <= 0) {
        if (!begin_assistant()) return 0;
    }
    last = cJSON_GetArrayItem(g_messages, cJSON_GetArraySize(g_messages) - 1);
    if (!last) return 0;
    text = cJSON_GetObjectItemCaseSensitive(last, "text");
    old = cJSON_IsString(text) && text->valuestring ? text->valuestring : "";
    old_length = strlen(old);
    if (old_length > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES - chunk_length) return 0;
    length = old_length + chunk_length + 1;
    merged = (char*)malloc(length);
    if (!merged) return 0;
    snprintf(merged, length, "%s%s", old, chunk);
    text = cJSON_CreateString(merged);
    free(merged);
    if (!text || !cJSON_ReplaceItemInObjectCaseSensitive(last, "text", text)) {
        cJSON_Delete(text);
        return 0;
    }
    mark_dirty();
    push_messages_ui();
    return 1;
}

static int set_last_assistant(const char* full, int replace_existing_assistant) {
    cJSON* last;
    cJSON* role;
    cJSON* text;
    const char* old;
    const char* replacement = full ? full : "";
    if (strlen(replacement) > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES) return 0;
    ensure_messages();
    last = cJSON_GetArrayItem(g_messages, cJSON_GetArraySize(g_messages) - 1);
    role = last ? cJSON_GetObjectItemCaseSensitive(last, "role") : NULL;
    text = last ? cJSON_GetObjectItemCaseSensitive(last, "text") : NULL;
    old = cJSON_IsString(text) && text->valuestring ? text->valuestring : "";
    if (replace_existing_assistant &&
        !(cJSON_IsString(role) && role->valuestring && strcmp(role->valuestring, "assistant") == 0)) {
        if (!append_msg("assistant", replacement)) return 0;
        g_chat_streaming = 0;
        return 1;
    }
    if (!g_chat_streaming && !replace_existing_assistant) {
        if (cJSON_IsString(role) && role->valuestring &&
            strcmp(role->valuestring, "assistant") == 0 && strcmp(old, replacement) == 0)
            return 1;
        if (!append_msg("assistant", replacement)) return 0;
        g_chat_streaming = 0;
        return 1;
    }
    if (strcmp(old, replacement) != 0) {
        cJSON* replacement_item = cJSON_CreateString(replacement);
        if (replacement_item && cJSON_ReplaceItemInObjectCaseSensitive(last, "text", replacement_item)) {
            mark_dirty();
            push_messages_ui();
        } else {
            cJSON_Delete(replacement_item);
            return 0;
        }
    }
    g_chat_streaming = 0;
    return 1;
}

static void set_can_send(int enabled) {
    if (g_page) ui_page_set_bool(g_page, "canSend", enabled ? 1 : 0);
}

static void discard_new_history_backup(void) {
    cJSON_Delete(g_new_history_backup);
    g_new_history_backup = NULL;
}

static void restore_new_history_backup(void) {
    if (g_turn_state != CHAT_TURN_NEW_WAIT_ACK) return;
    g_turn_state = CHAT_TURN_IDLE;
    if (!g_new_history_backup) return;
    cJSON_Delete(g_messages);
    g_messages = g_new_history_backup;
    g_new_history_backup = NULL;
    assign_ui_ids();
    mark_dirty();
    push_messages_ui();
    save_if_dirty();
}

void chat_ui_bind(UiPage page, int (*is_zh_fn)(void)) {
    g_page = page;
    g_is_zh = is_zh_fn;
}

void chat_ui_set_gateway(int (*health_fn)(void), int webui_port) {
    g_health_fn = health_fn;
    g_webui_port = (webui_port > 0 && webui_port <= 65535) ? webui_port : 8765;
}

void chat_ui_status(const char* utf8) { page_set_utf8("chatStatus", utf8 ? utf8 : ""); }

void chat_ui_status_i18n(const char* zh, const char* en) {
    chat_ui_status(g_is_zh && g_is_zh() ? zh : en);
}

void chat_ui_configure_history(const wchar_t* config_path) {
    cJSON* loaded = NULL;
    DWORD error = ERROR_SUCCESS;
    chat_history_load_result result;
    g_history_path[0] = L'\0';
    g_history_read_error = 0;
    if (chat_history_path_from_config(config_path, g_history_path, _countof(g_history_path)) != 0) return;
    result = chat_history_load(g_history_path, &loaded, &error);
    if (result == CHAT_HISTORY_LOAD_OK) {
        replace_history_messages(loaded);
    } else if (result == CHAT_HISTORY_LOAD_INVALID) {
        if (chat_history_quarantine_invalid(g_history_path, &error) == 0) {
            replace_history_messages(cJSON_CreateArray());
            invalid_history_backed_up_status();
        } else {
            g_history_read_error = 1;
            chat_ui_status_i18n("无效聊天记录无法备份，原文件未被覆盖", "Invalid chat history could not be backed up; existing file was not replaced");
        }
    } else if (result == CHAT_HISTORY_LOAD_IO_ERROR) {
        g_history_read_error = 1;
        chat_ui_status_i18n("聊天记录读取失败，现有记录未被覆盖", "Could not read chat history; existing file was not replaced");
    }
}

void chat_ui_reset_idle(void) {
    discard_new_history_backup();
    cJSON_Delete(g_messages);
    g_messages = cJSON_CreateArray();
    g_msg_next_id = 1;
    g_chat_streaming = 0;
    g_turn_state = CHAT_TURN_IDLE;
    g_revision = 0;
    g_saved_revision = 0;
    if (g_page) {
        ui_page_set_json(g_page, "messages", "[]");
        ui_page_set_text(g_page, "draft", L"");
        set_can_send(1);
    }
    chat_ui_status_i18n("启动 nanobot 后即可聊天", "Start nanobot to chat");
}

void chat_ui_shutdown(void) {
    restore_new_history_backup();
    save_if_dirty();
    discard_new_history_backup();
    cJSON_Delete(g_messages);
    g_messages = NULL;
    g_page = 0;
    g_is_zh = NULL;
    g_health_fn = NULL;
    g_history_path[0] = L'\0';
    g_history_read_error = 0;
    g_turn_state = CHAT_TURN_IDLE;
}

void chat_ui_on_gateway_stopped(void) {
    g_chat_streaming = 0;
    restore_new_history_backup();
    g_turn_state = CHAT_TURN_IDLE;
    save_if_dirty();
    chat_ui_status_i18n("启动 nanobot 后即可聊天", "Start nanobot to chat");
    set_can_send(1);
}

void chat_ui_on_event(WPARAM kind, char* detail) {
    switch ((int)kind) {
    case CHAT_EVT_CONNECTED:
        chat_ui_status_i18n("已连接…", "Connected…");
        set_can_send(g_turn_state == CHAT_TURN_IDLE && chat_is_ready());
        break;
    case CHAT_EVT_READY:
        chat_ui_status_i18n("已连接", "Connected");
        set_can_send(g_turn_state == CHAT_TURN_IDLE);
        break;
    case CHAT_EVT_DISCONNECTED:
    case CHAT_EVT_DISCONNECT_DONE:
        g_chat_streaming = 0;
        restore_new_history_backup();
        g_turn_state = CHAT_TURN_IDLE;
        save_if_dirty();
        chat_ui_status_i18n("未连接", "Disconnected");
        set_can_send(1);
        break;
    case CHAT_EVT_DELTA:
        if (g_turn_state == CHAT_TURN_NORMAL_WAIT_ACTIVITY)
            g_turn_state = CHAT_TURN_NORMAL_ACTIVE;
        if (g_turn_state == CHAT_TURN_NORMAL_ACTIVE && detail && !append_delta(detail))
            chat_ui_status_i18n("回复过长，已保留可保存的内容", "Reply is too long; the saved portion was kept");
        break;
    case CHAT_EVT_MESSAGE:
        if (g_turn_state == CHAT_TURN_NEW_WAIT_ACK) {
            if (detail && strcmp(detail, "New session started.") == 0) {
                g_chat_streaming = 0;
                discard_new_history_backup();
                g_turn_state = CHAT_TURN_IDLE;
                save_if_dirty();
                set_can_send(1);
            }
        } else if (g_turn_state == CHAT_TURN_NORMAL_WAIT_ACTIVITY) {
            if (detail && !set_last_assistant(detail, 0))
                chat_ui_status_i18n("回复过长，已保留可保存的内容", "Reply is too long; the saved portion was kept");
            g_chat_streaming = 0;
            g_turn_state = CHAT_TURN_IDLE;
            save_if_dirty();
            set_can_send(1);
        } else if (g_turn_state == CHAT_TURN_NORMAL_ACTIVE) {
            if (detail && !set_last_assistant(detail, 0))
                chat_ui_status_i18n("回复过长，已保留可保存的内容", "Reply is too long; the saved portion was kept");
            save_if_dirty();
        }
        break;
    case CHAT_EVT_STREAM_END:
        if (g_turn_state == CHAT_TURN_NORMAL_WAIT_ACTIVITY)
            g_turn_state = CHAT_TURN_NORMAL_ACTIVE;
        if (g_turn_state == CHAT_TURN_NORMAL_ACTIVE) {
            if (detail && !set_last_assistant(detail, 1))
                chat_ui_status_i18n("回复过长，已保留可保存的内容", "Reply is too long; the saved portion was kept");
            else if (!detail)
                g_chat_streaming = 0;
            save_if_dirty();
        }
        break;
    case CHAT_EVT_TURN_END:
        if (g_turn_state != CHAT_TURN_NORMAL_ACTIVE)
            break;
        g_chat_streaming = 0;
        g_turn_state = CHAT_TURN_IDLE;
        save_if_dirty();
        set_can_send(1);
        break;
    case CHAT_EVT_ERROR:
        g_chat_streaming = 0;
        restore_new_history_backup();
        g_turn_state = CHAT_TURN_IDLE;
        save_if_dirty();
        if (detail && detail[0]) chat_ui_status(detail);
        else chat_ui_status_i18n("连接失败", "Connection failed");
        set_can_send(1);
        break;
    default:
        break;
    }
    free(detail);
}

void chat_ui_on_send(int health_running) {
    char* raw;
    cJSON* json;
    const char* text;
    int is_new;
    cJSON* backup = NULL;
    cJSON* empty_history = NULL;
    if (!g_page || g_turn_state != CHAT_TURN_IDLE) return;
    raw = ui_page_get_json(g_page, "draft");
    if (!raw) return;
    json = cJSON_Parse(raw);
    ui_page_free(raw);
    text = json && cJSON_IsString(json) ? json->valuestring : NULL;
    if (!text || !text[0]) { cJSON_Delete(json); return; }
    if (!recover_history_before_send()) {
        cJSON_Delete(json);
        return;
    }
    if (!health_running) {
        chat_ui_status_i18n("请先启动 nanobot", "Start nanobot first");
        cJSON_Delete(json);
        return;
    }
    if (!chat_is_ready()) {
        chat_ui_status_i18n("连接中…连接后请再次发送", "Connecting… send again when connected");
        chat_connect();
        cJSON_Delete(json);
        return;
    }
    is_new = chat_history_is_new_command(text);
    if (strlen(text) > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES) {
        chat_ui_status_i18n("消息过长，无法发送", "Message is too long to send");
        cJSON_Delete(json);
        return;
    }
    if (is_new) {
        ensure_messages();
        backup = cJSON_Duplicate(g_messages, 1);
        empty_history = cJSON_CreateArray();
        if (!backup || !empty_history) {
            cJSON_Delete(backup);
            cJSON_Delete(empty_history);
            chat_ui_status_i18n("无法备份聊天记录，未发送 /new", "Could not back up chat history; /new was not sent");
            cJSON_Delete(json);
            return;
        }
    }
    if (chat_send(is_new ? "/new" : text) != 0) {
        cJSON_Delete(backup);
        cJSON_Delete(empty_history);
        chat_ui_status_i18n("发送失败", "Send failed");
        cJSON_Delete(json);
        return;
    }
    g_turn_state = is_new ? CHAT_TURN_NEW_WAIT_ACK : CHAT_TURN_NORMAL_WAIT_ACTIVITY;
    g_chat_streaming = 0;
    set_can_send(0);
    if (is_new) {
        discard_new_history_backup();
        g_new_history_backup = backup;
        cJSON_Delete(g_messages);
        g_messages = empty_history;
        g_msg_next_id = 1;
        mark_dirty();
        push_messages_ui();
    } else {
        append_msg("user", text);
    }
    ui_page_set_text(g_page, "draft", L"");
    save_if_dirty();
    cJSON_Delete(json);
}

static void on_send_click(UiWidget widget, void* userdata) {
    (void)widget;
    (void)userdata;
    chat_ui_on_send(g_health_fn ? g_health_fn() : 0);
}

void chat_ui_mount_send(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    ui_widget_on_click(widget, on_send_click, NULL);
}
