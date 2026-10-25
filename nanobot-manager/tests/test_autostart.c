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
#include <wchar.h>

static const wchar_t REG_PATH[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t REG_NAME[] = L"nanobot-manager";

/* Snapshot of the original registry value for restoration. `present` and
 * `captured` are separate states: a value that exists but could not be read
 * must never be treated as absent, or restore would delete the user's entry. */
typedef struct {
    int   present;          /* 1 if the value existed before the test */
    int   captured;         /* 1 if its bytes were successfully saved */
    DWORD type;             /* original value type (REG_SZ / REG_EXPAND_SZ) */
    BYTE* data;             /* malloc'd original bytes */
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
    if (q == ERROR_SUCCESS) {
        s->present = 1;   /* the value exists — never treat as absent */
        BYTE* buf = (BYTE*)malloc(size ? size : 1);
        if (buf && RegQueryValueExW(hKey, REG_NAME, NULL, &type, buf, &size)
                       == ERROR_SUCCESS) {
            s->captured = 1;
            s->type = type;
            s->data = buf;
            s->data_size = size;
        } else {
            free(buf);
            nm_check_failed = 1;
            printf("  [FAIL] existing Run value could not be captured\n");
        }
    } else if (q != ERROR_FILE_NOT_FOUND) {
        /* Query failed for another reason: original state unknown. Mark it
         * present so restore refuses to delete what we could not read. */
        s->present = 1;
        nm_check_failed = 1;
        printf("  [FAIL] Run value query failed (err=%ld)\n", q);
    }
    RegCloseKey(hKey);
}

/* Restore the captured value, or delete the value if it was originally
 * absent. Restoration failures latch a test failure: leaving a bogus
 * "C:\nm\test_autostart.exe --autostart" behind would launch at every
 * logon. */
static void snapshot_restore(const reg_snapshot* s) {
    /* Unknown original state: do not delete what we may have destroyed. */
    if (s->present && !s->captured) return;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_SET_VALUE, &hKey)
            != ERROR_SUCCESS) {
        nm_check_failed = 1;
        printf("  [FAIL] snapshot_restore: cannot open Run key\n");
        return;
    }
    LONG r;
    if (s->present && s->data) {
        r = RegSetValueExW(hKey, REG_NAME, 0, s->type, s->data, s->data_size);
    } else {
        r = RegDeleteValueW(hKey, REG_NAME);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;   /* already absent */
    }
    RegCloseKey(hKey);
    if (r != ERROR_SUCCESS) {
        nm_check_failed = 1;
        printf("  [FAIL] snapshot_restore failed (err=%ld)\n", r);
    }
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

    /* Guarded inputs must be rejected and must not write anything. */
    TEST_CHECK(autostart_enable(NULL) == -1);
    TEST_CHECK(autostart_enable(L"") == -1);
    TEST_CHECK(autostart_is_enabled() == 0);
    {
        static wchar_t too_long[MAX_PATH * 2];
        wmemset(too_long, L'a', MAX_PATH);   /* MAX_PATH chars > bound */
        too_long[MAX_PATH] = L'\0';
        TEST_CHECK(autostart_enable(too_long) == -1);
        TEST_CHECK(autostart_is_enabled() == 0);
    }

    /* Enable with a path, then observe the value appears. */
    const wchar_t* test_path = L"C:\\nm\\test_autostart.exe";
    TEST_CHECK(autostart_enable(test_path) == 0);
    TEST_CHECK(autostart_is_enabled() == 1);

    /* The written value must include the path and the --autostart flag.
     * Registry data is copied verbatim, so verify the type and force NUL
     * termination inside the received byte count before scanning. */
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_QUERY_VALUE, &hKey)
            == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH + 64];
        DWORD sz = sizeof(value), type = 0;
        LONG q = RegQueryValueExW(hKey, REG_NAME, NULL, &type, (LPBYTE)value, &sz);
        RegCloseKey(hKey);
        if (q == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)
                && sz >= sizeof(wchar_t) && sz <= sizeof(value)) {
            value[(sz / sizeof(wchar_t)) - 1] = L'\0';
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
            printf("  [FAIL] unexpected Run value after enable (q=%ld type=%lu sz=%lu)\n",
                   q, (unsigned long)type, (unsigned long)sz);
        }
    } else {
        nm_check_failed = 1;
        printf("  [FAIL] could not open Run key for verification\n");
    }

    /* Disable, then observe the value is gone. */
    TEST_CHECK(autostart_disable() == 0);
    TEST_CHECK(autostart_is_enabled() == 0);

    /* Current behavior: disable is not idempotent and returns -1 when the
     * value is already absent (RegDeleteValueW -> ERROR_FILE_NOT_FOUND).
     * Assert the observable contract (the value stays gone) instead of the
     * incidental return code. */
    autostart_disable();
    TEST_CHECK(autostart_is_enabled() == 0);

    /* ALWAYS restore the user's original state. */
    snapshot_restore(&snap);
    snapshot_free(&snap);
}

TEST_LIST = {
    { "autostart_roundtrip", test_autostart_roundtrip },
    { NULL, NULL }
};
