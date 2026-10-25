/* ChatDraft: bounded streaming buffer feeding the chat page's virtual
 * trailing bubble. Pure logic, no UI or Win32 deps. */

#include "nm_test.h"
#include "chat_draft.hpp"

#include <string.h>

#define DRAFT_MAX (256U * 1024U)

static void test_append_accumulates(void) {
    ChatDraft draft;
    TEST_CHECK(draft.append("Hello, ", DRAFT_MAX));
    TEST_CHECK(draft.append("world!", DRAFT_MAX));
    TEST_CHECK(draft.active);
    TEST_CHECK(strcmp(draft.text.c_str(), "Hello, world!") == 0);
}

static void test_empty_chunk_is_noop_success(void) {
    ChatDraft draft;
    TEST_CHECK(draft.append("", DRAFT_MAX));
    TEST_CHECK(!draft.active);   /* no bubble created for an empty delta */
    TEST_CHECK(draft.append(NULL, DRAFT_MAX));
    TEST_CHECK(!draft.active);
    /* A real chunk still starts the draft afterwards. */
    TEST_CHECK(draft.append("hi", DRAFT_MAX));
    TEST_CHECK(draft.active);
}

static void test_overflow_rejects_and_keeps_content(void) {
    ChatDraft draft;
    const char* big = "0123456789";   /* 10 bytes */
    /* Fill exactly to the bound (exact fit is allowed, like the old
       `old_length > MAX - chunk` check). */
    for (int i = 0; i < 10; i++)
        TEST_CHECK(draft.append(big, 100));
    TEST_CHECK(draft.text.size() == 100);
    /* Anything past the bound is rejected; content stays intact. */
    TEST_CHECK(!draft.append("0", 100));
    TEST_CHECK(!draft.append("0123456789", 100));
    TEST_CHECK(draft.text.size() == 100);
    TEST_CHECK(strcmp(draft.text.c_str() + 90, "0123456789") == 0);
}

static void test_commit_semantics_independent_of_buffer(void) {
    /* The draft only stores text; fixation (turn -> message) is the owner's
       decision, so clear() must fully reset both flags and capacity. */
    ChatDraft draft;
    TEST_CHECK(draft.append("partial", DRAFT_MAX));
    draft.clear();
    TEST_CHECK(!draft.active);
    TEST_CHECK(draft.text.empty());
    TEST_CHECK(draft.append("next turn", DRAFT_MAX));
    TEST_CHECK(strcmp(draft.text.c_str(), "next turn") == 0);
}

static void test_begin_restarts_stream(void) {
    ChatDraft draft;
    TEST_CHECK(draft.append("first", DRAFT_MAX));
    draft.begin();
    TEST_CHECK(draft.active);
    TEST_CHECK(draft.text.empty());
    TEST_CHECK(draft.append("second", DRAFT_MAX));
    TEST_CHECK(strcmp(draft.text.c_str(), "second") == 0);
}

TEST_LIST = {
    { "append_accumulates", test_append_accumulates },
    { "empty_chunk_noop", test_empty_chunk_is_noop_success },
    { "overflow_rejects", test_overflow_rejects_and_keeps_content },
    { "clear_resets", test_commit_semantics_independent_of_buffer },
    { "begin_restarts", test_begin_restarts_stream },
    { NULL, NULL }
};
