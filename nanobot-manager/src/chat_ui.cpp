#include "chat_ui.hpp"

#include "chat_protocol.h"
#include "util.hpp"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAT_ATTACH_TIMEOUT_MS 10000ULL
#define CHAT_TURN_TIMEOUT_MS   60000ULL
#define CHAT_FOLLOW_THRESHOLD_DIP 32.0f
/* UI renders at most the newest N messages so per-push work (serialize +
 * JSON parse + v-for rebind) stays bounded on long histories. The full
 * vector is still what chat_history persists; only the rendered slice is
 * capped. */
#define CHAT_UI_DISPLAY_CAP 200

ChatUi& chat_ui() {
    static ChatUi instance;
    return instance;
}

/* --- model + publish ------------------------------------------------------ */

cJSON* ChatUi::messagesToJson(const std::vector<ChatMsg>& messages) {
    cJSON* array = cJSON_CreateArray();
    if (!array) return NULL;
    for (const ChatMsg& message : messages) {
        cJSON* item = cJSON_CreateObject();
        if (!item ||
            !cJSON_AddNumberToObject(item, "id", message.id) ||
            !cJSON_AddStringToObject(item, "role", message.role.c_str()) ||
            !cJSON_AddStringToObject(item, "text", message.text.c_str())) {
            cJSON_Delete(item);
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddItemToArray(array, item);
    }
    return array;
}

/* Serialize the newest CHAT_UI_DISPLAY_CAP messages; while a stream is
 * active the draft renders as a virtual trailing bubble (it is not part of
 * messages_ until committed). Caller frees the returned string. */
char* ChatUi::serializeVisible() {
    cJSON* array = cJSON_CreateArray();
    if (!array) return NULL;
    size_t count = messages_.size();
    size_t start = count > CHAT_UI_DISPLAY_CAP ? count - CHAT_UI_DISPLAY_CAP : 0;
    for (size_t index = start; index < count; index++) {
        const ChatMsg& message = messages_[index];
        cJSON* item = cJSON_CreateObject();
        if (!item ||
            !cJSON_AddNumberToObject(item, "id", message.id) ||
            !cJSON_AddStringToObject(item, "role", message.role.c_str()) ||
            !cJSON_AddStringToObject(item, "text", message.text.c_str())) {
            cJSON_Delete(item);
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddItemToArray(array, item);
    }
    if (draft_.active) {
        cJSON* item = cJSON_CreateObject();
        if (!item ||
            !cJSON_AddNumberToObject(item, "id", next_msg_id_) ||
            !cJSON_AddStringToObject(item, "role", "assistant") ||
            !cJSON_AddStringToObject(item, "text", draft_.text.c_str())) {
            cJSON_Delete(item);
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddItemToArray(array, item);
    }
    char* json = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return json;
}

void ChatUi::pushMessagesUi() {
    if (!page_) return;
    /* While the chat page is unmounted (v-if switched to another page) the
     * v-for has no live watchers, so the O(history) serialize + JSON parse
     * would feed nobody. Leave the dirty flag set; the scroll mount hook
     * flushes on page re-entry. */
    if (!chat_scroll_) {
        messages_ui_dirty_ = true;
        return;
    }
    int follow = 0;
    {
        float y = ui_scroll_get_y(chat_scroll_);
        float max_y = ui_scroll_get_max_y(chat_scroll_);
        follow = max_y <= 0.0f || max_y - y <= CHAT_FOLLOW_THRESHOLD_DIP;
    }
    char* json = serializeVisible();
    if (json) {
        ui_page_set_json(page_, "messages", json);
        if (follow) ui_scroll_to_end(chat_scroll_);
        messages_ui_dirty_ = false;
        free(json);
    }
}

void ChatUi::flushMessagesUi() {
    if (messages_ui_dirty_) pushMessagesUi();
}

void ChatUi::appendMsg(const char* role, const std::string& text) {
    messages_.push_back({next_msg_id_++, role ? role : "assistant", text});
    revision_++;
    setDirty();
}

int ChatUi::commitCanonical(const char* canonical_or_null) {
    const std::string replacement = canonical_or_null ? canonical_or_null : draft_.text;
    if (replacement.size() > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES) return 0;
    bool was_streaming = draft_.active;
    /* Mid-turn commits (canonical text after deltas, or fixation at
     * TURN_END / disconnect) keep the turn to ONE bubble: the canonical
     * text replaces the streamed text. A commit outside a turn starts a
     * new bubble. */
    bool mid_turn = was_streaming || turn_state_ == ChatTurnState::NormalActive;
    draft_.clear();
    bool last_is_assistant = !messages_.empty() && messages_.back().role == "assistant";
    if (mid_turn && last_is_assistant) {
        if (messages_.back().text != replacement) {
            messages_.back().text = replacement;
            revision_++;
        }
        setDirty();
        return 1;
    }
    if (last_is_assistant && messages_.back().text == replacement) {
        /* Duplicate delivery of an identical non-stream reply. */
        if (was_streaming) setDirty();
        return 1;
    }
    messages_.push_back({next_msg_id_++, "assistant", replacement});
    revision_++;
    setDirty();
    return 1;
}

/* --- history --------------------------------------------------------------- */

void ChatUi::saveFailureStatus() {
    statusI18n("聊天记录保存失败，将在之后重试",
               "Could not save chat history; it will be retried");
}

/* chat_history predicates (can_resume) expect document.messages to be an
 * array; the content lives in the vector, so the idle document carries an
 * empty placeholder that save calls swap out and restore. */
void ChatUi::setHistoryArrayPlaceholder() {
    cJSON_Delete(history_.messages);
    history_.messages = cJSON_CreateArray();
}

int ChatUi::persistBinding(int resume_safe) {
    DWORD error = ERROR_SUCCESS;
    history_.resume_safe = resume_safe ? 1 : 0;
    if (!history_path_[0] || history_read_error_ || history_.version != 2) {
        saveFailureStatus();
        return 0;
    }
    cJSON* array = messagesToJson(messages_);
    history_.messages = array;
    int rc = array ? chat_history_save(history_path_, &history_, &error) : -1;
    cJSON_Delete(array);
    setHistoryArrayPlaceholder();
    if (rc != 0) {
        saveFailureStatus();
        return 0;
    }
    saved_revision_ = revision_;
    return 1;
}

void ChatUi::saveIfDirty() {
    DWORD error = ERROR_SUCCESS;
    if (!history_path_[0] || history_read_error_ ||
        turn_state_ == ChatTurnState::NewWaitAck || saved_revision_ == revision_) return;
    cJSON* array = messagesToJson(messages_);
    history_.messages = array;
    int rc = (history_.version == 2 && array)
        ? chat_history_save(history_path_, &history_, &error) : -1;
    cJSON_Delete(array);
    setHistoryArrayPlaceholder();
    if (rc == 0)
        saved_revision_ = revision_;
    else
        saveFailureStatus();
}

void ChatUi::adoptDocument(chat_history_document* loaded) {
    history_.messages = NULL;
    chat_history_document_free(&history_);
    history_ = *loaded;

    messages_.clear();
    int count = cJSON_GetArraySize(loaded->messages);
    for (int index = 0; index < count; index++) {
        cJSON* item = cJSON_GetArrayItem(loaded->messages, index);
        if (!cJSON_IsObject(item)) continue;
        cJSON* role = cJSON_GetObjectItemCaseSensitive(item, "role");
        cJSON* text = cJSON_GetObjectItemCaseSensitive(item, "text");
        messages_.push_back({index + 1,
            cJSON_IsString(role) && role->valuestring ? role->valuestring : "assistant",
            cJSON_IsString(text) && text->valuestring ? text->valuestring : ""});
    }
    next_msg_id_ = (int)messages_.size() + 1;

    loaded->messages = NULL;
    chat_history_document_free(loaded);
    setHistoryArrayPlaceholder();
    revision_ = 0;
    saved_revision_ = 0;
    setDirty();
}

bool ChatUi::canResume() const {
    return chat_history_document_can_resume(&history_, webui_port_) != 0;
}

bool ChatUi::prepareFreshSession() {
    DWORD error = ERROR_SUCCESS;
    if (history_.version != 0 && history_path_[0]) {
        wchar_t archive_path[MAX_PATH + 64];
        if (chat_history_copy_to_archive(history_path_, archive_path,
                _countof(archive_path), &error) != 0) {
            statusI18n("无法备份旧聊天记录，未连接",
                       "Could not archive old chat history; not connecting");
            return false;
        }
    }
    discardFreshSession();
    fresh_pending_ = true;
    return true;
}

void ChatUi::discardFreshSession() {
    fresh_messages_.clear();
    fresh_pending_ = false;
}

void ChatUi::discardNewHistoryBackup() {
    new_history_backup_.clear();
}

void ChatUi::restoreNewHistoryBackup() {
    if (turn_state_ != ChatTurnState::NewWaitAck) return;
    turn_state_ = ChatTurnState::Idle;
    turn_deadline_ = 0;
    if (new_history_backup_.empty()) return;
    messages_ = std::move(new_history_backup_);
    new_history_backup_.clear();
    next_msg_id_ = (int)messages_.size() + 1;
    revision_++;
    setDirty();
    saveIfDirty();
}

bool ChatUi::recoverHistoryBeforeSend() {
    chat_history_document loaded;
    DWORD error = ERROR_SUCCESS;
    chat_history_load_result result;
    if (!history_read_error_) return true;
    chat_history_document_init(&loaded);
    result = chat_history_load(history_path_, &loaded, &error);
    if (result == CHAT_HISTORY_LOAD_OK) {
        adoptDocument(&loaded);
        history_read_error_ = false;
        return true;
    }
    if (result == CHAT_HISTORY_LOAD_ABSENT) {
        history_read_error_ = false;
        return true;
    }
    if (result == CHAT_HISTORY_LOAD_INVALID &&
        chat_history_quarantine_invalid(history_path_, &error) == 0) {
        messages_.clear();
        next_msg_id_ = 1;
        revision_ = 0;
        saved_revision_ = 0;
        setDirty();
        history_read_error_ = false;
        statusI18n("无效历史已备份", "Invalid chat history was backed up");
        return true;
    }
    chat_history_document_free(&loaded);
    statusI18n("聊天记录暂时不可用，未发送消息",
               "Chat history is temporarily unavailable; message was not sent");
    return false;
}

/* --- derived state ---------------------------------------------------------- */

bool ChatUi::canSendNow() const {
    return turn_state_ == ChatTurnState::Idle && chat_client().isReady() &&
        (!health_fn_ || health_fn_());
}

bool ChatUi::canRetryNow() const {
    return chat_protocol_retry_allowed(auto_connect_blocked_ ? 1 : 0,
        turn_state_ == ChatTurnState::Idle ? 1 : 0,
        (!health_fn_ || health_fn_()) ? 1 : 0,
        chat_client().isConnected() ? 1 : 0,
        chat_client().needsConnect() ? 0 : 1,
        attach_pending_ ? 1 : 0) != 0;
}

void ChatUi::setCanSend(int enabled) {
    if (page_) ui_page_set_bool(page_, "canSend", enabled ? 1 : 0);
}

void ChatUi::statusUtf8(const char* utf8) {
    nm_page_set_utf8(page_, "chatStatus", utf8);
}

void ChatUi::statusI18n(const char* zh, const char* en) {
    statusUtf8(is_zh_ && is_zh_() ? zh : en);
}

void ChatUi::attachScroll(UiWidget widget) {
    chat_scroll_ = widget;
    /* Flush anything deferred while the page was unmounted before jumping to
     * the end, so re-entering chat shows the latest stream state. */
    flushMessagesUi();
    ui_scroll_to_end(widget);
}

void ChatUi::detachScroll(UiWidget widget) {
    if (chat_scroll_ == widget) chat_scroll_ = 0;
}

void ChatUi::attachAfterReady(const char* offered_id) {
    const char* wanted = offered_id;
    if (!chat_history_is_valid_chat_id(offered_id)) {
        terminalFailure("服务器会话无效", "Server offered an invalid chat session", NULL);
        return;
    }
    if (canResume())
        wanted = history_.chat_id;
    else if (!prepareFreshSession()) {
        terminalFailure("无法准备新的聊天记录", "Could not prepare new chat history", NULL);
        return;
    }
    if (chat_client().attach(wanted) != 0) {
        terminalFailure("无法附加聊天会话", "Could not attach chat session", NULL);
        return;
    }
    attach_pending_ = true;
    attach_deadline_ = GetTickCount64() + CHAT_ATTACH_TIMEOUT_MS;
    setCanSend(0);
    statusI18n("正在恢复会话…", "Restoring session…");
}

void ChatUi::terminalFailure(const char* zh, const char* en, const char* detail) {
    int had_fresh = fresh_pending_ ? 1 : 0;
    if (draft_.active)
        commitCanonical(NULL);   /* fixate the partial reply before teardown */
    flushMessagesUi();
    attach_pending_ = false;
    attach_deadline_ = 0;
    turn_deadline_ = 0;
    restoreNewHistoryBackup();
    discardFreshSession();
    turn_state_ = ChatTurnState::Idle;
    /* Only an attempted message makes the binding unsafe. Attachment and
       connection failures preserve a previously safe resume point. */
    if (chat_protocol_should_persist_unsafe(had_fresh, history_.resume_safe))
        persistBinding(0);
    auto_connect_blocked_ = true;
    if (detail && detail[0]) statusUtf8(detail);
    else statusI18n(zh, en);
    setCanSend(0);
    chat_client().disconnectAsync();
}

/* --- per-event handlers ------------------------------------------------------ */

void ChatUi::onConnected() {
    /* CONNECTED can be queued behind ATTACHED; never regress a ready UI. */
    if (!chat_client().isReady()) {
        /* Cover the CONNECTED -> READY window too: if the gateway completes
         * the upgrade but never offers a session, the attach deadline would
         * otherwise never be armed. attachAfterReady re-arms it on READY. */
        attach_pending_ = true;
        attach_deadline_ = GetTickCount64() + CHAT_ATTACH_TIMEOUT_MS;
        statusI18n("已连接…", "Connected…");
        setCanSend(0);
    }
}

void ChatUi::onReady(const char* offered_id) {
    attachAfterReady(offered_id);
}

void ChatUi::onAttached(const char* attached_id) {
    if (!attach_pending_ || !attached_id || !chat_history_is_valid_chat_id(attached_id))
        return;
    attach_pending_ = false;
    attach_deadline_ = 0;
    int fresh_committed = 0;
    if (fresh_pending_) {
        chat_history_document fresh;
        chat_history_document_init(&fresh);
        fresh.version = 2;
        fresh.webui_port = webui_port_;
        strcpy_s(fresh.chat_id, sizeof(fresh.chat_id), attached_id);
        fresh.resume_safe = 1;
        cJSON* array = messagesToJson(fresh_messages_);
        fresh.messages = array;
        if (!history_path_[0] || history_read_error_ || !array ||
            chat_history_save(history_path_, &fresh, NULL) != 0) {
            fresh.messages = NULL;
            cJSON_Delete(array);
            terminalFailure("聊天记录保存失败", "Could not save chat history", NULL);
            return;
        }
        history_.messages = NULL;
        chat_history_document_free(&history_);
        fresh.messages = NULL;
        history_ = fresh;
        cJSON_Delete(array);
        messages_ = std::move(fresh_messages_);
        discardFreshSession();
        next_msg_id_ = (int)messages_.size() + 1;
        revision_ = saved_revision_ = 0;
        setHistoryArrayPlaceholder();
        setDirty();
        fresh_committed = 1;
    }
    history_.version = 2;
    history_.webui_port = webui_port_;
    strcpy_s(history_.chat_id, sizeof(history_.chat_id), attached_id);
    history_.resume_safe = turn_state_ == ChatTurnState::Idle ? 1 : 0;
    if (!fresh_committed && !persistBinding(history_.resume_safe)) {
        terminalFailure("聊天记录保存失败", "Could not save chat history", NULL);
        return;
    }
    statusI18n("已连接", "Connected");
    setCanSend(canSendNow() ? 1 : 0);
}

void ChatUi::onDisconnected() {
    flushMessagesUi();
    attach_pending_ = false;
    attach_deadline_ = 0;
    turn_deadline_ = 0;
    if (turn_state_ != ChatTurnState::Idle) {
        commitCanonical(NULL);   /* fixate a half-streamed reply */
        persistBinding(0);
    }
    restoreNewHistoryBackup();
    turn_state_ = ChatTurnState::Idle;
    saveIfDirty();
    if (!auto_connect_blocked_)
        statusI18n("未连接", "Disconnected");
    setCanSend(canRetryNow() ? 1 : 0);
}

void ChatUi::onDelta(const char* chunk) {
    if (turn_state_ == ChatTurnState::NormalWaitActivity)
        turn_state_ = ChatTurnState::NormalActive;
    if (turn_state_ != ChatTurnState::NormalActive) return;
    turn_deadline_ = GetTickCount64() + CHAT_TURN_TIMEOUT_MS;   /* activity heartbeat */
    if (!chunk) return;
    if (!draft_.append(chunk, CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES))
        statusI18n("回复过长，已保留可保存的内容",
                   "Reply is too long; the saved portion was kept");
    else
        setDirty();
}

void ChatUi::onMessage(const char* detail) {
    if (turn_state_ == ChatTurnState::NewWaitAck) {
        if (detail && strcmp(detail, "New session started.") == 0) {
            draft_.clear();
            discardNewHistoryBackup();
            turn_state_ = ChatTurnState::Idle;
            turn_deadline_ = 0;
            persistBinding(1);
            setCanSend(canSendNow() ? 1 : 0);
        }
    } else if (turn_state_ == ChatTurnState::NormalWaitActivity) {
        if (detail && !commitCanonical(detail))
            statusI18n("回复过长，已保留可保存的内容",
                       "Reply is too long; the saved portion was kept");
        turn_state_ = ChatTurnState::Idle;
        turn_deadline_ = 0;
        persistBinding(1);
        setCanSend(canSendNow() ? 1 : 0);
    } else if (turn_state_ == ChatTurnState::NormalActive) {
        turn_deadline_ = GetTickCount64() + CHAT_TURN_TIMEOUT_MS;   /* activity heartbeat */
        if (detail && !commitCanonical(detail))
            statusI18n("回复过长，已保留可保存的内容",
                       "Reply is too long; the saved portion was kept");
        saveIfDirty();
    }
    flushMessagesUi();
}

void ChatUi::onStreamEnd(const char* detail) {
    if (turn_state_ == ChatTurnState::NormalWaitActivity)
        turn_state_ = ChatTurnState::NormalActive;
    if (turn_state_ != ChatTurnState::NormalActive) return;
    /* TURN_END (or a terminal MESSAGE) must follow shortly; keep the
     * watchdog armed for that gap. */
    turn_deadline_ = GetTickCount64() + CHAT_TURN_TIMEOUT_MS;
    if (detail && !commitCanonical(detail))
        statusI18n("回复过长，已保留可保存的内容",
                   "Reply is too long; the saved portion was kept");
    /* Without canonical text the draft stays active and is fixated at
     * TURN_END / disconnect. */
    saveIfDirty();
    flushMessagesUi();
}

void ChatUi::onTurnEnd() {
    if (turn_state_ != ChatTurnState::NormalActive) return;
    if (draft_.active)
        commitCanonical(NULL);   /* fixate a stream without canonical text */
    turn_state_ = ChatTurnState::Idle;
    turn_deadline_ = 0;
    persistBinding(1);
    setCanSend(canSendNow() ? 1 : 0);
    flushMessagesUi();
}

void ChatUi::onError(const char* detail) {
    terminalFailure("连接失败", "Connection failed", detail);
}

/* --- event dispatch ----------------------------------------------------------- */

void ChatUi::onEvent(WPARAM kind, chat_event* event) {
    if (!event) return;
    if (event->generation != chat_client().currentGeneration()) {
        chat_event_free(event);
        return;
    }
    const char* detail = event->detail;
    switch ((int)kind) {
    case CHAT_EVT_CONNECTED:       onConnected(); break;
    case CHAT_EVT_READY:           onReady(detail); break;
    case CHAT_EVT_ATTACHED:        onAttached(detail); break;
    case CHAT_EVT_DISCONNECTED:
    case CHAT_EVT_DISCONNECT_DONE: onDisconnected(); break;
    case CHAT_EVT_DELTA:           onDelta(detail); break;
    case CHAT_EVT_MESSAGE:         onMessage(detail); break;
    case CHAT_EVT_STREAM_END:      onStreamEnd(detail); break;
    case CHAT_EVT_TURN_END:        onTurnEnd(); break;
    case CHAT_EVT_ERROR:           onError(detail); break;
    default: break;
    }
    chat_event_free(event);
}

/* --- send ---------------------------------------------------------------------- */

void ChatUi::onSend(int health_running) {
    if (!page_ || turn_state_ != ChatTurnState::Idle) return;
    if (canRetryNow()) {
        auto_connect_blocked_ = false;
        setCanSend(0);
        statusI18n("正在连接…", "Connecting…");
        chat_client().connect();
        return;
    }
    char* raw = ui_page_get_json(page_, "draft");
    if (!raw) return;
    cJSON* json = cJSON_Parse(raw);
    ui_page_free(raw);
    const char* text = json && cJSON_IsString(json) ? json->valuestring : NULL;
    if (!text) { cJSON_Delete(json); return; }
    if (strlen(text) > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES) {
        statusI18n("消息过长，无法发送", "Message is too long to send");
        cJSON_Delete(json);
        return;
    }
    if (chat_history_is_blank_message(text)) { cJSON_Delete(json); return; }
    if (!recoverHistoryBeforeSend()) {
        cJSON_Delete(json);
        return;
    }
    if (!health_running) {
        statusI18n("请先启动 nanobot", "Start nanobot first");
        cJSON_Delete(json);
        return;
    }
    if (!chat_client().isReady()) {
        statusI18n("连接中…连接后请再次发送", "Connecting… send again when connected");
        auto_connect_blocked_ = false;
        chat_client().connect();
        cJSON_Delete(json);
        return;
    }
    int is_new = chat_history_is_new_command(text);
    std::vector<ChatMsg> backup;
    if (is_new) backup = messages_;
    /* Allocate /new compensation before making the on-disk binding unsafe. */
    int prior_resume_safe = history_.resume_safe;
    if (!persistBinding(0)) {
        history_.resume_safe = prior_resume_safe;
        statusI18n("聊天记录保存失败，未发送消息",
                   "Could not save chat history; message was not sent");
        cJSON_Delete(json);
        return;
    }
    if (chat_client().send(is_new ? "/new" : text) != 0) {
        persistBinding(prior_resume_safe);
        statusI18n("发送失败", "Send failed");
        cJSON_Delete(json);
        return;
    }
    turn_state_ = is_new ? ChatTurnState::NewWaitAck : ChatTurnState::NormalWaitActivity;
    turn_deadline_ = GetTickCount64() + CHAT_TURN_TIMEOUT_MS;
    setCanSend(0);
    if (is_new) {
        new_history_backup_ = std::move(backup);
        messages_.clear();
        next_msg_id_ = 1;
        revision_++;
        setDirty();
    } else {
        appendMsg("user", text);
    }
    ui_page_set_text(page_, "draft", L"");
    saveIfDirty();
    flushMessagesUi();
    cJSON_Delete(json);
}

/* --- lifecycle ------------------------------------------------------------------ */

void ChatUi::bind(UiPage page, int (*is_zh)(void)) {
    page_ = page;
    is_zh_ = is_zh;
}

void ChatUi::setGateway(int (*health_fn)(void), int webui_port) {
    int next_port = (webui_port > 0 && webui_port <= 65535) ? webui_port : 8765;
    health_fn_ = health_fn;
    if (webui_port_ != next_port && history_.version == 2)
        setCanSend(0);
    webui_port_ = next_port;
}

void ChatUi::configureHistory(const wchar_t* config_path) {
    chat_history_document loaded;
    DWORD error = ERROR_SUCCESS;
    chat_history_load_result result;
    history_path_[0] = L'\0';
    history_read_error_ = false;
    if (chat_history_path_from_config(config_path, history_path_, _countof(history_path_)) != 0) return;
    chat_history_document_init(&loaded);
    result = chat_history_load(history_path_, &loaded, &error);
    if (result == CHAT_HISTORY_LOAD_OK) {
        adoptDocument(&loaded);
    } else if (result == CHAT_HISTORY_LOAD_INVALID) {
        if (chat_history_quarantine_invalid(history_path_, &error) == 0) {
            messages_.clear();
            next_msg_id_ = 1;
            revision_ = 0;
            saved_revision_ = 0;
            setDirty();
            statusI18n("无效历史已备份", "Invalid chat history was backed up");
        } else {
            history_read_error_ = true;
            statusI18n("无效聊天记录无法备份，原文件未被覆盖",
                       "Invalid chat history could not be backed up; existing file was not replaced");
        }
    } else if (result == CHAT_HISTORY_LOAD_IO_ERROR) {
        history_read_error_ = true;
        statusI18n("聊天记录读取失败，现有记录未被覆盖",
                   "Could not read chat history; existing file was not replaced");
    }
    chat_history_document_free(&loaded);
}

void ChatUi::resetIdle() {
    discardNewHistoryBackup();
    messages_.clear();
    next_msg_id_ = 1;
    draft_.clear();
    turn_state_ = ChatTurnState::Idle;
    turn_deadline_ = 0;
    revision_ = 0;
    saved_revision_ = 0;
    if (page_) {
        setDirty();
        flushMessagesUi();
        ui_page_set_text(page_, "draft", L"");
        setCanSend(0);
    }
    statusI18n("启动 nanobot 后即可聊天", "Start nanobot to chat");
}

void ChatUi::shutdown() {
    flushMessagesUi();
    restoreNewHistoryBackup();
    saveIfDirty();
    discardNewHistoryBackup();
    discardFreshSession();
    draft_.clear();
    messages_.clear();
    history_.messages = NULL;
    chat_history_document_free(&history_);
    chat_history_document_init(&history_);
    page_ = 0;
    chat_scroll_ = 0;
    messages_ui_dirty_ = false;
    is_zh_ = NULL;
    health_fn_ = NULL;
    history_path_[0] = L'\0';
    history_read_error_ = false;
    turn_state_ = ChatTurnState::Idle;
    turn_deadline_ = 0;
}

void ChatUi::onGatewayStopped() {
    flushMessagesUi();
    auto_connect_blocked_ = false;
    attach_pending_ = false;
    attach_deadline_ = 0;
    turn_deadline_ = 0;
    if (draft_.active)
        commitCanonical(NULL);   /* fixate a half-streamed reply */
    restoreNewHistoryBackup();
    turn_state_ = ChatTurnState::Idle;
    saveIfDirty();
    statusI18n("启动 nanobot 后即可聊天", "Start nanobot to chat");
    setCanSend(0);
}

void ChatUi::tick(ULONGLONG now) {
    if (attach_pending_ && now >= attach_deadline_)
        terminalFailure("恢复会话超时", "Session restore timed out", NULL);
    else if (turn_state_ != ChatTurnState::Idle && turn_deadline_ && now >= turn_deadline_)
        terminalFailure("等待回复超时", "Timed out waiting for a reply", NULL);
}

void ChatUi::publishTick() {
    flushMessagesUi();
}

/* --- mount trampolines ------------------------------------------------------------ */

static void on_send_click(UiWidget widget, void* userdata) {
    (void)widget;
    (void)userdata;
    ChatUi& ui = chat_ui();
    if (ui.canSendNow() || ui.canRetryNow())
        ui.onSend(ui.gatewayRunning() ? 1 : 0);
}

void chat_ui_mount_send(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    ui_widget_on_click(widget, on_send_click, NULL);
}

void chat_ui_mount_input(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    ui_widget_on_submit(widget, on_send_click, NULL);
}

void chat_ui_mount_scroll(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    chat_ui().attachScroll(widget);
}

void chat_ui_unmount_scroll(UiPage page, UiWidget widget, void* userdata) {
    (void)page;
    (void)userdata;
    chat_ui().detachScroll(widget);
}
