#include "chat_protocol.h"
#include "chat_history.h"

#include <string.h>

int chat_protocol_resolve_attached(const char* pending_id, const char* attached_id,
    int attached_id_present, char* resolved_id, size_t resolved_id_size) {
    const char* resolved;
    if (!resolved_id || resolved_id_size == 0) return 0;
    resolved_id[0] = '\0';   /* output stays defined even on rejection */
    if (!chat_history_is_valid_chat_id(pending_id))
        return 0;
    if (attached_id_present) {
        if (!chat_history_is_valid_chat_id(attached_id) || strcmp(pending_id, attached_id) != 0)
            return 0;
        resolved = attached_id;
    } else {
        resolved = pending_id;
    }
    if (strlen(resolved) >= resolved_id_size) return 0;
    strcpy_s(resolved_id, resolved_id_size, resolved);
    return 1;
}

uint64_t chat_protocol_next_generation(uint64_t generation) {
    /* Wraps to 0 at UINT64_MAX; uniqueness is only reused after a full wrap,
     * which cannot coincide with any in-flight event in practice. */
    return generation + 1;
}

int chat_protocol_retry_allowed(int auto_connect_blocked, int turn_idle, int healthy,
    int connected, int connecting, int attach_pending) {
    return auto_connect_blocked && turn_idle && healthy && !connected && !connecting &&
        !attach_pending;
}

int chat_protocol_should_persist_unsafe(int fresh_pending, int resume_safe) {
    return !fresh_pending && !resume_safe;
}

int chat_protocol_send_allowed(int turn_active, int is_new) {
    /* Models gateway queueing only (turn_active vs is_new): mid-turn normal
     * text is queued into the active turn's _pending_queues, while a fresh
     * /new needs an idle turn. The UI layer enforces stricter send gating on
     * top of this (e.g. it blocks input while a NewWaitAck is pending). */
    return !turn_active || !is_new;
}

int chat_protocol_has_text(const char* s) {
    /* Non-empty payload only: gateway tool results and pure-reasoning
     * stream_ends arrive as message text:"" / stream_end text:"", which must
     * not be committed as (empty) canonical reply text. */
    return s && s[0];
}

int chat_protocol_lifecycle_operational(int lifecycle_event_ready, int lifecycle_thread_ready,
    int shutting_down) {
    return lifecycle_event_ready && lifecycle_thread_ready && !shutting_down;
}
