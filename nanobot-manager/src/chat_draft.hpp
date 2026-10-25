#ifndef CHAT_DRAFT_HPP
#define CHAT_DRAFT_HPP

#include <string>
#include <cstring>

/* Streaming reply draft. DELTA chunks accumulate here with amortized O(1)
 * appends (the old implementation re-copied the whole accumulated text for
 * every chunk). The draft is NOT part of the message model until committed:
 * it renders as a virtual trailing bubble, is never persisted mid-stream,
 * and CHAT_EVT_MESSAGE / CHAT_EVT_STREAM_END canonical text (or fixation at
 * TURN_END / disconnect) turns it into a real assistant message. */
struct ChatDraft {
    bool active = false;
    std::string text;

    void begin() {
        active = true;
        text.clear();
    }

    void clear() {
        active = false;
        text.clear();
        text.shrink_to_fit();
    }

    /* Append one delta chunk. Returns false (draft unchanged) when the
     * accumulated text would exceed max_bytes; empty chunks are a no-op
     * success (an empty delta must not read as "reply too long"). */
    bool append(const char* chunk, size_t max_bytes) {
        if (!chunk || !chunk[0]) return true;
        size_t chunk_len = strlen(chunk);
        if (chunk_len > max_bytes || text.size() > max_bytes - chunk_len)
            return false;
        if (!active) begin();
        text += chunk;
        return true;
    }
};

#endif
