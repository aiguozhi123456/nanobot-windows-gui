/*
 * nm_test.h — minimal self-contained C test framework for nanobot-manager.
 *
 * Zero external dependencies. Each test executable includes this header once
 * and defines TEST_LIST. main() is generated automatically unless
 * NM_TEST_NO_MAIN is defined before inclusion.
 *
 * Usage:
 *
 *     #include "nm_test.h"
 *     #include "config.h"
 *
 *     void test_something(void) {
 *         TEST_CHECK(1 + 1 == 2);
 *         TEST_MSG("expected 2, sanity still holds");
 *     }
 *
 *     TEST_LIST = {
 *         { "something", test_something },
 *         { NULL, NULL }          // terminator required
 *     };
 */
#ifndef NM_TEST_H
#define NM_TEST_H

#include <stdio.h>

typedef void (*nm_test_fn)(void);

typedef struct {
    const char*  name;
    nm_test_fn   fn;
} nm_test_entry;

/* Per-test state (TU-local; one test .c per executable). */
static int         nm_check_failed;
static const char* nm_current_test_name;

/* Record a boolean assertion. Failure is latched for the current test but
 * does not abort — the test body continues so cleanup always runs. */
#define TEST_CHECK(cond)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            nm_check_failed = 1;                                            \
            printf("  [FAIL] %s:%d: TEST_CHECK(%s)\n",                      \
                   __FILE__, __LINE__, #cond);                              \
        }                                                                   \
    } while (0)

/* Print an explanatory message after a failure. The first argument is a C
 * string literal (optionally with printf format specifiers + args). */
#define TEST_MSG(...) \
    do { printf("         " __VA_ARGS__); putchar('\n'); } while (0)

/* A test executable defines this array via `TEST_LIST = { ... };`, which
 * expands to a typed definition: `nm_test_entry nm_test_list[] = { ... };`. */
#define TEST_LIST nm_test_entry nm_test_list[]
extern nm_test_entry nm_test_list[];

/* Generated runner. Iterates TEST_LIST, runs each test, reports a summary,
 * and returns non-zero from main if anything failed. */
#ifndef NM_TEST_NO_MAIN
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int total = 0, failed = 0;
    for (nm_test_entry* e = nm_test_list; e->name != NULL; e++) {
        total++;
        nm_current_test_name = e->name;
        nm_check_failed = 0;
        printf("[ RUN      ] %s\n", e->name);
        e->fn();
        if (nm_check_failed) {
            failed++;
            printf("[  FAILED  ] %s\n\n", e->name);
        } else {
            printf("[       OK ] %s\n\n", e->name);
        }
    }
    printf("%d test(s) run, %d passed, %d failed\n",
           total, total - failed, failed);
    return failed ? 1 : 0;
}
#endif

#endif /* NM_TEST_H */
