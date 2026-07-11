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
static int       g_tray_enabled_effective = 0;
static int       g_allow_exit = 0;
static int       g_tray_added = 0;
static int       g_tray_icon_owned = 0;
static HICON     g_tray_icon = NULL;
static HWND      g_hwnd = NULL;
static int       g_autostart_silent = 0;

#define WM_ASYNC_FIND_CMD   (WM_APP + 1)
#define WM_ASYNC_STOP_DONE  (WM_APP + 2)
#define WM_TRAY_ICON        (WM_APP + 3)
#define TIMER_HEALTH_CHECK_AFTER_START 1001
#define TIMER_HEALTH_CHECK_AFTER_STOP  1002
#define TIMER_UPTIME 1003
#define ID_TRAY_SHOW 40001
#define ID_TRAY_QUIT 40002

static void do_health_check(void);

static void tray_remove(void);

static void show_main_window(void) {
    if (!g_win) return;
    ui_window_show(g_win);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
}

static void tray_show_menu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show window");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

static void tray_add(HWND hwnd) {
    if (g_tray_added) return;

    g_tray_icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR);
    if (!g_tray_icon)
        g_tray_icon = LoadIconW(NULL, IDI_APPLICATION);
    else
        g_tray_icon_owned = 1;

    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_ICON;
    nid.hIcon = g_tray_icon;
    wcscpy_s(nid.szTip, L"nanobot manager");

    if (Shell_NotifyIconW(NIM_ADD, &nid))
        g_tray_added = 1;
}

static void tray_remove(void) {
    if (g_tray_added && g_win) {
        NOTIFYICONDATAW nid;
        memset(&nid, 0, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_tray_added = 0;
    }

    if (g_tray_icon && g_tray_icon_owned) {
        DestroyIcon(g_tray_icon);
    }
    g_tray_icon = NULL;
    g_tray_icon_owned = 0;
}

static int is_zh(void) {
    return g_cfg.language[0] != '\0' && strcmp(g_cfg.language, "zh") == 0;
}

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

static void set_status_i18n(const char* status,
                            const char* text_zh, const char* text_en,
                            const char* sub_zh,  const char* sub_en) {
    set_status(status, is_zh() ? text_zh : text_en, is_zh() ? sub_zh : sub_en);
}

static void on_timer_health_check(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)tick;
    KillTimer(hwnd, id);
    do_health_check();
}

static void do_health_check(void) {
    if (!g_installed) {
        set_status_i18n("stopped",
            "\xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85", "Not installed",
            "nanobot \xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85", "nanobot is not installed");
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
        set_status_i18n("running",
            "\xe8\xbf\x90\xe8\xa1\x8c\xe4\xb8\xad", "Running",
            "nanobot \xe7\xbd\x91\xe5\x85\xb3\xe5\xb7\xb2\xe5\x90\xaf\xe5\x8a\xa8", "nanobot gateway is active");
        page_set_int("pid", pid > 0 ? pid : 0);
        if (!g_start_tick)
            start_uptime_timer(pid);
    } else {
        set_status_i18n("stopped",
            "\xe5\xb7\xb2\xe5\x81\x9c\xe6\xad\xa2", "Stopped",
            "nanobot \xe6\x9c\xaa\xe8\xbf\x90\xe8\xa1\x8c", "nanobot is not running");
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
        set_status_i18n("stopped",
            "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0", "Not found",
            "\xe6\x89\xbe\xe4\xb8\x8d\xe5\x88\xb0 nanobot\xef\xbc\x8c\xe8\xaf\xb7\xe5\xae\x89\xe8\xa3\x85\xe6\x88\x96\xe8\xae\xbe\xe7\xbd\xae\xe8\xb7\xaf\xe5\xbe\x84",
            "Cannot find nanobot. Please install or set path.");
        return;
    }

    g_nanobot_pid = 0;
    int rc = nanobot_start(cmd, &g_nanobot_pid);
    free(cmd);

    if (rc != 0) {
        char buf_zh[256], buf_en[256];
        snprintf(buf_zh, sizeof(buf_zh),
            "\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5 (\xe9\x94\x99\xe8\xaf\xaf %d)", rc);
        snprintf(buf_en, sizeof(buf_en), "Start failed (error %d)", rc);
        set_status("stopped", is_zh() ? buf_zh : buf_en, "");
        return;
    }

    set_status_i18n("starting",
        "\xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\xad\xe2\x80\xa6", "Starting\xe2\x80\xa6",
        "\xe7\xad\x89\xe5\xbe\x85\xe5\x81\xa5\xe5\xba\xb7\xe6\xa3\x80\xe6\x9f\xa5", "Waiting for health check");
    page_set_int("pid", g_nanobot_pid);

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    SetTimer(hwnd, TIMER_HEALTH_CHECK_AFTER_START, 2000, on_timer_health_check);
}

static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR uid, DWORD_PTR ref) {
    (void)uid; (void)ref;
    if (msg == WM_CLOSE && g_tray_enabled_effective && !g_allow_exit) {
        ui_window_hide(g_win);
        return 0;
    }
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
    if (msg == WM_TRAY_ICON) {
        if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) {
            show_main_window();
            return 0;
        }
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            tray_show_menu(hwnd);
            return 0;
        }
    }
    if (msg == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case ID_TRAY_SHOW:
            show_main_window();
            return 0;
        case ID_TRAY_QUIT:
            g_allow_exit = 1;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void on_start(UiWidget w, void* ud) {
    (void)w; (void)ud;

    if (!g_installed) {
        set_status_i18n("stopped",
            "\xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85", "Not installed",
            "pip install nanobot-ai", "pip install nanobot-ai");
        ui_page_set_bool(g_page, "installed", 0);
        return;
    }

    set_status_i18n("starting",
        "\xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\xad\xe2\x80\xa6", "Starting\xe2\x80\xa6",
        "\xe6\xa3\x80\xe6\xb5\x8b\xe4\xb8\xad\xe2\x80\xa6", "Detecting nanobot\xe2\x80\xa6");

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
    set_status_i18n("starting",
        "\xe5\x81\x9c\xe6\xad\xa2\xe4\xb8\xad\xe2\x80\xa6", "Stopping\xe2\x80\xa6",
        "\xe6\xad\xa3\xe5\x9c\xa8\xe5\x85\xb3\xe9\x97\xad nanobot", "Shutting down nanobot");

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

static void on_tray_toggle(UiWidget w, int val, void* ud) {
    (void)w; (void)ud;
    g_cfg.stay_in_tray = val;
    config_save(&g_cfg);
    ui_page_set_bool(g_page, "stayInTray", val);
    g_tray_enabled_effective = val;
    if (val && g_hwnd)
        tray_add(g_hwnd);
    else if (!val)
        tray_remove();
}

static void on_tray_toggle_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_toggle_on_changed(w, on_tray_toggle, NULL);
}

static void apply_lang(const char* loc) {
    strncpy(g_cfg.language, loc, sizeof(g_cfg.language) - 1);
    config_save(&g_cfg);
    ui_page_set_locale(g_page, loc);
    page_set("lang", loc);
}

static void on_lang_zh(UiWidget w, void* ud) {
    (void)w; (void)ud;
    apply_lang("zh");
}

static void on_lang_en(UiWidget w, void* ud) {
    (void)w; (void)ud;
    apply_lang("en");
}

static void on_lang_zh_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_lang_zh, NULL);
}

static void on_lang_en_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_lang_en, NULL);
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

    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(log_path, GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER sz;
            sz.LowPart = fad.nFileSizeLow;
            sz.HighPart = fad.nFileSizeHigh;
            if (sz.QuadPart > 512 * 1024) {
                wchar_t bak[MAX_PATH];
                swprintf_s(bak, _countof(bak),
                    L"%s\\nanobot-manager-autostart.bak", exe_dir);
                MoveFileExW(log_path, bak, MOVEFILE_REPLACE_EXISTING);
            }
        }
    }

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
    g_tray_enabled_effective = g_cfg.stay_in_tray;

    LPCWSTR wcmd = GetCommandLineW();
    if (has_arg(wcmd, L"--autostart")) {
        if (!g_cfg.stay_in_tray) {
            int rc = run_autostart_mode();
            config_free(&g_cfg);
            return rc;
        }
        g_autostart_silent = 1;
        g_installed = nanobot_detect_install();
        if (g_installed && !nanobot_check_running(g_cfg.health_check_port)) {
            char* cmd = find_nanobot_cmd();
            if (!cmd && g_cfg.nanobot_path && g_cfg.nanobot_path[0])
                cmd = _strdup(g_cfg.nanobot_path);
            if (cmd) {
                int pid = 0;
                nanobot_start(cmd, &pid);
                g_nanobot_pid = pid;
                autostart_log("silent tray mode: nanobot process started");
                free(cmd);
            } else {
                autostart_log("silent tray mode: nanobot command not found");
            }
        } else if (g_installed) {
            autostart_log("silent tray mode: nanobot already running");
        } else {
            autostart_log("silent tray mode: nanobot not installed");
        }
    }

    if (!g_autostart_silent)
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

    {
        const char* zh =
            "app_title=nanobot \xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\n"
            "chat_coming_soon=\xe5\x8d\xb3\xe5\xb0\x86\xe6\x8e\xa8\xe5\x87\xba\n"
            "chat_coming_soon_desc=\xe8\x81\x8a\xe5\xa4\xa9\xe5\x8a\x9f\xe8\x83\xbd\xe6\xad\xa3\xe5\x9c\xa8\xe5\xbc\x80\xe5\x8f\x91\xe4\xb8\xad\n"
            "install_not_found=\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0 nanobot\n"
            "install_hint=\xe8\xaf\xb7\xe5\xae\x89\xe8\xa3\x85\xef\xbc\x9apip install nanobot-ai\n"
            "metric_port=\xe7\xab\xaf\xe5\x8f\xa3\n"
            "metric_pid=\xe8\xbf\x9b\xe7\xa8\x8b\n"
            "metric_uptime=\xe8\xbf\x90\xe8\xa1\x8c\xe6\x97\xb6\xe9\x97\xb4\n"
            "btn_start=\xe5\x90\xaf\xe5\x8a\xa8\n"
            "btn_stop=\xe5\x81\x9c\xe6\xad\xa2\n"
            "settings_general=\xe9\x80\x9a\xe7\x94\xa8\n"
            "settings_appearance=\xe5\xa4\x96\xe8\xa7\x82\n"
            "setting_autostart=\xe5\xbc\x80\xe6\x9c\xba\xe8\x87\xaa\xe5\x90\xaf\n"
            "setting_autostart_sub=Windows \xe5\x90\xaf\xe5\x8a\xa8\xe6\x97\xb6\xe8\x87\xaa\xe5\x8a\xa8\xe8\xbf\x90\xe8\xa1\x8c\n"
            "setting_tray=\xe6\x9c\x80\xe5\xb0\x8f\xe5\x8c\x96\xe5\x88\xb0\xe6\x89\x98\xe7\x9b\x98\n"
            "setting_tray_sub=\xe5\x85\xb3\xe9\x97\xad\xe7\xaa\x97\xe5\x8f\xa3\xe6\x97\xb6\xe9\x9a\x90\xe8\x97\x8f\xe5\x88\xb0\xe7\xb3\xbb\xe7\xbb\x9f\xe6\x89\x98\xe7\x9b\x98\n"
            "setting_path=nanobot \xe8\xb7\xaf\xe5\xbe\x84\n"
            "setting_path_change=\xe6\x9b\xb4\xe6\x94\xb9\n"
            "setting_theme=\xe4\xb8\xbb\xe9\xa2\x98\n"
            "setting_language=\xe8\xaf\xad\xe8\xa8\x80\n"
            "open_webui=\xe6\x89\x93\xe5\xbc\x80 WebUI\n"
            "status_stopped=\xe5\xb7\xb2\xe5\x81\x9c\xe6\xad\xa2\n"
            "status_stopped_sub=nanobot \xe6\x9c\xaa\xe8\xbf\x90\xe8\xa1\x8c\n"
            "status_running=\xe8\xbf\x90\xe8\xa1\x8c\xe4\xb8\xad\n"
            "status_running_sub=nanobot \xe7\xbd\x91\xe5\x85\xb3\xe5\xb7\xb2\xe5\x90\xaf\xe5\x8a\xa8\n"
            "status_starting=\xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\xad\xe2\x80\xa6\n"
            "status_starting_sub=\xe7\xad\x89\xe5\xbe\x85\xe5\x81\xa5\xe5\xba\xb7\xe6\xa3\x80\xe6\x9f\xa5\n"
            "status_not_found=\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0\n"
            "status_not_found_sub=\xe6\x89\xbe\xe4\xb8\x8d\xe5\x88\xb0 nanobot\xef\xbc\x8c\xe8\xaf\xb7\xe5\xae\x89\xe8\xa3\x85\xe6\x88\x96\xe8\xae\xbe\xe7\xbd\xae\xe8\xb7\xaf\xe5\xbe\x84\n"
            "status_start_fail=\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5\n"
            "status_stopping=\xe5\x81\x9c\xe6\xad\xa2\xe4\xb8\xad\xe2\x80\xa6\n"
            "status_stopping_sub=\xe6\xad\xa3\xe5\x9c\xa8\xe5\x85\xb3\xe9\x97\xad nanobot\n"
            "status_detecting=\xe6\xa3\x80\xe6\xb5\x8b\xe4\xb8\xad\xe2\x80\xa6\n"
            "status_not_installed=\xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85\n"
            "status_not_installed_sub=nanobot \xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85\n"
            "auto_detect=\xe8\x87\xaa\xe5\x8a\xa8\xe6\xa3\x80\xe6\xb5\x8b\n";
        const char* en =
            "app_title=nanobot manager\n"
            "chat_coming_soon=Coming Soon\n"
            "chat_coming_soon_desc=Chat feature is under development\n"
            "install_not_found=nanobot not found\n"
            "install_hint=Please install: pip install nanobot-ai\n"
            "metric_port=Port\n"
            "metric_pid=PID\n"
            "metric_uptime=Uptime\n"
            "btn_start=Start\n"
            "btn_stop=Stop\n"
            "settings_general=General\n"
            "settings_appearance=Appearance\n"
            "setting_autostart=Auto-start on boot\n"
            "setting_autostart_sub=Launch when Windows starts\n"
            "setting_tray=Minimize to tray\n"
            "setting_tray_sub=Hide to system tray on close\n"
            "setting_path=nanobot path\n"
            "setting_path_change=Change\n"
            "setting_theme=Theme\n"
            "setting_language=Language\n"
            "open_webui=Open WebUI\n"
            "status_stopped=Stopped\n"
            "status_stopped_sub=nanobot is not running\n"
            "status_running=Running\n"
            "status_running_sub=nanobot gateway is active\n"
            "status_starting=Starting\xe2\x80\xa6\n"
            "status_starting_sub=Waiting for health check\n"
            "status_not_found=Not found\n"
            "status_not_found_sub=Cannot find nanobot. Please install or set path.\n"
            "status_start_fail=Start failed\n"
            "status_stopping=Stopping\xe2\x80\xa6\n"
            "status_stopping_sub=Shutting down nanobot\n"
            "status_detecting=Detecting\xe2\x80\xa6\n"
            "status_not_installed=Not installed\n"
            "status_not_installed_sub=nanobot is not installed\n"
            "auto_detect=Auto-detect\n";
        ui_page_load_language_string(g_page, "zh", zh);
        ui_page_load_language_string(g_page, "en", en);
    }

    ui_page_set_text(g_page, "status", L"stopped");
    page_set("statusText", is_zh() ? "\xe5\xb7\xb2\xe5\x81\x9c\xe6\xad\xa2" : "Stopped");
    page_set("statusSub", is_zh() ? "nanobot \xe6\x9c\xaa\xe8\xbf\x90\xe8\xa1\x8c" : "nanobot is not running");
    ui_page_set_bool(g_page, "installed", g_installed);
    ui_page_set_bool(g_page, "canStart", g_installed ? 1 : 0);
    ui_page_set_bool(g_page, "canStop", 0);
    ui_page_set_bool(g_page, "autostart", autostart_is_enabled());
    ui_page_set_bool(g_page, "stayInTray", g_cfg.stay_in_tray);

    wchar_t port_w[16];
    swprintf_s(port_w, _countof(port_w), L"%d", g_cfg.health_check_port);
    ui_page_set_text(g_page, "port", port_w);

    if (g_cfg.nanobot_path && g_cfg.nanobot_path[0]) {
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, g_cfg.nanobot_path, -1, wpath, MAX_PATH);
        ui_page_set_text(g_page, "nanobotPath", wpath);
    } else {
        page_set("nanobotPath", is_zh() ? "\xe8\x87\xaa\xe5\x8a\xa8\xe6\xa3\x80\xe6\xb5\x8b" : "Auto-detect");
    }

    apply_theme(g_cfg.theme_mode);

    {
        const char* loc = g_cfg.language[0] ? g_cfg.language : "zh";
        ui_page_set_locale(g_page, loc);
        page_set("lang", loc);
    }

    {
        wchar_t ver_w[32];
        swprintf_s(ver_w, _countof(ver_w), L"v" NANOBOT_MANAGER_VERSION);
        ui_page_set_text(g_page, "footer_version", ver_w);
    }

    ui_page_on_widget_mount(g_page, "btn_start", on_btn_start_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_stop",  on_btn_stop_mount,  NULL);
    ui_page_on_widget_mount(g_page, "toggle_autostart", on_toggle_mount, NULL);
    ui_page_on_widget_mount(g_page, "toggle_tray", on_tray_toggle_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_change_path", on_btn_change_path_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_webui", on_btn_webui_mount, NULL);
    ui_page_on_widget_mount(g_page, "theme_light", on_theme_light_mount, NULL);
    ui_page_on_widget_mount(g_page, "theme_dark", on_theme_dark_mount, NULL);
    ui_page_on_widget_mount(g_page, "theme_system", on_theme_system_mount, NULL);
    ui_page_on_widget_mount(g_page, "lang_zh", on_lang_zh_mount, NULL);
    ui_page_on_widget_mount(g_page, "lang_en", on_lang_en_mount, NULL);

    if (g_autostart_silent)
        g_win = ui_page_prepare_window(g_page, NULL);
    else
        g_win = ui_page_open_window(g_page, NULL);
    if (!g_win) {
        ui_page_destroy(g_page);
        config_free(&g_cfg);
        return 2;
    }

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    g_hwnd = hwnd;
    SetWindowSubclass(hwnd, subclass_proc, 0, 0);
    if (g_tray_enabled_effective)
        tray_add(hwnd);

    do_health_check();

    int ret = ui_run();

    stop_uptime_timer();
    tray_remove();
    RemoveWindowSubclass(hwnd, subclass_proc, 0);
    ui_page_destroy(g_page);
    config_free(&g_cfg);
    return ret;
}
