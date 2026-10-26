/* Unified logging pipeline: single nanobot-manager.log beside the exe with
 * level + module prefixes, thread-safe writes, size-based rotation to .bak.
 * Replaces the former per-module autostart_log/process_log helpers. The log
 * is opened in binary mode so g_bytes matches the on-disk size (LF endings,
 * no CRLF translation). On any file failure the logger degrades to a no-op
 * and retries the open on the next call: logging must never take the app
 * down. */
#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>   /* _stricmp, strlen */
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
static int            g_initialized = 0;   /* logger_init set a path; gates lazy reopen */

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
    /* unsigned long is 32-bit on Windows (LLP64): clamp instead of wrapping. */
    if (sz.QuadPart > 0xFFFFFFFFUL)
        return 0xFFFFFFFFUL;
    return (unsigned long)sz.QuadPart;
}

/* Caller holds g_lock. Closes, renames the log to .bak, reopens fresh. When
 * the rename fails (the .bak is held open by a viewer without
 * FILE_SHARE_DELETE, path too long for the .bak name), the log is reopened
 * in APPEND mode so no prior entry is lost. Returns the new FILE* or NULL. */
static FILE* rotate_locked(void) {
    wchar_t bak[MAX_PATH];
    int rotated = 0;
    close_locked();
    if (g_path[0]) {
        if (wcslen(g_path) + wcslen(LOGGER_BAK_SUFFIX) < _countof(bak) &&
            swprintf_s(bak, _countof(bak), L"%s%s", g_path, LOGGER_BAK_SUFFIX) > 0) {
            rotated = (MoveFileExW(g_path, bak, MOVEFILE_REPLACE_EXISTING) != 0);
        }
        /* Truncate only when the old content was actually moved aside. */
        g_file = _wfopen(g_path, rotated ? L"wb" : L"ab");
    }
    g_bytes = query_file_size(g_path);
    return g_file;
}

/* Truncation by vsnprintf/snprintf can split a UTF-8 multi-byte sequence: if
 * the first length bytes end with a lead byte or a dangling chain of
 * continuation bytes ((c & 0xC0) == 0x80), back up (at most 3 bytes) and drop
 * the incomplete tail so logged text stays valid UTF-8. NUL-terminates at the
 * cut when trimming; returns the resulting length. No-op for complete tails. */
static size_t utf8_trim_incomplete_tail(char* s, size_t length) {
    size_t end = length;
    unsigned int continuations = 0;
    unsigned char lead;
    size_t expected;

    if (!s || length == 0) return 0;
    while (end > 0 && continuations < 3 && ((unsigned char)s[end - 1] & 0xC0) == 0x80) {
        end--;
        continuations++;
    }
    if (end == 0) {   /* nothing but continuation bytes: all dangling */
        s[0] = '\0';
        return 0;
    }
    lead = (unsigned char)s[end - 1];
    expected = (lead >= 0xC2 && lead <= 0xDF) ? 2 :
               (lead >= 0xE0 && lead <= 0xEF) ? 3 :
               (lead >= 0xF0 && lead <= 0xF4) ? 4 : 0;
    if (expected == 0) {
        /* Not a lead byte (ASCII or invalid): any continuation bytes walked
         * over are a dangling chain without their lead. */
        if (continuations) {
            s[end] = '\0';
            return end;
        }
        return length;
    }
    if (continuations + 1 < expected) {   /* lead byte missing continuations */
        end--;
        s[end] = '\0';
        return end;
    }
    return length;   /* trailing sequence is complete: nothing to trim */
}

int logger_init(const wchar_t* dir, nm_log_level level) {
    wchar_t path[MAX_PATH];

    if (dir && *dir) {
        /* Pre-check instead of trusting swprintf_s to fail: on overflow it
         * invokes the CRT invalid-parameter handler and kills the process. */
        if (wcslen(dir) + 1 + wcslen(LOGGER_FILE_NAME) >= _countof(path))
            return -1;
        swprintf_s(path, _countof(path), L"%s\\%s", dir, LOGGER_FILE_NAME);
    } else {
        wchar_t exe[MAX_PATH];
        wchar_t* slash;
        if (!GetModuleFileNameW(NULL, exe, _countof(exe)))
            return -1;
        slash = wcsrchr(exe, L'\\');
        if (!slash) return -1;
        *slash = L'\0';
        if (wcslen(exe) + 1 + wcslen(LOGGER_FILE_NAME) >= _countof(path))
            return -1;
        swprintf_s(path, _countof(path), L"%s\\%s", exe, LOGGER_FILE_NAME);
    }

    AcquireSRWLockExclusive(&g_lock);
    close_locked();
    wcscpy_s(g_path, _countof(g_path), path);
    g_level = level;
    g_bytes = query_file_size(path);
    g_file = _wfopen(path, L"ab");
    g_initialized = 1;   /* path is set: nm_log may lazily retry the open */
    ReleaseSRWLockExclusive(&g_lock);
    return g_file ? 0 : -1;
}

void logger_shutdown(void) {
    AcquireSRWLockExclusive(&g_lock);
    close_locked();
    g_path[0] = L'\0';
    g_bytes = 0;
    g_max_bytes = LOGGER_DEFAULT_MAX_BYTES;   /* a re-init must not inherit a test override */
    g_initialized = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void nm_log(nm_log_level level, const char* module, const char* fmt, ...) {
    SYSTEMTIME st;
    char body[LOGGER_LINE_BODY];
    char line[LOGGER_LINE_BODY + 96];
    va_list ap;

    if (!fmt) return;   /* vsnprintf(NULL, ...) is UB; guard like module below */

    AcquireSRWLockShared(&g_lock);
    int active = (g_file || (g_initialized && g_path[0])) && level >= g_level;
    ReleaseSRWLockShared(&g_lock);
    if (!active) return;

    GetLocalTime(&st);
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = '\0';
    /* A body-level truncation can split a UTF-8 sequence: drop the dangling
     * tail (no-op when the body ends on a complete sequence). */
    utf8_trim_incomplete_tail(body, strlen(body));
    /* Enforce the single-line contract even for caller-supplied %s text
     * (e.g. the gateway state-file identity read in process.c). */
    for (char* p = body; *p; p++) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }

    int n = snprintf(line, sizeof(line),
             "[%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s] %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             level_tag(level), module ? module : "-", body);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) {
        /* Truncated: mark the cut so the loss is visible, still one line.
         * Trim a split UTF-8 tail first so "..." does not land mid-sequence. */
        size_t kept = utf8_trim_incomplete_tail(line, sizeof(line) - 5);
        memcpy(&line[kept], "...\n", 5);   /* 4 chars + NUL */
    }

    AcquireSRWLockExclusive(&g_lock);
    if ((g_file || (g_initialized && g_path[0])) && level >= g_level) {
        /* Self-heal after a lost handle (failed rotation reopen, failed
         * write): retry the open instead of no-op-ing forever. */
        if (!g_file && g_path[0])
            g_file = _wfopen(g_path, L"ab");
        if (g_file && g_bytes > g_max_bytes)
            rotate_locked();
        if (g_file) {
            size_t len = strlen(line);
            if (fputs(line, g_file) >= 0 && fflush(g_file) == 0)
                g_bytes += (unsigned long)len;
            else
                close_locked();   /* next nm_log retries the open */
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}
