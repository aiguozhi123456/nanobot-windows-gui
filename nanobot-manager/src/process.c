#include "process.h"
#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void process_log(const char* fmt, ...) {
    wchar_t exe_dir[MAX_PATH];
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';

    wchar_t log_path[MAX_PATH];
    swprintf_s(log_path, _countof(log_path),
        L"%s\\nanobot-manager-process.log", exe_dir);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    FILE* f = _wfopen(log_path, L"a");
    if (f) {
        fputs(prefix, f);
        fputs(body, f);
        fputc('\n', f);
        fclose(f);
    }
}

char* find_nanobot_cmd(void) {
    char found[MAX_PATH];

    if (SearchPathA(NULL, "nanobot.exe", NULL, MAX_PATH, found, NULL))
        return _strdup(found);

    const char* candidates[] = {"python.exe", "python3.exe"};
    for (int i = 0; i < 2; i++) {
        if (!SearchPathA(NULL, candidates[i], NULL, MAX_PATH, found, NULL))
            continue;
        char test_cmd[MAX_PATH + 128];
        snprintf(test_cmd, sizeof(test_cmd),
            "\"%s\" -c \"import importlib.util; "
            "exit(0 if importlib.util.find_spec('nanobot') else 1)\"",
            found);
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (CreateProcessA(NULL, test_cmd, NULL, NULL, FALSE,
                CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            DWORD wait = WaitForSingleObject(pi.hProcess, 10000);
            if (wait == WAIT_TIMEOUT)
                TerminateProcess(pi.hProcess, 1);
            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (exit_code == 0)
                return _strdup(found);
        }
    }
    return NULL;
}

int nanobot_detect_install(void) {
    char* cmd = find_nanobot_cmd();
    if (cmd) { free(cmd); return 1; }
    return 0;
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

static int process_cmdline_contains(DWORD pid, const wchar_t* substr) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) return 0;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) { CloseHandle(h); return 0; }

    typedef NTSTATUS (NTAPI *fn_query)(HANDLE, PROCESSINFOCLASS,
                                        PVOID, ULONG, PULONG);
    fn_query NtQIP = (fn_query)GetProcAddress(ntdll, "NtQueryInformationProcess");
    if (!NtQIP) { CloseHandle(h); return 0; }

    PROCESS_BASIC_INFORMATION pbi;
    ULONG retlen;
    if (NtQIP(h, ProcessBasicInformation, &pbi, sizeof(pbi), &retlen) < 0) {
        CloseHandle(h);
        return 0;
    }

    PEB peb;
    SIZE_T read;
    if (!ReadProcessMemory(h, pbi.PebBaseAddress, &peb, sizeof(peb), &read)) {
        CloseHandle(h);
        return 0;
    }

    RTL_USER_PROCESS_PARAMETERS params;
    if (!ReadProcessMemory(h, peb.ProcessParameters, &params,
            sizeof(params), &read)) {
        CloseHandle(h);
        return 0;
    }

    int wchar_count = params.CommandLine.Length / sizeof(wchar_t);
    wchar_t* cmdline = (wchar_t*)malloc((wchar_count + 1) * sizeof(wchar_t));
    if (!cmdline) { CloseHandle(h); return 0; }

    BOOL ok = ReadProcessMemory(h, params.CommandLine.Buffer, cmdline,
                                 params.CommandLine.Length, &read);
    CloseHandle(h);
    if (!ok) { free(cmdline); return 0; }
    cmdline[wchar_count] = L'\0';

    int found = (wcsstr(cmdline, substr) != NULL);
    free(cmdline);
    return found;
}

int find_nanobot_python_pid(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int pid = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"nanobot.exe") == 0) {
                pid = (int)pe.th32ProcessID;
                break;
            }

            if (_wcsicmp(pe.szExeFile, L"python.exe") != 0 &&
                _wcsicmp(pe.szExeFile, L"python3.exe") != 0 &&
                _wcsicmp(pe.szExeFile, L"pythonw.exe") != 0)
                continue;

            if (process_cmdline_contains(pe.th32ProcessID, L"nanobot")) {
                pid = (int)pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return pid;
}

static unsigned long long filetime_to_ull(FILETIME ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
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

static HANDLE g_nanobot_handle = NULL;

void nanobot_set_handle(void* handle) {
    if (g_nanobot_handle) {
        CloseHandle(g_nanobot_handle);
    }
    g_nanobot_handle = (HANDLE)handle;
}

void* nanobot_get_handle(void) {
    return (void*)g_nanobot_handle;
}

void nanobot_clear_handle(void) {
    if (g_nanobot_handle) {
        CloseHandle(g_nanobot_handle);
        g_nanobot_handle = NULL;
    }
}

int nanobot_start(const char* cmd, int* out_pid) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    char workdir[MAX_PATH];
    strncpy(workdir, cmd, MAX_PATH - 1);
    workdir[MAX_PATH - 1] = '\0';
    char* slash = strrchr(workdir, '\\');
    if (!slash) slash = strrchr(workdir, '/');
    if (slash) *slash = '\0'; else workdir[0] = '\0';

    const char* base = strrchr(cmd, '\\');
    if (!base) base = strrchr(cmd, '/');
    base = base ? base + 1 : cmd;

    char cmdline[MAX_PATH * 2];
    if (_stricmp(base, "nanobot.exe") == 0)
        snprintf(cmdline, sizeof(cmdline), "\"%s\" gateway", cmd);
    else
        snprintf(cmdline, sizeof(cmdline), "\"%s\" -m nanobot gateway", cmd);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            NULL, workdir[0] ? workdir : NULL, &si, &pi))
        return (int)GetLastError();

    if (out_pid) *out_pid = (int)pi.dwProcessId;
    CloseHandle(pi.hThread);
    nanobot_set_handle(pi.hProcess);
    process_log("nanobot started (pid=%d)", (int)pi.dwProcessId);
    return 0;
}

static void kill_process_tree(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ParentProcessID == pid)
                    kill_process_tree(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (h) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }
}

int nanobot_stop(int pid, int port) {
    (void)port;

    if (pid <= 0)
        pid = find_nanobot_python_pid();

    if (pid <= 0) {
        process_log("nanobot stop: pid not found");
        return -1;
    }

    HANDLE handle = g_nanobot_handle;
    if (handle) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(handle, &exit_code) && exit_code != STILL_ACTIVE) {
            process_log("nanobot stop: already exited (pid=%d)", pid);
            CloseHandle(handle);
            g_nanobot_handle = NULL;
            return 0;
        }

        process_log("nanobot stop: sending CTRL_BREAK_EVENT (pid=%d)", pid);
        FreeConsole();
        if (AttachConsole((DWORD)pid)) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)pid);
            FreeConsole();
        } else {
            process_log("nanobot stop: AttachConsole failed (err=%d), will wait", (int)GetLastError());
            FreeConsole();
        }

        DWORD wait = WaitForSingleObject(handle, 5000);
        if (wait == WAIT_TIMEOUT) {
            process_log("nanobot stop: graceful shutdown timed out, force killing (pid=%d)", pid);
            kill_process_tree((DWORD)pid);
        } else {
            process_log("nanobot stop: graceful shutdown succeeded (pid=%d)", pid);
        }

        CloseHandle(handle);
        g_nanobot_handle = NULL;
    } else {
        process_log("nanobot stop: no handle, force killing (pid=%d)", pid);
        kill_process_tree((DWORD)pid);
    }
    return 0;
}
