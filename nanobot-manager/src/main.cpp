#include "config.h"
#include "process.h"
#include "autostart.h"
#include "chat.h"
#include "chat_ui.h"
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
static int       g_theme_mode = 0;
static int       g_tray_enabled_effective = 0;
static int       g_allow_exit = 0;
static int       g_tray_added = 0;
static int       g_tray_icon_owned = 0;
static HICON     g_tray_icon = NULL;
static HWND      g_hwnd = NULL;
static int       g_autostart_silent = 0;

#define TIMER_UPTIME 1003
#define ID_TRAY_SHOW 40001
#define ID_TRAY_QUIT 40002

#define WM_ASYNC_STOP_DONE      (WM_APP + 2)
#define WM_TRAY_ICON            (WM_APP + 3)
#define WM_CHILD_EXITED         (WM_APP + 4)   /* P5a: posted by the thread-pool wait callback
                                                * when g_state.handle becomes signaled       */
#define WM_ASYNC_HEALTH_RESULT  (WM_APP + 5)   /* WPARAM = running(0/1)                                       */
#define WM_ASYNC_RESOLVE_DONE   (WM_APP + 6)   /* WPARAM = start_intent(0/1), LPARAM = resolve_payload* (malloc'd; handler frees) */
#define WM_CHAT_EVENT           (WM_APP + 10)

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
    } else if (strcmp(status, "starting") == 0 || strcmp(status, "stopping") == 0) {
        /* M5: "stopping" disables both buttons -- same as "starting". */
        ui_page_set_bool(g_page, "canStart", 0);
        ui_page_set_bool(g_page, "canStop", 0);
    } else {
        ui_page_set_bool(g_page, "canStart", 1);
        ui_page_set_bool(g_page, "canStop", 0);
    }
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
    { "status_stopping",             "停止中…",                        "Stopping…" },
    { "status_stopping_sub",         "正在关闭 nanobot",               "Shutting down nanobot" },
    { "status_detecting",            "检测中…",                        "Detecting…" },
    { "status_detecting_nanobot",    "检测中…",                        "Detecting nanobot…" },
    { "status_not_found",            "未找到",                         "Not found" },
    { "status_not_found_sub",        "找不到 nanobot，请安装或设置路径",
                                     "Cannot find nanobot. Please install or set path." },
    { "status_start_fail",           "启动失败",                       "Start failed" },
    { "status_not_installed",        "未安装",                         "Not installed" },
    { "status_not_installed_sub",    "nanobot 未安装",                 "nanobot is not installed" },
    { "status_fail_no_statefile_sub","后台启动器未写入状态文件",
                                     "Background launcher did not write state file" },
    { "status_fail_open_failed_sub", "OpenProcess 失败",               "OpenProcess failed" },
    { "status_fail_stale_pid_sub",   "状态文件中的 PID 已失效",        "State file PID is stale" },
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

static void apply_health_result(int running) {
    if (running) {
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
        if (!chat_is_ready()) {
            chat_set_endpoint(g_cfg.webui_port, g_cfg.webui_auth);
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
    chat_disconnect_async();
    chat_ui_on_gateway_stopped();
}

/* --- Health worker (boot-phase backoff -> steady) --------------------------
 * SOLE caller of nanobot_check_running after P3b (besides run_autostart_mode's
 * synchronous loop, which has no UI thread). Posts WM_ASYNC_HEALTH_RESULT; the
 * UI thread never blocks on HTTP.
 *
 * Generation counter: each kick increments g_health_gen and the fresh worker
 * captures its own gen. A newer kick stale-marks older workers, which exit on
 * the next bail check without posting more results. */
static volatile LONG g_health_gen = 0;

static DWORD WINAPI health_worker(LPVOID param) {
    HWND hwnd = (HWND)param;
    LONG my_gen = InterlockedIncrement(&g_health_gen);
    int  port   = g_cfg.health_check_port;

    static const int cadence[] = {1000,1000,1500,2000,2000,3000,3000,5000,5000,5000};
    const int boot_count = (int)(sizeof(cadence) / sizeof(cadence[0]));
    int i = 0;

    for (;;) {
        int running = nanobot_check_running(port);

        /* M7: bail BEFORE posting so a superseded worker does not inject a
         * stale result after a newer kick. The stale blip was masked by streak
         * tolerance, but the ordering should be correct. */
        if (InterlockedCompareExchange(&g_health_gen, my_gen, my_gen) != my_gen)
            return 0;
        /* Stop/dead happened on the UI thread? RESOLVING is allowed so the
         * detect-only startup path (M2 leaves RESOLVING until the first health
         * result) can post; apply_health_result then settles to RUNNING/IDLE. */
        nm_state s = nm_state_get();
        if (s != NM_STATE_STARTING && s != NM_STATE_RUNNING && s != NM_STATE_RESOLVING)
            return 0;

        PostMessageW(hwnd, WM_ASYNC_HEALTH_RESULT, (WPARAM)(running ? 1 : 0), 0);

        int delay = (i < boot_count) ? cadence[i] : 15000;
        Sleep(delay);
        if (i < boot_count) i++;
    }
    return 0;
}

static void kick_health_worker(HWND hwnd) {
    InterlockedIncrement(&g_health_gen);  /* stale-mark any running worker    */
    CloseHandle(CreateThread(NULL, 0, health_worker, hwnd, 0, NULL));
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
    nm_resolution res;
    if (nanobot_resolve(&cfg_snap, &res, 0) && res.found) {
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
    resolve_ctx* c = (resolve_ctx*)calloc(1, sizeof(resolve_ctx));
    if (!c) return;
    c->hwnd = hwnd;
    c->start_intent = start_intent;
    /* I1: snapshot nanobot_path on the UI thread (serialized with on_change_path). */
    c->nanobot_path_snap = _strdup(g_cfg.nanobot_path ? g_cfg.nanobot_path : "");
    /* M2: reflect the resolving phase. Cleared in on_resolve_done (-> STARTING
     * on start success, -> IDLE on terminal paths) and in apply_health_result
     * (-> RUNNING / IDLE). */
    nm_state_set_state(NM_STATE_RESOLVING);
    CloseHandle(CreateThread(NULL, 0, resolve_worker, c, 0, NULL));
}

/* UI-thread handler for WM_ASYNC_RESOLVE_DONE. Frees payload (exe + struct)
 * on every return path. */
static void on_resolve_done(resolve_payload* p, int start_intent) {
    if (!p) return;

    nm_state_set_installed(p->found ? 1 : 0);
    ui_page_set_bool(g_page, "installed", p->found ? 1 : 0);
    ui_page_set_bool(g_page, "canStart",  p->found ? 1 : 0);

    if (p->exe) {
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, p->exe, -1, wpath, MAX_PATH);
        ui_page_set_text(g_page, "nanobotPath", wpath);
    }

    if (!p->found) {
        /* M2: resolve failed -> settle to IDLE (clears RESOLVING). */
        nm_state_set_state(NM_STATE_IDLE);
        set_status_i18n("stopped", "status_not_found", "status_not_found_sub");
        free(p->exe);
        free(p);
        return;
    }

    if (start_intent) {
        int pid_local = 0;
        int rc = nanobot_start(p->exe, p->is_python, &pid_local);

        if (rc == 0) {
            nm_state_set_state(NM_STATE_STARTING);   /* M2: clears RESOLVING */
            nm_state_set_pid((DWORD)pid_local);
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
        apply_health_result((int)wp);
        return 0;
    }
    if (msg == WM_ASYNC_STOP_DONE) {
        /* Stop is authoritative -- the stop_worker already killed the gateway.
         * No re-probe needed; just flip UI + guarded state. */
        nm_state_set_state(NM_STATE_IDLE);
        set_status_i18n("stopped", "status_stopped", "status_stopped_sub");
        stop_uptime_timer();
        nm_state_set_pid(0);
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
        chat_disconnect_async();
        chat_ui_on_gateway_stopped();
        return 0;
    }
    if (msg == WM_CHAT_EVENT) {
        chat_ui_on_event(wp, (char*)lp);
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
    int webui_port = g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765;
    swprintf_s(url, _countof(url), L"http://localhost:%d", webui_port);
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

static void on_btn_send_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    chat_ui_mount_send(p, w, NULL);
}

/* Apply WebUI port + auth from the Network section of the settings page. */
static void on_apply_network(UiWidget w, void* ud) {
    (void)w; (void)ud;
    /* Read webuiPort (number input may serialize as string or number) */
    int new_port = 0;
    char* raw_port = ui_page_get_json(g_page, "webuiPort");
    if (raw_port) {
        cJSON* jp = cJSON_Parse(raw_port);
        if (jp && cJSON_IsString(jp))
            new_port = atoi(jp->valuestring);
        else if (jp && cJSON_IsNumber(jp))
            new_port = jp->valueint;
        cJSON_Delete(jp);
        ui_page_free(raw_port);
    }
    if (new_port < 1 || new_port > 65535) {
        /* Invalid: restore the current value and bail */
        wchar_t wpw[16];
        swprintf_s(wpw, _countof(wpw), L"%d",
                   g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
        ui_page_set_text(g_page, "webuiPort", wpw);
        return;
    }
    /* Read webuiAuth (optional) */
    char new_auth[128] = {0};
    char* raw_auth = ui_page_get_json(g_page, "webuiAuth");
    if (raw_auth) {
        cJSON* ja = cJSON_Parse(raw_auth);
        if (ja && cJSON_IsString(ja) && ja->valuestring) {
            strncpy(new_auth, ja->valuestring, sizeof(new_auth) - 1);
            new_auth[sizeof(new_auth) - 1] = '\0';
        }
        cJSON_Delete(ja);
        ui_page_free(raw_auth);
    }
    g_cfg.webui_port = new_port;
    strncpy(g_cfg.webui_auth, new_auth, sizeof(g_cfg.webui_auth) - 1);
    g_cfg.webui_auth[sizeof(g_cfg.webui_auth) - 1] = '\0';
    config_save(&g_cfg);
    /* Sync into the chat subsystem and reconnect */
    chat_ui_set_gateway(health_running_thunk, g_cfg.webui_port, g_cfg.webui_auth);
    chat_set_endpoint(g_cfg.webui_port, g_cfg.webui_auth);
    chat_connect();
}

static void on_btn_apply_network_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_apply_network, NULL);
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

    /* No UI / no message loop in this path -- synchronous is fine (nothing to
     * freeze). This is the documented exception to "no resolve/health on the
     * UI thread". */
    nm_resolution res;
    if (!nanobot_resolve(&g_cfg, &res, 0) || !res.found) {
        nm_resolution_free(&res);
        nm_state_set_installed(0);
        autostart_log("nanobot not installed, skipping");
        return 0;
    }
    nm_state_set_installed(1);

    if (nanobot_check_running(g_cfg.health_check_port)) {
        nm_resolution_free(&res);
        autostart_log("nanobot already running");
        return 0;
    }

    int pid = 0;
    int rc = nanobot_start(res.exe, res.is_python, &pid);
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
        if (nanobot_check_running(g_cfg.health_check_port)) {
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
            "install_hint=请安装：pip install nanobot-ai\n"
            "metric_port=端口\n"
            "metric_pid=进程\n"
            "metric_uptime=运行时间\n"
            "btn_start=启动\n"
            "btn_stop=停止\n"
            "settings_general=通用\n"
            "settings_appearance=外观\n"
            "setting_autostart=开机自启\n"
            "setting_autostart_sub=Windows 启动时自动运行\n"
            "setting_tray=最小化到托盘\n"
            "setting_tray_sub=关闭窗口时隐藏到系统托盘\n"
            "setting_path=nanobot 路径\n"
            "setting_path_change=更改\n"
            "setting_theme=主题\n"
            "setting_language=语言\n"
            "open_webui=打开 WebUI\n"
            "settings_network=网络\n"
            "setting_webui_port=WebUI 端口\n"
            "setting_webui_port_sub=本地 WebSocket 网关端口\n"
            "setting_webui_auth=认证密钥\n"
            "setting_webui_auth_sub=可选，用于引导握手（tokenIssueSecret）\n"
            "setting_apply=应用\n"
            "auto_detect=自动检测\n";
        const char* en_prefix =
            "app_title=nanobot manager\n"
            "chat_empty=Start a conversation\n"
            "chat_placeholder=Type a message…\n"
            "chat_send=Send\n"
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
            "settings_network=Network\n"
            "setting_webui_port=WebUI Port\n"
            "setting_webui_port_sub=Local WebSocket gateway port\n"
            "setting_webui_auth=Auth Secret\n"
            "setting_webui_auth_sub=Optional, bootstrap handshake secret (tokenIssueSecret)\n"
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

    wchar_t port_w[16];
    swprintf_s(port_w, _countof(port_w), L"%d", g_cfg.health_check_port);
    ui_page_set_text(g_page, "port", port_w);

    if (g_cfg.nanobot_path && g_cfg.nanobot_path[0]) {
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, g_cfg.nanobot_path, -1, wpath, MAX_PATH);
        ui_page_set_text(g_page, "nanobotPath", wpath);
    } else {
        page_set("nanobotPath", is_zh() ? "自动检测" : "Auto-detect");
    }

    /* WebUI port + auth initial values */
    {
        wchar_t wpw[16];
        swprintf_s(wpw, _countof(wpw), L"%d",
                   g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
        ui_page_set_text(g_page, "webuiPort", wpw);
        page_set("webuiAuth", g_cfg.webui_auth[0] ? g_cfg.webui_auth : "");
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

    /* Chat initial state */
    chat_ui_bind(g_page, is_zh);
    chat_ui_set_gateway(health_running_thunk, g_cfg.webui_port, g_cfg.webui_auth);
    chat_ui_reset_idle();

    ui_page_on_widget_mount(g_page, "btn_start", on_btn_start_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_stop",  on_btn_stop_mount,  NULL);
    ui_page_on_widget_mount(g_page, "toggle_autostart", on_toggle_mount, NULL);
    ui_page_on_widget_mount(g_page, "toggle_tray", on_tray_toggle_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_change_path", on_btn_change_path_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_webui", on_btn_webui_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_apply_network", on_btn_apply_network_mount, NULL);
    ui_page_on_widget_mount(g_page, "btn_send", on_btn_send_mount, NULL);
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
    if (g_tray_enabled_effective)
        tray_add(hwnd);

    chat_set_endpoint(g_cfg.webui_port, g_cfg.webui_auth);
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
            free((void*)m.lParam);
        }
    }

    stop_uptime_timer();
    tray_remove();
    RemoveWindowSubclass(hwnd, subclass_proc, 0);
    chat_ui_shutdown();
    ui_page_destroy(g_page);
    config_free(&g_cfg);
    return ret;
}
