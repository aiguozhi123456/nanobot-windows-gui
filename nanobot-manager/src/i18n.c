#include "i18n.h"

#include <stdio.h>
#include <string.h>

/* Capacity of the assembled tables. The prefix is ~1.4 KB and the status
 * table adds ~30 short lines; 8192 matches the WinMain stack buffers this
 * module replaced. */
#define I18N_TABLE_CAP 8192

/* --- Non-status keys ---------------------------------------------------------
 * Referenced only from dashboard.uix via $t(). Static keys deliberately stay
 * out of the status table: they have no C++ push path, so there is nothing to
 * drift against. Every key in the zh block must appear in the en block and
 * vice versa (enforced by tests/test_i18n.c). */
static const char s_prefix_zh[] =
    "app_title=nanobot 管理器\n"
    "chat_empty=开始对话\n"
    "chat_placeholder=输入消息…\n"
    "chat_send=发送\n"
    "install_not_found=未找到 nanobot\n"
    "install_hint=PowerShell 一键安装：irm https://raw.githubusercontent.com/HKUDS/nanobot/main/scripts/install.ps1 | iex\n"
    "install_action=一键安装\n"
    "install_running=安装窗口已打开，关闭后将自动重新检测\n"
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
    "setting_hotkey_sub=显示或隐藏窗口 (Ctrl+Shift+N)\n"
    "setting_path=nanobot 路径\n"
    "setting_path_change=更改\n"
    "setting_theme=主题\n"
    "setting_language=语言\n"
    "open_webui=打开 WebUI\n"
    "settings_network=网络\n"
    "setting_gateway_port=Gateway 端口\n"
    "setting_webui_port=WebUI 端口\n"
    "setting_credential_status=认证密钥\n"
    "setting_credential_manage=管理\n"
    "setting_credential_clear=清除\n"
    "setting_apply=应用\n"
    "auto_detect=自动检测\n";

static const char s_prefix_en[] =
    "app_title=nanobot manager\n"
    "chat_empty=Start a conversation\n"
    "chat_placeholder=Type a message…\n"
    "chat_send=Send\n"
    "install_not_found=nanobot not found\n"
    "install_hint=One-click install in PowerShell: irm https://raw.githubusercontent.com/HKUDS/nanobot/main/scripts/install.ps1 | iex\n"
    "install_action=Install nanobot\n"
    "install_running=The install window is open; detection re-runs when it closes\n"
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
    "setting_hotkey_sub=Show or hide window (Ctrl+Shift+N)\n"
    "setting_path=nanobot path\n"
    "setting_path_change=Change\n"
    "setting_theme=Theme\n"
    "setting_language=Language\n"
    "open_webui=Open WebUI\n"
    "settings_network=Network\n"
    "setting_gateway_port=Gateway Port\n"
    "setting_webui_port=WebUI Port\n"
    "setting_credential_status=Bootstrap Secret\n"
    "setting_credential_manage=Manage\n"
    "setting_credential_clear=Clear\n"
    "setting_apply=Apply\n"
    "auto_detect=Auto-detect\n";

/* --- Status keys: single wording source ---------------------------------------
 * Status text is both $t('status_*') in the template and pushed directly from
 * C++ via page_set. This table is the ONLY wording source: i18n_status()
 * resolves keys for the C++ push, and build_table() generates the status_*
 * portion of the blob passed to ui_page_load_language_string — the two sides
 * read one table and cannot drift. */
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

const char* i18n_status(const char* key, int zh) {
    if (!key || !key[0]) return "";
    for (int i = 0; i < (int)(sizeof(g_status_i18n) / sizeof(g_status_i18n[0])); i++) {
        if (strcmp(g_status_i18n[i].key, key) == 0)
            return zh ? g_status_i18n[i].zh : g_status_i18n[i].en;
    }
    return key;   /* missing key surfaces itself (same fallback as $t) */
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

static char s_table_zh[I18N_TABLE_CAP];
static char s_table_en[I18N_TABLE_CAP];
static int s_tables_built = 0;

/* Copy prefix, then append the status_* lines from g_status_i18n. On overflow
 * (unreachable with the current key set) fall back to the prefix alone so the
 * UI still boots — status keys then fall back to their key in $t(). */
static void build_table(char* buf, size_t cap, const char* prefix, int zh) {
    size_t used = 0;
    size_t n = strlen(prefix);
    int ok = (n + 1 <= cap);
    if (ok) { memcpy(buf, prefix, n + 1); used = n; }
    if (ok) ok = append_status_i18n_lang(buf, cap, &used, zh);
    if (!ok) {
        n = strlen(prefix);
        if (n + 1 > cap) n = cap - 1;   /* defensive; cannot happen today */
        memcpy(buf, prefix, n);
        buf[n] = '\0';
    }
}

const char* i18n_table(const char* locale) {
    if (!locale) return NULL;
    if (!s_tables_built) {
        build_table(s_table_zh, sizeof(s_table_zh), s_prefix_zh, 1);
        build_table(s_table_en, sizeof(s_table_en), s_prefix_en, 0);
        s_tables_built = 1;
    }
    if (strcmp(locale, "zh") == 0) return s_table_zh;
    if (strcmp(locale, "en") == 0) return s_table_en;
    return NULL;
}
