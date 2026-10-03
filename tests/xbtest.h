/* xbtest.h — tiny test harness: no dependency, works everywhere the daemon does.
 *
 * Usage:
 *   XB_TEST(json_roundtrip) {
 *       XB_CHECK(strcmp("a","a") == 0);
 *       XB_CHECK_EQ_INT(2 + 2, 4);
 *   }
 *   int main(void) { return xb_test_run("json"); }
 */
#ifndef XBTEST_H
#define XBTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*xb_test_fn)(void);

typedef struct {
    const char *name;
    xb_test_fn  fn;
    const char *suite;
} xb_test_case;

#define XB_TEST_MAX 512

/* One registry for the whole test binary, shared across translation units. */
extern xb_test_case xb_tests[XB_TEST_MAX];
extern int xb_test_count;
extern int xb_test_failures;
extern int xb_test_checks;
extern const char *xb_current;
extern const char *xb_suite;

void xb_test_register(const char *name, xb_test_fn fn, const char *suite);
/* Register a teardown hook; hooks run after the last test and before the
 * leak check, which is where a suite releases process-global state. */
void xb_test_atexit(void (*fn)(void));
int  xb_test_run_all(void);

#ifdef _WIN32
#  define XB_CTOR
#else
#  define XB_CTOR __attribute__((constructor))
#endif

/* Suites declare their name once; individual tests register themselves. */
#define XB_SUITE(n) static const char *const xb_this_suite = n

#define XB_TEST(name)                                                       \
    static void xb_test_##name(void);                                       \
    XB_CTOR static void xb_reg_##name(void) {                               \
        xb_test_register(#name, xb_test_##name, xb_this_suite);             \
    }                                                                       \
    static void xb_test_##name(void)

#define XB_FAILF(...)                                                       \
    do {                                                                    \
        fprintf(stderr, "  FAIL %s:%d [%s] ", __FILE__, __LINE__, xb_current); \
        fprintf(stderr, __VA_ARGS__);                                       \
        fprintf(stderr, "\n");                                              \
        xb_test_failures++;                                                 \
    } while (0)

#define XB_CHECK(cond)                                                      \
    do {                                                                    \
        xb_test_checks++;                                                   \
        if (!(cond)) XB_FAILF("expected %s", #cond);                        \
    } while (0)

#define XB_CHECK_MSG(cond, ...)                                             \
    do {                                                                    \
        xb_test_checks++;                                                   \
        if (!(cond)) XB_FAILF(__VA_ARGS__);                                 \
    } while (0)

#define XB_CHECK_EQ_INT(a, b)                                               \
    do {                                                                    \
        long long xa = (long long)(a), xb = (long long)(b);                 \
        xb_test_checks++;                                                   \
        if (xa != xb) XB_FAILF("%s == %s (%lld != %lld)", #a, #b, xa, xb);  \
    } while (0)

#define XB_CHECK_EQ_STR(a, b)                                               \
    do {                                                                    \
        const char *xa = (a), *xb = (b);                                    \
        xb_test_checks++;                                                   \
        if (!xa || !xb || strcmp(xa, xb) != 0)                              \
            XB_FAILF("%s == %s (\"%s\" != \"%s\")", #a, #b,                 \
                     xa ? xa : "(null)", xb ? xb : "(null)");               \
    } while (0)

#define XB_CHECK_NEAR(a, b, eps)                                            \
    do {                                                                    \
        double xa = (a), xb = (b);                                          \
        xb_test_checks++;                                                   \
        if (!(xa - xb < (eps) && xb - xa < (eps)))                          \
            XB_FAILF("%s ~= %s (%g vs %g)", #a, #b, xa, xb);                \
    } while (0)

#endif /* XBTEST_H */
