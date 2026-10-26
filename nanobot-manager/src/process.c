#include "nm_process.h"
#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <shlobj.h>      /* SHGetKnownFolderPath, FOLDERID_Profile, CoTaskMemFree */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "logger.h"

/* --- Python interpreter name set -------------------------------------------
 * Single source of truth for "what counts as a python interpreter". The narrow
 * table drives SearchPathW probing in the candidate builder. The names are
 * version-tolerant by prefix so python3.11.exe, pythonw3.12.exe, ... are
 * recognized; callers reject mismatches via the probe.
 */
static const char* const g_python_exes[] = {
    "py.exe", "python.exe", "python3.exe", "pythonw.exe"
};

static wchar_t* utf8_to_wide_alloc(const char* text) {
    if (!text) return NULL;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wchar_t* wide = (wchar_t*)malloc((size_t)length * sizeof(*wide));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char* wide_to_utf8_alloc(const wchar_t* text) {
    if (!text) return NULL;
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0,
                                     NULL, NULL);
    if (length <= 0) return NULL;
    char* utf8 = (char*)malloc((size_t)length);
    if (!utf8) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, utf8, length,
                             NULL, NULL)) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

static wchar_t* build_command_line(const wchar_t* exe, const wchar_t* arguments) {
    if (!exe || !arguments) return NULL;
    size_t exe_len = wcslen(exe);
    size_t arg_len = wcslen(arguments);
    if (exe_len > (SIZE_MAX - arg_len - 5) / sizeof(wchar_t)) return NULL;
    wchar_t* command = (wchar_t*)malloc((exe_len + arg_len + 5) * sizeof(*command));
    if (!command) return NULL;
    /* Note: the space between the closing quote and `arguments` is required --
     * callers pass the raw args WITHOUT a leading space. The buffer math above
     * (+5) accounts for `"`, `"`, the space, and a margin. */
    if (swprintf_s(command, exe_len + arg_len + 5, L"\"%s\" %s", exe, arguments) < 0) {
        free(command);
        return NULL;
    }
    return command;
}

/* --- Pure candidate builder ------------------------------------------------ */

/* The one-click installer's managed venv location. install.ps1 resolves the
 * venv as %NANOBOT_VENV% when set (non-empty), else ~\.nanobot\venv -- mirror
 * that exactly so we look where the installer actually put things. Returns 0
 * when no usable path could be derived. */
static int managed_venv_dir(wchar_t* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = L'\0';
    /* Unset / empty / longer than the buffer all fall through to the default
     * (a too-long override is unusable for us either way). */
    DWORD n = GetEnvironmentVariableW(L"NANOBOT_VENV", out, (DWORD)cap);
    if (n > 0 && n < cap)
        return 1;

    PWSTR profile = NULL;
    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &profile);
    if (FAILED(hr) || !profile) {
        if (profile) CoTaskMemFree(profile);
        return 0;
    }
    /* _TRUNCATE so an over-long profile path yields -1; plain swprintf_s
     * invokes the CRT invalid-parameter handler (process termination) on
     * overflow instead of returning. */
    int written = _snwprintf_s(out, cap, _TRUNCATE, L"%s\\.nanobot\\venv", profile);
    CoTaskMemFree(profile);                 /* always free, even on truncation-fail */
    return (written > 0) ? 1 : 0;
}

int nanobot_resolve_candidates(const app_config* cfg,
                                char*** out_exes, int** out_is_python,
                                nm_source** out_sources, int* out_count) {
    if (out_count) *out_count = 0;
    if (!out_exes || !out_is_python || !out_sources || !out_count)
        return 0;

    /* At most: 1 (config) + 1 (nanobot.exe on PATH) + 2 (managed venv) +
     * 4 (py/python/python3/pythonw). CAP keeps a small margin on top. */
    const int CAP = 10;
    char**     exes    = (char**)calloc((size_t)CAP, sizeof(char*));
    int*       is_py   = (int*)calloc((size_t)CAP, sizeof(int));
    nm_source* sources = (nm_source*)calloc((size_t)CAP, sizeof(nm_source));
    if (!exes || !is_py || !sources) {
        free(exes); free(is_py); free(sources);
        *out_exes = NULL; *out_is_python = NULL; *out_sources = NULL;
        *out_count = 0;
        return 0;
    }
    int   count = 0;
    wchar_t found[MAX_PATH];

    /* Tier 1: config path, existence-validated (GetFileAttributesW). */
    if (cfg && cfg->nanobot_path && cfg->nanobot_path[0]) {
        wchar_t* wpath = utf8_to_wide_alloc(cfg->nanobot_path);
        if (wpath) {
            DWORD attr = GetFileAttributesW(wpath);
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                const char* base = strrchr(cfg->nanobot_path, '\\');
                if (!base) base = strrchr(cfg->nanobot_path, '/');
                base = base ? base + 1 : cfg->nanobot_path;
                char* configured_exe = _strdup(cfg->nanobot_path);
                if (configured_exe) {
                    exes[count] = configured_exe;
                    is_py[count] = (_stricmp(base, "nanobot.exe") != 0) ? 1 : 0;
                    sources[count] = NM_SRC_CONFIG;
                    count++;
                }
            }
            free(wpath);
        }
    }

    /* Tier 2: nanobot.exe on PATH (trusted). */
    if (count < CAP && SearchPathW(NULL, L"nanobot.exe", NULL, _countof(found), found, NULL)) {
        char* found_utf8 = wide_to_utf8_alloc(found);
        if (found_utf8) {
            exes[count] = found_utf8;
            is_py[count] = 0;
            sources[count] = NM_SRC_PATH_NANOBOT;
            count++;
        }
    }

    /* Tier 3: one-click installer's managed venv. install.ps1 never touches
     * PATH, the registry, or shims, so SearchPathW cannot see it -- probe the
     * canonical location directly. nanobot.exe (wheel console script) first,
     * then python.exe -m nanobot (covers wheels without a console script; the
     * gateway --help probe filters out venvs that lack the module). */
    if (count < CAP) {
        wchar_t venv[MAX_PATH];
        if (managed_venv_dir(venv, _countof(venv))) {
            static const struct {
                const wchar_t* rel;        /* relative to the venv root       */
                int            is_python;
            } venv_entries[] = {
                { L"Scripts\\nanobot.exe", 0 },  /* wheel console script       */
                { L"Scripts\\python.exe",  1 },  /* python -m nanobot fallback */
            };
            for (int i = 0; i < (int)_countof(venv_entries) && count < CAP; i++) {
                wchar_t candidate[MAX_PATH];
                /* _TRUNCATE: a venv path that cannot fit is skipped (-1), not
                 * a CRT constraint violation (see managed_venv_dir). */
                if (_snwprintf_s(candidate, _countof(candidate), _TRUNCATE,
                                 L"%s\\%s", venv, venv_entries[i].rel) < 0)
                    continue;
                DWORD attr = GetFileAttributesW(candidate);
                if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                char* candidate_utf8 = wide_to_utf8_alloc(candidate);
                if (candidate_utf8) {
                    exes[count] = candidate_utf8;
                    is_py[count] = venv_entries[i].is_python;
                    sources[count] = NM_SRC_MANAGED_VENV;
                    count++;
                }
            }
        }
    }

    /* Tier 4: py.exe (PEP 397 launcher) then python{,3,w}.exe on PATH. */
    for (int i = 0; i < (int)_countof(g_python_exes) && count < CAP; i++) {
        wchar_t* name = utf8_to_wide_alloc(g_python_exes[i]);
        if (name && SearchPathW(NULL, name, NULL, _countof(found), found, NULL)) {
            char* found_utf8 = wide_to_utf8_alloc(found);
            if (found_utf8) {
                exes[count] = found_utf8;
                is_py[count] = 1;
                sources[count] = NM_SRC_PYTHON;
                count++;
            }
        }
        free(name);
    }

    *out_exes = exes;
    *out_is_python = is_py;
    *out_sources = sources;
    *out_count = count;
    return count;
}

void nm_candidates_free(char** exes, int* is_python, nm_source* sources, int count) {
    if (exes) {
        for (int i = 0; i < count; i++)
            free(exes[i]);
        free(exes);
    }
    free(is_python);
    free(sources);
}

/* --- Capability probe ------------------------------------------------------- */
typedef enum { PROBE_READY, PROBE_INCOMPATIBLE, PROBE_FAILED } probe_result;
typedef enum { OUTPUT_HAS_TOKEN, OUTPUT_NO_TOKEN, OUTPUT_READ_FAILED } output_result;

static int token_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static output_result output_has_background_token(const wchar_t* path) {
    FILE* file = _wfopen(path, L"rb");
    char previous = '\0';
    int matched = 0;
    int found = 0;
    int byte;
    if (!file) return OUTPUT_READ_FAILED;
    while ((byte = fgetc(file)) != EOF) {
        const char* expected = "--background";
        if ((char)byte == expected[matched]) {
            if (matched == 0 && token_char((unsigned char)previous)) continue;
            matched++;
            if (matched == 12) {
                int next = fgetc(file);
                if (!found)   /* sticky: a later "--background-mode" token must not unfind it */
                    found = next == EOF || !token_char((unsigned char)next);
                if (next != EOF) ungetc(next, file);
                matched = 0;
            }
        } else {
            matched = ((char)byte == '-') ? 1 : 0;
        }
        previous = (char)byte;
    }
    /* Always close before deciding: the short-circuit `ferror() || fclose()`
     * form skips fclose() on a read error, leaking the FILE handle. */
    int read_err = ferror(file);
    if (fclose(file) != 0) read_err = 1;
    if (read_err) return OUTPUT_READ_FAILED;
    return found ? OUTPUT_HAS_TOKEN : OUTPUT_NO_TOKEN;
}

static probe_result run_probe(const char* exe, int is_python) {
    wchar_t directory[MAX_PATH], output_path[MAX_PATH];
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOEXW siex;
    PROCESS_INFORMATION pi;
    HANDLE output = INVALID_HANDLE_VALUE, input = INVALID_HANDLE_VALUE;
    SIZE_T attribute_size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    HANDLE inheritable_handles[2];
    int attributes_initialized = 0;
    probe_result result = PROBE_FAILED;
    output_path[0] = L'\0';
    wchar_t* wide_exe = utf8_to_wide_alloc(exe);
    wchar_t* cmdline = build_command_line(wide_exe,
        is_python ? L"-m nanobot gateway --help" : L"gateway --help");
    if (!wide_exe || !cmdline) {
        free(wide_exe);
        free(cmdline);
        return result;
    }
    if (!GetTempPathW(_countof(directory), directory) ||
        !GetTempFileNameW(directory, L"nmp", 0, output_path)) goto cleanup;
    output = CreateFileW(output_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                         TRUNCATE_EXISTING, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (output == INVALID_HANDLE_VALUE) goto cleanup;
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE) goto cleanup;

    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size);
    if (!attributes || !InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size))
        goto cleanup;
    attributes_initialized = 1;
    inheritable_handles[0] = input;
    inheritable_handles[1] = output;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inheritable_handles, sizeof(inheritable_handles),
                                   NULL, NULL))
        goto cleanup;

    ZeroMemory(&siex, sizeof(siex)); ZeroMemory(&pi, sizeof(pi));
    siex.StartupInfo.cb = sizeof(siex);
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdInput = input;
    siex.StartupInfo.hStdOutput = output;
    siex.StartupInfo.hStdError = output;
    siex.lpAttributeList = attributes;
    if (CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                       NULL, NULL, &siex.StartupInfo, &pi)) {
        CloseHandle(output); output = INVALID_HANDLE_VALUE;
        CloseHandle(input); input = INVALID_HANDLE_VALUE;
        DWORD wait = WaitForSingleObject(pi.hProcess, 10000);
        if (wait == WAIT_TIMEOUT) {
            if (TerminateProcess(pi.hProcess, 1))
                WaitForSingleObject(pi.hProcess, 2000);
        } else if (wait == WAIT_OBJECT_0) {
            DWORD exit_code = 1;
            if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0) {
                output_result output_state = output_has_background_token(output_path);
                if (output_state == OUTPUT_HAS_TOKEN)
                    result = PROBE_READY;
                else if (output_state == OUTPUT_NO_TOKEN)
                    result = PROBE_INCOMPATIBLE;
            }
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
cleanup:
    if (attributes_initialized)
        DeleteProcThreadAttributeList(attributes);
    if (attributes)
        free(attributes);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (output_path[0]) DeleteFileW(output_path);
    free(wide_exe);
    free(cmdline);
    return result;
}

/* --- Probe cache (success-only, session-scoped) ----------------------------
 * A path that probed OK is sticky for the session (python detection is
 * deterministic enough). Failures are NOT cached -- they could be transient
 * (slow import, AV interference). Guarded with an SRWLOCK (no init needed).
 */
#define NM_PROBE_CACHE_MAX 8
static SRWLOCK g_probe_lock  = SRWLOCK_INIT;
static int     g_probe_count = 0;
static char    g_probe_cache[NM_PROBE_CACHE_MAX][MAX_PATH];

static int probe_cache_hit(const char* exe) {
    int hit = 0;
    AcquireSRWLockShared(&g_probe_lock);
    for (int i = 0; i < g_probe_count; i++) {
        if (_stricmp(g_probe_cache[i], exe) == 0) { hit = 1; break; }
    }
    ReleaseSRWLockShared(&g_probe_lock);
    return hit;
}

static void probe_cache_add(const char* exe) {
    AcquireSRWLockExclusive(&g_probe_lock);
    if (g_probe_count < NM_PROBE_CACHE_MAX) {
        strncpy(g_probe_cache[g_probe_count], exe, MAX_PATH - 1);
        g_probe_cache[g_probe_count][MAX_PATH - 1] = '\0';
        g_probe_count++;
    }
    ReleaseSRWLockExclusive(&g_probe_lock);
}

/* --- Resolver -------------------------------------------------------------- */

int nanobot_resolve(const app_config* cfg, nm_resolution* out, int quick) {
    if (!out) return 0;
    out->found = 0; out->exe = NULL; out->is_python = 0; out->source = NM_SRC_NONE;
    out->status = NM_RES_NOT_FOUND;

    if (cfg && cfg->nanobot_path && cfg->nanobot_path[0]) {
        wchar_t configured_path[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, cfg->nanobot_path,
                                 -1, configured_path, _countof(configured_path))) {
            out->status = NM_RES_PROBE_FAILED;
            return 0;
        }
        DWORD configured_attributes = GetFileAttributesW(configured_path);
        if (configured_attributes == INVALID_FILE_ATTRIBUTES ||
            (configured_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            out->status = NM_RES_PROBE_FAILED;
            return 0;
        }
    }

    char**     exes = NULL;
    int*       is_py = NULL;
    nm_source* sources = NULL;
    int    count = 0;
    int    saw_incompatible = 0;
    int    saw_failed = 0;
    nanobot_resolve_candidates(cfg, &exes, &is_py, &sources, &count);

    for (int i = 0; i < count; i++) {
        const char* exe = exes[i];
        int py = is_py[i];
        nm_source src = sources[i];   /* attribution from the candidate builder */

        if (!quick) {
            if (!probe_cache_hit(exe)) {
                probe_result probe = run_probe(exe, py);
                if (probe != PROBE_READY) {
                    if (src == NM_SRC_CONFIG) {
                        out->exe = _strdup(exe); out->is_python = py; out->source = src;
                        out->status = probe == PROBE_INCOMPATIBLE
                            ? NM_RES_INCOMPATIBLE : NM_RES_PROBE_FAILED;
                        nm_candidates_free(exes, is_py, sources, count);
                        return 0;
                    }
                    if (probe == PROBE_INCOMPATIBLE)
                        saw_incompatible = 1;
                    else
                        saw_failed = 1;
                    continue;
                }
                probe_cache_add(exe);    /* sticky success                      */
            }
        }

        out->status = quick ? NM_RES_UNCHECKED : NM_RES_READY;
        out->found = 1;
        out->exe = _strdup(exe);
        out->is_python = py;
        out->source = src;
        nm_candidates_free(exes, is_py, sources, count);
        return 1;
    }

    nm_candidates_free(exes, is_py, sources, count);
    if (saw_incompatible)
        out->status = NM_RES_INCOMPATIBLE;
    else if (saw_failed)
        out->status = NM_RES_PROBE_FAILED;
    return 0;
}

void nm_resolution_free(nm_resolution* r) {
    if (!r) return;
    free(r->exe);
    r->exe = NULL;
    r->found = 0;
    r->is_python = 0;
    r->source = NM_SRC_NONE;
    r->status = NM_RES_NOT_FOUND;
}

int nanobot_check_running(int port) {
    HINTERNET sess = WinHttpOpen(L"nanobot-manager/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return 0;

    WinHttpSetTimeouts(sess, 5000, 3000, 5000, 5000);

    HINTERNET conn = WinHttpConnect(sess, L"localhost", (INTERNET_PORT)port, 0);
    if (!conn) { WinHttpCloseHandle(sess); return 0; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/health",
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return 0; }

    BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    int running = 0;
    if (sent) {
        WinHttpReceiveResponse(req, NULL);
        DWORD sc = 0, sz = sizeof(sc);
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sz,
            WINHTTP_NO_HEADER_INDEX);
        running = (sc == 200) ? 1 : 0;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return running;
}

static unsigned long long filetime_to_ull(FILETIME ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

static BOOL get_process_creation_time(HANDLE process, FILETIME* create_time) {
    FILETIME exit_time, kernel_time, user_time;
    if (!process || process == INVALID_HANDLE_VALUE || !create_time)
        return FALSE;
    return GetProcessTimes(process, create_time, &exit_time, &kernel_time, &user_time);
}

int nanobot_unverified_candidate_is_owned(DWORD launcher_wait,
                                          DWORD launcher_exit_code,
                                          DWORD candidate_pid,
                                          int prior_state_found,
                                          DWORD prior_pid,
                                          int candidate_creation_known,
                                          FILETIME candidate_creation_ft,
                                          FILETIME launcher_started_ft) {
    if (launcher_wait != WAIT_OBJECT_0 || launcher_exit_code != 0 ||
        candidate_pid == 0 || !candidate_creation_known)
        return 0;
    if (prior_state_found && candidate_pid == prior_pid)
        return 0;
    return filetime_to_ull(candidate_creation_ft) >= filetime_to_ull(launcher_started_ft);
}

int nanobot_get_process_uptime_seconds(int pid, unsigned long long* out_seconds) {
    if (pid <= 0 || !out_seconds)
        return 0;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        return 0;

    FILETIME create_time, exit_time, kernel_time, user_time, now_time;
    BOOL ok = GetProcessTimes(h, &create_time, &exit_time, &kernel_time, &user_time);
    CloseHandle(h);
    if (!ok)
        return 0;

    GetSystemTimeAsFileTime(&now_time);
    unsigned long long created = filetime_to_ull(create_time);
    unsigned long long now = filetime_to_ull(now_time);
    *out_seconds = now > created ? (now - created) / 10000000ULL : 0;
    return 1;
}

/* --- Guarded process state (P1 refactor) -----------------------------------
 * The four values that used to be racing file-scope globals
 * (g_installed, g_nanobot_pid, g_start_tick in main.cpp; g_nanobot_handle here)
 * now live behind one CRITICAL_SECTION. Storage for the handle moved from the
 * old `static HANDLE g_nanobot_handle` into g_state.handle.
 *
 * P1 is a pure refactor: the only observable change is that reads/writes are
 * serialized. No new transitions, threads, or messages.
 */
static CRITICAL_SECTION g_state_cs;
static struct {
    nm_state    state;
    int         installed;
    DWORD       pid;
    HANDLE      handle;
    ULONGLONG   start_tick;
    FILETIME    create_ft;              /* PID-reuse guard (P4); zeroed only by nm_state_clear() */
                                         /* and nanobot_stop cleanup; crash-path close freezes it */
    char        resolved_exe[MAX_PATH]; /* cache for stop-CLI spawn (P4)                        */
    int         resolved_is_python;     /*    "                                                  */
    HANDLE      wait_handle;            /* P5a: thread-pool wait registration; NULL when unarmed  */
} g_state;

void nm_state_init(void) {
    InitializeCriticalSection(&g_state_cs);
    ZeroMemory(&g_state, sizeof(g_state));
}

void nm_state_snapshot(nm_process_view* out) {
    if (!out) return;
    EnterCriticalSection(&g_state_cs);
    out->state      = g_state.state;
    out->installed  = g_state.installed;
    out->pid        = g_state.pid;
    out->handle     = g_state.handle;
    out->start_tick = g_state.start_tick;
    out->create_ft  = g_state.create_ft;
    LeaveCriticalSection(&g_state_cs);
}

nm_state nm_state_get(void) {
    nm_state s;
    EnterCriticalSection(&g_state_cs);
    s = g_state.state;
    LeaveCriticalSection(&g_state_cs);
    return s;
}

void nm_state_set_state(nm_state v) {
    EnterCriticalSection(&g_state_cs);
    g_state.state = v;
    LeaveCriticalSection(&g_state_cs);
}

int nm_state_is_installed(void) {
    int v;
    EnterCriticalSection(&g_state_cs);
    v = g_state.installed;
    LeaveCriticalSection(&g_state_cs);
    return v;
}

void nm_state_set_installed(int v) {
    EnterCriticalSection(&g_state_cs);
    g_state.installed = v;
    LeaveCriticalSection(&g_state_cs);
}

DWORD nm_state_get_pid(void) {
    DWORD v;
    EnterCriticalSection(&g_state_cs);
    v = g_state.pid;
    LeaveCriticalSection(&g_state_cs);
    return v;
}

void nm_state_set_pid(DWORD v) {
    EnterCriticalSection(&g_state_cs);
    g_state.pid = v;
    LeaveCriticalSection(&g_state_cs);
}

HANDLE nm_state_get_handle(void) {
    HANDLE v;
    EnterCriticalSection(&g_state_cs);
    v = g_state.handle;
    LeaveCriticalSection(&g_state_cs);
    return v;
}

void nm_state_set_handle(HANDLE v) {
    EnterCriticalSection(&g_state_cs);
    g_state.handle = v;
    LeaveCriticalSection(&g_state_cs);
}

ULONGLONG nm_state_get_start_tick(void) {
    ULONGLONG v;
    EnterCriticalSection(&g_state_cs);
    v = g_state.start_tick;
    LeaveCriticalSection(&g_state_cs);
    return v;
}

void nm_state_set_start_tick(ULONGLONG v) {
    EnterCriticalSection(&g_state_cs);
    g_state.start_tick = v;
    LeaveCriticalSection(&g_state_cs);
}

void nm_state_get_create_ft(FILETIME* out) {
    if (!out) return;
    EnterCriticalSection(&g_state_cs);
    *out = g_state.create_ft;
    LeaveCriticalSection(&g_state_cs);
}

void nm_state_set_create_ft(const FILETIME* v) {
    if (!v) return;
    EnterCriticalSection(&g_state_cs);
    g_state.create_ft = *v;
    LeaveCriticalSection(&g_state_cs);
}

void nm_state_get_resolved_exe(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    EnterCriticalSection(&g_state_cs);
    strncpy(buf, g_state.resolved_exe, buf_size - 1);
    buf[buf_size - 1] = '\0';
    LeaveCriticalSection(&g_state_cs);
}

void nm_state_set_resolved_exe(const char* exe) {
    EnterCriticalSection(&g_state_cs);
    if (exe) {
        strncpy(g_state.resolved_exe, exe, sizeof(g_state.resolved_exe) - 1);
        g_state.resolved_exe[sizeof(g_state.resolved_exe) - 1] = '\0';
    } else {
        g_state.resolved_exe[0] = '\0';
    }
    LeaveCriticalSection(&g_state_cs);
}

int nm_state_get_resolved_is_python(void) {
    int v;
    EnterCriticalSection(&g_state_cs);
    v = g_state.resolved_is_python;
    LeaveCriticalSection(&g_state_cs);
    return v;
}

void nm_state_set_resolved_is_python(int v) {
    EnterCriticalSection(&g_state_cs);
    g_state.resolved_is_python = v;
    LeaveCriticalSection(&g_state_cs);
}

/* P5a: wait_handle accessors (NULL when no thread-pool wait is armed). */
HANDLE nm_state_get_wait_handle(void) {
    HANDLE v;
    EnterCriticalSection(&g_state_cs);
    v = g_state.wait_handle;
    LeaveCriticalSection(&g_state_cs);
    return v;
}

void nm_state_set_wait_handle(HANDLE v) {
    EnterCriticalSection(&g_state_cs);
    g_state.wait_handle = v;
    LeaveCriticalSection(&g_state_cs);
}

/* --- P5a: thread-pool wait on the gateway handle ----------------------------
 * Arming a one-shot RegisterWaitForSingleObject on g_state.handle lets the UI
 * learn of a crash in ~0ms instead of waiting for the next /health poll (which
 * can be 15s out in steady state). When the handle becomes signaled the wait
 * callback simply PostMessage(WM_CHILD_EXITED); all state work happens on the
 * UI thread (the handler in main.cpp).
 *
 * CALLBACK DISCIPLINE: wait_callback must NOT touch g_state. The handle is
 * closed centrally via nm_state_close_handle (either the WM_CHILD_EXITED crash
 * branch on the UI thread, or stop_worker's cleanup on an invited stop), which
 * calls nanobot_unwatch_handle to drain in-flight callbacks before CloseHandle.
 * The callback only uses its own heap-allocated ctx (hwnd+msg) and frees it
 * exactly once (WT_EXECUTEONLYONCE guarantees single-fire).
 */
struct wait_ctx { HWND hwnd; UINT msg; };   /* heap; owned by the armed wait */

/* ctx of the currently armed wait. Cleared by wait_callback when it consumes
 * it, and by nanobot_unwatch_handle when a cancelled (never-fired) one-shot
 * leaves the ctx otherwise orphaned. Not part of g_state, but the same
 * arm/drain ordering discipline applies: unwatch drains before clearing. */
static struct wait_ctx* g_wait_ctx = NULL;

static VOID CALLBACK wait_callback(PVOID param, BOOLEAN timeout) {
    (void)timeout;                         /* INFINITE wait -- timeout is always FALSE      */
    struct wait_ctx* ctx = (struct wait_ctx*)param;
    if (!ctx) return;
    /* Post, don't send -- the callback runs on a thread-pool thread that must
     * not block on the UI thread's message pump. */
    PostMessageW(ctx->hwnd, ctx->msg, 0, 0);
    if (g_wait_ctx == ctx) g_wait_ctx = NULL;   /* consumed; unwatch must not free it */
    free(ctx);                             /* single-fire => freed exactly once             */
}

int nanobot_watch_handle(HWND hwnd, UINT msg) {
    HANDLE h = nm_state_get_handle();
    if (h == NULL) {
        nm_log(NM_LOG_DEBUG, "process", "watch_handle: no handle, HTTP-only liveness");
        return 0;                          /* HTTP-only fallback (e.g. P5b-attach path)     */
    }

    struct wait_ctx* ctx = (struct wait_ctx*)malloc(sizeof(*ctx));
    if (!ctx) {
        nm_log(NM_LOG_ERROR, "process", "watch_handle: malloc failed");
        return 1;
    }
    ctx->hwnd = hwnd;
    ctx->msg  = msg;

    /* Self-defense: never orphan an already-armed registration (it would leak
     * and could double-post WM_CHILD_EXITED). Unregister BEFORE arming the
     * replacement so both can never fire for the same signal; the drain inside
     * unwatch also guarantees the old callback consumed/cleared g_wait_ctx. */
    if (nm_state_get_wait_handle() != NULL)
        nanobot_unwatch_handle();

    /* Publish ctx BEFORE registering: with an already-signaled process the
     * callback can run (consume + free g_wait_ctx) before the register call
     * returns. A failed registration means the callback never ran and never
     * will, so clearing + freeing in the error branch is safe. */
    g_wait_ctx = ctx;
    HANDLE wait = NULL;
    BOOL ok = RegisterWaitForSingleObject(&wait, h, wait_callback, ctx, INFINITE,
                  WT_EXECUTEONLYONCE | WT_EXECUTEINPERSISTENTTHREAD);
    if (!ok) {
        DWORD err = GetLastError();
        g_wait_ctx = NULL;
        free(ctx);
        nm_log(NM_LOG_ERROR, "process", "watch_handle: RegisterWaitForSingleObject failed err=%d", (int)err);
        return (int)err ? (int)err : 1;
    }

    nm_state_set_wait_handle(wait);
    nm_log(NM_LOG_DEBUG, "process", "watch_handle: armed one-shot wait");
    return 0;
}

void nanobot_unwatch_handle(void) {
    HANDLE w = nm_state_get_wait_handle();
    if (w == NULL) return;                 /* nothing armed (HTTP-only / P5b / re-entry)    */

    /* UnregisterWaitEx's 2nd param is a CompletionEvent HANDLE (NOT flags).
     * INVALID_HANDLE_VALUE makes the call block until any in-flight callback
     * completes (the drain we need). We registered with WT_EXECUTEONLYONCE, so
     * the callback is single-fire -- no risk of a second fire after this
     * returns. MUST be called before CloseHandle on g_state.handle (otherwise
     * the callback could fire into a closed handle). */
    UnregisterWaitEx(w, INVALID_HANDLE_VALUE);
    nm_state_set_wait_handle(NULL);
    /* The drain above guarantees the callback is not running: either it fired
     * and cleared g_wait_ctx itself, or it never will and the ctx is ours to
     * free (this is the normal shutdown path -- nm_state_clear unwatches a
     * still-alive gateway, cancelling the one-shot). */
    if (g_wait_ctx) {
        struct wait_ctx* ctx = g_wait_ctx;
        g_wait_ctx = NULL;
        free(ctx);
    }
    nm_log(NM_LOG_DEBUG, "process", "watch_handle: unregistered");
}

/* C1b: centralized close helper. Atomically steal the gateway handle under the
 * CS so any concurrent caller observes NULL, then drain any armed RegisterWait
 * (blocking) BEFORE CloseHandle -- so an in-flight wait callback can never fire
 * into a closed handle. Serializes closes: two threads racing to close the same
 * handle (e.g. WM_CHILD_EXITED crash branch vs stop_worker cleanup) can no longer
 * double-close. No-op if the handle was already cleared/stolen by another caller. */
void nm_state_close_handle(void) {
    HANDLE h;
    EnterCriticalSection(&g_state_cs);
    h = g_state.handle;
    g_state.handle = NULL;          /* steal -- any concurrent caller gets NULL */
    LeaveCriticalSection(&g_state_cs);
    if (h) {
        nanobot_unwatch_handle();   /* drain in-flight callback (blocking) BEFORE close */
        CloseHandle(h);
    }
}

/* Shutdown helper. Idempotent. Closing an OpenProcess handle does NOT kill the
 * process -- safe to call when stop_on_exit is off (gateway stays running). */
void nm_state_clear(void) {
    FILETIME zero;
    ZeroMemory(&zero, sizeof(zero));
    nm_state_close_handle();              /* unwatch + close, serialized            */
    nm_state_set_pid(0);
    nm_state_set_start_tick(0);
    nm_state_set_create_ft(&zero);        /* reuse guard must not outlive the handle */
}

/* --- P5b: attach to an already-running gateway we didn't start ---------------
 * When the manager relaunches (or the user ran `nanobot gateway` themselves)
 * the gateway is already up but g_state.handle is NULL -- the manager never
 * saw CreateProcess for it. Without attach, PID shows as 0 and crash detection
 * degrades to the 15s HTTP poll. This latches onto the running gateway via the
 * state file + OpenProcess so full liveness (instant handle-signal detection)
 * applies. Runs on the UI thread (apply_health_result) -- all ops are fast and
 * non-blocking (file read, OpenProcess, GetProcessTimes, WaitForSingleObject(0)).
 *
 * Idempotent: no-op (returns 0) if a handle is already tracked. On any failure
 * the handle stays NULL and the caller falls back to HTTP-only liveness.
 *
 * NOTE: does NOT arm nanobot_watch_handle -- that needs hwnd + WM_CHILD_EXITED
 * which live in main.cpp. The caller arms the watch after a successful attach. */
int nanobot_attach_running(void) {
    /* 1. Idempotent: already attached/started. */
    if (nm_state_get_handle() != NULL)
        return 0;

    /* 2. Discover a running gateway pid from the state file. */
    nm_gateway_state gs;
    if (nanobot_read_gateway_state(&gs) != 0 || !gs.found || gs.pid == 0)
        return -1;                       /* no state -> nothing to attach         */

    /* 3. OpenProcess with the rights the wait + query + stop paths need. */
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                           PROCESS_TERMINATE, FALSE, gs.pid);
    if (!h) {
        nm_log(NM_LOG_WARN, "process", "attach: OpenProcess failed pid=%u err=%lu",
            (unsigned)gs.pid, (unsigned long)GetLastError());
        return -2;
    }

    /* 4. Stale guard: if the pid is already dead, the state file is left over
     *    from a prior run -- don't latch onto a dead handle. */
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        CloseHandle(h);
        nm_log(NM_LOG_INFO, "process", "attach: gateway pid %u already dead", (unsigned)gs.pid);
        return -3;
    }

    /* 5. Capture creation time for the PID-reuse guard (inert if this fails).
     *    Matches nanobot_start's tolerance: zeroed on failure -> guard inert. */
    FILETIME create;
    ZeroMemory(&create, sizeof(create));
    get_process_creation_time(h, &create);
    nm_state_set_create_ft(&create);       /* zeroed on failure => guard inert   */

    /* 6. Publish: hand ownership of `h` to g_state. start_tick is approximate
     *    (the gateway's true start time is the state file's started_at, but
     *    this is good enough for uptime display; parsing started_at is out of
     *    scope for P5b). */
    nm_state_set_pid(gs.pid);
    nm_state_set_handle(h);
    nm_state_set_start_tick(GetTickCount64());

    nm_log(NM_LOG_INFO, "process", "attach: latched onto running gateway pid=%u", (unsigned)gs.pid);
    return 0;
}

/* --- Gateway state file reader (P3a) ----------------------------------------
 * `nanobot gateway --background` forks a foreground gateway and writes its pid
 * to %USERPROFILE%\.nanobot\run\gateway.json. That pid is the AUTHORITATIVE
 * identity of the real gateway -- it dissolves the old "stub re-execs python,
 * pid disagrees" problem. We parse only what we need (pid; port/identity are
 * for logging). Tolerant of a missing file (found=0, return 0) and of a
 * present-but-pid-less file (found=1, pid=0).
 */

/* M4 PID-reuse cross-check helpers.
 *
 * Strategy (in order):
 *   1. identity field -- preferred when present. Upstream often records the
 *      process creation-time identity (FILETIME as decimal ull, hex ull, or
 *      "high:low" DWORD pair). Exact match against GetProcessTimes create.
 *   2. started_at ISO 8601 -- fallback. Handles Z / ±offset / fractional
 *      seconds. Naive (no zone) timestamps accept a match against EITHER
 *      UTC or local interpretation within ±30s (avoids TZ false mismatches
 *      without weakening real pid-reuse detection, which is minutes+ off).
 * Parse/conversion failure => skip the cross-check (defense-in-depth, not a
 * gate). Scope: this lenient policy governs nanobot_start's cross-check only;
 * gateway_state_pid_is_verified_live intentionally FAILS CLOSED when identity
 * is present but unparseable (returns 0 = do not trust the state file).
 */

/* ±30s in FILETIME 100ns units. */
#define NM_PID_REUSE_TOL_FT  (30ULL * 10000000ULL)

static int filetimes_within_tol(unsigned long long a, unsigned long long b,
                                unsigned long long tol) {
    unsigned long long diff = a > b ? (a - b) : (b - a);
    return diff <= tol ? 1 : 0;
}

/* Parse identity as a process-create FILETIME.
 *
 * Upstream (nanobot/process_runtime.py `_windows_process_identity`):
 *   return str(creation_time.value)
 * where creation_time.value = (high << 32) | low from GetProcessTimes.
 * So the canonical Windows form is a **decimal** 64-bit FILETIME string,
 * e.g. "133123456789012345". Equality check upstream is exact string match
 * of that decimal value against a fresh GetProcessTimes read.
 *
 * We also accept high:low / 0x-hex as defensive extras (hand-edited state).
 * Returns 1 on success. */
static int identity_to_filetime(const char* id, FILETIME* out) {
    if (!id || !id[0] || !out) return 0;

    /* Canonical: decimal 64-bit FILETIME (str(creation_time.value)). */
    {
        char* end = NULL;
        unsigned long long v = _strtoui64(id, &end, 10);
        if (end && end != id && *end == '\0' && v != 0) {
            ULARGE_INTEGER u;
            u.QuadPart = v;
            out->dwLowDateTime  = u.LowPart;
            out->dwHighDateTime = u.HighPart;
            return 1;
        }
    }

    /* high:low pair (decimal or 0x-hex each side). */
    {
        const char* sep = strchr(id, ':');
        if (sep && sep > id && sep[1]) {
            char left[32], right[32];
            size_t llen = (size_t)(sep - id);
            size_t rlen = strlen(sep + 1);
            /* Reject (rather than truncate) an over-long side: a silently
             * truncated number would parse as valid and turn a parse failure
             * into a confident, WRONG create time. */
            if (llen < sizeof(left) && rlen < sizeof(right)) {
                memcpy(left, id, llen);
                left[llen] = '\0';
                memcpy(right, sep + 1, rlen);
                right[rlen] = '\0';
                char* end_l = NULL;
                char* end_r = NULL;
                unsigned long hi = strtoul(left, &end_l, 0);
                unsigned long lo = strtoul(right, &end_r, 0);
                if (end_l && *end_l == '\0' && end_r && *end_r == '\0') {
                    out->dwHighDateTime = (DWORD)hi;
                    out->dwLowDateTime  = (DWORD)lo;
                    return 1;
                }
            }
        }
    }

    /* 0x-prefixed or plain hex ull (defensive). */
    {
        char* end = NULL;
        unsigned long long v = _strtoui64(id, &end, 0);
        if (!(end && end != id && *end == '\0' && v != 0 &&
              (id[0] == '0' && (id[1] == 'x' || id[1] == 'X')))) {
            end = NULL;
            v = _strtoui64(id, &end, 16);
        }
        if (end && end != id && *end == '\0' && v != 0) {
            ULARGE_INTEGER u;
            u.QuadPart = v;
            out->dwLowDateTime  = u.LowPart;
            out->dwHighDateTime = u.HighPart;
            return 1;
        }
    }
    return 0;
}

/* Parse ISO 8601 wall time into SYSTEMTIME fields + optional zone info.
 * Returns 1 on numeric parse success. *has_zone=1 when Z or ±offset present;
 * *offset_min is east-of-UTC minutes to SUBTRACT to get UTC (i.e. the offset
 * written after the time: +08:00 => offset_min=+480). */
static int iso8601_parse(const char* iso, SYSTEMTIME* st,
                         int* has_zone, int* offset_min) {
    if (!iso || !st || !has_zone || !offset_min) return 0;
    int Y, M, D, h, m, s;
    if (sscanf_s(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6)
        return 0;

    ZeroMemory(st, sizeof(*st));
    st->wYear = (WORD)Y;
    st->wMonth = (WORD)M;
    st->wDay = (WORD)D;
    st->wHour = (WORD)h;
    st->wMinute = (WORD)m;
    st->wSecond = (WORD)s;

    /* Skip past "YYYY-MM-DDTHH:MM:SS" (19 chars) then optional .fff… */
    const char* p = iso;
    int colons = 0;
    while (*p && colons < 2) { if (*p == ':') colons++; p++; }
    while (*p && *p != 'Z' && *p != 'z' && *p != '+' && *p != '-') p++;
    /* If we landed on the date's '-' (shouldn't: we start after seconds), or
     * nothing left, treat as naive. */
    *has_zone = 0;
    *offset_min = 0;
    if (*p == 'Z' || *p == 'z') {
        *has_zone = 1;
        *offset_min = 0;
        return 1;
    }
    if (*p == '+' || *p == '-') {
        int sign = (*p == '+') ? 1 : -1;
        int oh = 0, om = 0;
        /* Clamp the zone before any arithmetic: oh/om come straight from a
         * state-file string and large values would overflow the minute math
         * (or make sscanf itself undefined). Real zones are <= +14:00. */
        /* +HH:MM or +HHMM */
        if (sscanf_s(p + 1, "%d:%d", &oh, &om) == 2 && oh >= 0 && oh <= 14 && om >= 0 && om <= 59) {
            *has_zone = 1;
            *offset_min = sign * (oh * 60 + om);
            return 1;
        }
        if (sscanf_s(p + 1, "%2d%2d", &oh, &om) == 2 && oh >= 0 && oh <= 14 && om >= 0 && om <= 59) {
            *has_zone = 1;
            *offset_min = sign * (oh * 60 + om);
            return 1;
        }
        if (sscanf_s(p + 1, "%d", &oh) == 1 && oh >= 0 && oh <= 14) {
            *has_zone = 1;
            *offset_min = sign * (oh * 60);
            return 1;
        }
        return 0;   /* sign present but unparseable */
    }
    return 1;       /* naive -- no zone marker */
}

/* Convert a parsed SYSTEMTIME + offset to UTC FILETIME.
 * offset_min is east-of-UTC minutes encoded in the string (subtract to UTC). */
static int systemtime_offset_to_filetime(const SYSTEMTIME* st, int offset_min,
                                         FILETIME* out) {
    FILETIME ft;
    if (!SystemTimeToFileTime(st, &ft)) return 0;
    unsigned long long ull = filetime_to_ull(ft);
    /* offset_min > 0 means local is ahead of UTC => UTC = local - offset */
    long long delta = (long long)offset_min * 60LL * 10000000LL;
    if (delta > 0 && (unsigned long long)delta > ull) return 0;
    ull = (unsigned long long)((long long)ull - delta);
    ULARGE_INTEGER u;
    u.QuadPart = ull;
    out->dwLowDateTime  = u.LowPart;
    out->dwHighDateTime = u.HighPart;
    return 1;
}

/* started_at -> candidate FILETIME(s) for comparison.
 * On success fills utc_out always; if the stamp was zone-naive, also fills
 * local_out (via TzSpecificLocalTimeToSystemTime reverse: treat as local).
 * *have_local=1 when local_out is valid. Returns 1 if at least utc_out is set. */
static int iso8601_to_filetimes(const char* iso,
                                FILETIME* utc_out, FILETIME* local_out,
                                int* have_local) {
    if (have_local) *have_local = 0;
    if (!iso || !utc_out) return 0;

    SYSTEMTIME st;
    int has_zone = 0, offset_min = 0;
    if (!iso8601_parse(iso, &st, &has_zone, &offset_min))
        return 0;

    if (has_zone) {
        return systemtime_offset_to_filetime(&st, offset_min, utc_out);
    }

    /* Naive: produce BOTH UTC interpretation and local interpretation.
     * UTC path: SystemTimeToFileTime treats fields as UTC. */
    if (!SystemTimeToFileTime(&st, utc_out))
        return 0;

    /* Local path: treat st as local wall time -> UTC FILETIME. */
    if (local_out) {
        SYSTEMTIME utc_st;
        if (TzSpecificLocalTimeToSystemTime(NULL, &st, &utc_st)
            && SystemTimeToFileTime(&utc_st, local_out)) {
            if (have_local) *have_local = 1;
        }
    }
    return 1;
}

/* --- WebUI endpoint detection (port + bootstrap secret) -------------------- */

/* Mirror of nanobot's cli/webui_support.webui_bootstrap_secret: the first
 * non-empty (after trimming ASCII whitespace) of tokenIssueSecret / token,
 * copied capped (cap includes the NUL) into out. Leaves out empty when the
 * channel carries no usable secret. */
static void webui_extract_secret(cJSON* ws, char* out, size_t cap) {
    static const char* const keys[] = { "tokenIssueSecret", "token" };
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!cJSON_IsObject(ws)) return;
    for (int i = 0; i < (int)_countof(keys); i++) {
        cJSON* item = cJSON_GetObjectItem(ws, keys[i]);
        if (!item || !cJSON_IsString(item) || !item->valuestring) continue;
        const char* s = item->valuestring;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        size_t len = strlen(s);
        while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                           s[len - 1] == '\r' || s[len - 1] == '\n'))
            len--;
        if (len == 0) continue;
        size_t copy = (len < cap - 1) ? len : cap - 1;
        memcpy(out, s, copy);
        out[copy] = '\0';
        return;
    }
}

/* nanobot serves its WebUI from the channels.websocket channel, so its own
 * config is authoritative for where the UI listens and which bootstrap secret
 * it expects. Pure: fills out_port (1..65535, else 0 when absent, invalid, or
 * the channel is disabled) and out_secret (see webui_extract_secret) from one
 * document. Returns 1 when an enabled websocket channel exists. Absent enabled
 * counts as on (nanobot's WebSocketConfig default). */
int nanobot_config_webui_info(const char* json, int* out_port,
                              char* out_secret, size_t secret_cap) {
    if (out_port) *out_port = 0;
    if (out_secret && secret_cap) out_secret[0] = '\0';
    if (!json || !json[0]) return 0;
    cJSON* root = cJSON_Parse(json);
    if (!root) return 0;
    int found = 0;
    cJSON* channels = cJSON_GetObjectItem(root, "channels");
    cJSON* ws = cJSON_IsObject(channels)
                    ? cJSON_GetObjectItem(channels, "websocket") : NULL;
    if (cJSON_IsObject(ws)) {
        cJSON* enabled = cJSON_GetObjectItem(ws, "enabled");
        if (!cJSON_IsBool(enabled) || cJSON_IsTrue(enabled)) {
            found = 1;
            if (out_port) {
                cJSON* jport = cJSON_GetObjectItem(ws, "port");
                if (jport && cJSON_IsNumber(jport) &&
                    jport->valueint >= 1 && jport->valueint <= 65535)
                    *out_port = jport->valueint;
            }
            webui_extract_secret(ws, out_secret, secret_cap);
        }
    }
    cJSON_Delete(root);
    return found;
}

/* Resolve nanobot's WebUI endpoint with one read of the shared
 * ~\.nanobot\config.json (same Path.home() semantics as
 * nanobot_read_gateway_state): fills out_port / out_secret from the file and
 * returns the detected port, or `fallback_port` when the file is missing,
 * unreadable, or carries no usable channel. */
int nanobot_detect_webui(int* out_port, char* out_secret, size_t secret_cap,
                         int fallback_port) {
    if (out_port) *out_port = 0;
    if (out_secret && secret_cap) out_secret[0] = '\0';
    /* "~" on Windows == %USERPROFILE% == Python's Path.home(). */
    PWSTR profile = NULL;
    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &profile);
    if (FAILED(hr) || !profile) {
        if (profile) CoTaskMemFree(profile);
        return fallback_port;
    }
    wchar_t wpath[MAX_PATH];
    int n = _snwprintf_s(wpath, _countof(wpath), _TRUNCATE,
        L"%s\\.nanobot\\config.json", profile);
    CoTaskMemFree(profile);             /* always free, even on truncation-fail */
    if (n <= 0) return fallback_port;

    FILE* f = _wfopen(wpath, L"rb");
    if (!f) return fallback_port;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* nanobot's config carries model presets and MCP servers and reaches a few
     * hundred KB; 4 MiB is generous headroom without allowing the unbounded
     * malloc a hostile file could trigger. */
    if (len <= 0 || len > (4L << 20)) { fclose(f); return fallback_port; }

    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return fallback_port; }
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);

    int port = 0;
    nanobot_config_webui_info(buf, &port, out_secret, secret_cap);
    if (out_port && port > 0)
        *out_port = port;
    SecureZeroMemory(buf, (size_t)len + 1);
    free(buf);
    return port > 0 ? port : fallback_port;
}

int nanobot_read_gateway_state(nm_gateway_state* out) {
    if (!out) return -1;
    out->found = 0;
    out->pid = 0;
    out->port = 0;
    out->identity[0] = '\0';
    out->started_at[0] = '\0';
    out->command[0] = '\0';

    /* "~" on Windows == %USERPROFILE% == Python's Path.home(). */
    PWSTR profile = NULL;
    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &profile);
    if (FAILED(hr) || !profile) {
        if (profile) CoTaskMemFree(profile);
        return -1;
    }

    wchar_t wpath[MAX_PATH];
    int n = _snwprintf_s(wpath, _countof(wpath), _TRUNCATE,
        L"%s\\.nanobot\\run\\gateway.json", profile);
    CoTaskMemFree(profile);             /* always free, even on truncation-fail */
    if (n <= 0) return -1;

    /* fopen/ftell/fread pattern from config.c (wide path => _wfopen, same as
     * process_log). cJSON is already linked (config.c uses it). */
    FILE* f = _wfopen(wpath, L"rb");
    if (!f) return 0;                   /* missing file => not found, not an error */

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* The gateway state file is a tiny JSON document. Cap the allocation so a
     * corrupt/hostile file (e.g. a symlink to something huge placed under the
     * user profile) cannot trigger an unbounded malloc. 1 MiB is far above any
     * legitimate gateway.json and still trivially affordable. */
    if (len <= 0 || len > (1L << 20)) { fclose(f); return 0; }

    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(buf);
    SecureZeroMemory(buf, (size_t)len + 1);
    free(buf);
    if (!root) return 0;                /* malformed JSON => treat as not found */

    out->found = 1;
    cJSON* jpid = cJSON_GetObjectItem(root, "pid");
    if (jpid && cJSON_IsNumber(jpid))
        out->pid = (DWORD)jpid->valueint;

    cJSON* jport = cJSON_GetObjectItem(root, "port");
    if (jport && cJSON_IsNumber(jport))
        out->port = jport->valueint;

    cJSON* jid = cJSON_GetObjectItem(root, "identity");
    if (jid && cJSON_IsString(jid) && jid->valuestring) {
        strncpy(out->identity, jid->valuestring, sizeof(out->identity) - 1);
        out->identity[sizeof(out->identity) - 1] = '\0';
    }

    cJSON* jstarted = cJSON_GetObjectItem(root, "started_at");
    if (jstarted && cJSON_IsString(jstarted) && jstarted->valuestring) {
        strncpy(out->started_at, jstarted->valuestring, sizeof(out->started_at) - 1);
        out->started_at[sizeof(out->started_at) - 1] = '\0';
    }

    /* Upstream (nanobot/process_runtime.py) writes command as a JSON array of
     * argv tokens, e.g. ["C:\\…\\python.exe","-m","nanobot","gateway",…].
     * Older/hand-edited state may use a single string. Reconstruct a
     * space-joined command line so M8's first-token + "-m nanobot" parse works. */
    cJSON* jcmd = cJSON_GetObjectItem(root, "command");
    if (jcmd && cJSON_IsString(jcmd) && jcmd->valuestring) {
        strncpy(out->command, jcmd->valuestring, sizeof(out->command) - 1);
        out->command[sizeof(out->command) - 1] = '\0';
    } else if (jcmd && cJSON_IsArray(jcmd)) {
        size_t used = 0;
        out->command[0] = '\0';
        const cJSON* el = NULL;
        cJSON_ArrayForEach(el, jcmd) {
            if (!cJSON_IsString(el) || !el->valuestring) continue;
            const char* tok = el->valuestring;
            int need_quote = (strchr(tok, ' ') != NULL || strchr(tok, '\t') != NULL);
            size_t tlen = strlen(tok);
            size_t need = tlen + (need_quote ? 2 : 0) + (used ? 1 : 0);
            if (used + need + 1 >= sizeof(out->command)) break;
            if (used) out->command[used++] = ' ';
            if (need_quote) out->command[used++] = '"';
            memcpy(out->command + used, tok, tlen);
            used += tlen;
            if (need_quote) out->command[used++] = '"';
            out->command[used] = '\0';
        }
    }

    cJSON_Delete(root);
    return 0;
}

/* Trust a state-file port only when its claimed process is still alive and its
 * recorded process identity matches. Without this guard, a stale state file
 * can turn an unrelated service that reused the port into a false Running UI. */
static int gateway_state_pid_is_verified_live(const nm_gateway_state* state) {
    HANDLE process;
    FILETIME created, expected_utc, expected_local;
    unsigned long long actual;
    int have_local = 0;

    if (!state || !state->pid) return 0;
    process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                          FALSE, state->pid);
    if (!process) return 0;
    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0 ||
        !get_process_creation_time(process, &created)) {
        CloseHandle(process);
        return 0;
    }
    CloseHandle(process);
    actual = filetime_to_ull(created);

    if (state->identity[0]) {
        FILETIME identity;
        if (!identity_to_filetime(state->identity, &identity)) return 0;
        return filetimes_within_tol(filetime_to_ull(identity), actual,
                                    2ULL * 10000000ULL);
    }
    if (!state->started_at[0] ||
        !iso8601_to_filetimes(state->started_at, &expected_utc, &expected_local,
                              &have_local))
        return 0;
    if (filetimes_within_tol(filetime_to_ull(expected_utc), actual,
                             NM_PID_REUSE_TOL_FT))
        return 1;
    return have_local && filetimes_within_tol(filetime_to_ull(expected_local), actual,
                                               NM_PID_REUSE_TOL_FT);
}

int nanobot_detect_running_port(int configured_port, int* out_port) {
    nm_gateway_state state;
    int state_probed;
    if (out_port) *out_port = 0;
    ZeroMemory(&state, sizeof(state));
    /* Only suppress the configured-port probe when the state branch actually
     * probed that port: a state file whose port EQUALS the configured port but
     * whose pid fails verification (stale/pid-reused) must not hide a live
     * gateway running on the configured port. */
    state_probed = nanobot_read_gateway_state(&state) == 0 && state.found &&
        state.port >= 1 && state.port <= 65535 &&
        gateway_state_pid_is_verified_live(&state);
    if (state_probed && nanobot_check_running(state.port)) {
        if (out_port) *out_port = state.port;
        return 1;
    }
    if (configured_port >= 1 && configured_port <= 65535 &&
        (!state_probed || state.port != configured_port) &&
        nanobot_check_running(configured_port)) {
        if (out_port) *out_port = configured_port;
        return 1;
    }
    return 0;
}

/* P3a: `nanobot gateway --background` spawns a short-lived launcher that forks
 * the real foreground gateway, writes ~/.nanobot/run/gateway.json, and exits
 * within ~1s. We must NOT keep the launcher's handle: WaitForSingleObject on it
 * would fire the instant the launcher finishes its job -- a false crash.
 *
 * LOAD-BEARING INVARIANT: g_state.handle is ALWAYS the OpenProcess'd REAL
 * gateway pid (read from the state file), NEVER the --background launcher's
 * handle. The launcher handle (pi.hProcess) is only ever passed to
 * WaitForSingleObject + CloseHandle. If you store the launcher handle, the
 * health/death detector will report a "crash" ~1s after every successful start.
 */
/* Open nanobot-gateway.log beside the exe for child stdout/stderr redirection,
 * rotating to .bak at 512 KiB (same policy as the manager log). Opened with
 * FILE_SHARE_DELETE so a later rotation isn't blocked by a still-running
 * gateway holding an inherited write handle. Returns NULL on any failure;
 * callers degrade to a spawn without redirection. */
static HANDLE open_gateway_log_handle(void) {
    wchar_t exe[MAX_PATH];
    wchar_t path[MAX_PATH];
    wchar_t bak[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, _countof(exe))) return NULL;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return NULL;
    *slash = L'\0';
    if (_snwprintf_s(path, _countof(path), _TRUNCATE,
                     L"%s\\nanobot-gateway.log", exe) < 0)
        return NULL;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER sz;
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        if (sz.QuadPart > 512LL * 1024LL &&
            _snwprintf_s(bak, _countof(bak), _TRUNCATE,
                         L"%s\\nanobot-gateway.bak", exe) >= 0) {
            MoveFileExW(path, bak, MOVEFILE_REPLACE_EXISTING);
        }
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;             /* the child inherits the write end */
    sa.lpSecurityDescriptor = NULL;
    HANDLE h = CreateFileW(path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, &sa,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        /* Keep the degrade-to-no-redirection behavior (logging must never
         * fail the start), but leave a trace -- e.g. ERROR_SHARING_VIOLATION
         * while a previous gateway still holds the log open. */
        nm_log(NM_LOG_WARN, "process",
            "open_gateway_log_handle: CreateFileW failed err=%d (spawning without redirection)",
            (int)GetLastError());
        return NULL;
    }
    SetFilePointer(h, 0, NULL, FILE_END); /* append across manager restarts */
    return h;
}

int nanobot_start(const char* exe, int is_python, int health_port, int* out_pid,
                   int* out_effective_health_port) {
    if (out_pid) *out_pid = 0;
    if (out_effective_health_port) *out_effective_health_port = health_port;
    if (!exe || !exe[0] || health_port < 1 || health_port > 65535)
        return ERROR_INVALID_PARAMETER;

    /* A mismatched-port record must prove process identity before we publish
     * it. Keep the prior PID so recovery only terminates a process this launch
     * demonstrably replaced, never a pre-existing instance. */
    nm_gateway_state prior_state;
    ZeroMemory(&prior_state, sizeof(prior_state));
    int prior_state_found = nanobot_read_gateway_state(&prior_state) == 0 &&
                            prior_state.found && prior_state.pid > 0;

    /* Workdir-derivation (unchanged from pre-P3a): strip the last path
     * component from the exe path so nanobot runs from its install dir. */
    wchar_t* wide_exe = utf8_to_wide_alloc(exe);
    if (!wide_exe) return ERROR_NO_UNICODE_TRANSLATION;
    wchar_t* workdir = _wcsdup(wide_exe);
    if (!workdir) { free(wide_exe); return ERROR_NOT_ENOUGH_MEMORY; }
    wchar_t* slash = wcsrchr(workdir, L'\\');
    if (!slash) slash = wcsrchr(workdir, L'/');
    if (slash) *slash = '\0'; else workdir[0] = '\0';

    /* Gateway output capture: redirect the launcher's stdout/stderr to
     * nanobot-gateway.log. File redirection -- NOT a pipe -- because the
     * gateway outlives the manager; a pipe would lose its reader once the
     * manager exits and a full pipe could block the child. The attribute
     * list restricts inheritance to exactly this one handle (same pattern as
     * the capability probe above). A NULL handle degrades to the legacy
     * void-redirected spawn; logging must never fail the start. The launcher
     * prints its own diagnostics here, and the forked gateway inherits the
     * handles only insofar as nanobot propagates them. */
    HANDLE gateway_log = open_gateway_log_handle();

    STARTUPINFOEXW siex;
    PROCESS_INFORMATION pi;
    ZeroMemory(&siex, sizeof(siex));
    ZeroMemory(&pi, sizeof(pi));
    siex.StartupInfo.cb = sizeof(siex);
    siex.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    siex.StartupInfo.wShowWindow = SW_HIDE;

    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    int attributes_initialized = 0;   /* Delete* must never run on an uninitialized list */
    if (gateway_log) {
        siex.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        siex.StartupInfo.hStdOutput = gateway_log;
        siex.StartupInfo.hStdError  = gateway_log;
        siex.StartupInfo.hStdInput  = NULL;
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
        attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size);
        if (attributes)
            attributes_initialized =
                InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size);
        if (attributes_initialized) {
            HANDLE inheritable_handles[1];
            inheritable_handles[0] = gateway_log;
            if (!UpdateProcThreadAttribute(attributes, 0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inheritable_handles, sizeof(inheritable_handles), NULL, NULL)) {
                DeleteProcThreadAttributeList(attributes);
                free(attributes);
                attributes = NULL;
                attributes_initialized = 0;
            } else {
                siex.lpAttributeList = attributes;
            }
        }
    }

    wchar_t arguments[96];
    swprintf_s(arguments, _countof(arguments),
        is_python ? L"-m nanobot gateway --background --port %d"
                  : L"gateway --background --port %d", health_port);
    wchar_t* cmdline = build_command_line(wide_exe, arguments);
    if (!cmdline) {
        if (attributes) { DeleteProcThreadAttributeList(attributes); free(attributes); }
        if (gateway_log) CloseHandle(gateway_log);
        free(workdir); free(wide_exe);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    FILETIME launcher_started_ft;
    GetSystemTimeAsFileTime(&launcher_started_ft);
    DWORD spawn_flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (siex.lpAttributeList)
        spawn_flags |= EXTENDED_STARTUPINFO_PRESENT;
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, siex.lpAttributeList ? TRUE : FALSE,
            spawn_flags, NULL, workdir[0] ? workdir : NULL, &siex.StartupInfo, &pi))
    {
        int error = (int)GetLastError();
        if (attributes) { DeleteProcThreadAttributeList(attributes); free(attributes); }
        if (gateway_log) CloseHandle(gateway_log);
        free(cmdline); free(workdir); free(wide_exe);
        return error;
    }
    if (siex.lpAttributeList)
        nm_log(NM_LOG_INFO, "process",
               "nanobot_start: gateway stdout/stderr -> nanobot-gateway.log");
    if (attributes) { DeleteProcThreadAttributeList(attributes); free(attributes); }
    if (gateway_log) CloseHandle(gateway_log);
    free(cmdline); free(workdir); free(wide_exe);

    /* --- The launcher is short-lived: wait for it to finish writing the state
     * file, then close BOTH handles immediately. We NEVER keep the launcher
     * handle (see invariant above). A non-zero/timed-out launcher exit is a
     * hint but not proof of failure -- fall through to the state-file read. */
    DWORD dwExit = 0;
    DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &dwExit);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    pi.hThread  = NULL;
    pi.hProcess = NULL;

    if (wait == WAIT_TIMEOUT)
        nm_log(NM_LOG_WARN, "process", "nanobot_start: launcher still active after 5s (continuing)");
    else if (dwExit != 0)
        nm_log(NM_LOG_WARN, "process", "nanobot_start: launcher exited code=%d (continuing)", (int)dwExit);

    /* --- Retry-read the state file for the REAL gateway pid. The launcher may
     * not have written it yet, so poll ~10 x 200ms = ~2s. */
    nm_gateway_state gs;
    memset(&gs, 0, sizeof(gs));
    int got_pid = 0;
    for (int i = 0; i < 10; i++) {
        if (nanobot_read_gateway_state(&gs) == 0 && gs.found && gs.pid != 0) {
            got_pid = 1;
            break;
        }
        Sleep(200);
    }
    if (!got_pid) {
        if (wait == WAIT_OBJECT_0 && dwExit != 0) {
            nm_log(NM_LOG_ERROR, "process", "nanobot_start: launcher exit=%lu; no valid gateway pid", dwExit);
            return NANOBOT_START_LAUNCHER_FAILED;
        }
        nm_log(NM_LOG_ERROR, "process", "nanobot_start: state file missing or no pid after ~2s");
        return NANOBOT_START_NO_STATEFILE;
    }

    nm_log(NM_LOG_INFO, "process", "nanobot_start: expected port=%d; state file reports pid=%d port=%d identity=%s%s",
        health_port, (int)gs.pid, gs.port, gs.identity[0] ? gs.identity : "(none)",
        gs.port > 0 && gs.port != health_port ? " (port mismatch)" : "");

    /* --- OpenProcess the real gateway pid. If the pid is ALREADY dead the
     * state file is stale (left over from a prior run); give the new launcher
     * a couple of retries to overwrite it before giving up. */
    HANDLE h = NULL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) Sleep(200);    /* let the launcher overwrite stale file */

        /* PROCESS_TERMINATE is ALWAYS required: nanobot_stop's tier-1 hard
         * kill runs TerminateProcess on this published handle in the common
         * (port-matching) case too — without it Stop fails with
         * ERROR_ACCESS_DENIED while the UI still flips to IDLE. */
        DWORD process_access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                               PROCESS_TERMINATE;
        h = OpenProcess(process_access, FALSE, gs.pid);
        if (!h) {
            nm_log(NM_LOG_ERROR, "process", "nanobot_start: OpenProcess(pid=%d) failed err=%d",
                (int)gs.pid, (int)GetLastError());
            return NANOBOT_START_OPEN_FAILED;
        }
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            /* pid already dead => stale state file */
            CloseHandle(h);
            h = NULL;
            nm_log(NM_LOG_WARN, "process", "nanobot_start: stale gateway pid=%d", (int)gs.pid);
            /* If the file now references a DIFFERENT pid, retry on that one. */
            nm_gateway_state gs2;
            if (nanobot_read_gateway_state(&gs2) == 0 && gs2.found && gs2.pid != 0
                && gs2.pid != gs.pid) {
                gs = gs2;
                nm_log(NM_LOG_DEBUG, "process", "nanobot_start: state file refreshed to pid=%d",
                    (int)gs.pid);
            }
            continue;
        }
        /* M4: PID-reuse cross-check. The pid is alive, but it could be a
         * DIFFERENT process that recycled this pid (stale state file). Prefer
         * identity (create-time FILETIME) over started_at (ISO, TZ-ambiguous).
         * On mismatch -> retry state-file read; on parse failure -> skip
         * (defense-in-depth, not a gate). */
        {
            FILETIME create_check;
            if (!get_process_creation_time(h, &create_check)) {
                nm_log(NM_LOG_DEBUG, "process", "nanobot_start: PID-reuse cross-check skipped "
                    "(GetProcessTimes failed) pid=%d", (int)gs.pid);
                break;                  /* live handle; check inert */
            }
            unsigned long long ct = filetime_to_ull(create_check);
            int checked = 0;
            int mismatch = 0;

            /* 1) identity first (exact FILETIME match, small tol for rounding). */
            if (gs.identity[0]) {
                FILETIME id_ft;
                if (identity_to_filetime(gs.identity, &id_ft)) {
                    checked = 1;
                    unsigned long long idv = filetime_to_ull(id_ft);
                    /* 2s tol: identity should be exact, but allow tiny skew. */
                    if (!filetimes_within_tol(idv, ct, 2ULL * 10000000ULL)) {
                        nm_log(NM_LOG_WARN, "process", "nanobot_start: identity mismatch, suspecting "
                            "pid reuse (identity_ull=%llu create_ull=%llu pid=%d)",
                            idv, ct, (int)gs.pid);
                        mismatch = 1;
                    }
                } else {
                    nm_log(NM_LOG_DEBUG, "process", "nanobot_start: identity unparseable (%s), "
                        "falling through to started_at pid=%d",
                        gs.identity, (int)gs.pid);
                }
            }

            /* 2) started_at ISO fallback (only if identity did not decide). */
            if (!checked && !mismatch && gs.started_at[0]) {
                FILETIME utc_ft, local_ft;
                int have_local = 0;
                if (iso8601_to_filetimes(gs.started_at, &utc_ft, &local_ft,
                                         &have_local)) {
                    checked = 1;
                    unsigned long long utc = filetime_to_ull(utc_ft);
                    int ok = filetimes_within_tol(utc, ct, NM_PID_REUSE_TOL_FT);
                    if (!ok && have_local) {
                        unsigned long long loc = filetime_to_ull(local_ft);
                        ok = filetimes_within_tol(loc, ct, NM_PID_REUSE_TOL_FT);
                    }
                    if (!ok) {
                        nm_log(NM_LOG_WARN, "process", "nanobot_start: started_at mismatch, suspecting "
                            "pid reuse (started_at=%s create_ull=%llu pid=%d)",
                            gs.started_at, ct, (int)gs.pid);
                        mismatch = 1;
                    }
                } else {
                    nm_log(NM_LOG_DEBUG, "process", "nanobot_start: started_at cross-check skipped "
                        "(unparseable) pid=%d", (int)gs.pid);
                }
            }

            if (!checked && !mismatch
                && !gs.identity[0] && !gs.started_at[0]) {
                /* Nothing to check -- accept live handle. */
            }

            if (mismatch) {
                CloseHandle(h);
                h = NULL;
                nm_gateway_state gs2;
                if (nanobot_read_gateway_state(&gs2) == 0 && gs2.found
                    && gs2.pid != 0 && gs2.pid != gs.pid) {
                    gs = gs2;
                    nm_log(NM_LOG_DEBUG, "process", "nanobot_start: state file refreshed to pid=%d",
                        (int)gs.pid);
                }
                continue;
            }
        }
        break;                          /* live handle */
    }
    if (!h) {
        nm_log(NM_LOG_ERROR, "process", "nanobot_start: gateway pid stale after retries");
        return NANOBOT_START_STALE_PID;
    }

    /* Only the port-mismatch path changes the endpoint we expose.  Wait for
     * the writer to finish refreshing identity/started_at, then trust it only
     * when the live process matches that record. */
    if (gs.port >= 1 && gs.port <= 65535 && gs.port != health_port) {
        int verified = gateway_state_pid_is_verified_live(&gs);
        for (int attempt = 0; !verified && attempt < 10; attempt++) {
            nm_gateway_state refreshed;
            Sleep(200);
            ZeroMemory(&refreshed, sizeof(refreshed));
            if (nanobot_read_gateway_state(&refreshed) != 0 || !refreshed.found ||
                refreshed.pid == 0)
                continue;
            if (refreshed.pid != gs.pid) {
                HANDLE refreshed_handle = OpenProcess(
                    SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                    FALSE, refreshed.pid);
                if (!refreshed_handle ||
                    WaitForSingleObject(refreshed_handle, 0) == WAIT_OBJECT_0) {
                    if (refreshed_handle) CloseHandle(refreshed_handle);
                    continue;
                }
                CloseHandle(h);
                h = refreshed_handle;
                nm_log(NM_LOG_INFO, "process", "nanobot_start: state record refreshed to pid=%d while verifying port mismatch",
                            (int)refreshed.pid);
            }
            gs = refreshed;
            if (gs.port < 1 || gs.port > 65535 || gs.port == health_port)
                break; /* requested-port compatibility path remains unchanged */
            verified = gateway_state_pid_is_verified_live(&gs);
        }
        if (gs.port >= 1 && gs.port <= 65535 && gs.port != health_port && !verified) {
            FILETIME candidate_creation_ft;
            int candidate_creation_known = get_process_creation_time(
                h, &candidate_creation_ft) ? 1 : 0;
            int ownership_proven = nanobot_unverified_candidate_is_owned(
                wait, dwExit, gs.pid, prior_state_found, prior_state.pid,
                candidate_creation_known, candidate_creation_ft, launcher_started_ft);
            nm_log(NM_LOG_WARN, "process", "nanobot_start: refusing unverified state candidate pid=%d port=%d",
                        (int)gs.pid, gs.port);
            if (ownership_proven) {
                if (TerminateProcess(h, ERROR_INVALID_DATA)) {
                    DWORD terminate_wait = WaitForSingleObject(h, 5000);
                    nm_log(NM_LOG_INFO, "process", "nanobot_start: cleanup performed for unverified candidate pid=%d wait=%lu",
                                (int)gs.pid, (unsigned long)terminate_wait);
                } else {
                    nm_log(NM_LOG_ERROR, "process", "nanobot_start: cleanup failed for owned unverified candidate pid=%d err=%d",
                                (int)gs.pid, (int)GetLastError());
                }
            } else {
                nm_log(NM_LOG_INFO, "process", "nanobot_start: cleanup skipped because ownership is unproven");
            }
            CloseHandle(h);
            return NANOBOT_START_UNVERIFIED_STATE;
        }
    }

    int effective_port = (gs.port >= 1 && gs.port <= 65535) ? gs.port : health_port;
    if (effective_port != health_port)
        nm_log(NM_LOG_INFO, "process", "nanobot_start: adopted actual health port=%d (requested=%d)",
                    effective_port, health_port);
    else if (gs.port < 1 || gs.port > 65535)
        nm_log(NM_LOG_WARN, "process", "nanobot_start: state port invalid; using requested health port=%d",
                    health_port);

    /* --- Live: capture creation time (PID-reuse guard for P4), then publish.
     * Close any previously-stored handle first to avoid a leak on re-start.
     * C2: route through nm_state_close_handle so an armed RegisterWait (e.g.
     * streak-DEAD before WM_CHILD_EXITED fired) is drained BEFORE the close --
     * the old CloseHandle(prev) on an armed handle was a UAF-on-callback hole.
     *
     * Publish ordering: capture create_ft BEFORE mutating shared state, then
     * close the old handle, publish the new handle/pid, and only THEN write
     * create_ft. Writing create_ft earlier left a window where a concurrent
     * snapshot paired the OLD handle with the NEW creation time, defeating the
     * PID-reuse guards in nanobot_stop. */
    FILETIME create_ft, exit_ft, kern_ft, user_ft;
    ZeroMemory(&create_ft, sizeof(create_ft));
    int got_create_ft = GetProcessTimes(h, &create_ft, &exit_ft, &kern_ft, &user_ft);
    if (!got_create_ft) {
        nm_log(NM_LOG_WARN, "process", "nanobot_start: GetProcessTimes failed err=%d",
            (int)GetLastError());
        /* create_ft stays zeroed => reuse guard inert (by design). */
    }

    nm_state_close_handle();           /* C2: unwatch + close prev (if any)        */
    /* <-- the OpenProcess'd REAL gateway handle, never the launcher's */
    nm_state_set_handle(h);
    nm_state_set_pid(gs.pid);
    nm_state_set_start_tick(GetTickCount64());
    nm_state_set_create_ft(&create_ft);  /* publish AFTER the new handle/pid */
    nm_state_set_resolved_exe(exe);             /* cached for P4's stop-CLI spawn */
    nm_state_set_resolved_is_python(is_python);

    if (out_pid) *out_pid = (int)gs.pid;
    if (out_effective_health_port) *out_effective_health_port = effective_port;
    nm_log(NM_LOG_INFO, "process", "nanobot started (gateway pid=%d health_port=%d)", (int)gs.pid,
                effective_port);
    return 0;
}

/* Depth cap for the kill recursion: deeper than any real process tree, but
 * finite -- PID reuse can forge a cycle in the snapshot's parent chain, which
 * would otherwise recurse forever. */
#define NM_KILL_TREE_MAX_DEPTH 32

/* Recursively terminate a process and its descendants. `expected_create` is an
 * optional PID-reuse guard: if non-NULL and the live process's creation time
 * differs, the pid has been recycled by the OS and we MUST NOT terminate it.
 * Children are always recursed with NULL (we have no create-time history for
 * them). */
static void kill_process_tree_impl(DWORD pid, const FILETIME* expected_create,
                                   int depth) {
    if (depth > NM_KILL_TREE_MAX_DEPTH) {
        nm_log(NM_LOG_WARN, "process",
            "kill_process_tree: depth cap %d exceeded at pid=%u (parent-pid cycle?)",
            NM_KILL_TREE_MAX_DEPTH, (unsigned)pid);
        return;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ParentProcessID == pid)
                    kill_process_tree_impl(pe.th32ProcessID, NULL, depth + 1);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    /* PID-reuse guard: confirm the live process is the one we recorded. */
    if (expected_create) {
        HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (q) {
            FILETIME ft;
            BOOL ok = get_process_creation_time(q, &ft);
            CloseHandle(q);
            if (ok && filetime_to_ull(ft) != filetime_to_ull(*expected_create)) {
                nm_log(NM_LOG_WARN, "process", "pid reuse suspected, skipping terminate pid=%u", (unsigned)pid);
                return;
            }
        }
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (h) {
        if (!TerminateProcess(h, 0))
            nm_log(NM_LOG_WARN, "process",
                "kill_process_tree: TerminateProcess failed pid=%u err=%d",
                (unsigned)pid, (int)GetLastError());
        if (WaitForSingleObject(h, 3000) == WAIT_FAILED)
            nm_log(NM_LOG_WARN, "process",
                "kill_process_tree: post-terminate wait failed pid=%u err=%d",
                (unsigned)pid, (int)GetLastError());
        CloseHandle(h);
    }
}

/* Public entry: see kill_process_tree_impl above. */
static void kill_process_tree(DWORD pid, const FILETIME* expected_create) {
    kill_process_tree_impl(pid, expected_create, 0);
}

/* --- nanobot_stop (P4: control-plane-first) ---------------------------------
 *
 * Strategy (in order, escalating only on failure):
 *   1. Reconcile target pid: prefer our tracked gateway pid; if we never
 *      tracked one (e.g. the manager didn't start it) but the state file has
 *      a live pid, adopt that and capture its creation time as the tier-2
 *      PID-reuse guard (a pid that cannot be opened is already dead: treat
 *      it as "not running").
 *   2. Spawn `nanobot gateway stop --timeout 20` (the state file is the
 *      rendezvous -- P3a starts via --background so it is always present).
 *      Capture stdout to a temp file so we can match the result tokens.
 *   3. Parse stdout (case-insensitive substring match -- survives wording
 *      drift between Rich and plain output):
 *        "gateway stopped."      -> clean success
 *        "gateway_not_running"   -> nothing to stop. With a tracked handle,
 *                                   still confirm death via the handle; with
 *                                   none, skip the kill entirely (the CLI's
 *                                   state-file rendezvous saw no gateway).
 *        "gateway_stop_timeout"  -> escalate
 *        "gateway_state_stale"   -> escalate
 *        (any other non-zero)    -> escalate
 *   4. Death confirmation: wait on the tracked handle (8s). If it doesn't
 *      die, escalate to TerminateProcess (tier 1). With no tracked handle,
 *      "gateway_not_running" goes straight to cleanup; every other outcome
 *      escalates to tier 2 (kill_process_tree, PID-reuse-guarded).
 *
 * All handles/resources are closed on every path: stop_pi process+thread,
 * the stdout temp file handle + DeleteFileW, the snapshot handle (closed
 * before clearing state).
 */
int nanobot_stop(void) {
    nm_process_view v;
    nm_state_snapshot(&v);

    /* --- Step 1: reconcile the target pid. --------------------------------- */
    DWORD effective_pid = v.pid;
    /* Create time captured when adopting a state-file pid with no tracked
     * handle: becomes the tier-2 PID-reuse guard below. Zeroed on capture
     * failure => guard inert (mirrors attach step 5). */
    FILETIME adopted_create;
    int      adopted_create_known = 0;
    ZeroMemory(&adopted_create, sizeof(adopted_create));
    if (effective_pid == 0) {
        /* We're not tracking a gateway -- but the user (or another manager
         * instance) may have started one. The state file is the rendezvous:
         * a live pid there means a gateway is running; pid=0 means nothing
         * to stop. */
        nm_gateway_state gs;
        if (nanobot_read_gateway_state(&gs) == 0 && gs.pid == 0) {
            nm_log(NM_LOG_INFO, "process", "nanobot stop: gateway not running");
            /* I2: do NOT flip to IDLE here -- this runs on stop_worker.
             * WM_ASYNC_STOP_DONE owns the STOPPING->IDLE flip (UI thread). */
            return 0;
        }
        if (gs.found && gs.pid != 0) {
            effective_pid = gs.pid;
            nm_log(NM_LOG_INFO, "process", "nanobot stop: adopting state-file pid=%d (no tracked handle)",
                (int)effective_pid);
            if (v.handle == NULL) {
                /* The adopted pid has no tracked handle, so the tier-2 kills
                 * below would otherwise run with no PID-reuse guard at all.
                 * Open it with the attach mask and capture its create time
                 * (mirrors attach step 5). */
                HANDLE adopted = OpenProcess(SYNCHRONIZE |
                                             PROCESS_QUERY_LIMITED_INFORMATION |
                                             PROCESS_TERMINATE, FALSE, effective_pid);
                if (!adopted) {
                    /* Unopenable => pid already dead: stale state file,
                     * nothing to stop. */
                    nm_log(NM_LOG_INFO, "process", "nanobot stop: adopted pid=%d already dead",
                        (int)effective_pid);
                    /* I2: do NOT flip to IDLE here -- same as above. */
                    return 0;
                }
                adopted_create_known =
                    get_process_creation_time(adopted, &adopted_create) ? 1 : 0;
                CloseHandle(adopted);
            }
        }
    }

    if (effective_pid == 0) {
        nm_log(NM_LOG_INFO, "process", "nanobot stop: no pid to stop");
        /* I2: do NOT flip to IDLE here -- same as above. */
        return 0;
    }

    /* Tier-2 PID-reuse guard for every kill_process_tree call below: the
     * create time just captured for an adopted state-file pid, else the
     * tracked v.create_ft while its handle still exists. The crash branch's
     * frozen v.create_ft pairs with the old (cleared) pid, so it is never a
     * valid guard for an adopted pid. NULL when neither is known => inert. */
    const FILETIME* reuse_guard = NULL;
    if (adopted_create_known)
        reuse_guard = &adopted_create;
    else if (v.handle && (v.create_ft.dwLowDateTime || v.create_ft.dwHighDateTime))
        reuse_guard = &v.create_ft;

    /* --- Step 2: resolve the stop-CLI exe (cached from start, else probe). - */
    char resolved_exe[MAX_PATH];
    int  resolved_is_python = 0;
    nm_state_get_resolved_exe(resolved_exe, sizeof(resolved_exe));
    resolved_is_python = nm_state_get_resolved_is_python();

    nm_resolution res;
    ZeroMemory(&res, sizeof(res));
    if (resolved_exe[0] == '\0') {
        /* No cached resolution (e.g. we adopted a pid we didn't start). M8:
         * prefer the state file's `command` field -- it records how the gateway
         * was launched, so we can derive the exe without a ~10s python probe. */
        nm_gateway_state gs;
        if (nanobot_read_gateway_state(&gs) == 0 && gs.found && gs.command[0]) {
            const char* cmd = gs.command;
            const char* exe_start = cmd;
            const char* exe_end = NULL;
            if (cmd[0] == '"') {
                exe_start = cmd + 1;
                exe_end = strchr(exe_start, '"');
            } else {
                exe_end = strchr(cmd, ' ');
            }
            if (exe_end && exe_end > exe_start) {
                size_t len = (size_t)(exe_end - exe_start);
                if (len > 0 && len < sizeof(resolved_exe)) {
                    memcpy(resolved_exe, exe_start, len);
                    resolved_exe[len] = '\0';
                    resolved_is_python = (strstr(cmd, "-m nanobot") != NULL) ? 1 : 0;
                    nm_log(NM_LOG_DEBUG, "process", "nanobot stop: derived exe from state file command "
                        "(%s, is_python=%d)", resolved_exe, resolved_is_python);
                }
            }
        }
    }
    if (resolved_exe[0] == '\0') {
        /* M8: command field absent/unparseable -> fall back to the resolver.
         * quick=1: accept the python tier on existence alone (no ~10s probe) --
         * we just need ANY nanobot exe to invoke `gateway stop`; a false accept
         * fails fast and we escalate to TerminateProcess. */
        if (nanobot_resolve(NULL, &res, 1) && res.found && res.exe) {
            strncpy(resolved_exe, res.exe, sizeof(resolved_exe) - 1);
            resolved_exe[sizeof(resolved_exe) - 1] = '\0';
            resolved_is_python = res.is_python;
        }
        nm_resolution_free(&res);
    }

    if (resolved_exe[0] == '\0') {
        nm_log(NM_LOG_WARN, "process", "nanobot stop: cannot resolve nanobot exe for stop CLI; "
                    "falling back to direct kill pid=%d", (int)effective_pid);
        /* No CLI -> skip tiers 0/1, go straight to the kill tree. */
        if (v.handle) {
            if (!TerminateProcess(v.handle, 0))
                nm_log(NM_LOG_WARN, "process", "nanobot stop: TerminateProcess failed pid=%d err=%d",
                    (int)effective_pid, (int)GetLastError());
            if (WaitForSingleObject(v.handle, 3000) == WAIT_FAILED)
                nm_log(NM_LOG_WARN, "process", "nanobot stop: post-terminate wait failed pid=%d err=%d",
                    (int)effective_pid, (int)GetLastError());
        } else {
            kill_process_tree(effective_pid, reuse_guard);
        }
        goto cleanup;
    }

    /* --- Step 3: spawn `nanobot gateway stop` with stdout -> temp file. ---- */
    wchar_t temp_path[MAX_PATH];
    wchar_t temp_file[MAX_PATH];
    temp_file[0] = L'\0';
    DWORD tplen = GetTempPathW(MAX_PATH, temp_path);
    if (tplen > 0 && tplen < MAX_PATH) {
        /* Unique name (same pattern as run_probe): a predictable
         * %TEMP%\nanobot-stop-<pid>.txt can be pre-created by anything with
         * temp-dir write access, and a fixed name breaks on overlapping
         * stops. GetTempFileNameW reserves the name; CREATE_ALWAYS below
         * re-opens it for the child. */
        if (!GetTempFileNameW(temp_path, L"nms", 0, temp_file))
            temp_file[0] = L'\0';   /* too long / no temp: spawn without capture */
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;          /* child must inherit stdout */
    sa.lpSecurityDescriptor = NULL;

    HANDLE stdout_file = NULL;
    if (temp_file[0]) {
        /* FILE_SHARE_DELETE so a stop CLI that outlives the wait cannot keep
         * its inherited handle blocking the later DeleteFileW cleanup. */
        stdout_file = CreateFileW(temp_file,
            GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa,
            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (stdout_file == INVALID_HANDLE_VALUE) {
            nm_log(NM_LOG_WARN, "process", "nanobot stop: CreateFileW(temp) failed err=%d",
                (int)GetLastError());
            stdout_file = NULL;        /* degrade: spawn w/o capture */
        }
    }

    wchar_t* wide_exe = utf8_to_wide_alloc(resolved_exe);
    wchar_t* cmdline = build_command_line(wide_exe,
        resolved_is_python ? L"-m nanobot gateway stop --timeout 20"
                           : L"gateway stop --timeout 20");

    STARTUPINFOW si;
    PROCESS_INFORMATION stop_pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (stdout_file) {
        si.dwFlags   = STARTF_USESTDHANDLES;
        si.hStdOutput = stdout_file;
        si.hStdError  = stdout_file;
        si.hStdInput  = NULL;
    }

    int spawned = 0;
    if (cmdline && CreateProcessW(NULL, cmdline, NULL, NULL, stdout_file ? TRUE : FALSE,
            CREATE_NO_WINDOW, NULL, NULL, &si, &stop_pi)) {
        spawned = 1;
    } else {
        nm_log(NM_LOG_ERROR, "process", "nanobot stop: CreateProcessW failed err=%d (escalating)",
            (int)GetLastError());
    }
    free(cmdline);
    free(wide_exe);

    int stop_exit_code = -1;
    if (spawned) {
        DWORD wait = WaitForSingleObject(stop_pi.hProcess, 25000);
        if (wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            if (GetExitCodeProcess(stop_pi.hProcess, &code))
                stop_exit_code = (int)code;
        }
        CloseHandle(stop_pi.hThread);
        CloseHandle(stop_pi.hProcess);
    }

    /* We're done writing -- close our copy of the stdout file before reading
     * it back (and unconditionally before DeleteFileW below). */
    if (stdout_file) { CloseHandle(stdout_file); stdout_file = NULL; }

    /* --- Step 4: parse stdout tokens (case-insensitive substring). -------- */
    char stdout_buf[1600];
    stdout_buf[0] = '\0';
    if (temp_file[0]) {
        FILE* tf = _wfopen(temp_file, L"rb");
        if (tf) {
            size_t n = fread(stdout_buf, 1, sizeof(stdout_buf) - 1, tf);
            stdout_buf[n] = '\0';
            fclose(tf);
        }
    }
    for (char* p = stdout_buf; *p; p++) {        /* lowercase in place */
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') *p = (char)(c + ('a' - 'A'));
    }

    int tok_stopped      = (strstr(stdout_buf, "gateway stopped.")      != NULL);
    int tok_not_running  = (strstr(stdout_buf, "gateway_not_running")   != NULL);
    int tok_timeout      = (strstr(stdout_buf, "gateway_stop_timeout")  != NULL);
    int tok_stale        = (strstr(stdout_buf, "gateway_state_stale")   != NULL);

    nm_log(NM_LOG_INFO, "process", "nanobot stop: CLI spawned=%d exit=%d tok(stopped=%d,not_running=%d,"
        "timeout=%d,stale=%d)", spawned, stop_exit_code,
        tok_stopped, tok_not_running, tok_timeout, tok_stale);

    int escalate = 0;
    if (!spawned) {
        escalate = 1;                       /* couldn't even run the CLI */
    } else if (tok_timeout || tok_stale) {
        escalate = 1;                       /* upstream signaled failure */
    } else if (stop_exit_code != 0 && !tok_stopped && !tok_not_running) {
        escalate = 1;                       /* exit 1/2 with no success token */
    }

    /* DeleteFileW unconditionally -- success, parse-fail, timeout, escalate. */
    if (temp_file[0]) DeleteFileW(temp_file);

    /* --- Step 5: death confirmation + fallback tiers. --------------------- */
    if (!escalate) {
        if (v.handle) {
            /* Wait for the gateway handle to signal exit. 8s covers the
             * upstream --timeout 20 only loosely; the CLI already waited,
             * so this is just confirm-or-escalate. */
            DWORD wait = WaitForSingleObject(v.handle, 8000);
            if (wait == WAIT_OBJECT_0) {
                nm_log(NM_LOG_INFO, "process", "nanobot stop: gateway exited cleanly pid=%d",
                    (int)effective_pid);
                goto cleanup;
            }
            nm_log(NM_LOG_WARN, "process", "nanobot stop: clean exit not confirmed, escalating "
                "(tier 1: TerminateProcess) pid=%d", (int)effective_pid);
            if (!TerminateProcess(v.handle, 0))
                nm_log(NM_LOG_WARN, "process", "nanobot stop: TerminateProcess failed pid=%d err=%d",
                    (int)effective_pid, (int)GetLastError());
            if (WaitForSingleObject(v.handle, 3000) == WAIT_FAILED)
                nm_log(NM_LOG_WARN, "process", "nanobot stop: post-terminate wait failed pid=%d err=%d",
                    (int)effective_pid, (int)GetLastError());
            goto cleanup;
        }
        if (tok_not_running) {
            /* The CLI's state-file rendezvous already proved no gateway is
             * running, and we hold no handle to second-guess it with -- the
             * effective pid may since have been recycled. Skip the hard kill. */
            nm_log(NM_LOG_INFO, "process", "nanobot stop: gateway_not_running with no tracked "
                "handle; skipping tier-2 kill pid=%d", (int)effective_pid);
            goto cleanup;
        }
        /* No tracked handle -> tier 2. */
        nm_log(NM_LOG_WARN, "process", "nanobot stop: no tracked handle, escalating (tier 2: "
            "kill_process_tree) pid=%d", (int)effective_pid);
        kill_process_tree(effective_pid, reuse_guard);
        goto cleanup;
    }

    /* Escalation path. */
    nm_log(NM_LOG_WARN, "process", "nanobot stop: escalating after stop CLI (escalate=%d) pid=%d",
        escalate, (int)effective_pid);
    if (v.handle) {
        /* Tier 1: we own the handle, hard-kill it. */
        if (!TerminateProcess(v.handle, 0))
            nm_log(NM_LOG_WARN, "process", "nanobot stop: TerminateProcess failed pid=%d err=%d",
                (int)effective_pid, (int)GetLastError());
        if (WaitForSingleObject(v.handle, 3000) == WAIT_FAILED)
            nm_log(NM_LOG_WARN, "process", "nanobot stop: post-terminate wait failed pid=%d err=%d",
                (int)effective_pid, (int)GetLastError());
    } else {
        /* Tier 2: no handle, kill the tree by pid (reuse-guarded). */
        kill_process_tree(effective_pid, reuse_guard);
    }

cleanup:
    /* C1b: route the close through the centralized helper. It drains any armed
     * RegisterWait (blocking) BEFORE CloseHandle and serializes the close so
     * the WM_CHILD_EXITED thread and this worker can never double-close. No-op
     * if the handle was already cleared (e.g. crash branch stole it first).
     * v.handle reads above (WaitForSingleObject/TerminateProcess) are safe: in
     * the STOPPING case C1a makes WM_CHILD_EXITED return without touching the
     * handle, so stop_worker has exclusive access to v.handle for the duration. */
    nm_state_close_handle();
    nm_state_set_pid(0);
    nm_state_set_start_tick(0);
    {
        /* A stale create_ft would make a later stop that adopts a state-file
         * pid suspect "pid reuse" against the OLD creation time and skip the
         * kill while still reporting success. */
        FILETIME zero_ft;
        ZeroMemory(&zero_ft, sizeof(zero_ft));
        nm_state_set_create_ft(&zero_ft);
    }
    /* NOTE: do NOT nm_state_set_state(NM_STATE_IDLE) here. The state was set to
     * STOPPING by on_stop() on the UI thread, and the WM_ASYNC_STOP_DONE handler
     * owns the STOPPING->IDLE flip. Clearing it here would race with a pending
     * WM_CHILD_EXITED (which classifies STOPPING=expected vs else=crash). */
    return 0;
}
