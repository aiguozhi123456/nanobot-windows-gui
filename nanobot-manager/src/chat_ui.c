#include "chat_ui.h"
#include "chat.h"
#include "chat_history.h"
#include "chat_protocol.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UiPage g_page = 0;
static int (*g_is_zh)(void) = NULL;
static int (*g_health_fn)(void) = NULL;
static int g_webui_port = 8765;
static cJSON* g_messages = NULL;
static chat_history_document g_history_document;
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
static cJSON* g_fresh_messages = NULL;
static int g_fresh_pending = 0;
static int g_attach_pending = 0;
static ULONGLONG g_attach_deadline = 0;
static int g_auto_connect_blocked = 0;
static UiWidget g_chat_scroll = 0;
static int g_messages_ui_dirty = 0;

#define CHAT_ATTACH_TIMEOUT_MS 10000ULL
#define CHAT_FOLLOW_THRESHOLD_DIP 32.0f

static void restore_new_history_backup(void);
static void terminal_failure(const char* zh, const char* en, const char* detail);

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
    int follow = 0;
    if (!g_page) return;
    if (g_chat_scroll) {
        float y = ui_scroll_get_y(g_chat_scroll);
        float max_y = ui_scroll_get_max_y(g_chat_scroll);
        follow = max_y <= 0.0f || max_y - y <= CHAT_FOLLOW_THRESHOLD_DIP;
    }
    ensure_messages();
    json = cJSON_PrintUnformatted(g_messages);
    if (json) {
        ui_page_set_json(g_page, "messages", json);
        if (follow) ui_scroll_to_end(g_chat_scroll);
        g_messages_ui_dirty = 0;
        free(json);
    }
}

static void flush_messages_ui(void) {
    if (g_messages_ui_dirty) push_messages_ui();
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
    if (!g_history_path[0] || g_history_read_error ||
        g_turn_state == CHAT_TURN_NEW_WAIT_ACK || g_saved_revision == g_revision) return;
    g_history_document.messages = g_messages;
    if (g_history_document.version == 2 && chat_history_save(g_history_path, &g_history_document, &error) == 0)
        g_saved_revision = g_revision;
    else
        save_failure_status();
}

static int persist_binding(int resume_safe) {
    DWORD error;
    g_history_document.messages = g_messages;
    g_history_document.resume_safe = resume_safe ? 1 : 0;
    if (!g_history_path[0] || g_history_read_error || g_history_document.version != 2 ||
        chat_history_save(g_history_path, &g_history_document, &error) != 0) {
        save_failure_status();
        return 0;
    }
    g_saved_revision = g_revision;
    return 1;
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

/* g_messages is the sole owner. The document only borrows it while saving. */
static int replace_history_messages(cJSON* messages) {
    if (!messages) return 0;
    g_history_document.messages = NULL;
    cJSON_Delete(g_messages);
    g_messages = messages;
    g_history_document.messages = g_messages;
    assign_ui_ids();
    g_revision = 0;
    g_saved_revision = 0;
    push_messages_ui();
    return 1;
}

static void discard_fresh_session(void) {
    cJSON_Delete(g_fresh_messages);
    g_fresh_messages = NULL;
    g_fresh_pending = 0;
}

static int prepare_fresh_session(void) {
    DWORD error = ERROR_SUCCESS;
    cJSON* fresh = cJSON_CreateArray();
    if (!fresh) {
        chat_ui_status_i18n("无法创建新聊天记录，未连接", "Could not create new chat history; not connecting");
        return 0;
    }
    if (g_history_document.version != 0 && g_history_path[0]) {
        wchar_t archive_path[MAX_PATH + 64];
        if (chat_history_copy_to_archive(g_history_path, archive_path, _countof(archive_path), &error) != 0) {
            cJSON_Delete(fresh);
            chat_ui_status_i18n("无法备份旧聊天记录，未连接", "Could not archive old chat history; not connecting");
            return 0;
        }
    }
    discard_fresh_session();
    g_fresh_messages = fresh;
    g_fresh_pending = 1;
    return 1;
}

static void install_loaded_history(chat_history_document* loaded) {
    g_history_document.messages = NULL;
    chat_history_document_free(&g_history_document);
    g_history_document = *loaded;
    chat_history_document_init(loaded);
    replace_history_messages(g_history_document.messages);
}

static void invalid_history_backed_up_status(void) {
    chat_ui_status_i18n("无效历史已备份", "Invalid chat history was backed up");
}

static int recover_history_before_send(void) {
    chat_history_document loaded;
    DWORD error = ERROR_SUCCESS;
    chat_history_load_result result;
    if (!g_history_read_error) return 1;
    chat_history_document_init(&loaded);
    result = chat_history_load(g_history_path, &loaded, &error);
    if (result == CHAT_HISTORY_LOAD_OK) {
        install_loaded_history(&loaded);
        g_history_read_error = 0;
        return 1;
    }
    if (result == CHAT_HISTORY_LOAD_ABSENT) {
        g_history_read_error = 0;
        return 1;
    }
    if (result == CHAT_HISTORY_LOAD_INVALID && chat_history_quarantine_invalid(g_history_path, &error) == 0) {
        cJSON* fresh = cJSON_CreateArray();
        if (!fresh) {
            chat_ui_status_i18n("无法创建新聊天记录，未发送消息", "Could not create new chat history; message was not sent");
            return 0;
        }
        replace_history_messages(fresh);
        g_history_read_error = 0;
        invalid_history_backed_up_status();
        return 1;
    }
    chat_history_document_free(&loaded);
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
    g_messages_ui_dirty = 1;
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

static int can_send_now(void) {
    return g_turn_state == CHAT_TURN_IDLE && chat_is_ready() &&
        (!g_health_fn || g_health_fn());
}

static int can_retry_now(void) {
    return chat_protocol_retry_allowed(g_auto_connect_blocked,
        g_turn_state == CHAT_TURN_IDLE, !g_health_fn || g_health_fn(),
        chat_is_connected(), !chat_needs_connect(), g_attach_pending);
}

static void attach_after_ready(const char* offered_id) {
    const char* wanted = offered_id;
    if (!chat_history_is_valid_chat_id(offered_id)) {
        terminal_failure("服务器会话无效", "Server offered an invalid chat session", NULL);
        return;
    }
    if (chat_history_document_can_resume(&g_history_document, g_webui_port))
        wanted = g_history_document.chat_id;
    else if (!prepare_fresh_session()) {
        terminal_failure("无法准备新的聊天记录", "Could not prepare new chat history", NULL);
        return;
    }
    if (chat_attach(wanted) != 0) {
        terminal_failure("无法附加聊天会话", "Could not attach chat session", NULL);
        return;
    }
    g_attach_pending = 1;
    g_attach_deadline = GetTickCount64() + CHAT_ATTACH_TIMEOUT_MS;
    set_can_send(0);
    chat_ui_status_i18n("正在恢复会话…", "Restoring session…");
}

static void terminal_failure(const char* zh, const char* en, const char* detail) {
    int had_fresh = g_fresh_pending;
    flush_messages_ui();
    g_attach_pending = 0;
    g_attach_deadline = 0;
    g_chat_streaming = 0;
    restore_new_history_backup();
    discard_fresh_session();
    g_turn_state = CHAT_TURN_IDLE;
    /* Only an attempted message makes the binding unsafe. Attachment and
       connection failures preserve a previously safe resume point. */
    if (chat_protocol_should_persist_unsafe(had_fresh, g_history_document.resume_safe))
        persist_binding(0);
    g_auto_connect_blocked = 1;
    if (detail && detail[0]) chat_ui_status(detail);
    else chat_ui_status_i18n(zh, en);
    set_can_send(0);
    chat_disconnect_async();
}

static void discard_new_history_backup(void) {
    cJSON_Delete(g_new_history_backup);
    g_new_history_backup = NULL;
}

static void restore_new_history_backup(void) {
    if (g_turn_state != CHAT_TURN_NEW_WAIT_ACK) return;
    g_turn_state = CHAT_TURN_IDLE;
    if (!g_new_history_backup) return;
    g_history_document.messages = NULL;
    cJSON_Delete(g_messages);
    g_messages = g_new_history_backup;
    g_new_history_backup = NULL;
    g_history_document.messages = g_messages;
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
    int next_port = (webui_port > 0 && webui_port <= 65535) ? webui_port : 8765;
    g_health_fn = health_fn;
    if (g_webui_port != next_port && g_history_document.version == 2)
        set_can_send(0);
    g_webui_port = next_port;
}

void chat_ui_status(const char* utf8) { page_set_utf8("chatStatus", utf8 ? utf8 : ""); }

void chat_ui_status_i18n(const char* zh, const char* en) {
    chat_ui_status(g_is_zh && g_is_zh() ? zh : en);
}

void chat_ui_configure_history(const wchar_t* config_path) {
    chat_history_document loaded;
    DWORD error = ERROR_SUCCESS;
    chat_history_load_result result;
    g_history_path[0] = L'\0';
    g_history_read_error = 0;
    if (chat_history_path_from_config(config_path, g_history_path, _countof(g_history_path)) != 0) return;
    chat_history_document_init(&loaded);
    result = chat_history_load(g_history_path, &loaded, &error);
    if (result == CHAT_HISTORY_LOAD_OK) {
        install_loaded_history(&loaded);
    } else if (result == CHAT_HISTORY_LOAD_INVALID) {
        if (chat_history_quarantine_invalid(g_history_path, &error) == 0) {
            cJSON* fresh = cJSON_CreateArray();
            if (fresh) replace_history_messages(fresh);
            else {
                g_history_read_error = 1;
                chat_ui_status_i18n("无法创建新聊天记录，原文件未被覆盖", "Could not create new chat history; existing file was not replaced");
                return;
            }
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
    g_history_document.messages = NULL;
    cJSON_Delete(g_messages);
    g_messages = cJSON_CreateArray();
    g_msg_next_id = 1;
    g_chat_streaming = 0;
    g_turn_state = CHAT_TURN_IDLE;
    g_revision = 0;
    g_saved_revision = 0;
    if (g_page) {
        push_messages_ui();
        ui_page_set_text(g_page, "draft", L"");
        set_can_send(0);
    }
    chat_ui_status_i18n("启动 nanobot 后即可聊天", "Start nanobot to chat");
}

void chat_ui_shutdown(void) {
    flush_messages_ui();
    restore_new_history_backup();
    save_if_dirty();
    discard_new_history_backup();
    discard_fresh_session();
    g_history_document.messages = NULL;
    cJSON_Delete(g_messages);
    chat_history_document_free(&g_history_document);
    g_messages = NULL;
    g_page = 0;
    g_chat_scroll = 0;
    g_messages_ui_dirty = 0;
    g_is_zh = NULL;
    g_health_fn = NULL;
    g_history_path[0] = L'\0';
    g_history_read_error = 0;
    g_turn_state = CHAT_TURN_IDLE;
}

void chat_ui_on_gateway_stopped(void) {
    flush_messages_ui();
    g_auto_connect_blocked = 0;
    g_attach_pending = 0;
    g_attach_deadline = 0;
    g_chat_streaming = 0;
    restore_new_history_backup();
    g_turn_state = CHAT_TURN_IDLE;
    save_if_dirty();
    chat_ui_status_i18n("启动 nanobot 后即可聊天", "Start nanobot to chat");
    set_can_send(0);
}

void chat_ui_tick(ULONGLONG now) {
    if (g_attach_pending && now >= g_attach_deadline)
        terminal_failure("恢复会话超时", "Session restore timed out", NULL);
}

void chat_ui_publish_tick(void) {
    flush_messages_ui();
}

int chat_ui_can_auto_connect(void) { return !g_auto_connect_blocked; }

void chat_ui_on_event(WPARAM kind, chat_event* event) {
    char* detail;
    if (!event) return;
    if (event->generation != chat_current_generation()) {
        chat_event_free(event);
        return;
    }
    detail = event->detail;
    switch ((int)kind) {
    case CHAT_EVT_CONNECTED:
        /* CONNECTED can be queued behind ATTACHED; never regress a ready UI. */
        if (!chat_is_ready()) {
            chat_ui_status_i18n("已连接…", "Connected…");
            set_can_send(0);
        }
        break;
    case CHAT_EVT_READY:
        attach_after_ready(detail);
        break;
    case CHAT_EVT_ATTACHED:
        if (g_attach_pending && detail && chat_history_is_valid_chat_id(detail)) {
            int fresh_committed = 0;
            g_attach_pending = 0;
            g_attach_deadline = 0;
            if (g_fresh_pending) {
                chat_history_document fresh_document;
                cJSON* previous_messages = g_messages;
                chat_history_document_init(&fresh_document);
                fresh_document.version = 2;
                fresh_document.webui_port = g_webui_port;
                strcpy_s(fresh_document.chat_id, sizeof(fresh_document.chat_id), detail);
                fresh_document.resume_safe = 1;
                fresh_document.messages = g_fresh_messages;
                if (!g_history_path[0] || g_history_read_error ||
                    chat_history_save(g_history_path, &fresh_document, NULL) != 0) {
                    terminal_failure("聊天记录保存失败", "Could not save chat history", NULL);
                    break;
                }
                g_history_document.messages = NULL;
                chat_history_document_free(&g_history_document);
                g_history_document = fresh_document;
                g_fresh_messages = NULL;
                g_fresh_pending = 0;
                g_messages = g_history_document.messages;
                cJSON_Delete(previous_messages);
                assign_ui_ids();
                g_revision = g_saved_revision = 0;
                push_messages_ui();
                fresh_committed = 1;
            }
            g_history_document.version = 2;
            g_history_document.webui_port = g_webui_port;
            strcpy_s(g_history_document.chat_id, sizeof(g_history_document.chat_id), detail);
            g_history_document.resume_safe = g_turn_state == CHAT_TURN_IDLE;
            if (!fresh_committed && !persist_binding(g_history_document.resume_safe)) {
                terminal_failure("聊天记录保存失败", "Could not save chat history", NULL);
                break;
            }
            chat_ui_status_i18n("已连接", "Connected");
            set_can_send(can_send_now());
        }
        break;
    case CHAT_EVT_DISCONNECTED:
    case CHAT_EVT_DISCONNECT_DONE:
        flush_messages_ui();
        g_attach_pending = 0;
        g_attach_deadline = 0;
        if (g_turn_state != CHAT_TURN_IDLE) persist_binding(0);
        g_chat_streaming = 0;
        restore_new_history_backup();
        g_turn_state = CHAT_TURN_IDLE;
        save_if_dirty();
        if (!g_auto_connect_blocked)
            chat_ui_status_i18n("未连接", "Disconnected");
        set_can_send(can_retry_now());
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
                persist_binding(1);
                set_can_send(can_send_now());
            }
        } else if (g_turn_state == CHAT_TURN_NORMAL_WAIT_ACTIVITY) {
            if (detail && !set_last_assistant(detail, 0))
                chat_ui_status_i18n("回复过长，已保留可保存的内容", "Reply is too long; the saved portion was kept");
            g_chat_streaming = 0;
            g_turn_state = CHAT_TURN_IDLE;
            persist_binding(1);
            set_can_send(can_send_now());
        } else if (g_turn_state == CHAT_TURN_NORMAL_ACTIVE) {
            if (detail && !set_last_assistant(detail, 0))
                chat_ui_status_i18n("回复过长，已保留可保存的内容", "Reply is too long; the saved portion was kept");
            save_if_dirty();
        }
        flush_messages_ui();
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
        flush_messages_ui();
        break;
    case CHAT_EVT_TURN_END:
        flush_messages_ui();
        if (g_turn_state != CHAT_TURN_NORMAL_ACTIVE)
            break;
        g_chat_streaming = 0;
        g_turn_state = CHAT_TURN_IDLE;
        persist_binding(1);
        set_can_send(can_send_now());
        break;
    case CHAT_EVT_ERROR:
        terminal_failure("连接失败", "Connection failed", detail);
        break;
    default:
        break;
    }
    chat_event_free(event);
}

void chat_ui_on_send(int health_running) {
    char* raw;
    cJSON* json;
    const char* text;
    size_t text_length;
    int is_new;
    int prior_resume_safe;
    cJSON* backup = NULL;
    cJSON* empty_history = NULL;
    if (!g_page || g_turn_state != CHAT_TURN_IDLE) return;
    if (can_retry_now()) {
        g_auto_connect_blocked = 0;
        set_can_send(0);
        chat_ui_status_i18n("正在连接…", "Connecting…");
        chat_connect();
        return;
    }
    raw = ui_page_get_json(g_page, "draft");
    if (!raw) return;
    json = cJSON_Parse(raw);
    ui_page_free(raw);
    text = json && cJSON_IsString(json) ? json->valuestring : NULL;
    if (!text) { cJSON_Delete(json); return; }
    text_length = strlen(text);
    if (text_length > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES) {
        chat_ui_status_i18n("消息过长，无法发送", "Message is too long to send");
        cJSON_Delete(json);
        return;
    }
    if (chat_history_is_blank_message(text)) { cJSON_Delete(json); return; }
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
        g_auto_connect_blocked = 0;
        chat_connect();
        cJSON_Delete(json);
        return;
    }
    is_new = chat_history_is_new_command(text);
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
    /* Allocate /new compensation before making the on-disk binding unsafe. */
    prior_resume_safe = g_history_document.resume_safe;
    if (!persist_binding(0)) {
        cJSON_Delete(backup);
        cJSON_Delete(empty_history);
        g_history_document.resume_safe = prior_resume_safe;
        chat_ui_status_i18n("聊天记录保存失败，未发送消息", "Could not save chat history; message was not sent");
        cJSON_Delete(json);
        return;
    }
    if (chat_send(is_new ? "/new" : text) != 0) {
        cJSON_Delete(backup);
        cJSON_Delete(empty_history);
        persist_binding(prior_resume_safe);
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
        g_history_document.messages = NULL;
        cJSON_Delete(g_messages);
        g_messages = empty_history;
        g_history_document.messages = g_messages;
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

static void on_send(UiWidget widget, void* userdata) {
    (void)widget;
    (void)userdata;
    if (can_send_now() || can_retry_now()) chat_ui_on_send(g_health_fn ? g_health_fn() : 0);
}

void chat_ui_mount_send(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    ui_widget_on_click(widget, on_send, NULL);
}

void chat_ui_mount_input(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    ui_text_input_on_submit(widget, on_send, NULL);
}

void chat_ui_mount_scroll(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    g_chat_scroll = widget;
    ui_scroll_to_end(widget);
}

void chat_ui_unmount_scroll(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    if (g_chat_scroll == widget) g_chat_scroll = 0;
}
