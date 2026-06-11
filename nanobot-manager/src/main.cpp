#include "config.h"
#include "process.h"
#include "autostart.h"

#include "ui_core.h"

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UiPage    g_page  = 0;
static UiWindow  g_win   = 0;
static app_config g_cfg  = {0};
static int       g_installed = 0;
static int       g_nanobot_pid = 0;

#define WM_ASYNC_FIND_CMD   (WM_APP + 1)
#define TIMER_HEALTH_CHECK_AFTER_START 1001
#define TIMER_HEALTH_CHECK_AFTER_STOP  1002

static void do_health_check(void);

static void page_set(const char* name, const char* utf8) {
    wchar_t w[1024];
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return;
    if (len > 1024) len = 1024;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, len);
    w[len - 1] = L'\0';
    ui_page_set_text(g_page, name, w);
}

static void on_timer_health_check(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)tick;
    KillTimer(hwnd, id);
    do_health_check();
}

static void do_health_check(void) {
    page_set("errorMessage", "");

    if (!g_installed) {
        ui_page_set_text(g_page, "status", L"error");
        page_set("statusText", "nanobot not installed");
        return;
    }

    int running = nanobot_check_running(g_cfg.health_check_port);
    if (running) {
        ui_page_set_text(g_page, "status", L"running");
        page_set("statusText", "nanobot running");
    } else {
        ui_page_set_text(g_page, "status", L"stopped");
        page_set("statusText", "nanobot not running");
    }
}

static DWORD WINAPI find_cmd_worker(LPVOID param) {
    HWND hwnd = (HWND)param;
    char* cmd = find_nanobot_cmd();
    if (!cmd && g_cfg.nanobot_path && g_cfg.nanobot_path[0])
        cmd = _strdup(g_cfg.nanobot_path);
    PostMessageW(hwnd, WM_ASYNC_FIND_CMD, 0, (LPARAM)cmd);
    return 0;
}

static void handle_async_find_cmd(char* cmd) {
    if (!cmd) {
        ui_page_set_text(g_page, "status", L"error");
        page_set("statusText", "nanobot not found");
        page_set("errorMessage",
            "Cannot find nanobot. Please install or set path in config.");
        return;
    }

    g_nanobot_pid = 0;
    int rc = nanobot_start(cmd, &g_nanobot_pid);
    free(cmd);

    if (rc != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Start failed (error %d)", rc);
        page_set("errorMessage", buf);
    }

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    SetTimer(hwnd, TIMER_HEALTH_CHECK_AFTER_START, 2000, on_timer_health_check);
}

static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR uid, DWORD_PTR ref) {
    (void)uid; (void)ref;
    if (msg == WM_ASYNC_FIND_CMD) {
        char* cmd = (char*)lp;
        handle_async_find_cmd(cmd);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void on_start(UiWidget w, void* ud) {
    (void)w; (void)ud;

    if (!g_installed) {
        ui_page_set_text(g_page, "status", L"error");
        page_set("statusText", "nanobot not installed");
        page_set("errorMessage",
            "Please install nanobot: pip install nanobot-ai");
        return;
    }

    page_set("statusText", "Detecting...");
    ui_window_invalidate(g_win);

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    CreateThread(NULL, 0, find_cmd_worker, (LPVOID)hwnd, 0, NULL);
}

static void on_stop(UiWidget w, void* ud) {
    (void)w; (void)ud;
    page_set("statusText", "Stopping...");
    ui_window_invalidate(g_win);

    nanobot_stop(g_nanobot_pid, g_cfg.health_check_port);
    g_nanobot_pid = 0;

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    SetTimer(hwnd, TIMER_HEALTH_CHECK_AFTER_STOP, 800, on_timer_health_check);
}

static void on_autostart_toggle(UiWidget w, int val, void* ud) {
    (void)w; (void)ud;
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);

    if (val) {
        autostart_enable(exe_path);
        g_cfg.autostart_nanobot = 1;
    } else {
        autostart_disable();
        g_cfg.autostart_nanobot = 0;
    }
    config_save(&g_cfg);
}

static void on_btn_start_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_start, NULL);
}

static void on_btn_stop_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_stop, NULL);
}

static void on_toggle_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_toggle_on_changed(w, on_autostart_toggle, NULL);
}

static int has_arg(LPCWSTR cmd, const wchar_t* arg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmd, &argc);
    if (!argv) return 0;
    int found = 0;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], arg) == 0) { found = 1; break; }
    }
    LocalFree(argv);
    return found;
}

static void autostart_log(const char* msg) {
    wchar_t exe_dir[MAX_PATH];
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';

    wchar_t log_path[MAX_PATH];
    swprintf_s(log_path, _countof(log_path),
        L"%s\\nanobot-manager-autostart.log", exe_dir);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[512];
    snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, msg);

    FILE* f = _wfopen(log_path, L"a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}

static int run_autostart_mode(void) {
    if (!g_cfg.autostart_nanobot)
        return 0;
    g_installed = nanobot_detect_install();
    if (!g_installed) {
        autostart_log("nanobot not installed, skipping");
        return 0;
    }

    if (nanobot_check_running(g_cfg.health_check_port)) {
        autostart_log("nanobot already running");
        return 0;
    }

    char* cmd = find_nanobot_cmd();
    if (!cmd && g_cfg.nanobot_path && g_cfg.nanobot_path[0])
        cmd = _strdup(g_cfg.nanobot_path);
    if (cmd) {
        int pid = 0;
        int rc = nanobot_start(cmd, &pid);
        free(cmd);
        if (rc == 0) {
            int healthy = 0;
            for (int i = 0; i < 10; i++) {
                Sleep(1000);
                if (nanobot_check_running(g_cfg.health_check_port)) {
                    healthy = 1;
                    break;
                }
            }
            if (healthy)
                autostart_log("nanobot started and health check passed");
            else
                autostart_log("nanobot process created but health check failed after 10s");
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "nanobot start failed (error %d)", rc);
            autostart_log(buf);
        }
    } else {
        autostart_log("nanobot command not found");
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    (void)hInst; (void)hPrev; (void)cmdLine; (void)nShow;

    config_load(&g_cfg);

    LPCWSTR wcmd = GetCommandLineW();
    if (has_arg(wcmd, L"--autostart")) {
        int rc = run_autostart_mode();
        config_free(&g_cfg);
        return rc;
    }

    g_installed = nanobot_detect_install();

    ui_init_with_theme(UI_THEME_LIGHT);
    ui_theme_set_cjk_font(L"Segoe UI", L"Microsoft YaHei UI");

    wchar_t exe_dir[MAX_PATH];
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';

    wchar_t uix_path[MAX_PATH];
    swprintf_s(uix_path, _countof(uix_path), L"%s\\ui\\dashboard.uix", exe_dir);

    g_page = ui_page_load_file(uix_path);
    if (!g_page) {
        MessageBoxW(NULL, uix_path, L"nanobot-manager - UI load failed",
            MB_ICONERROR);
        config_free(&g_cfg);
        return 1;
    }

    ui_page_set_text(g_page, "status", L"connecting");
    page_set("statusText", "Detecting...");
    ui_page_set_bool(g_page, "installed", g_installed);
    ui_page_set_bool(g_page, "autostart", autostart_is_enabled());

    ui_page_on_widget_mount(g_page, "btn_start", on_btn_start_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_stop",  on_btn_stop_mount,  NULL);
    ui_page_on_widget_mount(g_page, "toggle_autostart", on_toggle_mount, NULL);

    g_win = ui_page_open_window(g_page, NULL);
    if (!g_win) {
        ui_page_destroy(g_page);
        config_free(&g_cfg);
        return 2;
    }

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    SetWindowSubclass(hwnd, subclass_proc, 0, 0);

    do_health_check();

    int ret = ui_run();

    RemoveWindowSubclass(hwnd, subclass_proc, 0);
    ui_page_destroy(g_page);
    config_free(&g_cfg);
    return ret;
}
