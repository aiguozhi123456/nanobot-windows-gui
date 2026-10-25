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
    TEST_CHECK(error == ERROR_SUCCESS);   /* absent is not an error */
    TEST_CHECK(credential_store_for_target(target, "test-token_123", &error) == 0);
    TEST_CHECK(error == ERROR_SUCCESS);
    TEST_CHECK(credential_load_for_target(target, value, sizeof(value), &error) == 1);
    TEST_CHECK(error == ERROR_SUCCESS);
    TEST_CHECK(strcmp(value, "test-token_123") == 0);
    SecureZeroMemory(value, sizeof(value));
    TEST_CHECK(credential_store_for_target(target, "replacement-token_456", &error) == 0);
    TEST_CHECK(error == ERROR_SUCCESS);
    TEST_CHECK(credential_load_for_target(target, value, sizeof(value), &error) == 1);
    TEST_CHECK(strcmp(value, "replacement-token_456") == 0);
    SecureZeroMemory(value, sizeof(value));
    TEST_CHECK(credential_delete_for_target(target, &error) == 0);
    TEST_CHECK(error == ERROR_SUCCESS);
    TEST_CHECK(credential_load_for_target(target, value, sizeof(value), &error) == 0);
}

/* Validation and boundary paths: error codes must propagate and bad input
 * must never leave a value behind. */
static void test_credential_validation(void) {
    char target[160];
    char value[128] = {0};
    DWORD error = 0;
    snprintf(target, sizeof(target), "nanobot-manager/test/%lu", (unsigned long)GetCurrentProcessId());
    credential_delete_for_target(target, &error);

    /* store rejects NULL / empty / over-length secrets. */
    TEST_CHECK(credential_store_for_target(target, NULL, &error) == -1);
    TEST_CHECK(error == ERROR_INVALID_PARAMETER);
    TEST_CHECK(credential_store_for_target(target, "", &error) == -1);
    TEST_CHECK(error == ERROR_INVALID_PARAMETER);
    {
        static char too_long[CREDENTIAL_SECRET_MAX + 2];
        memset(too_long, 'x', sizeof(too_long) - 1);
        too_long[sizeof(too_long) - 1] = '\0';
        TEST_CHECK(credential_store_for_target(target, too_long, &error) == -1);
        TEST_CHECK(error == ERROR_INVALID_PARAMETER);
    }
    TEST_CHECK(credential_load_for_target(target, value, sizeof(value), &error) == 0);
    TEST_CHECK(error == ERROR_SUCCESS);   /* rejected stores wrote nothing */

    /* load rejects a caller buffer smaller than the stored secret. */
    TEST_CHECK(credential_store_for_target(target, "test-token_123", &error) == 0);
    {
        char small[8];
        TEST_CHECK(credential_load_for_target(target, small, sizeof(small), &error) == -1);
        TEST_CHECK(error == ERROR_INSUFFICIENT_BUFFER);
    }

    TEST_CHECK(credential_delete_for_target(target, &error) == 0);
    TEST_CHECK(error == ERROR_SUCCESS);
}

TEST_LIST = {
    { "credential_roundtrip", test_credential_roundtrip },
    { "credential_validation", test_credential_validation },
    { NULL, NULL }
};
