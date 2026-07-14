#include "nm_test.h"
#include "credential.h"

#include <stdio.h>
#include <string.h>

static void test_credential_roundtrip(void) {
    char target[160];
    char value[128] = {0};
    DWORD error = ERROR_SUCCESS;
    snprintf(target, sizeof(target), "nanobot-manager/test/%lu", (unsigned long)GetCurrentProcessId());
    credential_delete_for_target(target, &error);
    TEST_CHECK(credential_load_for_target(target, value, sizeof(value), &error) == 0);
    TEST_CHECK(credential_store_for_target(target, "test-token_123", &error) == 0);
    TEST_CHECK(credential_load_for_target(target, value, sizeof(value), &error) == 1);
    TEST_CHECK(strcmp(value, "test-token_123") == 0);
    SecureZeroMemory(value, sizeof(value));
    TEST_CHECK(credential_store_for_target(target, "replacement-token_456", &error) == 0);
    TEST_CHECK(credential_load_for_target(target, value, sizeof(value), &error) == 1);
    TEST_CHECK(strcmp(value, "replacement-token_456") == 0);
    SecureZeroMemory(value, sizeof(value));
    TEST_CHECK(credential_delete_for_target(target, &error) == 0);
    TEST_CHECK(credential_load_for_target(target, value, sizeof(value), &error) == 0);
}

TEST_LIST = {
    { "credential_roundtrip", test_credential_roundtrip },
    { NULL, NULL }
};
