/* test_cache.c — TTL, LRU eviction, expiry, single-flight coalescing. */
#include "xbtest.h"
#include "bridge/cache.h"
#include "bridge/util.h"

XB_SUITE("cache");

XB_TEST(basic_put_get)
{
    xb_cache *c = xb_cache_new(64, 1024 * 1024, 60000);
    XB_CHECK(xb_cache_get(c, "missing") == NULL);
    xb_cache_put(c, "k1", "{\"v\":1}", 60000);
    char *v = xb_cache_get(c, "k1");
    XB_CHECK(v != NULL);
    if (v) XB_CHECK_EQ_STR(v, "{\"v\":1}");
    xb_free(v);

    xb_cache_stats st;
    xb_cache_get_stats(c, &st);
    XB_CHECK_EQ_INT(st.hits, 1);
    XB_CHECK_EQ_INT(st.misses, 1);
    XB_CHECK_EQ_INT(st.puts, 1);
    XB_CHECK_EQ_INT(st.entries, 1);
    xb_cache_free(c);
}

XB_TEST(overwrite_replaces_value)
{
    xb_cache *c = xb_cache_new(64, 1024 * 1024, 60000);
    xb_cache_put(c, "k", "\"first\"", 60000);
    xb_cache_put(c, "k", "\"second\"", 60000);
    char *v = xb_cache_get(c, "k");
    XB_CHECK_EQ_STR(v, "\"second\"");
    xb_free(v);
    xb_cache_stats st;
    xb_cache_get_stats(c, &st);
    XB_CHECK_EQ_INT(st.entries, 1);
    xb_cache_free(c);
}

XB_TEST(ttl_zero_is_not_cached)
{
    xb_cache *c = xb_cache_new(64, 1024 * 1024, 60000);
    xb_cache_put(c, "k", "\"v\"", 0);
    XB_CHECK(xb_cache_get(c, "k") == NULL);
    xb_cache_free(c);
}

XB_TEST(expiry)
{
    xb_cache *c = xb_cache_new(64, 1024 * 1024, 60000);
    xb_cache_put(c, "k", "\"v\"", 40);
    char *before = xb_cache_get(c, "k");
    XB_CHECK(before != NULL);
    xb_free(before);
    xb_sleep_ms(80);
    XB_CHECK(xb_cache_get(c, "k") == NULL);
    xb_cache_stats st;
    xb_cache_get_stats(c, &st);
    XB_CHECK(st.expirations >= 1);
    xb_cache_free(c);
}

XB_TEST(lru_eviction_respects_entry_cap)
{
    xb_cache *c = xb_cache_new(8, 1024 * 1024, 60000);
    char key[32], val[64];
    for (int i = 0; i < 40; i++) {
        snprintf(key, sizeof key, "key-%02d", i);
        snprintf(val, sizeof val, "{\"i\":%d}", i);
        xb_cache_put(c, key, val, 60000);
    }
    xb_cache_stats st;
    xb_cache_get_stats(c, &st);
    XB_CHECK_MSG(st.entries <= 8, "entries=%llu", (unsigned long long)st.entries);
    XB_CHECK(st.evictions > 0);
    /* The newest key must still be present. */
    char *v = xb_cache_get(c, "key-39");
    XB_CHECK(v != NULL);
    xb_free(v);
    /* The oldest must have been evicted. */
    XB_CHECK(xb_cache_get(c, "key-00") == NULL);
    xb_cache_free(c);
}

XB_TEST(byte_cap_is_enforced)
{
    xb_cache *c = xb_cache_new(1000, 4096, 60000);
    char big[2048];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    for (int i = 0; i < 20; i++) {
        char key[32];
        snprintf(key, sizeof key, "big-%d", i);
        xb_cache_put(c, key, big, 60000);
    }
    xb_cache_stats st;
    xb_cache_get_stats(c, &st);
    XB_CHECK_MSG(st.bytes <= 4096, "bytes=%llu", (unsigned long long)st.bytes);
    xb_cache_free(c);
}

XB_TEST(oversized_single_entry_is_refused)
{
    xb_cache *c = xb_cache_new(100, 512, 60000);
    char big[4096];
    memset(big, 'y', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    xb_cache_put(c, "huge", big, 60000);
    XB_CHECK(xb_cache_get(c, "huge") == NULL);
    xb_cache_get_stats(c, &(xb_cache_stats){0});
    xb_cache_free(c);
}

/* --- single flight ------------------------------------------------------- */

typedef struct {
    xb_cache *cache;
    int       waiters_done;
    xb_mutex_t m;
} flight_ctx;

static XB_THREAD_FN(waiter_thread)
{
    flight_ctx *fc = (flight_ctx *)ud;
    xb_flight *f = NULL;
    xb_flight_role role = xb_flight_begin(fc->cache, "shared-key", 5000, &f);
    if (role == XB_FLIGHT_WAITER && f) {
        char *value = NULL, *err = NULL;
        int r = xb_flight_result(f, &value, &err);
        if (r == 1 && value && strcmp(value, "{\"answer\":42}") == 0) {
            XB_MUTEX_LOCK(&fc->m);
            fc->waiters_done++;
            XB_MUTEX_UNLOCK(&fc->m);
        }
        xb_free(value);
        xb_free(err);
        xb_flight_release(f);
    }
    (void)role;
    XB_THREAD_RETURN(0);
}

XB_TEST(single_flight_coalesces_concurrent_callers)
{
    xb_cache *c = xb_cache_new(64, 1024 * 1024, 60000);
    flight_ctx fc;
    fc.cache = c;
    fc.waiters_done = 0;
    XB_MUTEX_INIT(&fc.m);

    xb_flight *owner = NULL;
    XB_CHECK(xb_flight_begin(c, "shared-key", 5000, &owner) == XB_FLIGHT_OWNER);

    xb_thread_t threads[4];
    for (int i = 0; i < 4; i++) {
        XB_CHECK(xb_thread_spawn(&threads[i], waiter_thread, &fc) == 0);
    }
    xb_sleep_ms(60);   /* let the waiters block inside xb_flight_begin */

    xb_flight_finish(c, owner, "{\"answer\":42}", NULL);
    xb_flight_release(owner);
    xb_sleep_ms(150);

    XB_MUTEX_LOCK(&fc.m);
    XB_CHECK_EQ_INT(fc.waiters_done, 4);
    XB_MUTEX_UNLOCK(&fc.m);

    xb_cache_stats st;
    xb_cache_get_stats(c, &st);
    XB_CHECK(st.coalesced >= 4);

    XB_MUTEX_DESTROY(&fc.m);
    xb_cache_free(c);
}

XB_TEST(clear_empties_the_cache)
{
    xb_cache *c = xb_cache_new(64, 1024 * 1024, 60000);
    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof key, "k%d", i);
        xb_cache_put(c, key, "\"v\"", 60000);
    }
    xb_cache_clear(c);
    xb_cache_stats st;
    xb_cache_get_stats(c, &st);
    XB_CHECK_EQ_INT(st.entries, 0);
    XB_CHECK_EQ_INT(st.bytes, 0);
    xb_cache_free(c);
}
