#include "process.h"
#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int process_cmdline_contains(DWORD pid, const char* substr) {
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

    int bufsz = params.CommandLine.Length + 2;
    char* cmdline = (char*)malloc(bufsz);
    if (!cmdline) { CloseHandle(h); return 0; }

    BOOL ok = ReadProcessMemory(h, params.CommandLine.Buffer, cmdline,
                                params.CommandLine.Length, &read);
    CloseHandle(h);
    if (!ok) { free(cmdline); return 0; }
    cmdline[params.CommandLine.Length] = '\0';

    int found = (strstr(cmdline, substr) != NULL);
    free(cmdline);
    return found;
}

static int find_nanobot_python_pid(void) {
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

            if (process_cmdline_contains(pe.th32ProcessID, "nanobot")) {
                pid = (int)pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return pid;
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
            CREATE_NO_WINDOW | DETACHED_PROCESS,
            NULL, workdir[0] ? workdir : NULL, &si, &pi))
        return (int)GetLastError();

    if (out_pid) *out_pid = (int)pi.dwProcessId;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

int nanobot_stop(int pid, int port) {
    (void)port;

    if (pid > 0) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
        if (h) {
            BOOL ok = TerminateProcess(h, 0);
            if (ok) WaitForSingleObject(h, 5000);
            CloseHandle(h);
            return ok ? 0 : (int)GetLastError();
        }
    }

    pid = find_nanobot_python_pid();
    if (pid > 0) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
        if (h) {
            BOOL ok = TerminateProcess(h, 0);
            if (ok) WaitForSingleObject(h, 5000);
            CloseHandle(h);
            return ok ? 0 : (int)GetLastError();
        }
    }

    return -1;
}
