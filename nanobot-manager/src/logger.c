/* Unified logging pipeline: single nanobot-manager.log beside the exe with
 * level + module prefixes, thread-safe writes, size-based rotation to .bak.
 * Replaces the former per-module autostart_log/process_log helpers. On any
 * file failure the logger degrades to a no-op: logging must never take the
 * app down. */
#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#define LOGGER_FILE_NAME   L"nanobot-manager.log"
#define LOGGER_BAK_SUFFIX  L".bak"
#define LOGGER_DEFAULT_MAX_BYTES (512UL * 1024UL)
#define LOGGER_LINE_BODY   1024

/* SRWLOCK needs no initialization (static init), matching the probe-cache
 * pattern in process.c. All mutable state below is guarded by it. */
static SRWLOCK        g_lock = SRWLOCK_INIT;
static FILE*          g_file = NULL;
static nm_log_level   g_level = NM_LOG_INFO;
static unsigned long  g_max_bytes = LOGGER_DEFAULT_MAX_BYTES;
static unsigned long  g_bytes = 0;
static wchar_t        g_path[MAX_PATH] = {0};
static int            g_initialized = 0;

static const char* level_tag(nm_log_level level) {
    switch (level) {
    case NM_LOG_DEBUG: return "DEBUG";
    case NM_LOG_INFO:  return "INFO";
    case NM_LOG_WARN:  return "WARN";
    case NM_LOG_ERROR: return "ERROR";
    default:           return "INFO";
    }
}

const char* logger_level_to_string(nm_log_level level) {
    switch (level) {
    case NM_LOG_DEBUG: return "debug";
    case NM_LOG_INFO:  return "info";
    case NM_LOG_WARN:  return "warn";
    case NM_LOG_ERROR: return "error";
    default:           return "info";
    }
}

nm_log_level logger_level_from_string(const char* s) {
    if (!s) return NM_LOG_INFO;
    if (_stricmp(s, "debug") == 0) return NM_LOG_DEBUG;
    if (_stricmp(s, "info")  == 0) return NM_LOG_INFO;
    if (_stricmp(s, "warn")  == 0 || _stricmp(s, "warning") == 0) return NM_LOG_WARN;
    if (_stricmp(s, "error") == 0) return NM_LOG_ERROR;
    return NM_LOG_INFO;
}

void logger_set_level(nm_log_level level) {
    AcquireSRWLockExclusive(&g_lock);
    g_level = level;
    ReleaseSRWLockExclusive(&g_lock);
}

void logger_set_max_bytes(unsigned long bytes) {
    AcquireSRWLockExclusive(&g_lock);
    g_max_bytes = bytes ? bytes : LOGGER_DEFAULT_MAX_BYTES;
    ReleaseSRWLockExclusive(&g_lock);
}

static void close_locked(void) {
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
}

static unsigned long query_file_size(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER sz;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        return 0;
    sz.LowPart  = fad.nFileSizeLow;
    sz.HighPart = fad.nFileSizeHigh;
    return (unsigned long)sz.QuadPart;
}

/* Caller holds g_lock. Closes, renames the log to .bak, reopens fresh.
 * Returns the new FILE* or NULL (no-op degradation). */
static FILE* rotate_locked(void) {
    wchar_t bak[MAX_PATH];
    close_locked();
    if (g_path[0]) {
        if (swprintf_s(bak, _countof(bak), L"%s%s", g_path, LOGGER_BAK_SUFFIX) > 0)
            MoveFileExW(g_path, bak, MOVEFILE_REPLACE_EXISTING);
        g_file = _wfopen(g_path, L"w");
    }
    g_bytes = 0;
    return g_file;
}

int logger_init(const wchar_t* dir, nm_log_level level) {
    wchar_t path[MAX_PATH];

    if (dir && *dir) {
        if (swprintf_s(path, _countof(path), L"%s\\%s", dir, LOGGER_FILE_NAME) <= 0)
            return -1;
    } else {
        wchar_t exe[MAX_PATH];
        wchar_t* slash;
        if (!GetModuleFileNameW(NULL, exe, _countof(exe)))
            return -1;
        slash = wcsrchr(exe, L'\\');
        if (!slash) return -1;
        *slash = L'\0';
        if (swprintf_s(path, _countof(path), L"%s\\%s", exe, LOGGER_FILE_NAME) <= 0)
            return -1;
    }

    AcquireSRWLockExclusive(&g_lock);
    close_locked();
    wcscpy_s(g_path, _countof(g_path), path);
    g_level = level;
    g_bytes = query_file_size(path);
    g_file = _wfopen(path, L"a");
    g_initialized = (g_file != NULL);
    ReleaseSRWLockExclusive(&g_lock);
    return g_file ? 0 : -1;
}

void logger_shutdown(void) {
    AcquireSRWLockExclusive(&g_lock);
    close_locked();
    g_path[0] = L'\0';
    g_bytes = 0;
    g_initialized = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void nm_log(nm_log_level level, const char* module, const char* fmt, ...) {
    SYSTEMTIME st;
    char body[LOGGER_LINE_BODY];
    char line[LOGGER_LINE_BODY + 96];
    va_list ap;

    AcquireSRWLockShared(&g_lock);
    if (!g_file || level < g_level) {
        ReleaseSRWLockShared(&g_lock);
        return;
    }
    ReleaseSRWLockShared(&g_lock);

    GetLocalTime(&st);
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = '\0';

    snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s] %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             level_tag(level), module ? module : "-", body);
    line[sizeof(line) - 1] = '\0';

    AcquireSRWLockExclusive(&g_lock);
    if (g_file && level >= g_level) {
        if (g_bytes > g_max_bytes)
            rotate_locked();
        if (g_file) {
            size_t len = strlen(line);
            fputs(line, g_file);
            fflush(g_file);
            g_bytes += (unsigned long)len;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    (void)g_initialized;
}
