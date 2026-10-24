#include "nm_test.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int configs_equal(const app_config* left, const app_config* right) {
    const char* left_path = left->nanobot_path ? left->nanobot_path : "";
    const char* right_path = right->nanobot_path ? right->nanobot_path : "";
    return strcmp(left_path, right_path) == 0 &&
        left->autostart_nanobot == right->autostart_nanobot &&
        left->stay_in_tray == right->stay_in_tray &&
        left->hotkey_enabled == right->hotkey_enabled &&
        left->health_check_port == right->health_check_port &&
        left->theme_mode == right->theme_mode &&
        strcmp(left->language, right->language) == 0 &&
        strcmp(left->log_level, right->log_level) == 0 &&
        left->webui_port == right->webui_port;
}

static const char* FULL_JSON =
    "{\"nanobot_path\":\"C:\\\\tools\\\\nanobot.exe\","
    "\"autostart_nanobot\":true,\"stay_in_tray\":true,\"hotkey_enabled\":false,"
    "\"health_check_port\":19000,\"theme_mode\":2,"
    "\"language\":\"en\",\"webui_port\":9000,\"log_level\":\"warn\","
    "\"webui_auth\":\"nbwt_abcdef\"}";

static void test_defaults(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    TEST_CHECK(cfg.nanobot_path == NULL);
    TEST_CHECK(cfg.autostart_nanobot == 0);
    TEST_CHECK(cfg.stay_in_tray == 0);
    TEST_CHECK(cfg.hotkey_enabled == 1);
    TEST_CHECK(cfg.health_check_port == 0);   /* 0 = auto: follow the running gateway */
    TEST_CHECK(cfg.webui_port == 0);   /* 0 = auto-detect from nanobot config */
    TEST_CHECK(cfg.theme_mode == 0);
    TEST_CHECK(strcmp(cfg.language, "zh") == 0);
    TEST_CHECK(strcmp(cfg.log_level, "info") == 0);
    config_free(&cfg);
}

static void test_parse_full(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, FULL_JSON) == 0);
    TEST_CHECK(cfg.nanobot_path && strcmp(cfg.nanobot_path, "C:\\tools\\nanobot.exe") == 0);
    TEST_CHECK(cfg.autostart_nanobot == 1 && cfg.stay_in_tray == 1 && cfg.hotkey_enabled == 0);
    TEST_CHECK(cfg.health_check_port == 19000 && cfg.webui_port == 9000);
    TEST_CHECK(cfg.theme_mode == 2 && strcmp(cfg.language, "en") == 0);
    TEST_CHECK(strcmp(cfg.log_level, "warn") == 0);
    config_free(&cfg);
}

static void test_log_level_validation(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    /* Valid values pass through. */
    TEST_CHECK(config_parse_json(&cfg, "{\"log_level\":\"debug\"}") == 0);
    TEST_CHECK(strcmp(cfg.log_level, "debug") == 0);
    TEST_CHECK(config_parse_json(&cfg, "{\"log_level\":\"error\"}") == 0);
    TEST_CHECK(strcmp(cfg.log_level, "error") == 0);
    /* Invalid / missing values keep the prior value (default info). */
    TEST_CHECK(config_parse_json(&cfg, "{\"log_level\":\"verbose\"}") == 0);
    TEST_CHECK(strcmp(cfg.log_level, "error") == 0);
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, "{\"log_level\":\"\"}") == 0);
    TEST_CHECK(strcmp(cfg.log_level, "info") == 0);
    /* Serialized output contains the field; defaults round-trip to "info". */
    char* json = config_serialize_json(&cfg);
    TEST_CHECK(json && strstr(json, "\"log_level\"") != NULL);
    free(json);
    config_free(&cfg);
}

static void test_parse_partial_invalid_and_null(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, "{\"webui_port\":7777}") == 0);
    TEST_CHECK(cfg.health_check_port == 0 && cfg.webui_port == 7777);
    TEST_CHECK(config_parse_json(&cfg, "not json {{{") == -1);
    TEST_CHECK(cfg.webui_port == 7777);
    TEST_CHECK(config_parse_json(NULL, "{}") == -1);
    TEST_CHECK(config_parse_json(&cfg, NULL) == -1);
    config_free(&cfg);
}

static void test_webui_port_auto_sentinel(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    /* Managers before auto-detect always serialized 8765 (the old default,
     * equal to nanobot's own default): normalize it to the auto sentinel so
     * pre-existing configs keep following nanobot's config. */
    TEST_CHECK(config_parse_json(&cfg, "{\"webui_port\":8765}") == 0);
    TEST_CHECK(cfg.webui_port == 0);
    /* Explicit 0 stays 0; any other port passes through untouched. */
    TEST_CHECK(config_parse_json(&cfg, "{\"webui_port\":0}") == 0);
    TEST_CHECK(cfg.webui_port == 0);
    TEST_CHECK(config_parse_json(&cfg, "{\"webui_port\":18791}") == 0);
    TEST_CHECK(cfg.webui_port == 18791);
    config_free(&cfg);
}

static void test_health_port_auto_sentinel(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    /* Managers before auto-detect always serialized 18790 (the old default):
     * normalize it to the auto sentinel so pre-existing configs follow the
     * running gateway instead of pinning the old display value. */
    TEST_CHECK(config_parse_json(&cfg, "{\"health_check_port\":18790}") == 0);
    TEST_CHECK(cfg.health_check_port == 0);
    /* Explicit 0 stays 0; any other port passes through untouched. */
    TEST_CHECK(config_parse_json(&cfg, "{\"health_check_port\":0}") == 0);
    TEST_CHECK(cfg.health_check_port == 0);
    TEST_CHECK(config_parse_json(&cfg, "{\"health_check_port\":19000}") == 0);
    TEST_CHECK(cfg.health_check_port == 19000);
    config_free(&cfg);
}

static void test_ports_and_theme(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, "{\"health_check_port\":1,\"webui_port\":65535,\"theme_mode\":2}") == 0);
    TEST_CHECK(cfg.health_check_port == 1 && cfg.webui_port == 65535 && cfg.theme_mode == 2);
    TEST_CHECK(config_parse_json(&cfg, "{\"health_check_port\":0,\"webui_port\":65536,\"theme_mode\":3}") == 0);
    TEST_CHECK(cfg.health_check_port == 0 && cfg.webui_port == 65535 && cfg.theme_mode == 2);
    config_free(&cfg);
}

static void test_roundtrips(void) {
    app_config first, second;
    config_apply_defaults(&first);
    TEST_CHECK(config_parse_json(&first, FULL_JSON) == 0);
    char* json = config_serialize_json(&first);
    TEST_CHECK(json && strstr(json, "webui_auth") == NULL);
    config_apply_defaults(&second);
    TEST_CHECK(config_parse_json(&second, json) == 0);
    TEST_CHECK(configs_equal(&first, &second));
    free(json);
    config_free(&first);
    config_free(&second);

    config_apply_defaults(&first);
    json = config_serialize_json(&first);
    config_apply_defaults(&second);
    TEST_CHECK(json && config_parse_json(&second, json) == 0 && configs_equal(&first, &second));
    free(json);
    config_free(&first);
    config_free(&second);
}

static void test_language_and_path(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, "{\"nanobot_path\":null,\"language\":\"abcdefgh\"}") == 0);
    TEST_CHECK(cfg.nanobot_path == NULL && cfg.language[sizeof(cfg.language) - 1] == '\0');
    TEST_CHECK(config_parse_json(&cfg, "{\"nanobot_path\":\"C:\\\\nb.exe\"}") == 0);
    TEST_CHECK(cfg.nanobot_path && strcmp(cfg.nanobot_path, "C:\\nb.exe") == 0);
    config_free(&cfg);
    TEST_CHECK(config_serialize_json(NULL) == NULL);
}

static void test_strict_general_port(void) {
    int port = 0;
    TEST_CHECK(config_parse_port("1", &port) && port == 1);
    TEST_CHECK(config_parse_port("0008765", &port) && port == 8765);
    static const char* const invalid[] = { "", "0", "65536", "-1", "+1", " 1", "1 ", "1.0", "1e3", "12x", "１２", NULL };
    for (int index = 0; invalid[index]; index++)
        TEST_CHECK(!config_parse_port(invalid[index], &port));
}

static void test_legacy_parse_no_serialize(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, FULL_JSON) == 0);
    TEST_CHECK(cfg.legacy_migration_pending == 1);
    TEST_CHECK(strcmp(cfg.legacy_webui_auth, "nbwt_abcdef") == 0);
    char* json = config_serialize_json(&cfg);
    TEST_CHECK(json && strstr(json, "webui_auth") == NULL);
    free(json);
    config_free(&cfg);
}

static void test_oversize_migration(void) {
    app_config cfg;
    char secret[256], json[512];
    memset(secret, 'x', sizeof(secret) - 1);
    secret[sizeof(secret) - 1] = '\0';
    snprintf(json, sizeof(json), "{\"webui_auth\":\"%s\"}", secret);
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, json) == 0);
    TEST_CHECK(cfg.legacy_migration_pending == 2 && cfg.legacy_webui_auth[0] == '\0');
    config_free(&cfg);
}

static void test_legacy_scrub_json(void) {
    char* cleaned = NULL;
    TEST_CHECK(config_remove_legacy_auth_json("{\"unknown\":42,\"webui_auth\":\"secret\",\"nested\":{\"x\":true}}", &cleaned) == 0);
    TEST_CHECK(cleaned && strstr(cleaned, "webui_auth") == NULL);
    TEST_CHECK(strstr(cleaned, "unknown") && strstr(cleaned, "nested"));
    free(cleaned);
}

static void test_active_legacy_source_has_priority(void) {
    TEST_CHECK(!config_should_scan_adjacent_legacy(1));
    TEST_CHECK(!config_should_scan_adjacent_legacy(2));
    TEST_CHECK(config_should_scan_adjacent_legacy(0));
}

TEST_LIST = {
    { "config_defaults", test_defaults },
    { "config_parse_full", test_parse_full },
    { "config_log_level_validation", test_log_level_validation },
    { "config_partial_invalid_null", test_parse_partial_invalid_and_null },
    { "config_webui_port_auto_sentinel", test_webui_port_auto_sentinel },
    { "config_health_port_auto_sentinel", test_health_port_auto_sentinel },
    { "config_ports_theme", test_ports_and_theme },
    { "config_roundtrips", test_roundtrips },
    { "config_language_path", test_language_and_path },
    { "config_strict_port", test_strict_general_port },
    { "legacy_parse_no_serialize", test_legacy_parse_no_serialize },
    { "legacy_oversize", test_oversize_migration },
    { "legacy_scrub_json", test_legacy_scrub_json },
    { "active_legacy_source_has_priority", test_active_legacy_source_has_priority },
    { NULL, NULL }
};
