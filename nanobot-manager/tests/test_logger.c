/* logger: file pipeline tests (level filter, line format, rotation,
 * concurrency) against a per-run temp directory. */
#include "nm_test.h"
#include "logger.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wchar_t g_dir[MAX_PATH];

static void make_temp_dir(void) {
    wchar_t base[MAX_PATH];
    wchar_t candidate[MAX_PATH];
    for (int attempt = 0; attempt < 100; attempt++) {
        if (!GetTempPathW(_countof(base), base)) return;
        swprintf_s(candidate, _countof(candidate), L"%snm-log-test-%d-%d",
                   base, (int)GetCurrentProcessId(), attempt);
        if (CreateDirectoryW(candidate, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
            wcscpy_s(g_dir, _countof(g_dir), candidate);
            return;
        }
    }
    g_dir[0] = L'\0';
}

static void remove_temp_dir(void) {
    wchar_t log_path[MAX_PATH];
    wchar_t bak_path[MAX_PATH];
    if (!g_dir[0]) return;
    swprintf_s(log_path, _countof(log_path), L"%s\\nanobot-manager.log", g_dir);
    swprintf_s(bak_path, _countof(bak_path), L"%s\\nanobot-manager.log.bak", g_dir);
    DeleteFileW(log_path);
    DeleteFileW(bak_path);
    RemoveDirectoryW(g_dir);
}

/* Reads the whole log (or .bak when bak != 0) into out; returns length. */
static size_t read_log_file(int bak, char* out, size_t cap) {
    wchar_t path[MAX_PATH];
    FILE* f;
    size_t n = 0;
    swprintf_s(path, _countof(path), L"%s\\nanobot-manager.log%s", g_dir,
               bak ? L".bak" : L"");
    out[0] = '\0';
    f = _wfopen(path, L"rb");
    if (!f) return 0;
    n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return n;
}

static size_t count_lines(const char* text) {
    size_t lines = 0;
    for (const char* p = text; *p; p++)
        if (*p == '\n') lines++;
    return lines;
}

static void test_level_strings(void) {
    TEST_CHECK(logger_level_from_string("debug") == NM_LOG_DEBUG);
    TEST_CHECK(logger_level_from_string("INFO") == NM_LOG_INFO);
    TEST_CHECK(logger_level_from_string("warn") == NM_LOG_WARN);
    TEST_CHECK(logger_level_from_string("warning") == NM_LOG_WARN);
    TEST_CHECK(logger_level_from_string("error") == NM_LOG_ERROR);
    TEST_CHECK(logger_level_from_string("bogus") == NM_LOG_INFO);
    TEST_CHECK(logger_level_from_string(NULL) == NM_LOG_INFO);
    TEST_CHECK(strcmp(logger_level_to_string(NM_LOG_DEBUG), "debug") == 0);
    TEST_CHECK(strcmp(logger_level_to_string(NM_LOG_ERROR), "error") == 0);
}

static void test_noop_before_init(void) {
    /* Never crashes, never writes anywhere observable. */
    nm_log(NM_LOG_ERROR, "test", "before init");
    TEST_CHECK(1);
}

static void test_line_format_and_filtering(void) {
    char text[8192];
    make_temp_dir();
    TEST_CHECK(g_dir[0] != L'\0');
    TEST_CHECK(logger_init(g_dir, NM_LOG_INFO) == 0);

    nm_log(NM_LOG_DEBUG, "proc", "hidden debug %d", 1);
    nm_log(NM_LOG_INFO,  "proc", "hello %d", 42);
    nm_log(NM_LOG_ERROR, NULL, "no module");

    read_log_file(0, text, sizeof(text));
    TEST_CHECK(strstr(text, "[INFO] [proc] hello 42") != NULL);
    TEST_CHECK(strstr(text, "[ERROR] [-] no module") != NULL);
    TEST_CHECK(strstr(text, "[DEBUG]") == NULL);          /* filtered at INFO */
    TEST_CHECK(count_lines(text) == 2);

    /* Runtime level switch enables DEBUG. */
    logger_set_level(NM_LOG_DEBUG);
    nm_log(NM_LOG_DEBUG, "proc", "visible debug");
    read_log_file(0, text, sizeof(text));
    TEST_CHECK(strstr(text, "[DEBUG] [proc] visible debug") != NULL);
    TEST_CHECK(count_lines(text) == 3);

    logger_shutdown();
    remove_temp_dir();
}

static void test_rotation_preserves_lines(void) {
    char text[8192];
    const int total = 10;
    make_temp_dir();
    TEST_CHECK(g_dir[0] != L'\0');
    TEST_CHECK(logger_init(g_dir, NM_LOG_INFO) == 0);
    logger_set_max_bytes(500);                 /* ~6 lines of ~90 bytes */

    for (int i = 0; i < total; i++)
        nm_log(NM_LOG_INFO, "rot", "line-%02d padding padding padding padding", i);

    {
        size_t n_log  = read_log_file(0, text, sizeof(text));
        size_t lines_log = count_lines(text);
        char bak[8192];
        size_t n_bak  = read_log_file(1, bak, sizeof(bak));
        size_t lines_bak = count_lines(bak);
        TEST_CHECK(n_bak > 0);                 /* rotation happened */
        TEST_CHECK(lines_log + lines_bak == (size_t)total); /* nothing lost */
        TEST_CHECK(n_log > 0);                 /* active log still writable */
    }

    logger_set_max_bytes(512 * 1024);          /* restore default for others */
    logger_shutdown();
    remove_temp_dir();
}

typedef struct { int id; } thread_arg;

static DWORD WINAPI writer_thread(LPVOID param) {
    thread_arg* arg = (thread_arg*)param;
    for (int i = 0; i < 50; i++)
        nm_log(NM_LOG_INFO, "conc", "t%d-%03d", arg->id, i);
    return 0;
}

static void test_concurrent_writers(void) {
    char text[65536];
    HANDLE threads[4];
    thread_arg args[4];
    make_temp_dir();
    TEST_CHECK(g_dir[0] != L'\0');
    TEST_CHECK(logger_init(g_dir, NM_LOG_INFO) == 0);

    for (int i = 0; i < 4; i++) {
        args[i].id = i;
        threads[i] = CreateThread(NULL, 0, writer_thread, &args[i], 0, NULL);
    }
    for (int i = 0; i < 4; i++) {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }

    read_log_file(0, text, sizeof(text));
    TEST_CHECK(count_lines(text) == 4 * 50);   /* no lost lines */
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < 50; i += 17) {     /* spot-check every thread */
            char needle[32];
            snprintf(needle, sizeof(needle), "[INFO] [conc] t%d-%03d", t, i);
            if (!strstr(text, needle)) {
                TEST_CHECK(0);
                TEST_MSG("missing %s", needle);
            }
        }
    }

    logger_shutdown();
    remove_temp_dir();
}

TEST_LIST = {
    { "level_strings", test_level_strings },
    { "noop_before_init", test_noop_before_init },
    { "line_format_and_filtering", test_line_format_and_filtering },
    { "rotation_preserves_lines", test_rotation_preserves_lines },
    { "concurrent_writers", test_concurrent_writers },
    { NULL, NULL }
};
