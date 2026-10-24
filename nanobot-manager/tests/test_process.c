#include "nm_test.h"
#include "process.h"
#include <stdio.h>
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

static wchar_t* g_venv_saved_value;
static int      g_venv_was_set;

static void venv_env_save(void) {
    DWORD need = GetEnvironmentVariableW(L"NANOBOT_VENV", NULL, 0);
    g_venv_was_set = 0;
    if (need == 0) return;                 /* unset (or empty == unset)        */
    g_venv_saved_value = (wchar_t*)malloc((size_t)need * sizeof(wchar_t));
    if (!g_venv_saved_value) return;
    g_venv_was_set =
        GetEnvironmentVariableW(L"NANOBOT_VENV", g_venv_saved_value, need)
        == need - 1;                       /* chars copied, excl. NUL          */
}

static void venv_env_restore(void) {
    if (g_venv_was_set)
        SetEnvironmentVariableW(L"NANOBOT_VENV", g_venv_saved_value);
    else
        SetEnvironmentVariableW(L"NANOBOT_VENV", NULL);
    free(g_venv_saved_value);
    g_venv_saved_value = NULL;
    g_venv_was_set = 0;
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

/* Index of the first candidate with the given source, or -1. */
static int first_index_with_source(const nm_source* sources, int count,
                                   nm_source want) {
    for (int i = 0; i < count; i++)
        if (sources[i] == want) return i;
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
    if (!make_temp_venv(dir, _countof(dir), L"nb")) {
        TEST_CHECK(0 && "fixture setup failed");
        venv_env_restore();
        return;
    }
    if (!create_stub(dir, L"Scripts\\nanobot.exe")) {
        TEST_CHECK(0 && "fixture setup failed");
        venv_env_restore();
        remove_temp_venv(dir);             /* don't leak the empty temp tree   */
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
            /* Documented precedence: config < PATH nanobot.exe < managed venv
             * < PATH python (asserted only for tiers present on this machine). */
            int path_idx = first_index_with_source(sources, count, NM_SRC_PATH_NANOBOT);
            int py_idx   = first_index_with_source(sources, count, NM_SRC_PYTHON);
            if (path_idx >= 0) TEST_CHECK(path_idx < idx);
            if (py_idx >= 0)   TEST_CHECK(idx < py_idx);
        }
    }

    nm_candidates_free(exes, is_py, sources, count);
    venv_env_restore();
    remove_temp_venv(dir);
}

static void test_candidates_managed_venv_python_only(void) {
    wchar_t dir[MAX_PATH];
    venv_env_save();
    if (!make_temp_venv(dir, _countof(dir), L"py")) {
        TEST_CHECK(0 && "fixture setup failed");
        venv_env_restore();
        return;
    }
    if (!create_stub(dir, L"Scripts\\python.exe")) {
        TEST_CHECK(0 && "fixture setup failed");
        venv_env_restore();
        remove_temp_venv(dir);             /* don't leak the empty temp tree   */
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
            /* Documented precedence: config < PATH nanobot.exe < managed venv
             * < PATH python (asserted only for tiers present on this machine). */
            int path_idx = first_index_with_source(sources, count, NM_SRC_PATH_NANOBOT);
            int py_idx   = first_index_with_source(sources, count, NM_SRC_PYTHON);
            if (path_idx >= 0) TEST_CHECK(path_idx < idx);
            if (py_idx >= 0)   TEST_CHECK(idx < py_idx);
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

static void test_candidates_managed_venv_empty_override_equals_unset(void) {
    /* install.ps1 treats %NANOBOT_VENV% as set only when non-empty; an empty
     * value must probe exactly the same candidates as an unset one. Compared
     * head-to-head so the assertion holds on any machine (a real
     * ~\.nanobot\venv shows up identically in both runs). */
    venv_env_save();
    SetEnvironmentVariableW(L"NANOBOT_VENV", L"");

    char** exes_a = NULL; int* is_py_a = NULL; nm_source* src_a = NULL; int count_a = 0;
    nanobot_resolve_candidates(NULL, &exes_a, &is_py_a, &src_a, &count_a);

    SetEnvironmentVariableW(L"NANOBOT_VENV", NULL);

    char** exes_b = NULL; int* is_py_b = NULL; nm_source* src_b = NULL; int count_b = 0;
    nanobot_resolve_candidates(NULL, &exes_b, &is_py_b, &src_b, &count_b);

    TEST_CHECK(count_a == count_b);
    if (count_a == count_b) {
        for (int i = 0; i < count_a; i++) {
            TEST_CHECK(_stricmp(exes_a[i], exes_b[i]) == 0);
            TEST_CHECK(is_py_a[i] == is_py_b[i]);
            TEST_CHECK(src_a[i] == src_b[i]);
        }
    }

    nm_candidates_free(exes_a, is_py_a, src_a, count_a);
    nm_candidates_free(exes_b, is_py_b, src_b, count_b);
    venv_env_restore();
}

/* --- WebUI endpoint policy (pure parser: port + bootstrap secret) ---------- */

static void test_config_webui_info_reads_port_and_secret(void) {
    int port = 0;
    char secret[128] = {0};
    TEST_CHECK(nanobot_config_webui_info(
        "{\"channels\":{\"websocket\":{\"enabled\":true,\"host\":\"127.0.0.1\","
        "\"port\":18791,\"path\":\"/ws\",\"token\":\"  tok  \"}}}",
        &port, secret, sizeof(secret)) == 1);
    TEST_CHECK(port == 18791);
    TEST_CHECK(strcmp(secret, "tok") == 0);      /* trimmed */

    /* tokenIssueSecret wins over token; enabled omitted counts as on. */
    port = 0; secret[0] = '\0';
    TEST_CHECK(nanobot_config_webui_info(
        "{\"channels\":{\"websocket\":{\"port\":9001,"
        "\"token\":\"tok\",\"tokenIssueSecret\":\"issue\"}}}",
        &port, secret, sizeof(secret)) == 1);
    TEST_CHECK(port == 9001);
    TEST_CHECK(strcmp(secret, "issue") == 0);

    /* Over-long secret truncates to the 127+NUL consumer cap. */
    char long_json[512];
    snprintf(long_json, sizeof(long_json),
             "{\"channels\":{\"websocket\":{\"port\":1,\"token\":\"%0*d\"}}}",
             200, 0);
    port = 0; secret[0] = '\0';
    TEST_CHECK(nanobot_config_webui_info(long_json, &port, secret,
                                         sizeof(secret)) == 1);
    TEST_CHECK(port == 1);
    TEST_CHECK(strlen(secret) == 127);
}

static void test_config_webui_info_rejects_unusable_values(void) {
    int port = -1;
    char secret[128] = "x";
    /* No websocket channel at all. */
    TEST_CHECK(nanobot_config_webui_info("{}", &port, secret, sizeof(secret)) == 0);
    TEST_CHECK(port == 0);
    TEST_CHECK(secret[0] == '\0');
    /* Explicitly disabled channel serves no WebUI: no port, no secret. */
    port = -1; strcpy(secret, "x");
    TEST_CHECK(nanobot_config_webui_info(
        "{\"channels\":{\"websocket\":{\"enabled\":false,\"port\":1,\"token\":\"t\"}}}",
        &port, secret, sizeof(secret)) == 0);
    TEST_CHECK(port == 0);
    TEST_CHECK(secret[0] == '\0');
    /* Out-of-range port still yields a usable secret. */
    port = -1; secret[0] = '\0';
    TEST_CHECK(nanobot_config_webui_info(
        "{\"channels\":{\"websocket\":{\"port\":65536,\"token\":\"t\"}}}",
        &port, secret, sizeof(secret)) == 1);
    TEST_CHECK(port == 0);
    TEST_CHECK(strcmp(secret, "t") == 0);
    /* Whitespace-only secret yields an empty one. */
    port = 0; secret[0] = '\0';
    TEST_CHECK(nanobot_config_webui_info(
        "{\"channels\":{\"websocket\":{\"port\":8765,\"token\":\"   \"}}}",
        &port, secret, sizeof(secret)) == 1);
    TEST_CHECK(port == 8765);
    TEST_CHECK(secret[0] == '\0');
    /* Malformed / empty / NULL input; NULL out params are safe. */
    TEST_CHECK(nanobot_config_webui_info("not json", &port, NULL, 0) == 0);
    TEST_CHECK(nanobot_config_webui_info("", &port, NULL, 0) == 0);
    TEST_CHECK(nanobot_config_webui_info(NULL, &port, NULL, 0) == 0);
    port = -1;
    TEST_CHECK(nanobot_config_webui_info(
        "{\"channels\":{\"websocket\":{\"port\":1234}}}", NULL, NULL, 0) == 1);
    TEST_CHECK(port == -1);                      /* untouched when out_port NULL */
}

TEST_LIST = {
    { "owned_candidate_requires_all_evidence", test_owned_candidate_requires_all_evidence },
    { "owned_candidate_must_not_predate_launcher", test_owned_candidate_must_not_predate_launcher },
    { "candidates_managed_venv_nanobot_exe", test_candidates_managed_venv_nanobot_exe },
    { "candidates_managed_venv_python_only", test_candidates_managed_venv_python_only },
    { "candidates_managed_venv_absent", test_candidates_managed_venv_absent },
    { "candidates_managed_venv_empty_override_equals_unset", test_candidates_managed_venv_empty_override_equals_unset },
    { "config_webui_info_reads_port_and_secret", test_config_webui_info_reads_port_and_secret },
    { "config_webui_info_rejects_unusable_values", test_config_webui_info_rejects_unusable_values },
    { NULL, NULL }
};
