#ifndef CHAT_UI_HPP
#define CHAT_UI_HPP

#include "ui_core.h"
#include "chat.hpp"
#include "chat_draft.hpp"
#include "chat_history.h"

#include <windows.h>

#include <string>
#include <vector>

/* Turn state machine (semantics per docs/solutions/architecture-patterns/
 * native-websocket-chat-integration.md):
 *   Idle --send /new--> NewWaitAck --"New session started."--> Idle
 *   Idle --send text--> NormalWaitActivity --DELTA/STREAM_END--> NormalActive
 *   NormalWaitActivity --MESSAGE--> Idle (non-stream reply)
 *   NormalActive --TURN_END--> Idle
 * Any non-Idle state + timeout/terminal error --> Idle (with compensation). */
enum class ChatTurnState {
    Idle,
    NewWaitAck,
    NormalWaitActivity,
    NormalActive
};

/* One rendered/persisted chat message. id is display-order only (v-for key). */
struct ChatMsg {
    int id;
    std::string role;   /* "user" | "assistant" */
    std::string text;
};

/* Chat page state machine + publish pipeline. UI-thread only.
 *
 * Message model: std::vector<ChatMsg> owned here; cJSON appears only at the
 * history-file boundary (chat_history API keeps its cJSON-based document)
 * and the publish boundary (ui_page_set_json). The streaming reply lives in
 * a ChatDraft and is published as a virtual trailing bubble; canonical text
 * commits it as a real message (one bubble per streamed segment).
 *
 * Publish path: every model change only sets a dirty flag; the 66 ms UI
 * timer (publishTick), the scroll mount hook, and shutdown are the only
 * flush entry points. */
class ChatUi {
public:
    /* Status text is key-based: statusKey() pushes an i18n key into the
     * chatStatusKey page field ($t resolves it, so locale switches refresh
     * the label); raw server error text goes to chatStatusDetail instead. */
    void bind(UiPage page);

    /* health_fn returns 1 if the gateway is running; drives the send button. */
    void setGateway(int (*health_fn)(void), int webui_port);
    void configureHistory(const wchar_t* config_path);
    void resetIdle();
    void shutdown();

    void onEvent(WPARAM kind, chat_event* event);
    void onSend(int health_running);
    void onGatewayStopped();

    /* Expire a pending session attachment / in-flight turn (1 s cadence). */
    void tick(ULONGLONG now);
    /* Flush coalesced streaming text (66 ms cadence). */
    void publishTick();

    /* False after a terminal chat failure until an explicit user retry. */
    bool canAutoConnect() const { return !auto_connect_blocked_; }

    /* True while a turn is in flight: drives the dedicated stop button
     * (cancelWait) and gates a mid-turn /new. */
    bool turnWaiting() const { return turn_state_ != ChatTurnState::Idle; }
    /* User-initiated stop of the in-flight turn: sends the gateway's
     * priority /stop and grants a short grace for the normal teardown,
     * falling back to the soft abort on send failure or during /new ack. */
    void cancelWait();

    /* Send-gate predicates + scroll attach/detach, used by the mount
     * trampolines below. */
    bool canSendNow() const;
    bool canRetryNow() const;
    bool gatewayRunning() const { return !health_fn_ || health_fn_() != 0; }
    void attachScroll(UiWidget widget);
    void detachScroll(UiWidget widget);
    /* Summon-time jump to the newest message when the chat page is already
     * mounted (no remount -> attachScroll will not fire). */
    void scrollToBottom();

private:
    /* --- model + publish ------------------------------------------------- */
    void setDirty() { messages_ui_dirty_ = true; }
    void flushMessagesUi();
    void pushMessagesUi();
    char* serializeVisible();   /* malloc'd JSON, caller frees */
    cJSON* messagesToJson(const std::vector<ChatMsg>& messages);
    void appendMsg(const char* role, const std::string& text);
    /* Canonical assistant text: always appends a bubble (one per streamed
     * segment); a redelivery identical to the last assistant message is
     * deduplicated. Returns 0 when over the size bound (draft/content kept). */
    int commitCanonical(const char* canonical_or_null);

    /* --- history ---------------------------------------------------------- */
    void adoptDocument(chat_history_document* loaded);
    void setHistoryArrayPlaceholder();
    int  persistBinding(int resume_safe);
    void saveIfDirty();
    void saveFailureStatus();
    bool recoverHistoryBeforeSend();
    bool prepareFreshSession();
    void discardFreshSession();
    void discardNewHistoryBackup();
    void restoreNewHistoryBackup();
    bool canResume() const;

    /* --- derived state ---------------------------------------------------- */
    void setCanSend(int enabled);
    /* Single mutation point for the turn state: keeps the turnWaiting page
     * field (send button label/enabled binding) in sync. */
    void setTurnState(ChatTurnState s);
    void statusKey(const char* key);
    void statusDetail(const char* utf8);
    void attachAfterReady(const char* offered_id);
    /* hard: protocol/attach failures - tear down and block auto-connect.
     * soft: turn timeout / user stop - keep the socket, re-enable sending
     * (a slow tool phase must not poison a healthy connection). */
    void terminalFailure(const char* key, const char* detail, bool hard);
    void softAbortTurn(const char* key);

    /* --- per-event handlers (CHAT_EVT_*) ---------------------------------- */
    void onConnected();
    void onReady(const char* offered_id);
    void onAttached(const char* attached_id);
    void onDisconnected();
    void onDelta(const char* chunk);
    void onMessage(const char* detail);
    void onStreamEnd(const char* detail);
    void onTurnEnd();
    void onError(const char* detail);
    void onActivity();

    UiPage page_ = 0;
    int (*health_fn_)(void) = nullptr;
    int webui_port_ = 8765;

    std::vector<ChatMsg> messages_;
    int next_msg_id_ = 1;

    ChatDraft draft_;
    ChatTurnState turn_state_ = ChatTurnState::Idle;

    /* Document fields are the resume binding; messages always live in the
     * vector. history_.messages holds an empty placeholder array whenever
     * the document is idle (chat_history predicates expect an array) and a
     * materialized copy only inside save calls. */
    chat_history_document history_{};
    wchar_t history_path_[MAX_PATH] = {0};
    bool history_read_error_ = false;

    unsigned long revision_ = 0;
    unsigned long saved_revision_ = 0;

    std::vector<ChatMsg> new_history_backup_;   /* /new compensation */
    std::vector<ChatMsg> fresh_messages_;       /* pending fresh session */
    bool fresh_pending_ = false;

    bool attach_pending_ = false;
    ULONGLONG attach_deadline_ = 0;
    /* Inactivity watchdog for an in-flight turn: armed on send, re-armed on
     * every DELTA/MESSAGE/STREAM_END frame, expired by tick(). Without it a
     * gateway that accepts a frame but never answers leaves canSend off. */
    ULONGLONG turn_deadline_ = 0;
    /* Set while a user-requested /stop grace window is open: suppresses the
     * DELTA/MESSAGE/ACTIVITY watchdog re-arm so the short CHAT_STOP_GRACE_MS
     * deadline is what expires, giving the gateway's own TURN_END the chance
     * to run its normal teardown first. */
    bool stop_requested_ = false;

    bool auto_connect_blocked_ = false;
    UiWidget chat_scroll_ = 0;
    /* Mount-time ui_scroll_to_end can freeze an inflated content height from
     * the first layout pass; the summon path re-scrolls after settle and is
     * the trusted semantic — mirror it one publish tick after mount. */
    bool resettle_bottom_ = false;
    bool messages_ui_dirty_ = false;
};

/* Mount trampolines for ui_page_on_widget_mount registration in main.cpp. */
void chat_ui_mount_send(UiPage page, UiWidget widget, void* userdata);
void chat_ui_mount_stop(UiPage page, UiWidget widget, void* userdata);
void chat_ui_mount_input(UiPage page, UiWidget widget, void* userdata);
void chat_ui_mount_scroll(UiPage page, UiWidget widget, void* userdata);
void chat_ui_unmount_scroll(UiPage page, UiWidget widget, void* userdata);

/* Process-wide instance (mirrors chat_client()). */
ChatUi& chat_ui();

#endif
