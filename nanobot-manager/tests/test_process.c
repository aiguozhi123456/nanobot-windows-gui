#include "nm_test.h"
#include "process.h"

static FILETIME filetime_from_ull(unsigned long long value) {
    ULARGE_INTEGER parts;
    FILETIME result;
    parts.QuadPart = value;
    result.dwLowDateTime = parts.LowPart;
    result.dwHighDateTime = parts.HighPart;
    return result;
}

static void test_owned_candidate_requires_all_evidence(void) {
    FILETIME launcher_started = filetime_from_ull(1000);
    FILETIME candidate_created = filetime_from_ull(1000);

    TEST_CHECK(nanobot_unverified_candidate_is_owned(
        WAIT_OBJECT_0, 0, 200, 0, 0, 1, candidate_created, launcher_started));
    TEST_CHECK(!nanobot_unverified_candidate_is_owned(
        WAIT_TIMEOUT, 0, 200, 0, 0, 1, candidate_created, launcher_started));
    TEST_CHECK(!nanobot_unverified_candidate_is_owned(
        WAIT_OBJECT_0, 1, 200, 0, 0, 1, candidate_created, launcher_started));
    TEST_CHECK(!nanobot_unverified_candidate_is_owned(
        WAIT_OBJECT_0, 0, 0, 0, 0, 1, candidate_created, launcher_started));
    TEST_CHECK(!nanobot_unverified_candidate_is_owned(
        WAIT_OBJECT_0, 0, 200, 1, 200, 1, candidate_created, launcher_started));
    TEST_CHECK(!nanobot_unverified_candidate_is_owned(
        WAIT_OBJECT_0, 0, 200, 0, 0, 0, candidate_created, launcher_started));
}

static void test_owned_candidate_must_not_predate_launcher(void) {
    FILETIME launcher_started = filetime_from_ull(1000);
    FILETIME before_launcher = filetime_from_ull(999);
    FILETIME after_launcher = filetime_from_ull(1001);

    TEST_CHECK(!nanobot_unverified_candidate_is_owned(
        WAIT_OBJECT_0, 0, 201, 0, 0, 1, before_launcher, launcher_started));
    TEST_CHECK(nanobot_unverified_candidate_is_owned(
        WAIT_OBJECT_0, 0, 201, 1, 200, 1, after_launcher, launcher_started));
}

TEST_LIST = {
    { "owned_candidate_requires_all_evidence", test_owned_candidate_requires_all_evidence },
    { "owned_candidate_must_not_predate_launcher", test_owned_candidate_must_not_predate_launcher },
    { NULL, NULL }
};
