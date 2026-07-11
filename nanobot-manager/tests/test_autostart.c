/*
 * Unit tests for nanobot-manager autostart (registry round-trip).
 *
 * Exercises the real HKCU\...\Run\nanobot-manager value: enable / is_enabled /
 * disable. To stay side-effect-free on the user's machine, the original value
 * is captured up front and restored at the end (the test body never aborts on
 * assertion failure, so cleanup always runs).
 */
#include "nm_test.h"
#include "autostart.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

static const wchar_t REG_PATH[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t REG_NAME[] = L"nanobot-manager";

/* Snapshot of the original registry value for restoration. */
typedef struct {
    int   present;          /* 1 if the value existed before the test */
    BYTE* data;             /* malloc'd original bytes (REG_SZ) */
    DWORD data_size;        /* size in bytes incl. trailing NUL (if any) */
} reg_snapshot;

/* Capture the current value (if any) so it can be restored later. */
static void snapshot_capture(reg_snapshot* s) {
    memset(s, 0, sizeof(*s));
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_QUERY_VALUE, &hKey)
            != ERROR_SUCCESS) {
        return;   /* key absent: nothing to restore */
    }
    DWORD type = 0, size = 0;
    LONG q = RegQueryValueExW(hKey, REG_NAME, NULL, &type, NULL, &size);
    if (q == ERROR_SUCCESS && size > 0) {
        BYTE* buf = (BYTE*)malloc(size);
        if (buf && RegQueryValueExW(hKey, REG_NAME, NULL, &type, buf, &size)
                       == ERROR_SUCCESS) {
            s->present = 1;
            s->data = buf;
            s->data_size = size;
        } else {
            free(buf);
        }
    }
    RegCloseKey(hKey);
}

/* Restore the captured value, or delete the value if it was originally absent. */
static void snapshot_restore(const reg_snapshot* s) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_SET_VALUE, &hKey)
            != ERROR_SUCCESS) {
        return;
    }
    if (s->present && s->data) {
        RegSetValueExW(hKey, REG_NAME, 0, REG_SZ, s->data, s->data_size);
    } else {
        RegDeleteValueW(hKey, REG_NAME);
    }
    RegCloseKey(hKey);
}

static void snapshot_free(reg_snapshot* s) {
    free(s->data);
    s->data = NULL;
}

/* --- test ------------------------------------------------------------------ */

static void test_autostart_roundtrip(void) {
    reg_snapshot snap;
    snapshot_capture(&snap);

    /* Clean slate regardless of prior state. */
    autostart_disable();
    TEST_CHECK(autostart_is_enabled() == 0);

    /* Enable with a path, then observe the value appears. */
    const char* test_path = "C:\\nm\\test_autostart.exe";
    TEST_CHECK(autostart_enable(test_path) == 0);
    TEST_CHECK(autostart_is_enabled() == 1);

    /* The written value must include the path and the --autostart flag. */
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_QUERY_VALUE, &hKey)
            == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH + 64];
        DWORD sz = sizeof(value);
        LONG q = RegQueryValueExW(hKey, REG_NAME, NULL, NULL, (LPBYTE)value, &sz);
        RegCloseKey(hKey);
        if (q == ERROR_SUCCESS) {
            if (wcsstr(value, L"--autostart") == NULL) {
                nm_check_failed = 1;
                printf("  [FAIL] written value missing --autostart flag\n");
            }
            if (wcsstr(value, L"test_autostart.exe") == NULL) {
                nm_check_failed = 1;
                printf("  [FAIL] written value missing the configured path\n");
            }
        } else {
            nm_check_failed = 1;
            printf("  [FAIL] could not query value after enable (err=%ld)\n", q);
        }
    } else {
        nm_check_failed = 1;
        printf("  [FAIL] could not open Run key for verification\n");
    }

    /* Disable, then observe the value is gone. */
    TEST_CHECK(autostart_disable() == 0);
    TEST_CHECK(autostart_is_enabled() == 0);

    /* Documented current behavior: disabling when the value is already absent
     * returns -1 (RegDeleteValueW fails with ERROR_FILE_NOT_FOUND). The value
     * stays gone either way. This is the real product semantic, not idempotent. */
    TEST_CHECK(autostart_disable() == -1);
    TEST_CHECK(autostart_is_enabled() == 0);

    /* ALWAYS restore the user's original state. */
    snapshot_restore(&snap);
    snapshot_free(&snap);
}

TEST_LIST = {
    { "autostart_roundtrip", test_autostart_roundtrip },
    { NULL, NULL }
};
