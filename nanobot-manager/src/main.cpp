#include "config.h"
#include "process.h"
#include "autostart.h"
#include "version.h"

#include "ui_core.h"

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UiPage    g_page  = 0;
static UiWindow  g_win   = 0;
static app_config g_cfg  = {0};
static int       g_installed = 0;
static int       g_nanobot_pid = 0;
static ULONGLONG g_start_tick = 0;
static UINT_PTR  g_uptime_timer = 0;
static int       g_theme_mode = 0;

#define WM_ASYNC_FIND_CMD   (WM_APP + 1)
#define WM_ASYNC_STOP_DONE  (WM_APP + 2)
#define TIMER_HEALTH_CHECK_AFTER_START 1001
#define TIMER_HEALTH_CHECK_AFTER_STOP  1002
#define TIMER_UPTIME 1003

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

static void page_set_int(const char* name, int val) {
    wchar_t w[32];
    swprintf_s(w, _countof(w), L"%d", val);
    ui_page_set_text(g_page, name, w);
}

static void update_uptime(void) {
    if (!g_start_tick) return;
    ULONGLONG elapsed = (GetTickCount64() - g_start_tick) / 1000;
    unsigned int s = (unsigned int)(elapsed % 60);
    unsigned int m = (unsigned int)((elapsed / 60) % 60);
    unsigned long long h = (unsigned long long)(elapsed / 3600);
    wchar_t w[16];
    if (h > 0)
        swprintf_s(w, _countof(w), L"%llu:%02u:%02u", h, m, s);
    else
        swprintf_s(w, _countof(w), L"%02u:%02u", m, s);
    ui_page_set_text(g_page, "uptime", w);
}

static VOID CALLBACK on_timer_uptime(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)tick;
    update_uptime();
}

static void start_uptime_timer(int pid) {
    ULONGLONG now = GetTickCount64();
    unsigned long long process_uptime = 0;
    if (nanobot_get_process_uptime_seconds(pid, &process_uptime))
        g_start_tick = now - process_uptime * 1000ULL;
    else
        g_start_tick = now;

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    g_uptime_timer = SetTimer(hwnd, TIMER_UPTIME, 1000, on_timer_uptime);
    update_uptime();
}

static void stop_uptime_timer(void) {
    if (g_uptime_timer) {
        KillTimer((HWND)ui_window_hwnd(g_win), TIMER_UPTIME);
        g_uptime_timer = 0;
    }
    g_start_tick = 0;
    ui_page_set_text(g_page, "uptime", L"00:00");
    ui_page_set_text(g_page, "pid", L"-");
}

static void set_status(const char* status, const char* text, const char* sub) {
    page_set("status", status);
    page_set("statusText", text);
    page_set("statusSub", sub);

    if (strcmp(status, "running") == 0) {
        ui_page_set_bool(g_page, "canStart", 0);
        ui_page_set_bool(g_page, "canStop", 1);
    } else if (strcmp(status, "starting") == 0) {
        ui_page_set_bool(g_page, "canStart", 0);
        ui_page_set_bool(g_page, "canStop", 0);
    } else {
        ui_page_set_bool(g_page, "canStart", 1);
        ui_page_set_bool(g_page, "canStop", 0);
    }
}

static void on_timer_health_check(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)tick;
    KillTimer(hwnd, id);
    do_health_check();
}

static void do_health_check(void) {
    if (!g_installed) {
        set_status("stopped", "Not installed", "nanobot is not installed");
        ui_page_set_bool(g_page, "installed", 0);
        return;
    }

    ui_page_set_bool(g_page, "installed", 1);

    int running = nanobot_check_running(g_cfg.health_check_port);
    if (running) {
        int pid = find_nanobot_python_pid();
        if (pid <= 0 && g_nanobot_pid > 0)
            pid = g_nanobot_pid;
        g_nanobot_pid = pid;
        set_status("running", "Running", "nanobot gateway is active");
        page_set_int("pid", pid > 0 ? pid : 0);
        if (!g_start_tick)
            start_uptime_timer(pid);
    } else {
        set_status("stopped", "Stopped", "nanobot is not running");
        stop_uptime_timer();
        g_nanobot_pid = 0;
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
        set_status("stopped", "Not found",
            "Cannot find nanobot. Please install or set path.");
        return;
    }

    g_nanobot_pid = 0;
    int rc = nanobot_start(cmd, &g_nanobot_pid);
    free(cmd);

    if (rc != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Start failed (error %d)", rc);
        set_status("stopped", "Start failed", buf);
        return;
    }

    set_status("starting", "Starting\u2026", "waiting for health check");
    page_set_int("pid", g_nanobot_pid);

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
    if (msg == WM_ASYNC_STOP_DONE) {
        HWND hwnd2 = (HWND)ui_window_hwnd(g_win);
        SetTimer(hwnd2, TIMER_HEALTH_CHECK_AFTER_STOP, 800, on_timer_health_check);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void on_start(UiWidget w, void* ud) {
    (void)w; (void)ud;

    if (!g_installed) {
        set_status("stopped", "Not installed", "pip install nanobot-ai");
        ui_page_set_bool(g_page, "installed", 0);
        return;
    }

    set_status("starting", "Starting\u2026", "detecting nanobot\u2026");

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    CreateThread(NULL, 0, find_cmd_worker, (LPVOID)hwnd, 0, NULL);
}

static DWORD WINAPI stop_worker(LPVOID param) {
    HWND hwnd = (HWND)param;
    nanobot_stop(g_nanobot_pid, g_cfg.health_check_port);
    g_nanobot_pid = 0;
    PostMessageW(hwnd, WM_ASYNC_STOP_DONE, 0, 0);
    return 0;
}

static void on_stop(UiWidget w, void* ud) {
    (void)w; (void)ud;
    set_status("starting", "Stopping\u2026", "shutting down nanobot");

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    CreateThread(NULL, 0, stop_worker, (LPVOID)hwnd, 0, NULL);
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

static void on_change_path(UiWidget w, void* ud) {
    (void)w; (void)ud;

    wchar_t file[MAX_PATH] = {0};
    if (g_cfg.nanobot_path && g_cfg.nanobot_path[0]) {
        MultiByteToWideChar(CP_UTF8, 0, g_cfg.nanobot_path, -1, file, MAX_PATH);
    }

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)ui_window_hwnd(g_win);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Executable\0*.exe\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Select nanobot executable";

    if (GetOpenFileNameW(&ofn)) {
        char utf8[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, MAX_PATH, NULL, NULL);

        if (g_cfg.nanobot_path) free(g_cfg.nanobot_path);
        g_cfg.nanobot_path = _strdup(utf8);
        config_save(&g_cfg);

        ui_page_set_text(g_page, "nanobotPath", file);
    }
}

static void on_open_webui(UiWidget w, void* ud) {
    (void)w; (void)ud;
    wchar_t url[128];
    int webui_port = g_cfg.health_check_port + 1;
    swprintf_s(url, _countof(url), L"http://localhost:%d", webui_port);
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

static void apply_theme(int mode) {
    g_theme_mode = mode;
    g_cfg.theme_mode = mode;
    if (mode == 1)
        ui_theme_set_mode(UI_THEME_DARK);
    else if (mode == 2)
        ui_theme_set_mode(UI_THEME_LIGHT);
    else {
        HKEY hk;
        DWORD val = 0, sz = sizeof(val);
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                0, KEY_READ, &hk) == ERROR_SUCCESS) {
            RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&val, &sz);
            RegCloseKey(hk);
        }
        ui_theme_set_mode(val ? UI_THEME_LIGHT : UI_THEME_DARK);
    }

    const char* labels[] = { "system", "dark", "light" };
    page_set("theme", labels[mode]);
    config_save(&g_cfg);
}

static void on_theme_light(UiWidget w, void* ud) {
    (void)w; (void)ud;
    apply_theme(2);
}

static void on_theme_dark(UiWidget w, void* ud) {
    (void)w; (void)ud;
    apply_theme(1);
}

static void on_theme_system(UiWidget w, void* ud) {
    (void)w; (void)ud;
    apply_theme(0);
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

static void on_btn_change_path_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_change_path, NULL);
}

static void on_btn_webui_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_open_webui, NULL);
}

static void on_theme_light_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_theme_light, NULL);
}

static void on_theme_dark_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_theme_dark, NULL);
}

static void on_theme_system_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_theme_system, NULL);
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

    int initial_theme = g_cfg.theme_mode;
    if (initial_theme == 0) {
        HKEY hk;
        DWORD val = 0, sz = sizeof(val);
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                0, KEY_READ, &hk) == ERROR_SUCCESS) {
            RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&val, &sz);
            RegCloseKey(hk);
        }
        initial_theme = val ? 2 : 1;
    }
    ui_init_with_theme(initial_theme == 1 ? UI_THEME_DARK : UI_THEME_LIGHT);
    ui_theme_set_cjk_font(L"Segoe UI", L"Microsoft YaHei UI");
    ui_theme_set_accent_hex("#4a7a8c");

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

    ui_page_set_text(g_page, "status", L"stopped");
    page_set("statusText", "Stopped");
    page_set("statusSub", "nanobot is not running");
    ui_page_set_bool(g_page, "installed", g_installed);
    ui_page_set_bool(g_page, "canStart", g_installed ? 1 : 0);
    ui_page_set_bool(g_page, "canStop", 0);
    ui_page_set_bool(g_page, "autostart", autostart_is_enabled());

    wchar_t port_w[16];
    swprintf_s(port_w, _countof(port_w), L"%d", g_cfg.health_check_port);
    ui_page_set_text(g_page, "port", port_w);

    if (g_cfg.nanobot_path && g_cfg.nanobot_path[0]) {
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, g_cfg.nanobot_path, -1, wpath, MAX_PATH);
        ui_page_set_text(g_page, "nanobotPath", wpath);
    } else {
        page_set("nanobotPath", "Auto-detect");
    }

    apply_theme(g_cfg.theme_mode);

    {
        wchar_t ver_w[32];
        swprintf_s(ver_w, _countof(ver_w), L"v" NANOBOT_MANAGER_VERSION);
        ui_page_set_text(g_page, "footer_version", ver_w);
    }

    ui_page_on_widget_mount(g_page, "btn_start", on_btn_start_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_stop",  on_btn_stop_mount,  NULL);
    ui_page_on_widget_mount(g_page, "toggle_autostart", on_toggle_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_change_path", on_btn_change_path_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_webui", on_btn_webui_mount, NULL);
    ui_page_on_widget_mount(g_page, "theme_light", on_theme_light_mount, NULL);
    ui_page_on_widget_mount(g_page, "theme_dark", on_theme_dark_mount, NULL);
    ui_page_on_widget_mount(g_page, "theme_system", on_theme_system_mount, NULL);

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

    stop_uptime_timer();
    RemoveWindowSubclass(hwnd, subclass_proc, 0);
    ui_page_destroy(g_page);
    config_free(&g_cfg);
    return ret;
}
