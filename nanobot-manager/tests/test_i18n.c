/* test_i18n — zh/en language table integrity for src/i18n.c.
 *
 * The convention (docs/solutions/conventions/i18n-embedded-literals-not-lang-files.md)
 * is "always both languages": a key present in one table but missing from the
 * other renders as the raw key via $t(). These tests parse both assembled
 * tables and diff their key sets, so drift is caught at test time instead of
 * showing up as a raw key in the UI. */

#include "nm_test.h"
#include "i18n.h"

#include <string.h>

#define MAX_KEYS 128
#define MAX_KEY_LEN 64

/* Parse a "key=value\n" table into its key names. Every line must contain '='
 * with a non-empty key and value, and the table must end with '\n' (the same
 * shape ui_page_load_language_string expects). Returns 0 on any violation. */
static int parse_keys(const char* table, char keys[][MAX_KEY_LEN], int* count) {
    *count = 0;
    const char* p = table;
    while (*p) {
        const char* eol = strchr(p, '\n');
        if (!eol) return 0;                 /* last line unterminated */
        const char* eq = strchr(p, '=');
        if (!eq || eq >= eol) return 0;     /* line without '=' */
        size_t klen = (size_t)(eq - p);
        if (klen == 0 || klen >= MAX_KEY_LEN) return 0;
        if (eq + 1 == eol) return 0;        /* empty value */
        if (*count >= MAX_KEYS) return 0;
        memcpy(keys[*count], p, klen);
        keys[*count][klen] = '\0';
        (*count)++;
        p = eol + 1;
    }
    return 1;
}

static int has_key(char keys[][MAX_KEY_LEN], int count, const char* key) {
    for (int i = 0; i < count; i++)
        if (strcmp(keys[i], key) == 0) return 1;
    return 0;
}

static void test_tables_parse(void) {
    const char* zh = i18n_table("zh");
    const char* en = i18n_table("en");
    TEST_CHECK(zh != NULL);
    TEST_CHECK(en != NULL);
    TEST_CHECK(zh != en);

    static char zk[MAX_KEYS][MAX_KEY_LEN], ek[MAX_KEYS][MAX_KEY_LEN];
    int zc = 0, ec = 0;
    TEST_CHECK(parse_keys(zh, zk, &zc));
    TEST_CHECK(parse_keys(en, ek, &ec));
    TEST_MSG("zh keys=%d en keys=%d", zc, ec);
    TEST_CHECK(zc > 0 && ec > 0);
}

static void test_key_parity(void) {
    static char zk[MAX_KEYS][MAX_KEY_LEN], ek[MAX_KEYS][MAX_KEY_LEN];
    int zc = 0, ec = 0;
    TEST_CHECK(parse_keys(i18n_table("zh"), zk, &zc));
    TEST_CHECK(parse_keys(i18n_table("en"), ek, &ec));
    TEST_CHECK(zc == ec);
    TEST_MSG("zh keys=%d en keys=%d", zc, ec);

    for (int i = 0; i < zc; i++) {
        if (!has_key(ek, ec, zk[i])) {
            TEST_CHECK(0);
            TEST_MSG("key '%s' exists in zh but is missing in en", zk[i]);
        }
    }
    for (int i = 0; i < ec; i++) {
        if (!has_key(zk, zc, ek[i])) {
            TEST_CHECK(0);
            TEST_MSG("key '%s' exists in en but is missing in zh", ek[i]);
        }
    }
}

/* Anti-drift: every status_* key shipped in the table must resolve via
 * i18n_status in BOTH languages (i.e. the C++ push path sees the same wording
 * the generated blob gives $t()). */
static void test_status_single_source(void) {
    static char zk[MAX_KEYS][MAX_KEY_LEN];
    int zc = 0;
    TEST_CHECK(parse_keys(i18n_table("zh"), zk, &zc));

    for (int i = 0; i < zc; i++) {
        if (strncmp(zk[i], "status_", 7) != 0) continue;
        if (strcmp(i18n_status(zk[i], 1), zk[i]) == 0) {
            TEST_CHECK(0);
            TEST_MSG("status key '%s' not resolvable via i18n_status (zh)", zk[i]);
        }
        if (strcmp(i18n_status(zk[i], 0), zk[i]) == 0) {
            TEST_CHECK(0);
            TEST_MSG("status key '%s' not resolvable via i18n_status (en)", zk[i]);
        }
    }

    /* Known wording spot-checks (also cover the value mapping itself). */
    TEST_CHECK(strcmp(i18n_status("status_running", 1), "运行中") == 0);
    TEST_CHECK(strcmp(i18n_status("status_running", 0), "Running") == 0);
}

static void test_status_fallbacks(void) {
    /* Missing key surfaces itself — same fallback as $t(). */
    TEST_CHECK(strcmp(i18n_status("status_no_such_key", 1), "status_no_such_key") == 0);
    TEST_CHECK(strcmp(i18n_status("status_no_such_key", 0), "status_no_such_key") == 0);
    TEST_CHECK(strcmp(i18n_status(NULL, 1), "") == 0);
    TEST_CHECK(strcmp(i18n_status("", 0), "") == 0);
}

static void test_unknown_locale(void) {
    TEST_CHECK(i18n_table(NULL) == NULL);
    TEST_CHECK(i18n_table("") == NULL);
    TEST_CHECK(i18n_table("fr") == NULL);
    /* Valid locales still work after bad lookups. */
    TEST_CHECK(i18n_table("zh") != NULL);
    TEST_CHECK(i18n_table("en") != NULL);
}

TEST_LIST = {
    { "tables_parse",        test_tables_parse },
    { "key_parity",          test_key_parity },
    { "status_single_source", test_status_single_source },
    { "status_fallbacks",    test_status_fallbacks },
    { "unknown_locale",      test_unknown_locale },
    { NULL, NULL }
};
