/*
 * Unit tests for nanobot-manager config (JSON parse / serialize / defaults).
 *
 * Exercises the pure-logic core (config_apply_defaults / config_parse_json /
 * config_serialize_json) without touching the filesystem. config.c is compiled
 * directly into this executable to avoid pulling main.cpp's WinMain.
 */
#include "nm_test.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>

/* --- helpers --------------------------------------------------------------- */

static int configs_equal(const app_config* a, const app_config* b) {
    /* nanobot_path is a pointer; compare by string value (NULL == NULL). */
    const char* pa = a->nanobot_path ? a->nanobot_path : "";
    const char* pb = b->nanobot_path ? b->nanobot_path : "";
    return strcmp(pa, pb) == 0 &&
           a->autostart_nanobot  == b->autostart_nanobot &&
           a->stay_in_tray       == b->stay_in_tray &&
           a->health_check_port  == b->health_check_port &&
           a->theme_mode         == b->theme_mode &&
           strcmp(a->language, b->language) == 0 &&
           a->webui_port         == b->webui_port &&
           strcmp(a->webui_auth, b->webui_auth) == 0;
}

/* Full fixture covering every field. */
static const char* FULL_JSON =
    "{"
    "  \"nanobot_path\": \"C:\\\\tools\\\\nanobot.exe\","
    "  \"autostart_nanobot\": true,"
    "  \"stay_in_tray\": true,"
    "  \"health_check_port\": 19000,"
    "  \"theme_mode\": 2,"
    "  \"language\": \"en\","
    "  \"webui_port\": 9000,"
    "  \"webui_auth\": \"nbwt_abcdef\""
    "}";

/* --- tests ----------------------------------------------------------------- */

static void test_defaults(void) {
    app_config cfg;
    config_apply_defaults(&cfg);

    TEST_CHECK(cfg.nanobot_path == NULL);
    TEST_CHECK(cfg.autostart_nanobot == 0);
    TEST_CHECK(cfg.stay_in_tray == 0);
    TEST_CHECK(cfg.health_check_port == 18790);
    TEST_CHECK(cfg.webui_port == 8765);
    TEST_CHECK(cfg.theme_mode == 0);
    TEST_CHECK(strcmp(cfg.language, "zh") == 0);
    TEST_CHECK(cfg.webui_auth[0] == '\0');

    config_free(&cfg);
}

static void test_parse_full(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, FULL_JSON) == 0);

    TEST_CHECK(cfg.nanobot_path != NULL);
    TEST_CHECK(strcmp(cfg.nanobot_path, "C:\\tools\\nanobot.exe") == 0);
    TEST_CHECK(cfg.autostart_nanobot == 1);
    TEST_CHECK(cfg.stay_in_tray == 1);
    TEST_CHECK(cfg.health_check_port == 19000);
    TEST_CHECK(cfg.webui_port == 9000);
    TEST_CHECK(cfg.theme_mode == 2);
    TEST_CHECK(strcmp(cfg.language, "en") == 0);
    TEST_CHECK(strcmp(cfg.webui_auth, "nbwt_abcdef") == 0);

    config_free(&cfg);
}

static void test_parse_partial_keeps_defaults(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    /* Only override one field; everything else must keep its default. */
    TEST_CHECK(config_parse_json(&cfg, "{\"webui_port\": 7777}") == 0);

    TEST_CHECK(cfg.health_check_port == 18790);   /* untouched */
    TEST_CHECK(cfg.webui_port == 7777);           /* overridden */
    TEST_CHECK(cfg.theme_mode == 0);
    TEST_CHECK(strcmp(cfg.language, "zh") == 0);
    TEST_CHECK(cfg.nanobot_path == NULL);

    config_free(&cfg);
}

static void test_parse_invalid_json(void) {
    app_config cfg;
    config_apply_defaults(&cfg);
    /* Garbage input: parse fails, all defaults preserved. */
    TEST_CHECK(config_parse_json(&cfg, "not json {{{") == -1);

    TEST_CHECK(cfg.health_check_port == 18790);
    TEST_CHECK(cfg.webui_port == 8765);
    TEST_CHECK(strcmp(cfg.language, "zh") == 0);

    config_free(&cfg);
}

static void test_port_validation(void) {
    app_config cfg;
    config_apply_defaults(&cfg);

    /* Out-of-range health_check_port is ignored, default kept. */
    config_parse_json(&cfg, "{\"health_check_port\": 0}");
    TEST_CHECK(cfg.health_check_port == 18790);

    config_parse_json(&cfg, "{\"health_check_port\": -1}");
    TEST_CHECK(cfg.health_check_port == 18790);

    config_parse_json(&cfg, "{\"health_check_port\": 70000}");
    TEST_CHECK(cfg.health_check_port == 18790);

    /* Boundary values accepted. */
    config_parse_json(&cfg, "{\"health_check_port\": 1}");
    TEST_CHECK(cfg.health_check_port == 1);

    config_parse_json(&cfg, "{\"health_check_port\": 65535}");
    TEST_CHECK(cfg.health_check_port == 65535);

    /* webui_port obeys the same range. */
    config_parse_json(&cfg, "{\"webui_port\": 99999}");
    TEST_CHECK(cfg.webui_port == 8765);

    config_parse_json(&cfg, "{\"webui_port\": 1234}");
    TEST_CHECK(cfg.webui_port == 1234);

    config_free(&cfg);
}

static void test_theme_validation(void) {
    app_config cfg;
    config_apply_defaults(&cfg);

    config_parse_json(&cfg, "{\"theme_mode\": 3}");   /* out of range */
    TEST_CHECK(cfg.theme_mode == 0);

    config_parse_json(&cfg, "{\"theme_mode\": -1}");
    TEST_CHECK(cfg.theme_mode == 0);

    config_parse_json(&cfg, "{\"theme_mode\": 1}");   /* dark */
    TEST_CHECK(cfg.theme_mode == 1);

    config_parse_json(&cfg, "{\"theme_mode\": 2}");   /* light */
    TEST_CHECK(cfg.theme_mode == 2);

    config_free(&cfg);
}

static void test_roundtrip(void) {
    app_config a, b;
    config_apply_defaults(&a);
    config_parse_json(&a, FULL_JSON);

    char* json = config_serialize_json(&a);
    TEST_CHECK(json != NULL);

    config_apply_defaults(&b);
    TEST_CHECK(config_parse_json(&b, json) == 0);

    TEST_CHECK(configs_equal(&a, &b));

    free(json);
    config_free(&a);
    config_free(&b);
}

static void test_roundtrip_defaults(void) {
    /* Defaults must survive a serialize -> parse round-trip too. */
    app_config a, b;
    config_apply_defaults(&a);

    char* json = config_serialize_json(&a);
    TEST_CHECK(json != NULL);

    config_apply_defaults(&b);
    TEST_CHECK(config_parse_json(&b, json) == 0);

    TEST_CHECK(configs_equal(&a, &b));

    free(json);
    config_free(&a);
    config_free(&b);
}

static void test_webui_auth_truncation(void) {
    /* webui_auth is char[128]; a value longer than 127 must be truncated and
     * NUL-terminated without overrunning the buffer. */
    app_config cfg;
    config_apply_defaults(&cfg);

    char long_secret[256];
    memset(long_secret, 'x', sizeof(long_secret) - 1);
    long_secret[sizeof(long_secret) - 1] = '\0';

    char json[512];
    snprintf(json, sizeof(json), "{\"webui_auth\": \"%s\"}", long_secret);

    config_parse_json(&cfg, json);
    TEST_CHECK(cfg.webui_auth[sizeof(cfg.webui_auth) - 1] == '\0');
    TEST_CHECK(strlen(cfg.webui_auth) == sizeof(cfg.webui_auth) - 1);

    config_free(&cfg);
}

static void test_language_truncation(void) {
    /* language is char[8]; overlong values truncate safely. */
    app_config cfg;
    config_apply_defaults(&cfg);

    config_parse_json(&cfg, "{\"language\": \"abcdefgh\"}");
    TEST_CHECK(cfg.language[sizeof(cfg.language) - 1] == '\0');
    TEST_CHECK(strlen(cfg.language) == sizeof(cfg.language) - 1);

    config_free(&cfg);
}

static void test_nanobot_path_null_vs_string(void) {
    app_config cfg;
    config_apply_defaults(&cfg);

    /* Explicit JSON null keeps nanobot_path == NULL. */
    config_parse_json(&cfg, "{\"nanobot_path\": null}");
    TEST_CHECK(cfg.nanobot_path == NULL);

    /* A non-empty string is duplicated into heap memory. */
    config_parse_json(&cfg, "{\"nanobot_path\": \"C:\\\\nb.exe\"}");
    TEST_CHECK(cfg.nanobot_path != NULL);
    TEST_CHECK(strcmp(cfg.nanobot_path, "C:\\nb.exe") == 0);

    /* An empty string is treated as absent (keeps NULL). */
    config_free(&cfg);
    config_apply_defaults(&cfg);
    config_parse_json(&cfg, "{\"nanobot_path\": \"\"}");
    TEST_CHECK(cfg.nanobot_path == NULL);

    config_free(&cfg);
}

static void test_parse_null_args(void) {
    TEST_CHECK(config_parse_json(NULL, "{}") == -1);
    app_config cfg;
    config_apply_defaults(&cfg);
    TEST_CHECK(config_parse_json(&cfg, NULL) == -1);
    config_free(&cfg);
}

static void test_serialize_null_cfg(void) {
    TEST_CHECK(config_serialize_json(NULL) == NULL);
}

TEST_LIST = {
    { "config_defaults",           test_defaults               },
    { "config_parse_full",         test_parse_full             },
    { "config_parse_partial",      test_parse_partial_keeps_defaults },
    { "config_parse_invalid",      test_parse_invalid_json     },
    { "config_port_validation",    test_port_validation        },
    { "config_theme_validation",   test_theme_validation       },
    { "config_roundtrip",          test_roundtrip              },
    { "config_roundtrip_defaults", test_roundtrip_defaults     },
    { "config_auth_truncation",    test_webui_auth_truncation  },
    { "config_lang_truncation",    test_language_truncation    },
    { "config_path_null_string",   test_nanobot_path_null_vs_string },
    { "config_parse_null_args",    test_parse_null_args        },
    { "config_serialize_null",     test_serialize_null_cfg     },
    { NULL, NULL }
};
