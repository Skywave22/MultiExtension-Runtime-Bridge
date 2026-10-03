/* bridge/cache.h — TTL'd LRU response cache + single-flight coalescing.
 *
 * Two problems solved together because they share a table:
 *   1. A UI that re-opens the same detail page should not hit the network again.
 *   2. A UI that fires the same request from three widgets simultaneously should
 *      trigger one engine call, not three.
 */
#ifndef BRIDGE_CACHE_H
#define BRIDGE_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct xb_cache xb_cache;

xb_cache *xb_cache_new(size_t max_entries, size_t max_bytes, int64_t default_ttl_ms);
void      xb_cache_free(xb_cache *c);
void      xb_cache_configure(xb_cache *c, size_t max_entries, size_t max_bytes,
                             int64_t default_ttl_ms);

/* Look up a fresh entry. Returns a malloc'd copy of the value JSON, or NULL. */
char *xb_cache_get(xb_cache *c, const char *key);

/* Store (copies key and value). ttl_ms <= 0 means "not cacheable". */
void xb_cache_put(xb_cache *c, const char *key, const char *value, int64_t ttl_ms);

/* --------------------------------------------------------- single flight -- */

typedef enum {
    XB_FLIGHT_OWNER,   /* you must do the work, then call xb_flight_finish */
    XB_FLIGHT_WAITER,  /* another thread is doing it; wait, then read result */
    XB_FLIGHT_ERROR
} xb_flight_role;

typedef struct xb_flight xb_flight;

/* Acquire the flight slot for `key`. `*out` receives the flight handle. */
xb_flight_role xb_flight_begin(xb_cache *c, const char *key, int64_t wait_ms,
                               xb_flight **out);

/* Owner publishes the outcome (value may be NULL for errors) and wakes waiters.
 * `error_code` travels with the message so a waiter can report exactly why the
 * shared call failed (-32001, -32002, -32003...) instead of a generic engine
 * error. 0 means "no stronger code than the default". */
void xb_flight_finish(xb_cache *c, xb_flight *f, const char *value,
                      const char *error, int error_code);

/* Waiter reads the result of the completed flight. Returns 1 if a value exists
 * (malloc'd copy into *out_value), 0 if the flight failed (error copied into
 * *out_error, code into *out_code), -1 if the flight is still running. */
int xb_flight_result(xb_flight *f, char **out_value, char **out_error,
                     int *out_code);

void xb_flight_release(xb_flight *f);

/* ----------------------------------------------------------------- stats -- */

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t puts;
    uint64_t evictions;
    uint64_t expirations;
    uint64_t coalesced;      /* requests that joined an in-flight one */
    uint64_t entries;
    uint64_t bytes;
} xb_cache_stats;

void xb_cache_get_stats(xb_cache *c, xb_cache_stats *out);

/* Test helper: drop everything. */
void xb_cache_clear(xb_cache *c);

#endif /* BRIDGE_CACHE_H */
