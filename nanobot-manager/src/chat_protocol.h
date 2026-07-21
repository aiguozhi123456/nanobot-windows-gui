#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int chat_protocol_resolve_attached(const char* pending_id, const char* attached_id,
    int attached_id_present, char* resolved_id, size_t resolved_id_size);
uint64_t chat_protocol_next_generation(uint64_t generation);
int chat_protocol_retry_allowed(int auto_connect_blocked, int turn_idle, int healthy,
    int connected, int connecting, int attach_pending);
int chat_protocol_should_persist_unsafe(int fresh_pending, int resume_safe);
int chat_protocol_lifecycle_operational(int lifecycle_event_ready, int lifecycle_thread_ready,
    int shutting_down);

#ifdef __cplusplus
}
#endif
#endif
