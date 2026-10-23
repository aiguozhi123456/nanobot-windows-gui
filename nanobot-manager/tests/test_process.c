#include "nm_test.h"
#include "process.h"
#include <stdlib.h>   /* _countof */
#include <string.h>

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

/* --- Managed-venv tier (one-click installer form) ----------------------------
 * nanobot's one-click installer (scripts/install.ps1) creates an isolated venv
 * at ~\.nanobot\venv without touching PATH, so the builder probes that
 * canonical location. It resolves the venv through %NANOBOT_VENV% first --
 * which also makes the tier deterministic in tests: point the env var at a
 * temp dir, lay down stub Scripts\ exes, then restore the original value. */

static wchar_t g_venv_saved_value[MAX_PATH];
static int     g_venv_was_set;

static void venv_env_save(void) {
    DWORD n = GetEnvironmentVariableW(L"NANOBOT_VENV",
                                      g_venv_saved_value, MAX_PATH);
    g_venv_was_set = (n > 0 && n < MAX_PATH);
}

static void venv_env_restore(void) {
    if (g_venv_was_set)
        SetEnvironmentVariableW(L"NANOBOT_VENV", g_venv_saved_value);
    else
        SetEnvironmentVariableW(L"NANOBOT_VENV", NULL);
}

/* Create an empty marker file at <dir>\<rel>; parent dirs must exist. */
static int create_stub(const wchar_t* dir, const wchar_t* rel) {
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, _countof(path), L"%s\\%s", dir, rel) <= 0) return 0;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

/* Build the UTF-8 candidate path the resolver is expected to produce for
 * <dir>\<rel>. Returns 0 on conversion failure. */
static int expected_utf8(const wchar_t* dir, const wchar_t* rel,
                         char* out, size_t cap) {
    wchar_t wide[MAX_PATH];
    if (swprintf_s(wide, _countof(wide), L"%s\\%s", dir, rel) <= 0) return 0;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                               out, (int)cap, NULL, NULL) > 0;
}

/* Index of the candidate equal to `expect` (case-insensitive), or -1. */
static int index_of_candidate(char** exes, int count, const char* expect) {
    for (int i = 0; i < count; i++)
        if (_stricmp(exes[i], expect) == 0) return i;
    return -1;
}

/* Create <tmp>\nm-test-venv-<tag> with a Scripts subdir (no exes). */
static int make_temp_venv(wchar_t* dir, size_t cap, const wchar_t* tag) {
    wchar_t tmp[MAX_PATH], scripts[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return 0;
    if (swprintf_s(dir, cap, L"%snm-test-venv-%s", tmp, tag) <= 0) return 0;
    if (swprintf_s(scripts, _countof(scripts), L"%s\\Scripts", dir) <= 0) return 0;
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    if (!CreateDirectoryW(scripts, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    return 1;
}

static void remove_temp_venv(const wchar_t* dir) {
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, _countof(path), L"%s\\Scripts\\nanobot.exe", dir) > 0)
        DeleteFileW(path);
    if (swprintf_s(path, _countof(path), L"%s\\Scripts\\python.exe", dir) > 0)
        DeleteFileW(path);
    if (swprintf_s(path, _countof(path), L"%s\\Scripts", dir) > 0)
        RemoveDirectoryW(path);
    RemoveDirectoryW(dir);
}

static void test_candidates_managed_venv_nanobot_exe(void) {
    wchar_t dir[MAX_PATH];
    venv_env_save();
    if (!make_temp_venv(dir, _countof(dir), L"nb") ||
        !create_stub(dir, L"Scripts\\nanobot.exe")) {
        TEST_CHECK(0 && "fixture setup failed");
        venv_env_restore();
        return;
    }
    SetEnvironmentVariableW(L"NANOBOT_VENV", dir);

    char** exes = NULL;
    int* is_py = NULL;
    nm_source* sources = NULL;
    int count = 0;
    nanobot_resolve_candidates(NULL, &exes, &is_py, &sources, &count);

    char expect[2 * MAX_PATH];
    if (!expected_utf8(dir, L"Scripts\\nanobot.exe", expect, sizeof(expect))) {
        TEST_CHECK(0 && "UTF-8 conversion failed");
    } else {
        int idx = index_of_candidate(exes, count, expect);
        TEST_CHECK(idx >= 0);
        if (idx >= 0) {
            TEST_CHECK(is_py[idx] == 0);
            TEST_CHECK(sources[idx] == NM_SRC_MANAGED_VENV);
        }
    }

    nm_candidates_free(exes, is_py, sources, count);
    venv_env_restore();
    remove_temp_venv(dir);
}

static void test_candidates_managed_venv_python_only(void) {
    wchar_t dir[MAX_PATH];
    venv_env_save();
    if (!make_temp_venv(dir, _countof(dir), L"py") ||
        !create_stub(dir, L"Scripts\\python.exe")) {
        TEST_CHECK(0 && "fixture setup failed");
        venv_env_restore();
        return;
    }
    SetEnvironmentVariableW(L"NANOBOT_VENV", dir);

    char** exes = NULL;
    int* is_py = NULL;
    nm_source* sources = NULL;
    int count = 0;
    nanobot_resolve_candidates(NULL, &exes, &is_py, &sources, &count);

    char expect[2 * MAX_PATH];
    if (!expected_utf8(dir, L"Scripts\\python.exe", expect, sizeof(expect))) {
        TEST_CHECK(0 && "UTF-8 conversion failed");
    } else {
        int idx = index_of_candidate(exes, count, expect);
        TEST_CHECK(idx >= 0);
        if (idx >= 0) {
            TEST_CHECK(is_py[idx] == 1);
            TEST_CHECK(sources[idx] == NM_SRC_MANAGED_VENV);
        }
    }

    nm_candidates_free(exes, is_py, sources, count);
    venv_env_restore();
    remove_temp_venv(dir);
}

static void test_candidates_managed_venv_absent(void) {
    /* NANOBOT_VENV pointing at a location with no venv must contribute no
     * candidates (and, being set, must suppress the default profile path). */
    wchar_t tmp[MAX_PATH], dir[MAX_PATH];
    venv_env_save();
    if (!GetTempPathW(MAX_PATH, tmp) ||
        swprintf_s(dir, _countof(dir), L"%snm-test-venv-missing", tmp) <= 0) {
        TEST_CHECK(0 && "fixture setup failed");
        venv_env_restore();
        return;
    }
    SetEnvironmentVariableW(L"NANOBOT_VENV", dir);

    char** exes = NULL;
    int* is_py = NULL;
    nm_source* sources = NULL;
    int count = 0;
    nanobot_resolve_candidates(NULL, &exes, &is_py, &sources, &count);

    char expect_nb[2 * MAX_PATH], expect_py[2 * MAX_PATH];
    if (!expected_utf8(dir, L"Scripts\\nanobot.exe", expect_nb, sizeof(expect_nb)) ||
        !expected_utf8(dir, L"Scripts\\python.exe", expect_py, sizeof(expect_py))) {
        TEST_CHECK(0 && "UTF-8 conversion failed");
    } else {
        TEST_CHECK(index_of_candidate(exes, count, expect_nb) < 0);
        TEST_CHECK(index_of_candidate(exes, count, expect_py) < 0);
    }

    nm_candidates_free(exes, is_py, sources, count);
    venv_env_restore();
}

TEST_LIST = {
    { "owned_candidate_requires_all_evidence", test_owned_candidate_requires_all_evidence },
    { "owned_candidate_must_not_predate_launcher", test_owned_candidate_must_not_predate_launcher },
    { "candidates_managed_venv_nanobot_exe", test_candidates_managed_venv_nanobot_exe },
    { "candidates_managed_venv_python_only", test_candidates_managed_venv_python_only },
    { "candidates_managed_venv_absent", test_candidates_managed_venv_absent },
    { NULL, NULL }
};
