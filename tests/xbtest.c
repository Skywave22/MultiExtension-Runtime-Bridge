/* xbtest.c — the shared test registry and runner. */
#include "xbtest.h"
#include "bridge/util.h"
#include <stdlib.h>

xb_test_case xb_tests[XB_TEST_MAX];
int xb_test_count = 0;
int xb_test_failures = 0;
int xb_test_checks = 0;
const char *xb_current = "";
const char *xb_suite = "";

void xb_test_register(const char *name, xb_test_fn fn, const char *suite)
{
    if (xb_test_count >= XB_TEST_MAX) return;
    xb_tests[xb_test_count].name = name;
    xb_tests[xb_test_count].fn = fn;
    xb_tests[xb_test_count].suite = suite;
    xb_test_count++;
}

static void (*g_atexit_hooks[16])(void);
static int g_atexit_count;

void xb_test_atexit(void (*fn)(void))
{
    if (fn && g_atexit_count < 16) g_atexit_hooks[g_atexit_count++] = fn;
}

int xb_test_run_all(void)
{
    int failed_tests = 0;
    int failed_suites = 0;
    const char *current_suite = "";

    printf("\n");
    for (int i = 0; i < xb_test_count; i++) {
        if (xb_tests[i].suite && strcmp(xb_tests[i].suite, current_suite) != 0) {
            if (failed_suites) printf("  -> suite %s: FAILED\n", current_suite);
            current_suite = xb_tests[i].suite ? xb_tests[i].suite : "";
            failed_suites = 0;
            printf("\n[%s]\n", current_suite);
        }
        xb_current = xb_tests[i].name;
        int before = xb_test_failures;
        size_t live_before = xb_total_allocated();
        xb_tests[i].fn();
        if (getenv("XBTEST_TRACE_ALLOC") && xb_total_allocated() != live_before) {
            printf("    [alloc] %s: %zu -> %zu\n", xb_tests[i].name,
                   live_before, xb_total_allocated());
        }
        if (xb_test_failures > before) {
            printf("  x %s\n", xb_tests[i].name);
            failed_tests++;
            failed_suites++;
        } else {
            printf("  . %s\n", xb_tests[i].name);
        }
    }
    if (failed_suites) printf("  -> suite %s: FAILED\n", current_suite);

    for (int i = 0; i < g_atexit_count; i++) g_atexit_hooks[i]();

    printf("\n%u tests, %u checks, %u failures\n",
           (unsigned)xb_test_count, (unsigned)xb_test_checks, (unsigned)failed_tests);
    if (failed_tests == 0) {
        /* Anything still allocated at this point is a leak; the harness reports
         * it as a failure so leaks cannot hide behind a green run. */
        size_t live = xb_total_allocated();
        if (live > 0) {
            printf("LEAK: %zu bytes in %zu blocks still allocated at exit\n",
                   live, xb_total_blocks());
#ifdef XB_ALLOC_TRACK
            xb_alloc_report();
#endif
            return 1;
        }
        printf("PASS (no leaks)\n");
    }
    return failed_tests == 0 ? 0 : 1;
}
