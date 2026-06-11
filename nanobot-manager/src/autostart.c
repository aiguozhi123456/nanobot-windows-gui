#include "autostart.h"
#include <windows.h>
#include <stdio.h>

static const wchar_t REG_PATH[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t REG_NAME[] = L"nanobot-manager";

int autostart_enable(const char* exe_path) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return -1;

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, exe_path, -1, wpath, MAX_PATH);

    wchar_t value[MAX_PATH + 32];
    swprintf_s(value, sizeof(value)/sizeof(wchar_t), L"\"%s\" --autostart", wpath);

    LONG r = RegSetValueExW(hKey, REG_NAME, 0, REG_SZ,
        (const BYTE*)value, (DWORD)(wcslen(value) + 1) * sizeof(wchar_t));
    RegCloseKey(hKey);
    return (r == ERROR_SUCCESS) ? 0 : -1;
}

int autostart_disable(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return -1;
    LONG r = RegDeleteValueW(hKey, REG_NAME);
    RegCloseKey(hKey);
    return (r == ERROR_SUCCESS) ? 0 : -1;
}

int autostart_is_enabled(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return 0;
    LONG r = RegQueryValueExW(hKey, REG_NAME, NULL, NULL, NULL, NULL);
    RegCloseKey(hKey);
    return (r == ERROR_SUCCESS) ? 1 : 0;
}
