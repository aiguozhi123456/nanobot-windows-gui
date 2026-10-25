#include "nm_test.h"
#include "chat_protocol.h"

#include <string.h>

static void test_attached_resolution(void) {
    char resolved[65];
    TEST_CHECK(chat_protocol_resolve_attached("session_1", NULL, 0, resolved, sizeof(resolved)));
    TEST_CHECK(strcmp(resolved, "session_1") == 0);
    TEST_CHECK(chat_protocol_resolve_attached("session_1", "session_1", 1, resolved, sizeof(resolved)));
    TEST_CHECK(!chat_protocol_resolve_attached("session_1", "other", 1, resolved, sizeof(resolved)));
    TEST_CHECK(!chat_protocol_resolve_attached("session_1", NULL, 1, resolved, sizeof(resolved)));

    /* Guard paths: NULL/zero-sized output, NULL/invalid pending id. */
    TEST_CHECK(!chat_protocol_resolve_attached("session_1", NULL, 0, NULL, sizeof(resolved)));
    TEST_CHECK(!chat_protocol_resolve_attached("session_1", NULL, 0, resolved, 0));
    TEST_CHECK(!chat_protocol_resolve_attached(NULL, NULL, 0, resolved, sizeof(resolved)));
    TEST_CHECK(strcmp(resolved, "") == 0);   /* output cleared on rejection */

    /* Max-length id (64 chars): exact fit succeeds, one byte short fails. */
    {
        static const char max_id[65] =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        char too_small[64];
        TEST_CHECK(chat_protocol_resolve_attached(max_id, NULL, 0, resolved, sizeof(resolved)));
        TEST_CHECK(strcmp(resolved, max_id) == 0);
        TEST_CHECK(!chat_protocol_resolve_attached(max_id, NULL, 0, too_small, sizeof(too_small)));
        TEST_CHECK(too_small[0] == '\0');
    }
}

static void test_generation_reservation(void) {
    uint64_t generation = 41;
    uint64_t replacement = chat_protocol_next_generation(generation);
    TEST_CHECK(replacement == 42 && generation == 41);
    TEST_CHECK(chat_protocol_next_generation(replacement) == 43);
}

static void test_retry_policy(void) {
    TEST_CHECK(chat_protocol_retry_allowed(1, 1, 1, 0, 0, 0));
    TEST_CHECK(!chat_protocol_retry_allowed(0, 1, 1, 0, 0, 0));
    TEST_CHECK(!chat_protocol_retry_allowed(1, 0, 1, 0, 0, 0));
    TEST_CHECK(!chat_protocol_retry_allowed(1, 1, 0, 0, 0, 0));
    TEST_CHECK(!chat_protocol_retry_allowed(1, 1, 1, 1, 0, 0));
    TEST_CHECK(!chat_protocol_retry_allowed(1, 1, 1, 0, 1, 0));
    TEST_CHECK(!chat_protocol_retry_allowed(1, 1, 1, 0, 0, 1));
}

static void test_preattachment_failure_preserves_safe_binding(void) {
    TEST_CHECK(!chat_protocol_should_persist_unsafe(0, 1));
    TEST_CHECK(!chat_protocol_should_persist_unsafe(1, 1));
    TEST_CHECK(!chat_protocol_should_persist_unsafe(1, 0));
    TEST_CHECK(chat_protocol_should_persist_unsafe(0, 0));
}

static void test_lifecycle_operational_policy(void) {
    TEST_CHECK(chat_protocol_lifecycle_operational(1, 1, 0));
    TEST_CHECK(!chat_protocol_lifecycle_operational(0, 1, 0));
    TEST_CHECK(!chat_protocol_lifecycle_operational(1, 0, 0));
    TEST_CHECK(!chat_protocol_lifecycle_operational(1, 1, 1));
}

TEST_LIST = {
    { "attached_resolution", test_attached_resolution },
    { "generation_reservation", test_generation_reservation },
    { "retry_policy", test_retry_policy },
    { "preattachment_failure_preserves_safe_binding", test_preattachment_failure_preserves_safe_binding },
    { "lifecycle_operational_policy", test_lifecycle_operational_policy },
    { NULL, NULL }
};
