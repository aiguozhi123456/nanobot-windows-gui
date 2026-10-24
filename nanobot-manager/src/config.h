#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* nanobot_path;
    int autostart_nanobot;
    int stay_in_tray;
    int hotkey_enabled;     /* global Ctrl+Shift+N summon hotkey, default on */
    int health_check_port;
    int theme_mode;
    char language[8];
    int webui_port;           /* 0 = auto-detect from nanobot config (default);
                                   legacy serialized 8765 normalizes to 0;
                                   other values pin the port explicitly */
    char legacy_webui_auth[128]; /* transient old JSON secret; never serialized */
    int legacy_migration_pending;
    wchar_t legacy_source_path[MAX_PATH]; /* file containing transient legacy secret */
} app_config;

int  config_load(app_config* cfg);
int  config_save(const app_config* cfg);
void config_free(app_config* cfg);
DWORD config_get_last_error(void);
const wchar_t* config_get_active_path(void);

/* --- Testable core (no filesystem dependency) --- */

/* Zero the struct and fill in defaults (ports, theme, language, empty auth). */
void config_apply_defaults(app_config* cfg);

/* Overlay parsed JSON fields onto cfg (call config_apply_defaults first).
 * Returns 0 on success (invalid JSON is tolerated: fields keep prior values).
 * Validates ranges: ports 1..65535, theme 0..2; out-of-range values ignored. */
int  config_parse_json(app_config* cfg, const char* json);

/* Serialize cfg to a malloc'd NUL-terminated JSON string. Caller frees. */
char* config_serialize_json(const app_config* cfg);

/* Remove only webui_auth from a JSON document while retaining every other
 * field (including fields unknown to this manager). Caller frees the result. */
int  config_remove_legacy_auth_json(const char* json, char** out_json);

/* Atomically remove webui_auth from the specified legacy JSON file. */
int  config_scrub_legacy_auth(const wchar_t* path);

/* Exact executable-adjacent config.json helpers. These intentionally accept no
 * caller-provided path so the elevated scrub command cannot be redirected. */
int  config_scrub_adjacent_legacy_auth(void);
int  config_adjacent_legacy_auth_present(int* out_present);

/* Pure migration-source policy: any active migration state owns this run. */
int  config_should_scan_adjacent_legacy(int active_legacy_migration_pending);

/* Strict ASCII decimal port parser. Accepts leading zeroes, but no whitespace,
 * signs, decimal/exponent notation, or non-ASCII numerals. */
int  config_parse_port(const char* text, int* out_port);

#ifdef __cplusplus
}
#endif

#endif
