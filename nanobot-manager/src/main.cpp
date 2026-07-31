#include "config.h"
#include "process.h"
#include "autostart.h"
#include "chat.h"
#include "chat_ui.h"
#include "credential.h"
#include "version.h"

#include "ui_core.h"
#include "cJSON.h"

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
static UINT_PTR  g_uptime_timer = 0;
static UINT_PTR  g_chat_ui_timer = 0;
static int       g_theme_mode = 0;
static int       g_tray_enabled_effective = 0;
static int       g_allow_exit = 0;
static int       g_tray_added = 0;
static int       g_tray_icon_owned = 0;
static HICON     g_tray_icon = NULL;
static HWND      g_hwnd = NULL;
static int       g_hotkey_registered = 0;
static int       g_autostart_silent = 0;
static int       g_effective_health_port = 18790;

#define TIMER_UPTIME 1003
#define TIMER_CHAT_UI 1004
#define CHAT_UI_FRAME_MS 33
#define ID_TRAY_SHOW 40001
#define ID_TRAY_QUIT 40002

/* Global summon hotkey: Ctrl+Shift+N. Registered against g_hwnd; WM_HOTKEY is
 * routed to subclass_proc by core-ui's ui_run() pump alongside WM_TRAY_ICON. */
#define ID_HOTKEY_SUMMON 0xB000
#define HOTKEY_MODS (MOD_CONTROL | MOD_SHIFT)
#define HOTKEY_VK   'N'

#define WM_ASYNC_STOP_DONE      (WM_APP + 2)
#define WM_TRAY_ICON            (WM_APP + 3)
#define WM_CHILD_EXITED         (WM_APP + 4)   /* P5a: posted by the thread-pool wait callback
                                                * when g_state.handle becomes signaled       */
#define WM_ASYNC_HEALTH_RESULT  (WM_APP + 5)   /* LPARAM = health_result_payload* (handler frees)             */
#define WM_ASYNC_RESOLVE_DONE   (WM_APP + 6)   /* WPARAM = start_intent(0/1), LPARAM = resolve_payload* (malloc'd; handler frees) */
#define WM_CHAT_EVENT           (WM_APP + 10)

static void tray_remove(void);
static int has_arg(LPCWSTR cmd, const wchar_t* arg);
static int can_edit_gateway_port(void);

static void show_main_window(void) {
    if (!g_win) return;

    /* SW_RESTORE restores a minimized window to its previous size/position,
     * and also shows a hidden one — more reliable than SW_SHOWNORMAL. */
    ShowWindow(g_hwnd, SW_RESTORE);

    /* SetForegroundWindow is silently ignored when our thread doesn't own
     * the foreground (common when activated from a tray notification or
     * global hotkey).  AttachThreadInput temporarily shares the input
     * queue with the foreground thread so the call succeeds. */
    HWND fg = GetForegroundWindow();
    int attached = 0;
    if (fg && fg != g_hwnd) {
        DWORD fg_tid = GetWindowThreadProcessId(fg, NULL);
        if (fg_tid != GetCurrentThreadId()) {
            AttachThreadInput(GetCurrentThreadId(), fg_tid, TRUE);
            attached = 1;
        }
    }
    SetForegroundWindow(g_hwnd);
    SetActiveWindow(g_hwnd);
    if (attached) {
        AttachThreadInput(GetCurrentThreadId(),
                          GetWindowThreadProcessId(GetForegroundWindow(), NULL),
                          FALSE);
    }

    ui_window_show(g_win);
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

static void hotkey_register(HWND hwnd) {
    if (g_hotkey_registered || !hwnd) return;
    if (RegisterHotKey(hwnd, ID_HOTKEY_SUMMON, HOTKEY_MODS, HOTKEY_VK))
        g_hotkey_registered = 1;
    /* If another app already owns Ctrl+Shift+N the call fails silently:
     * g_hotkey_registered stays 0, the toggle stays on, no crash/no popup. */
}

static void hotkey_unregister(void) {
    if (g_hotkey_registered && g_hwnd)
        UnregisterHotKey(g_hwnd, ID_HOTKEY_SUMMON);
    g_hotkey_registered = 0;
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

/* Cached gateway-running predicate for the chat send path. Used to live-call
 * nanobot_check_running (HTTP on the UI thread) -- now just reads the guarded
 * state set by apply_health_result. Unblocks chat_ui.c:252 with no HTTP. */
static int health_running_thunk(void) {
    return nm_state_get() == NM_STATE_RUNNING ? 1 : 0;
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
    ULONGLONG start_tick = nm_state_get_start_tick();
    if (!start_tick) return;
    ULONGLONG elapsed = (GetTickCount64() - start_tick) / 1000;
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
    chat_ui_tick(GetTickCount64());
}

static void start_uptime_timer(int pid) {
    ULONGLONG now = GetTickCount64();
    unsigned long long process_uptime = 0;
    if (nanobot_get_process_uptime_seconds(pid, &process_uptime))
        nm_state_set_start_tick(now - process_uptime * 1000ULL);
    else
        nm_state_set_start_tick(now);

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    g_uptime_timer = SetTimer(hwnd, TIMER_UPTIME, 1000, on_timer_uptime);
    update_uptime();
}

static void stop_uptime_timer(void) {
    if (g_uptime_timer) {
        KillTimer((HWND)ui_window_hwnd(g_win), TIMER_UPTIME);
        g_uptime_timer = 0;
    }
    nm_state_set_start_tick(0);
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
        ui_page_set_bool(g_page, "canRefresh", 1);
    } else if (strcmp(status, "starting") == 0 || strcmp(status, "stopping") == 0) {
        /* M5: "stopping" disables both buttons -- same as "starting".
         * M8: also disable refresh during transitions -- the worker guard
         * would no-op a click here anyway, but a disabled button is clearer. */
        ui_page_set_bool(g_page, "canStart", 0);
        ui_page_set_bool(g_page, "canStop", 0);
        ui_page_set_bool(g_page, "canRefresh", 0);
    } else {
        ui_page_set_bool(g_page, "canStart", 1);
        ui_page_set_bool(g_page, "canStop", 0);
        ui_page_set_bool(g_page, "canRefresh", 1);
    }
    ui_page_set_bool(g_page, "canEditGatewayPort", can_edit_gateway_port());
}

static VOID CALLBACK on_timer_chat_ui(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)id; (void)tick;
    chat_ui_publish_tick();
}

static int can_edit_gateway_port(void) {
    nm_state state = nm_state_get();
    return state == NM_STATE_IDLE || state == NM_STATE_DEAD;
}

static void refresh_effective_health_port(void) {
    page_set_int("port", g_effective_health_port);
}

/* --- Status i18n (single-source keys) ---------------------------------------
 * core-ui exposes ui_page_load_language_string / $t('key') for .uix templates
 * but has no C++ key→string lookup API. Status text is pushed from C++ via
 * page_set. g_status_i18n is the ONLY wording source for status_* keys:
 *   - t_status() resolves keys for set_status_i18n
 *   - append_status_i18n_lang() generates the status portion of the language
 *     blob passed to ui_page_load_language_string (so $t and C++ cannot drift) */
typedef struct { const char* key; const char* zh; const char* en; } status_i18n_entry;

static const status_i18n_entry g_status_i18n[] = {
    { "status_running",              "运行中",                         "Running" },
    { "status_running_sub",          "nanobot 网关已启动",              "nanobot gateway is active" },
    { "status_stopped",              "已停止",                         "Stopped" },
    { "status_stopped_sub",          "nanobot 未运行",                 "nanobot is not running" },
    { "status_exited_sub",           "nanobot 网关已退出",             "nanobot gateway exited" },
    { "status_starting",             "启动中…",                        "Starting…" },
    { "status_starting_sub",         "等待健康检查",                   "Waiting for health check" },
    { "status_health_worker_failed_sub", "状态检测线程无法启动",          "Health-check worker could not be started" },
    { "status_stopping",             "停止中…",                        "Stopping…" },
    { "status_stopping_sub",         "正在关闭 nanobot",               "Shutting down nanobot" },
    { "status_detecting",            "检测中…",                        "Detecting…" },
    { "status_detecting_nanobot",    "检测中…",                        "Detecting nanobot…" },
    { "status_not_found",            "未找到",                         "Not found" },
    { "status_not_found_sub",        "找不到 nanobot，请安装或设置路径",
                                     "Cannot find nanobot. Please install or set path." },
    { "status_incompatible",         "版本不兼容",                     "Incompatible version" },
    { "status_incompatible_sub",     "需要 nanobot-ai >= 0.2.2（缺少 gateway --background）", "Requires nanobot-ai >= 0.2.2 (missing gateway --background)" },
    { "status_probe_failed",         "检测失败",                       "Detection failed" },
    { "status_probe_failed_sub",     "无法检测 nanobot 的 gateway --background 支持", "Could not probe nanobot gateway --background support" },
    { "status_start_fail",           "启动失败",                       "Start failed" },
    { "status_not_installed",        "未安装",                         "Not installed" },
    { "status_not_installed_sub",    "nanobot 未安装",                 "nanobot is not installed" },
    { "status_fail_no_statefile_sub","后台启动器未写入状态文件",
                                     "Background launcher did not write state file" },
    { "status_fail_open_failed_sub", "OpenProcess 失败",               "OpenProcess failed" },
    { "status_fail_stale_pid_sub",   "状态文件中的 PID 已失效",        "State file PID is stale" },
    { "status_fail_launcher_sub",    "后台启动器异常退出，未发现有效网关进程", "Background launcher exited without a valid gateway process" },
    { "status_fail_unverified_state_sub", "状态记录身份无法验证",       "State record identity cannot be verified" },
};

static const char* t_status(const char* key) {
    if (!key || !key[0]) return "";
    for (int i = 0; i < (int)(sizeof(g_status_i18n) / sizeof(g_status_i18n[0])); i++) {
        if (strcmp(g_status_i18n[i].key, key) == 0)
            return is_zh() ? g_status_i18n[i].zh : g_status_i18n[i].en;
    }
    return key;   /* missing key surfaces itself (same fallback as $t) */
}

/* status = UI state code ("running"/"stopped"/"starting"/"stopping");
 * text_key / sub_key = entries in g_status_i18n (and the language table). */
static void set_status_i18n(const char* status,
                            const char* text_key, const char* sub_key) {
    set_status(status, t_status(text_key), t_status(sub_key));
}

static void apply_resolve_failure(void) {
    nm_state state = nm_state_get();
    if (state == NM_STATE_RUNNING || state == NM_STATE_STARTING ||
        state == NM_STATE_STOPPING)
        return;

    nm_state_set_state(NM_STATE_IDLE);
    nm_state_set_installed(0);
    set_status_i18n("stopped", "status_probe_failed", "status_probe_failed_sub");
    ui_page_set_bool(g_page, "installed", 0);
    ui_page_set_bool(g_page, "canStart", 0);
    ui_page_set_bool(g_page, "canStop", 0);
}

static void apply_health_worker_failure(void) {
    nm_state state = nm_state_get();
    if (state == NM_STATE_RUNNING || state == NM_STATE_STOPPING)
        return;
    if (state == NM_STATE_STARTING) {
        set_status_i18n("starting", "status_starting", "status_health_worker_failed_sub");
        return;
    }
    if (state == NM_STATE_RESOLVING) {
        nm_state_set_state(NM_STATE_IDLE);
        set_status_i18n("stopped", "status_probe_failed", "status_probe_failed_sub");
        ui_page_set_bool(g_page, "canStart", 0);
        ui_page_set_bool(g_page, "canStop", 0);
    }
}

/* Append "key=value\n" to a language blob buffer. Returns 0 on overflow. */
static int append_lang(char* buf, size_t cap, size_t* used,
                       const char* key, const char* val) {
    if (!buf || !used || !key || !val || *used >= cap) return 0;
    int n = snprintf(buf + *used, cap - *used, "%s=%s\n", key, val);
    if (n < 0 || (size_t)n >= cap - *used) return 0;
    *used += (size_t)n;
    return 1;
}

/* Generate the status_* portion of a language blob from g_status_i18n. */
static int append_status_i18n_lang(char* buf, size_t cap, size_t* used, int zh) {
    for (int i = 0; i < (int)(sizeof(g_status_i18n) / sizeof(g_status_i18n[0])); i++) {
        const char* val = zh ? g_status_i18n[i].zh : g_status_i18n[i].en;
        if (!append_lang(buf, cap, used, g_status_i18n[i].key, val))
            return 0;
    }
    return 1;
}

/* Apply an off-thread health probe result on the UI thread. The HTTP probe
 * (nanobot_check_running) now runs ONLY in health_worker; this updates UI +
 * guarded state. g_state.pid is authoritative from P3a's nanobot_start.
 *
 * P5a streak tolerance: a single failed probe while RUNNING is treated as a
 * transient blip (network hiccup, slow /health). Two in a row => DEAD. This
 * prevents a momentary 503/timeout from flipping a healthy gateway to Stopped.
 * The streak is UI-thread-local (single thread -> no lock) and is reset on
 * every successful probe and on every fresh start. */
static int s_health_fail_streak = 0;

static void apply_health_result(int running, int actual_port) {
    if (running) {
        if (actual_port >= 1 && actual_port <= 65535)
            g_effective_health_port = actual_port;
        refresh_effective_health_port();
        s_health_fail_streak = 0;
        /* P5b: if we discovered a running gateway we didn't start (manager
         * relaunch with the gateway still up, or an externally-started
         * `nanobot gateway`), latch onto it via the state file so PID/handle
         * are populated and instant handle-signal crash detection applies.
         * Idempotent: skipped once attached. On failure the handle stays NULL
         * and liveness falls back to the HTTP streak path (~30s detection). */
        if (nm_state_get_handle() == NULL) {
            if (nanobot_attach_running() == 0) {
                HWND hwnd = (HWND)ui_window_hwnd(g_win);
                nanobot_watch_handle(hwnd, WM_CHILD_EXITED);
            }
        }
        /* Re-snapshot AFTER attach (it mutated g_state.{pid,handle,start_tick}
         * under CS) so the UI work below shows the real pid/start_tick. */
        nm_process_view v;
        nm_state_snapshot(&v);
        int was_running = (v.state == NM_STATE_RUNNING);
        nm_state_set_state(NM_STATE_RUNNING);
        set_status_i18n("running", "status_running", "status_running_sub");
        if (v.start_tick == 0)
            nm_state_set_start_tick(GetTickCount64());
        if (!was_running)
            start_uptime_timer((int)v.pid);
        page_set_int("pid", (int)v.pid);
        /* Auto-connect chat when gateway becomes healthy */
        if (chat_needs_connect() && chat_ui_can_auto_connect()) {
            char secret[128] = {0};
            credential_load(secret, sizeof(secret), NULL);
            chat_set_endpoint(g_cfg.webui_port, secret);
            SecureZeroMemory(secret, sizeof(secret));
            chat_connect();
        }
        return;
    }

    /* Not running. */
    nm_state s = nm_state_get();
    if (s == NM_STATE_STARTING) {
        /* Cold-boot tolerance: a single failed probe during STARTING must not
         * flip the UI to Stopped -- the boot-phase poll continues. */
        return;
    }

    if (s == NM_STATE_RUNNING) {
        /* Steady-state streak: tolerate one blip before declaring DEAD. */
        s_health_fail_streak++;
        if (s_health_fail_streak < 2) {
            return;                        /* one blip tolerated, stay RUNNING               */
        }
        /* Two in a row => crash/DEAD. Fall through to the DEAD transition. */
    }

    /* DEAD transition: either we were RUNNING and the streak hit 2, or the
     * state was already IDLE/DEAD/etc. (e.g. initial detect found not-running).
     * In the latter case we settle on IDLE/Stopped (same UI as before P5a). */
    if (s == NM_STATE_RUNNING) {
        nm_state_set_state(NM_STATE_DEAD);
        set_status_i18n("stopped", "status_stopped", "status_exited_sub");
    } else {
        nm_state_set_state(NM_STATE_IDLE);
        set_status_i18n("stopped", "status_stopped", "status_stopped_sub");
    }
    s_health_fail_streak = 0;
    stop_uptime_timer();
    nm_state_set_pid(0);
    g_effective_health_port = g_cfg.health_check_port;
    refresh_effective_health_port();
    chat_disconnect_async();
    chat_ui_on_gateway_stopped();
}

/* --- Health worker (boot-phase backoff -> steady) --------------------------
 * SOLE caller of nanobot_check_running after P3b (besides run_autostart_mode's
 * synchronous loop, which has no UI thread). Posts WM_ASYNC_HEALTH_RESULT; the
 * UI thread never blocks on HTTP.
 *
 * Generation counter: each successful kick increments g_health_gen and passes
 * the generation to its worker. A newer kick stale-marks older workers, which
 * exit on the next bail check without posting more results. */
static volatile LONG g_health_gen = 0;

typedef struct { HWND hwnd; LONG gen; int configured_port; } health_ctx;
typedef struct { int running; int actual_port; } health_result_payload;

static DWORD WINAPI health_worker(LPVOID param) {
health_ctx* c = (health_ctx*)param;
    HWND hwnd = c->hwnd;
    LONG my_gen = c->gen;
    int configured_port = c->configured_port;
    free(c);

    static const int cadence[] = {1000,1000,1500,2000,2000,3000,3000,5000,5000,5000};
    const int boot_count = (int)(sizeof(cadence) / sizeof(cadence[0]));
    int i = 0;
    /* M8: first-iteration bypass for user-initiated refresh (btn_refresh
     * kicked while the worker is NOT yet polling, e.g. state=IDLE/DEAD).
     * The default polling guard bails BEFORE posting when state is not
     * STARTING/RUNNING/RESOLVING; that guard is correct for stopping
     * transitions (a stale worker must not inject "running=1" after stop)
     * but it makes a user click during IDLE a silent no-op. The bypass is
     * restricted to IDLE/DEAD on the FIRST iteration only, so a refresh
     * during STOPPING still bails and cannot undo an in-progress stop. */
    int first_iteration = 1;

    for (;;) {
        int actual_port = 0;
        int running = nanobot_detect_running_port(configured_port, &actual_port);

        /* M7: bail BEFORE posting so a superseded worker does not inject a
         * stale result after a newer kick. The stale blip was masked by streak
         * tolerance, but the ordering should be correct. */
        if (InterlockedCompareExchange(&g_health_gen, my_gen, my_gen) != my_gen)
            return 0;
        /* Polling-state guard: RESOLVING is allowed so the detect-only startup
         * path (M2 leaves RESOLVING until the first health result) can post;
         * apply_health_result then settles to RUNNING/IDLE. First iteration
         * from IDLE/DEAD bypasses the guard so btn_refresh delivers one result
         * (see M8 above). All other first-iteration entries (STARTING/RUNNING/
         * RESOLVING/STOPPING) and every subsequent iteration use the same
         * allow-list as before. */
        nm_state s = nm_state_get();
        int allow_post;
        if (first_iteration) {
            first_iteration = 0;
            allow_post = (s == NM_STATE_IDLE || s == NM_STATE_DEAD)
                || (s == NM_STATE_STARTING || s == NM_STATE_RUNNING || s == NM_STATE_RESOLVING);
        } else {
            allow_post = (s == NM_STATE_STARTING || s == NM_STATE_RUNNING || s == NM_STATE_RESOLVING);
        }
        if (!allow_post) return 0;

        health_result_payload* result = (health_result_payload*)malloc(sizeof(*result));
        if (!result) return 0;
        result->running = running;
        result->actual_port = actual_port;
        if (!PostMessageW(hwnd, WM_ASYNC_HEALTH_RESULT, 0, (LPARAM)result))
            free(result);

        int delay = (i < boot_count) ? cadence[i] : 15000;
        Sleep(delay);
        if (i < boot_count) i++;
    }
    return 0;
}

static void kick_health_worker(HWND hwnd) {
    health_ctx* c = (health_ctx*)calloc(1, sizeof(health_ctx));
    if (!c) {
        apply_health_worker_failure();
        return;
    }
    c->hwnd = hwnd;
    c->configured_port = g_effective_health_port;

    HANDLE thread = CreateThread(NULL, 0, health_worker, c, CREATE_SUSPENDED, NULL);
    if (thread) {
        c->gen = InterlockedIncrement(&g_health_gen);
        ResumeThread(thread);
        CloseHandle(thread);
    } else {
        free(c);
        apply_health_worker_failure();
    }
}

/* --- Resolve worker -------------------------------------------------------- */
/* Off-UI-thread resolve. Caller chooses detect-only (start_intent=0) vs start
 * (start_intent=1); the intent rides in WPARAM on WM_ASYNC_RESOLVE_DONE so the
 * UI-thread handler knows whether to start.
 *
 * I1: resolve_ctx carries an OWNED snapshot of g_cfg.nanobot_path taken on the
 * UI thread at kick time. The worker builds a local cfg_snap from g_cfg's other
 * fields and overrides nanobot_path with the snapshot, so it never dereferences
 * g_cfg.nanobot_path off-thread (on_change_path on the UI thread can free+replace
 * it under us). Snapshotting happens on the UI thread in kick_resolve_worker,
 * which is serialized with on_change_path (also UI thread) -> no race. */

typedef struct { HWND hwnd; int start_intent; char* nanobot_path_snap; } resolve_ctx;   /* heap; worker frees (incl. nanobot_path_snap) */

typedef struct {
    char*     exe;        /* NULL if !found; ownership moves in from nm_resolution */
    int       is_python;
    nm_source source;     /* diagnostic only */
    int       found;
    nm_resolution_status status;
} resolve_payload;        /* heap; handler frees exe + struct on every path */

static DWORD WINAPI resolve_worker(LPVOID param) {
    resolve_ctx* c = (resolve_ctx*)param;
    HWND hwnd = c->hwnd;
    int  start_intent = c->start_intent;
    char* path_snap = c->nanobot_path_snap;   /* I1: ownership moves here; freed below */

    /* I1: shallow-copy g_cfg's value fields, then override nanobot_path with the
     * stable snapshot. nanobot_resolve only reads cfg->nanobot_path, so racy
     * reads of the other fields (ints/ports) during the copy are benign. */
    app_config cfg_snap = g_cfg;
    cfg_snap.nanobot_path = path_snap;

    free(c);                  /* frees the ctx struct (path_snap preserved in local) */

    resolve_payload* p = (resolve_payload*)calloc(1, sizeof(resolve_payload));
    if (!p) {
        PostMessageW(hwnd, WM_ASYNC_RESOLVE_DONE, (WPARAM)start_intent, 0);
        free(path_snap);
        return 0;
    }
    nm_resolution res;
    nanobot_resolve(&cfg_snap, &res, 0);
    p->status = res.status;
    if (res.found) {
        p->exe        = res.exe;            /* take ownership (no double-strdup) */
        p->is_python  = res.is_python;
        p->source     = res.source;
        p->found      = 1;
        res.exe = NULL;
    }
    nm_resolution_free(&res);
    /* M3: if PostMessageW fails (window gone), free the payload to avoid a
     * leak -- matches chat subsystem's post_event discipline. */
    if (!PostMessageW(hwnd, WM_ASYNC_RESOLVE_DONE, (WPARAM)start_intent, (LPARAM)p)) {
        free(p->exe);
        free(p);
    }
    free(path_snap);
    return 0;
}

static void kick_resolve_worker(HWND hwnd, int start_intent) {
    nm_state previous_state = nm_state_get();
    resolve_ctx* c = (resolve_ctx*)calloc(1, sizeof(resolve_ctx));
    if (!c) {
        apply_resolve_failure();
        return;
    }
    c->hwnd = hwnd;
    c->start_intent = start_intent;
    /* I1: snapshot nanobot_path on the UI thread (serialized with on_change_path). */
    c->nanobot_path_snap = _strdup(g_cfg.nanobot_path ? g_cfg.nanobot_path : "");
    if (!c->nanobot_path_snap) {
        free(c);
        apply_resolve_failure();
        return;
    }
    /* A path probe must never overwrite a live gateway lifecycle state. */
    if (previous_state != NM_STATE_RUNNING && previous_state != NM_STATE_STARTING &&
        previous_state != NM_STATE_STOPPING) {
        nm_state_set_state(NM_STATE_RESOLVING);
        ui_page_set_bool(g_page, "canEditGatewayPort", 0);
    }
    HANDLE thread = CreateThread(NULL, 0, resolve_worker, c, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        free(c->nanobot_path_snap);
        free(c);
        apply_resolve_failure();
    }
}

/* UI-thread handler for WM_ASYNC_RESOLVE_DONE. Frees payload (exe + struct)
 * on every return path. */
static void on_resolve_done(resolve_payload* p, int start_intent) {
    if (!p) {
        apply_resolve_failure();
        return;
    }

    nm_state state = nm_state_get();
    if (state == NM_STATE_RUNNING || state == NM_STATE_STARTING ||
        state == NM_STATE_STOPPING) {
        free(p->exe);
        free(p);
        return;
    }

    int detected = p->status != NM_RES_NOT_FOUND;
    nm_state_set_installed(detected ? 1 : 0);
    ui_page_set_bool(g_page, "installed", detected ? 1 : 0);
    ui_page_set_bool(g_page, "canStart",  p->found ? 1 : 0);

    if (p->exe) {
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, p->exe, -1, wpath, MAX_PATH);
        ui_page_set_text(g_page, "nanobotPath", wpath);
    }

    if (!p->found) {
        /* M2: resolve failed -> settle to IDLE (clears RESOLVING). */
        nm_state_set_state(NM_STATE_IDLE);
        if (p->status == NM_RES_INCOMPATIBLE)
            set_status_i18n("stopped", "status_incompatible", "status_incompatible_sub");
        else if (p->status == NM_RES_PROBE_FAILED)
            set_status_i18n("stopped", "status_probe_failed", "status_probe_failed_sub");
        else
            set_status_i18n("stopped", "status_not_found", "status_not_found_sub");
        ui_page_set_bool(g_page, "canStart", 0);
        ui_page_set_bool(g_page, "canStop", 0);
        free(p->exe);
        free(p);
        return;
    }

    if (start_intent) {
        int pid_local = 0;
        int effective_port = g_cfg.health_check_port;
        int rc = nanobot_start(p->exe, p->is_python, g_cfg.health_check_port,
                               &pid_local, &effective_port);

        if (rc == 0) {
            nm_state_set_state(NM_STATE_STARTING);   /* M2: clears RESOLVING */
            nm_state_set_pid((DWORD)pid_local);
            g_effective_health_port = effective_port;
            refresh_effective_health_port();
            set_status_i18n("starting", "status_starting", "status_starting_sub");
            page_set_int("pid", pid_local);
            HWND hwnd = (HWND)ui_window_hwnd(g_win);
            /* P5a: fresh start -- reset the steady-state fail streak and arm
             * the one-shot thread-pool wait on the gateway handle. nanobot_start
             * populated g_state.handle (OpenProcess'd real gateway pid); the
             * wait posts WM_CHILD_EXITED the instant that handle signals. */
            s_health_fail_streak = 0;
            nanobot_watch_handle(hwnd, WM_CHILD_EXITED);
            kick_health_worker(hwnd);
        } else {
            /* Map P3a distinct failure codes to a key-based sub-text. Headline
             * embeds the numeric rc (not a pure table key). */
            const char* hint_key = "";
            if (rc == NANOBOT_START_NO_STATEFILE)
                hint_key = "status_fail_no_statefile_sub";
            else if (rc == NANOBOT_START_OPEN_FAILED)
                hint_key = "status_fail_open_failed_sub";
            else if (rc == NANOBOT_START_STALE_PID)
                hint_key = "status_fail_stale_pid_sub";
            else if (rc == NANOBOT_START_LAUNCHER_FAILED)
                hint_key = "status_fail_launcher_sub";
            else if (rc == NANOBOT_START_UNVERIFIED_STATE)
                hint_key = "status_fail_unverified_state_sub";
            char buf[256];
            if (is_zh())
                snprintf(buf, sizeof(buf), "启动失败 (错误 %d)", rc);
            else
                snprintf(buf, sizeof(buf), "Start failed (error %d)", rc);
            /* M2: start failed -> settle to IDLE (clears RESOLVING). */
            nm_state_set_state(NM_STATE_IDLE);
            set_status("stopped", buf, t_status(hint_key));
        }
    } else {
        /* Detect-only (startup probe). Found + installed -- leave RESOLVING
         * for the health worker: M2 must not drop to IDLE here or the worker's
         * state bail (STARTING|RUNNING only) would exit before the first post.
         * apply_health_result clears RESOLVING -> RUNNING / IDLE. */
        HWND hwnd = (HWND)ui_window_hwnd(g_win);
        kick_health_worker(hwnd);
    }

    free(p->exe);
    free(p);
}

static LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR uid, DWORD_PTR ref) {
    (void)uid; (void)ref;
    if (msg == WM_CLOSE && g_tray_enabled_effective && !g_allow_exit) {
        ui_window_hide(g_win);
        return 0;
    }
    if (msg == WM_ASYNC_RESOLVE_DONE) {
        on_resolve_done((resolve_payload*)lp, (int)wp);
        return 0;
    }
    if (msg == WM_ASYNC_HEALTH_RESULT) {
        health_result_payload* result = (health_result_payload*)lp;
        if (result) {
            apply_health_result(result->running, result->actual_port);
            free(result);
        }
        return 0;
    }
    if (msg == WM_ASYNC_STOP_DONE) {
        /* Stop is authoritative -- the stop_worker already killed the gateway.
         * No re-probe needed; just flip UI + guarded state. */
        nm_state_set_state(NM_STATE_IDLE);
        set_status_i18n("stopped", "status_stopped", "status_stopped_sub");
        stop_uptime_timer();
        nm_state_set_pid(0);
        g_effective_health_port = g_cfg.health_check_port;
        refresh_effective_health_port();
        s_health_fail_streak = 0;
        chat_disconnect_async();
        chat_ui_on_gateway_stopped();
        return 0;
    }
    if (msg == WM_CHILD_EXITED) {
        /* P5a: the thread-pool wait on g_state.handle fired -- the gateway
         * process has exited. Classify by state.
         *
         * STOPPING: expected death during a P4 invited stop. stop_worker owns
         *           the lifecycle during an invited stop -- return immediately
         *           WITHOUT unwatch/close; its own cleanup will drain the fired
         *           wait + close via nm_state_close_handle. The WM_ASYNC_STOP_DONE
         *           handler owns the UI flip (STOPPING->IDLE).
         * else    : unsolicited death while RUNNING/STARTING = crash. Close the
         *           handle via the centralized helper and tear down chat/uptime.
         *
         * C1a/C1b: the state check is FIRST so the crash branch and stop_worker
         * can never close the same handle. The wait already fired under
         * WT_EXECUTEONLYONCE (single-fire), so leaving the registration for
         * stop_worker's nm_state_close_handle to drain is correct. */
        nm_state s = nm_state_get();
        if (s == NM_STATE_STOPPING) {
            /* expected -- stop_worker owns cleanup + the UI flip */
            return 0;
        }
        /* RUNNING or STARTING (or any non-STOPPING) -> crash. Drain any
         * in-flight callback + close the now-signaled handle via the
         * centralized helper. pid/start_tick are left frozen for display. */
        nm_state_close_handle();
        nm_state_set_state(NM_STATE_DEAD);
        s_health_fail_streak = 0;
        set_status_i18n("stopped", "status_stopped", "status_exited_sub");
        stop_uptime_timer();
        g_effective_health_port = g_cfg.health_check_port;
        refresh_effective_health_port();
        chat_disconnect_async();
        chat_ui_on_gateway_stopped();
        return 0;
    }
    if (msg == WM_CHAT_EVENT) {
        chat_ui_on_event(wp, (chat_event*)lp);
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
    if (msg == WM_HOTKEY && wp == ID_HOTKEY_SUMMON) {
        show_main_window();
        return 0;
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

    /* Always route through the resolve worker (intent=1). The handler starts
     * when found, or shows the install hint when not -- so the old
     * `!installed` early-return is gone. */
    set_status_i18n("starting", "status_starting", "status_detecting_nanobot");

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    kick_resolve_worker(hwnd, 1);
}

static DWORD WINAPI stop_worker(LPVOID param) {
    HWND hwnd = (HWND)param;
    nanobot_stop();
    nm_state_set_pid(0);
    PostMessageW(hwnd, WM_ASYNC_STOP_DONE, 0, 0);
    return 0;
}

static void on_stop(UiWidget w, void* ud) {
    (void)w; (void)ud;
    /* M5: use the "stopping" status code (set_status now has a dedicated branch
     * that disables both buttons) instead of borrowing "starting". */
    set_status_i18n("stopping", "status_stopping", "status_stopping_sub");

    /* Mark STOPPING so the (P5) WM_CHILD_EXITED handler will later classify
     * death during a stop as expected. Forward-compatible: harmless now. */
    nm_state_set_state(NM_STATE_STOPPING);

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    CreateThread(NULL, 0, stop_worker, (LPVOID)hwnd, 0, NULL);
}

static void on_refresh(UiWidget w, void* ud) {
    (void)w; (void)ud;
    kick_health_worker((HWND)ui_window_hwnd(g_win));
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
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, file, -1,
                                 utf8, _countof(utf8), NULL, NULL)) {
            MessageBoxW((HWND)ui_window_hwnd(g_win),
                is_zh() ? L"路径无法转换为 UTF-8。" : L"The path could not be converted to UTF-8.",
                is_zh() ? L"保存失败" : L"Save failed", MB_OK | MB_ICONERROR);
            return;
        }

        char* new_path = _strdup(utf8);
        if (!new_path) {
            MessageBoxW((HWND)ui_window_hwnd(g_win),
                is_zh() ? L"内存不足，路径未更改。" : L"Out of memory. The path was not changed.",
                is_zh() ? L"保存失败" : L"Save failed", MB_OK | MB_ICONERROR);
            return;
        }
        app_config proposed = g_cfg;
        proposed.nanobot_path = new_path;
        if (config_save(&proposed) != 0) {
            wchar_t message[256];
            swprintf_s(message, _countof(message),
                is_zh() ? L"无法保存路径（错误 %lu）。路径未更改。"
                        : L"Could not save path (error %lu). The path was not changed.",
                (unsigned long)config_get_last_error());
            MessageBoxW((HWND)ui_window_hwnd(g_win), message,
                is_zh() ? L"保存失败" : L"Save failed", MB_OK | MB_ICONERROR);
            free(new_path);
            return;
        }
        free(g_cfg.nanobot_path);
        g_cfg.nanobot_path = new_path;

        ui_page_set_text(g_page, "nanobotPath", file);
        kick_resolve_worker((HWND)ui_window_hwnd(g_win), 0);
    }
}

static void on_open_webui(UiWidget w, void* ud) {
    (void)w; (void)ud;
    wchar_t url[128];
    int webui_port = g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765;
    swprintf_s(url, _countof(url), L"http://localhost:%d", webui_port);
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

static void restore_webui_port_input(void) {
    wchar_t port[16];
    swprintf_s(port, _countof(port), L"%d",
               g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
    ui_page_set_text(g_page, "webuiPort", port);
}

static void restore_health_port_input(void) {
    wchar_t port[16];
    swprintf_s(port, _countof(port), L"%d", g_cfg.health_check_port);
    ui_page_set_text(g_page, "healthPort", port);
}

static int read_port_input(const char* name, int* out_port) {
    char* raw = ui_page_get_json(g_page, name);
    if (!raw) return 0;
    cJSON* value = cJSON_Parse(raw);
    ui_page_free(raw);
    if (!value) return 0;
    int valid = cJSON_IsString(value) && value->valuestring &&
        config_parse_port(value->valuestring, out_port);
    cJSON_Delete(value);
    return valid;
}

static void set_chat_endpoint_from_credential(int reconnect) {
    char secret[128] = {0};
    credential_load(secret, sizeof(secret), NULL);
    chat_set_endpoint(g_cfg.webui_port, secret);
    SecureZeroMemory(secret, sizeof(secret));
    if (reconnect) chat_reconnect_async();
}

static void update_credential_ui(void) {
    char secret[128] = {0};
    DWORD error = ERROR_SUCCESS;
    int state = credential_load(secret, sizeof(secret), &error);
    SecureZeroMemory(secret, sizeof(secret));
    if (state == 1) {
        page_set("credentialStatus", is_zh() ? "已安全保存" : "Securely saved");
        ui_page_set_bool(g_page, "hasCredential", 1);
    } else if (state == 0) {
        page_set("credentialStatus", is_zh() ? "未设置" : "Not set");
        ui_page_set_bool(g_page, "hasCredential", 0);
    } else {
        char text[96];
        snprintf(text, sizeof(text), is_zh() ? "读取失败（错误 %lu）" : "Read failed (error %lu)",
                 (unsigned long)error);
        page_set("credentialStatus", text);
        ui_page_set_bool(g_page, "hasCredential", 0);
    }
}

typedef struct { HWND edit; int accepted; char secret[128]; } credential_prompt;
#define IDC_CREDENTIAL_EDIT 5101
#define IDC_CREDENTIAL_OK 5102
#define IDC_CREDENTIAL_CANCEL 5103

static LRESULT CALLBACK credential_prompt_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    credential_prompt* prompt = (credential_prompt*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        prompt = (credential_prompt*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)prompt);
    }
    if (message == WM_CREATE) {
        HWND label = CreateWindowExW(0, L"STATIC", is_zh() ? L"认证密钥：" : L"Bootstrap secret:", WS_CHILD | WS_VISIBLE, 18, 18, 290, 20, hwnd, NULL, NULL, NULL);
        prompt->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 18, 42, 290, 24, hwnd, (HMENU)(INT_PTR)IDC_CREDENTIAL_EDIT, NULL, NULL);
        HWND ok = CreateWindowExW(0, L"BUTTON", is_zh() ? L"确定" : L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 150, 78, 76, 26, hwnd, (HMENU)(INT_PTR)IDC_CREDENTIAL_OK, NULL, NULL);
        HWND cancel = CreateWindowExW(0, L"BUTTON", is_zh() ? L"取消" : L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 232, 78, 76, 26, hwnd, (HMENU)(INT_PTR)IDC_CREDENTIAL_CANCEL, NULL, NULL);
        if (!prompt || !label || !prompt->edit || !ok || !cancel) { DestroyWindow(hwnd); return -1; }
        SetFocus(prompt->edit);
        return 0;
    }
    if (message == WM_COMMAND && prompt && LOWORD(wp) == IDC_CREDENTIAL_OK) {
        wchar_t input[128] = {0};
        int length = GetWindowTextW(prompt->edit, input, _countof(input));
        int valid = length >= 1 && length <= 127;
        for (int i = 0; valid && i < length; i++) if (input[i] < 0x21 || input[i] > 0x7e) valid = 0;
        if (!valid) {
            MessageBoxW(hwnd, is_zh() ? L"请输入 1 到 127 个可打印 ASCII 字符。" : L"Enter 1 to 127 printable ASCII characters.", is_zh() ? L"无效密钥" : L"Invalid secret", MB_OK | MB_ICONERROR);
            SecureZeroMemory(input, sizeof(input));
            return 0;
        }
        for (int i = 0; i < length; i++) prompt->secret[i] = (char)input[i];
        prompt->accepted = 1;
        SecureZeroMemory(input, sizeof(input));
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_COMMAND && prompt && LOWORD(wp) == IDC_CREDENTIAL_CANCEL) { DestroyWindow(hwnd); return 0; }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return DefWindowProcW(hwnd, message, wp, lp);
}

static int prompt_for_credential(HWND owner, char secret[128]) {
    static ATOM prompt_class = 0;
    if (!prompt_class) {
        WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = credential_prompt_proc; wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"nanobotManagerCredentialPrompt"; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        prompt_class = RegisterClassW(&wc);
        if (!prompt_class && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
    }
    credential_prompt prompt; ZeroMemory(&prompt, sizeof(prompt));
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"nanobotManagerCredentialPrompt", is_zh() ? L"管理认证密钥" : L"Manage bootstrap secret", WS_CAPTION | WS_POPUP | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 330, 145, owner, NULL, GetModuleHandleW(NULL), &prompt);
    if (!dialog) return 0;
    EnableWindow(owner, FALSE); ShowWindow(dialog, SW_SHOW);
    MSG message;
    int quit = 0, quit_code = 0;
    while (IsWindow(dialog)) {
        BOOL result = GetMessageW(&message, NULL, 0, 0);
        if (result == -1) { DestroyWindow(dialog); break; }
        if (result == 0) { quit = 1; quit_code = (int)message.wParam; DestroyWindow(dialog); break; }
        if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner);
    if (prompt.accepted) memcpy(secret, prompt.secret, sizeof(prompt.secret));
    SecureZeroMemory(prompt.secret, sizeof(prompt.secret));
    if (quit) PostQuitMessage(quit_code);
    return prompt.accepted;
}

static void on_manage_credential(UiWidget w, void* ud) {
    (void)w; (void)ud;
    char secret[128] = {0};
    if (!prompt_for_credential((HWND)ui_window_hwnd(g_win), secret)) return;
    DWORD error = ERROR_SUCCESS;
    if (credential_store(secret, &error) == 0) { set_chat_endpoint_from_credential(1); update_credential_ui(); }
    else {
        wchar_t message[160];
        swprintf_s(message, _countof(message), is_zh() ? L"无法保存认证密钥（错误 %lu）。" : L"Could not save credential (error %lu).", (unsigned long)error);
        MessageBoxW((HWND)ui_window_hwnd(g_win), message, is_zh() ? L"保存失败" : L"Save failed", MB_OK | MB_ICONERROR);
    }
    SecureZeroMemory(secret, sizeof(secret));
}

static void on_clear_credential(UiWidget w, void* ud) {
    (void)w; (void)ud;
    HWND owner = (HWND)ui_window_hwnd(g_win);
    if (MessageBoxW(owner, is_zh() ? L"确定要清除已保存的认证密钥吗？" : L"Clear the saved bootstrap secret?", is_zh() ? L"确认" : L"Confirm", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    DWORD error = ERROR_SUCCESS;
    if (credential_delete(&error) == 0) { chat_set_endpoint(g_cfg.webui_port, ""); chat_reconnect_async(); update_credential_ui(); }
    else {
        wchar_t message[160];
        swprintf_s(message, _countof(message), is_zh() ? L"无法清除认证密钥（错误 %lu）。" : L"Could not clear credential (error %lu).", (unsigned long)error);
        MessageBoxW(owner, message, is_zh() ? L"清除失败" : L"Clear failed", MB_OK | MB_ICONERROR);
    }
}

/* Apply gateway health and independent manager WebUI ports transactionally. */
static void on_apply_network(UiWidget w, void* ud) {
    (void)w; (void)ud;
    int new_health_port = 0, new_webui_port = 0;
    if (!read_port_input("healthPort", &new_health_port) || !read_port_input("webuiPort", &new_webui_port)) { restore_health_port_input(); restore_webui_port_input(); return; }
    nm_state lifecycle = nm_state_get();
    if (new_health_port != g_cfg.health_check_port && !can_edit_gateway_port()) {
        restore_health_port_input();
        MessageBoxW((HWND)ui_window_hwnd(g_win), is_zh() ? L"请停止 Gateway 后再修改 Gateway 端口。" : L"Stop the Gateway before changing its port.", is_zh() ? L"端口未修改" : L"Port not changed", MB_OK | MB_ICONINFORMATION);
        return;
    }
    app_config proposed = g_cfg;
    proposed.health_check_port = new_health_port;
    proposed.webui_port = new_webui_port;
    if (config_save(&proposed) != 0) {
        wchar_t message[256];
        if (is_zh())
            swprintf_s(message, _countof(message), L"无法保存网络设置（错误 %lu）。输入内容未更改。",
                       (unsigned long)config_get_last_error());
        else
            swprintf_s(message, _countof(message), L"Could not save network settings (error %lu). Your input was kept.",
                       (unsigned long)config_get_last_error());
        MessageBoxW((HWND)ui_window_hwnd(g_win), message,
                    is_zh() ? L"保存失败" : L"Save failed", MB_OK | MB_ICONERROR);
        return;
    }
    int webui_changed = g_cfg.webui_port != proposed.webui_port;
    g_cfg.health_check_port = proposed.health_check_port;
    g_cfg.webui_port = proposed.webui_port;
    if (can_edit_gateway_port()) {
        g_effective_health_port = g_cfg.health_check_port;
        refresh_effective_health_port();
    }
    restore_health_port_input(); restore_webui_port_input();
    chat_ui_set_gateway(health_running_thunk, g_cfg.webui_port);
    if (webui_changed) set_chat_endpoint_from_credential(1);
}

static void on_btn_apply_network_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_apply_network, NULL);
}

static void on_manage_credential_mount(UiPage p, UiWidget w, void* ud) { (void)p; (void)ud; ui_widget_on_click(w, on_manage_credential, NULL); }
static void on_clear_credential_mount(UiPage p, UiWidget w, void* ud) { (void)p; (void)ud; ui_widget_on_click(w, on_clear_credential, NULL); }

typedef enum { MIGRATION_SUCCESS, MIGRATION_CLEANUP_PENDING, MIGRATION_CONFLICT,
               MIGRATION_FAILURE } migration_result;

static int elevate_adjacent_legacy_scrub(void) {
    wchar_t exe[MAX_PATH];
    SHELLEXECUTEINFOW info;
    DWORD exit_code = 1;
    int present = 1;
    if (!GetModuleFileNameW(NULL, exe, _countof(exe))) return 0;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.lpParameters = L"--scrub-adjacent-legacy-auth";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) return 0;
    WaitForSingleObject(info.hProcess, INFINITE);
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
    return exit_code == 0 && config_adjacent_legacy_auth_present(&present) == 0 && !present;
}

static migration_result migrate_legacy_credential(void) {
    if (!g_cfg.legacy_migration_pending) return MIGRATION_SUCCESS;
    if (g_cfg.legacy_migration_pending != 1) return MIGRATION_FAILURE;
    if (!g_cfg.legacy_webui_auth[0]) return MIGRATION_FAILURE;
    char existing[128] = {0};
    DWORD error = ERROR_SUCCESS;
    int state = credential_load(existing, sizeof(existing), &error);
    int safe_to_remove = 0;
    if (state == 0) {
        safe_to_remove = credential_store(g_cfg.legacy_webui_auth, &error) == 0;
    } else if (state == 1) {
        safe_to_remove = strcmp(existing, g_cfg.legacy_webui_auth) == 0;
        if (!safe_to_remove) error = ERROR_ALREADY_EXISTS;
    }
    SecureZeroMemory(existing, sizeof(existing));
    if (!safe_to_remove)
        return error == ERROR_ALREADY_EXISTS ? MIGRATION_CONFLICT : MIGRATION_FAILURE;
    app_config cleaned = g_cfg;
    cleaned.legacy_migration_pending = 0;
    SecureZeroMemory(cleaned.legacy_webui_auth, sizeof(cleaned.legacy_webui_auth));
    if (config_save(&cleaned) != 0) return MIGRATION_FAILURE;
    int scrubbed = 1;
    if (g_cfg.legacy_source_path[0] &&
        _wcsicmp(g_cfg.legacy_source_path, config_get_active_path()) != 0) {
        scrubbed = config_scrub_legacy_auth(g_cfg.legacy_source_path) == 0;
        if (!scrubbed && config_get_last_error() == ERROR_ACCESS_DENIED)
            scrubbed = elevate_adjacent_legacy_scrub();
    }
    g_cfg.legacy_migration_pending = 0;
    SecureZeroMemory(g_cfg.legacy_webui_auth, sizeof(g_cfg.legacy_webui_auth));
    return scrubbed ? MIGRATION_SUCCESS : MIGRATION_CLEANUP_PENDING;
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

static void on_btn_refresh_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_page_set_bool(g_page, "canRefresh", 1);
    ui_widget_on_click(w, on_refresh, NULL);
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

static void on_hotkey_toggle(UiWidget w, int val, void* ud) {
    (void)w; (void)ud;
    g_cfg.hotkey_enabled = val;
    config_save(&g_cfg);
    ui_page_set_bool(g_page, "hotkeyEnabled", val);
    if (val) hotkey_register(g_hwnd);
    else     hotkey_unregister();
}

static void on_hotkey_toggle_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_toggle_on_changed(w, on_hotkey_toggle, NULL);
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

    /* No UI / no message loop in this path -- synchronous is fine (nothing to
     * freeze). This is the documented exception to "no resolve/health on the
     * UI thread". */
    nm_resolution res;
    nanobot_resolve(&g_cfg, &res, 0);
    if (!res.found) {
        nm_state_set_installed(res.status != NM_RES_NOT_FOUND);
        if (res.status == NM_RES_INCOMPATIBLE)
            autostart_log("nanobot incompatible: requires nanobot-ai >= 0.2.2 (missing gateway --background)");
        else if (res.status == NM_RES_PROBE_FAILED)
            autostart_log("nanobot capability probe failed, skipping");
        else
            autostart_log("nanobot not installed, skipping");
        nm_resolution_free(&res);
        return 0;
    }
    nm_state_set_installed(1);

    int detected_port = 0;
    if (nanobot_detect_running_port(g_cfg.health_check_port, &detected_port)) {
        nm_resolution_free(&res);
        char message[128];
        snprintf(message, sizeof(message), "nanobot already running on health port %d", detected_port);
        autostart_log(message);
        return 0;
    }

    int pid = 0;
    int effective_port = g_cfg.health_check_port;
    int rc = nanobot_start(res.exe, res.is_python, g_cfg.health_check_port, &pid,
                           &effective_port);
    nm_resolution_free(&res);
    if (rc != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "nanobot start failed (error %d)", rc);
        autostart_log(buf);
        return 0;
    }

    /* Synchronous boot poll -- same cadence as health_worker, but blocking.
     * On 200 -> "started"; on exhaustion (~30s) -> "timed out". */
    static const int cadence[] = {1000,1000,1500,2000,2000,3000,3000,5000,5000,5000};
    const int boot_count = (int)(sizeof(cadence) / sizeof(cadence[0]));
    for (int i = 0; i < boot_count; i++) {
        Sleep(cadence[i]);
        if (nanobot_detect_running_port(effective_port, &detected_port)) {
            autostart_log("nanobot started and health check passed");
            /* M6: release the OpenProcess handle the manager won't track further
             * in this no-tray path (no watch was armed). OS would reclaim it on
             * exit, but this is disciplined and keeps the log clean. */
            nm_state_clear();
            return 0;
        }
    }
    autostart_log("nanobot health check timed out");
    /* M6: same -- release the handle before the process exits. */
    nm_state_clear();
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    (void)hInst; (void)hPrev; (void)cmdLine; (void)nShow;

    /* Initialize guarded process state before any worker thread or read/write. */
    nm_state_init();

    if (has_arg(GetCommandLineW(), L"--delete-credential")) {
        return credential_delete(NULL) == 0 ? 0 : 1;
    }
    if (has_arg(GetCommandLineW(), L"--scrub-adjacent-legacy-auth")) {
        return config_scrub_adjacent_legacy_auth() == 0 ? 0 : 1;
    }

    if (config_load(&g_cfg) != 0) {
        wchar_t message[MAX_PATH + 256];
        swprintf_s(message, _countof(message),
            L"无法读取配置，程序未启动。\nCould not load configuration; startup was cancelled.\n\n错误 / Error: %lu\n路径 / Path: %s",
            (unsigned long)config_get_last_error(), config_get_active_path());
        if (has_arg(GetCommandLineW(), L"--autostart"))
            autostart_log("configuration load failed; autostart cancelled");
        else
            MessageBoxW(NULL, message, L"nanobot-manager - Configuration error", MB_OK | MB_ICONERROR);
        config_free(&g_cfg);
        return 1;
    }
    migration_result migration = migrate_legacy_credential();
    if (migration == MIGRATION_CONFLICT) {
        MessageBoxW(NULL,
            L"旧配置密钥与 Credential Manager 中现有密钥不同；两个来源均未修改，程序未启动。\n"
            L"The legacy secret conflicts with the Credential Manager value; neither source was changed and startup was cancelled.",
            L"nanobot-manager - Credential migration", MB_OK | MB_ICONERROR);
        config_free(&g_cfg);
        return 1;
    }
    if (migration == MIGRATION_FAILURE) {
        MessageBoxW(NULL,
            L"旧认证密钥无法安全迁移，程序未启动。请先在原配置可写的环境中重试。\n"
            L"The legacy credential could not be migrated safely; startup was cancelled.",
            L"nanobot-manager - Credential migration", MB_OK | MB_ICONERROR);
        config_free(&g_cfg);
        return 1;
    }
    if (migration == MIGRATION_CLEANUP_PENDING) {
        wchar_t warning[MAX_PATH + 256];
        swprintf_s(warning, _countof(warning),
            L"认证密钥和当前配置已安全迁移，但旧配置中的明文清理未完成（可能已取消 UAC），本次启动已取消。\n路径 / Path: %s\n\n"
            L"Credential and active configuration were secured, but legacy plaintext cleanup is pending (the UAC request may have been cancelled). Startup was cancelled.\nPath: %s",
            g_cfg.legacy_source_path, g_cfg.legacy_source_path);
        MessageBoxW(NULL, warning, L"nanobot-manager - Credential migration", MB_OK | MB_ICONERROR);
        config_free(&g_cfg);
        return 1;
    }
    g_tray_enabled_effective = g_cfg.stay_in_tray;

    LPCWSTR wcmd = GetCommandLineW();
    if (has_arg(wcmd, L"--autostart")) {
        if (!g_cfg.stay_in_tray) {
            int rc = run_autostart_mode();
            config_free(&g_cfg);
            return rc;
        }
        g_autostart_silent = 1;
        /* Silent-tray resolve+start is routed through the resolve worker
         * (intent=1) after the hidden window + subclass are in place. No
         * UI-thread resolve/check_running here. */
    }

    /* Both normal startup (intent=0 detect) and silent-tray (intent=1 start)
     * kick the resolve worker after the window is created and subclassed -- the
     * worker posts to that HWND, which the message loop then dispatches. */

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
        /* Non-status keys only. status_* wording is generated from g_status_i18n
         * so C++ push and $t() cannot drift (single source). */
        const char* zh_prefix =
            "app_title=nanobot 管理器\n"
            "chat_empty=开始对话\n"
            "chat_placeholder=输入消息…\n"
            "chat_send=发送\n"
            "install_not_found=未找到 nanobot\n"
            "install_hint=请安装：python -m pip install -U \"nanobot-ai>=0.2.2\"\n"
            "metric_port=Gateway 端口\n"
            "metric_pid=进程\n"
            "metric_uptime=运行时间\n"
            "btn_start=启动\n"
            "btn_stop=停止\n"
            "btn_refresh=刷新\n"
            "settings_general=通用\n"
            "settings_appearance=外观\n"
            "setting_autostart=开机自启\n"
            "setting_autostart_sub=Windows 启动时自动运行\n"
            "setting_tray=最小化到托盘\n"
            "setting_tray_sub=关闭窗口时隐藏到系统托盘\n"
            "setting_hotkey=快捷键呼出\n"
            "setting_hotkey_sub=按 Ctrl+Shift+N 显示窗口\n"
            "setting_path=nanobot 路径\n"
            "setting_path_change=更改\n"
            "setting_theme=主题\n"
            "setting_language=语言\n"
            "open_webui=打开 WebUI\n"
            "settings_network=网络\n"
            "setting_gateway_port=Gateway 端口\n"
            "setting_gateway_port_sub=用于 /health；下次启动生效\n"
            "setting_webui_port=WebUI 端口\n"
            "setting_webui_port_sub=仅 Manager 连接覆盖，与 Gateway 端口独立\n"
            "setting_credential_status=认证密钥\n"
            "setting_credential_manage=管理\n"
            "setting_credential_clear=清除\n"
            "setting_apply=应用\n"
            "auto_detect=自动检测\n";
        const char* en_prefix =
            "app_title=nanobot manager\n"
            "chat_empty=Start a conversation\n"
            "chat_placeholder=Type a message…\n"
            "chat_send=Send\n"
            "install_not_found=nanobot not found\n"
            "install_hint=Please install: python -m pip install -U \"nanobot-ai>=0.2.2\"\n"
            "metric_port=Gateway Port\n"
            "metric_pid=PID\n"
            "metric_uptime=Uptime\n"
            "btn_start=Start\n"
            "btn_stop=Stop\n"
            "btn_refresh=Refresh\n"
            "settings_general=General\n"
            "settings_appearance=Appearance\n"
            "setting_autostart=Auto-start on boot\n"
            "setting_autostart_sub=Launch when Windows starts\n"
            "setting_tray=Minimize to tray\n"
            "setting_tray_sub=Hide to system tray on close\n"
            "setting_hotkey=Summon hotkey\n"
            "setting_hotkey_sub=Press Ctrl+Shift+N to show the window\n"
            "setting_path=nanobot path\n"
            "setting_path_change=Change\n"
            "setting_theme=Theme\n"
            "setting_language=Language\n"
            "open_webui=Open WebUI\n"
            "settings_network=Network\n"
            "setting_gateway_port=Gateway Port\n"
            "setting_gateway_port_sub=Used for /health; applies on next start\n"
            "setting_webui_port=WebUI Port\n"
            "setting_webui_port_sub=Manager connection override only; independent of Gateway port\n"
            "setting_credential_status=Bootstrap Secret\n"
            "setting_credential_manage=Manage\n"
            "setting_credential_clear=Clear\n"
            "setting_apply=Apply\n"
            "auto_detect=Auto-detect\n";

        char zh_blob[8192];
        char en_blob[8192];
        size_t zh_used = 0, en_used = 0;
        zh_blob[0] = '\0';
        en_blob[0] = '\0';

        /* Copy prefix, then append status_* lines from g_status_i18n. */
        int ok = 1;
        {
            size_t n = strlen(zh_prefix);
            if (n + 1 > sizeof(zh_blob)) ok = 0;
            else { memcpy(zh_blob, zh_prefix, n + 1); zh_used = n; }
        }
        {
            size_t n = strlen(en_prefix);
            if (n + 1 > sizeof(en_blob)) ok = 0;
            else { memcpy(en_blob, en_prefix, n + 1); en_used = n; }
        }
        if (ok) ok = append_status_i18n_lang(zh_blob, sizeof(zh_blob), &zh_used, 1);
        if (ok) ok = append_status_i18n_lang(en_blob, sizeof(en_blob), &en_used, 0);

        if (ok) {
            ui_page_load_language_string(g_page, "zh", zh_blob);
            ui_page_load_language_string(g_page, "en", en_blob);
        } else {
            /* Overflow should never happen with current key set; fall back to
             * prefix alone so the UI still boots (status keys fall back to key). */
            ui_page_load_language_string(g_page, "zh", zh_prefix);
            ui_page_load_language_string(g_page, "en", en_prefix);
        }
    }

    /* Initial UI: "Detecting...". The resolve worker (kicked below once the
     * window exists) flips installed/canStart and drives running/stopped via
     * WM_ASYNC_RESOLVE_DONE + WM_ASYNC_HEALTH_RESULT. */
    set_status_i18n("starting", "status_detecting", "status_detecting");
    ui_page_set_bool(g_page, "installed", 0);
    /* canStart/canStop are set by set_status_i18n("starting",...) above to 0/0 */
    ui_page_set_bool(g_page, "autostart", autostart_is_enabled());
    ui_page_set_bool(g_page, "stayInTray", g_cfg.stay_in_tray);
    ui_page_set_bool(g_page, "hotkeyEnabled", g_cfg.hotkey_enabled);

    wchar_t port_w[16];
    g_effective_health_port = g_cfg.health_check_port;
    swprintf_s(port_w, _countof(port_w), L"%d", g_cfg.health_check_port);
    ui_page_set_text(g_page, "port", port_w);
    ui_page_set_text(g_page, "healthPort", port_w);

    if (g_cfg.nanobot_path && g_cfg.nanobot_path[0]) {
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, g_cfg.nanobot_path, -1, wpath, MAX_PATH);
        ui_page_set_text(g_page, "nanobotPath", wpath);
    } else {
        page_set("nanobotPath", is_zh() ? "自动检测" : "Auto-detect");
    }

    /* WebUI port and Credential Manager state initial values. */
    {
        wchar_t wpw[16];
        swprintf_s(wpw, _countof(wpw), L"%d",
                   g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
        ui_page_set_text(g_page, "webuiPort", wpw);
    }
    update_credential_ui();

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

    /* Chat initial state */
    chat_ui_bind(g_page, is_zh);
    chat_ui_set_gateway(health_running_thunk, g_cfg.webui_port);
    chat_ui_reset_idle();
    chat_ui_configure_history(config_get_active_path());

    ui_page_on_widget_mount(g_page, "btn_start", on_btn_start_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_stop",  on_btn_stop_mount,  NULL);
    ui_page_on_widget_mount(g_page, "btn_refresh", on_btn_refresh_mount, NULL);
    ui_page_on_widget_mount(g_page, "toggle_autostart", on_toggle_mount, NULL);
    ui_page_on_widget_mount(g_page, "toggle_tray", on_tray_toggle_mount, NULL);
    ui_page_on_widget_mount(g_page, "toggle_hotkey", on_hotkey_toggle_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_change_path", on_btn_change_path_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_webui", on_btn_webui_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_apply_network", on_btn_apply_network_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_manage_credential", on_manage_credential_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_clear_credential", on_clear_credential_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_send", chat_ui_mount_send, NULL);
    ui_page_on_widget_mount(g_page, "chat_input", chat_ui_mount_input, NULL);
    ui_page_on_widget_mount(g_page, "chat_scroll", chat_ui_mount_scroll, NULL);
    ui_page_on_widget_unmount(g_page, "chat_scroll", chat_ui_unmount_scroll, NULL);
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
        chat_ui_shutdown();
        ui_page_destroy(g_page);
        config_free(&g_cfg);
        return 2;
    }

    HWND hwnd = (HWND)ui_window_hwnd(g_win);
    g_hwnd = hwnd;
    SetWindowSubclass(hwnd, subclass_proc, 0, 0);
    g_chat_ui_timer = SetTimer(hwnd, TIMER_CHAT_UI, CHAT_UI_FRAME_MS, on_timer_chat_ui);
    if (g_tray_enabled_effective)
        tray_add(hwnd);
    if (g_cfg.hotkey_enabled)
        hotkey_register(hwnd);

    set_chat_endpoint_from_credential(0);
    chat_init(hwnd, WM_CHAT_EVENT);

    /* Initial detect (or start, for silent-tray) off the UI thread. UI shows
     * "Detecting..." until the first WM_ASYNC_RESOLVE_DONE arrives. */
    kick_resolve_worker(hwnd, g_autostart_silent ? 1 : 0);

    int ret = ui_run();

    /* Drop UI first so any late chat events free LPARAMs without touching g_page. */
    chat_shutdown();

    /* P5a: unwatch + close our OpenProcess handle on shutdown. The stop_on_exit
     * decision is "leave running", so we do NOT call nanobot_stop() -- closing
     * an OpenProcess handle does NOT kill the process; the gateway keeps running. */
    nm_state_clear();

    /* Drain leftover WM_CHAT_EVENT payloads (malloc'd LPARAM) after shutdown. */
    {
        MSG m;
        while (PeekMessageW(&m, hwnd, WM_CHAT_EVENT, WM_CHAT_EVENT, PM_REMOVE)) {
            chat_event_free((chat_event*)m.lParam);
        }
    }

    stop_uptime_timer();
    tray_remove();
    hotkey_unregister();
    RemoveWindowSubclass(hwnd, subclass_proc, 0);
    if (g_chat_ui_timer) {
        KillTimer(hwnd, TIMER_CHAT_UI);
        g_chat_ui_timer = 0;
    }
    chat_ui_shutdown();
    ui_page_destroy(g_page);
    config_free(&g_cfg);
    return ret;
}
