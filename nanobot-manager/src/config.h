#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* nanobot_path;
    int autostart_nanobot;
    int stay_in_tray;
    int health_check_port;
    int theme_mode;
    char language[8];
    int webui_port;           /* default 8765 */
    char webui_auth[128];     /* optional tokenIssueSecret, default "" */
} app_config;

int  config_load(app_config* cfg);
int  config_save(const app_config* cfg);
void config_free(app_config* cfg);

/* --- Testable core (no filesystem dependency) --- */

/* Zero the struct and fill in defaults (ports, theme, language, empty auth). */
void config_apply_defaults(app_config* cfg);

/* Overlay parsed JSON fields onto cfg (call config_apply_defaults first).
 * Returns 0 on success (invalid JSON is tolerated: fields keep prior values).
 * Validates ranges: ports 1..65535, theme 0..2; out-of-range values ignored. */
int  config_parse_json(app_config* cfg, const char* json);

/* Serialize cfg to a malloc'd NUL-terminated JSON string. Caller frees. */
char* config_serialize_json(const app_config* cfg);

#ifdef __cplusplus
}
#endif

#endif
