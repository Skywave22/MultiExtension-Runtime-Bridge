/* cache.c — TTL LRU cache + single-flight coalescing.
 *
 * Structure: a hash table of buckets, each holding a doubly-linked LRU list and
 * in-flight entries. One mutex guards the table; critical sections only touch
 * pointers and lengths, never user work, so lock contention stays negligible
 * compared with an engine round-trip.
 */
#include "bridge/cache.h"
#include "bridge/util.h"
#include "bridge/log.h"

#include <string.h>

#define XB_CACHE_BUCKETS 256

typedef struct entry {
    char  *key;
    char  *value;
    size_t vlen;
    int64_t expires_ms;
    int64_t created_ms;
    struct entry *prev, *next;   /* LRU list within bucket */
} entry_t;

typedef struct flight {
    char   *key;
    bool    done;
    char   *value;
    char   *error;
    int     error_code;
    int     refs;
    struct flight *next;
} flight_t;

typedef struct bucket {
    entry_t  *head;    /* most recently used */
    entry_t  *tail;    /* least recently used */
    size_t    count;
    flight_t *flights;
} bucket_t;

struct xb_cache {
    bucket_t buckets[XB_CACHE_BUCKETS];
    size_t   max_entries;
    size_t   max_bytes;
    size_t   entries;
    size_t   bytes;
    int64_t  default_ttl_ms;
    xb_cache_stats stats;
    xb_mutex_t m;
    xb_cond_t  cv;
};

struct xb_flight { flight_t *f; };

static size_t bucket_of(const char *key)
{
    return (size_t)(xb_hash_str(key) % XB_CACHE_BUCKETS);
}

xb_cache *xb_cache_new(size_t max_entries, size_t max_bytes, int64_t default_ttl_ms)
{
    xb_cache *c = (xb_cache *)xb_alloc(sizeof *c);
    xb_cache_configure(c, max_entries, max_bytes, default_ttl_ms);
    XB_MUTEX_INIT(&c->m);
    XB_COND_INIT(&c->cv);
    return c;
}

void xb_cache_configure(xb_cache *c, size_t max_entries, size_t max_bytes,
                        int64_t default_ttl_ms)
{
    c->max_entries = max_entries ? max_entries : 512;
    c->max_bytes = max_bytes ? max_bytes : (size_t)32 * 1024 * 1024;
    c->default_ttl_ms = default_ttl_ms > 0 ? default_ttl_ms : 30000;
}

static void entry_unlink(bucket_t *b, entry_t *e)
{
    if (e->prev) e->prev->next = e->next; else b->head = e->next;
    if (e->next) e->next->prev = e->prev; else b->tail = e->prev;
    e->prev = e->next = NULL;
}

static void entry_push_front(bucket_t *b, entry_t *e)
{
    e->prev = NULL;
    e->next = b->head;
    if (b->head) b->head->prev = e;
    b->head = e;
    if (!b->tail) b->tail = e;
    b->count++;
}

static void entry_free(xb_cache *c, entry_t *e)
{
    c->bytes -= (e->vlen + strlen(e->key) + 2 * sizeof(entry_t));
    c->entries--;
    xb_free(e->key);
    xb_free(e->value);
    xb_free(e);
}

static void evict_lru(xb_cache *c)
{
    /* Find the globally least-recently-used bucket tail by creation time.
     * Scanning 256 buckets is ~ nanoseconds and keeps the structure simple. */
    int best = -1;
    int64_t oldest = 0;
    for (int i = 0; i < XB_CACHE_BUCKETS; i++) {
        entry_t *t = c->buckets[i].tail;
        if (!t) continue;
        if (best < 0 || t->created_ms < oldest) { best = i; oldest = t->created_ms; }
    }
    if (best < 0) return;
    entry_t *e = c->buckets[best].tail;
    entry_unlink(&c->buckets[best], e);
    entry_free(c, e);
    c->stats.evictions++;
}

char *xb_cache_get(xb_cache *c, const char *key)
{
    if (!c || !key) return NULL;
    size_t bi = bucket_of(key);
    int64_t now = xb_mono_ms();
    XB_MUTEX_LOCK(&c->m);
    bucket_t *b = &c->buckets[bi];
    for (entry_t *e = b->head; e; e = e->next) {
        if (strcmp(e->key, key) != 0) continue;
        if (e->expires_ms > 0 && e->expires_ms <= now) {
            entry_unlink(b, e);
            entry_free(c, e);
            c->stats.expirations++;
            c->stats.misses++;
            XB_MUTEX_UNLOCK(&c->m);
            return NULL;
        }
        entry_unlink(b, e);
        entry_push_front(b, e);
        char *copy = xb_strdup(e->value);
        c->stats.hits++;
        XB_MUTEX_UNLOCK(&c->m);
        return copy;
    }
    c->stats.misses++;
    XB_MUTEX_UNLOCK(&c->m);
    return NULL;
}

void xb_cache_put(xb_cache *c, const char *key, const char *value, int64_t ttl_ms)
{
    if (!c || !key || !value || ttl_ms <= 0) return;
    size_t bi = bucket_of(key);
    size_t vlen = strlen(value);
    size_t cost = vlen + strlen(key) + 2 * sizeof(entry_t);
    if (cost > c->max_bytes) return;   /* single entry larger than the whole cache */

    XB_MUTEX_LOCK(&c->m);
    bucket_t *b = &c->buckets[bi];
    for (entry_t *e = b->head; e; e = e->next) {
        if (strcmp(e->key, key) != 0) continue;
        c->bytes -= (e->vlen + strlen(e->key) + 2 * sizeof(entry_t));
        xb_free(e->value);
        e->value = xb_strdup(value);
        e->vlen = vlen;
        c->bytes += cost;
        e->expires_ms = xb_mono_ms() + ttl_ms;
        entry_unlink(b, e);
        entry_push_front(b, e);
        c->stats.puts++;
        XB_MUTEX_UNLOCK(&c->m);
        return;
    }

    entry_t *e = (entry_t *)xb_alloc(sizeof *e);
    e->key = xb_strdup(key);
    e->value = xb_strdup(value);
    e->vlen = vlen;
    e->created_ms = xb_mono_ms();
    e->expires_ms = e->created_ms + ttl_ms;
    entry_push_front(b, e);
    c->entries++;
    c->bytes += cost;

    while ((c->entries > c->max_entries || c->bytes > c->max_bytes) && c->entries > 1)
        evict_lru(c);

    c->stats.puts++;
    XB_MUTEX_UNLOCK(&c->m);
}

/* --------------------------------------------------------- single flight -- */

xb_flight_role xb_flight_begin(xb_cache *c, const char *key, int64_t wait_ms,
                               xb_flight **out)
{
    if (out) *out = NULL;
    if (!c || !key) return XB_FLIGHT_ERROR;
    size_t bi = bucket_of(key);
    int64_t deadline = xb_mono_ms() + (wait_ms > 0 ? wait_ms : 60000);

    XB_MUTEX_LOCK(&c->m);
    bucket_t *b = &c->buckets[bi];
    for (flight_t *f = b->flights; f; f = f->next) {
        if (strcmp(f->key, key) != 0) continue;
        f->refs++;
        while (!f->done) {
            int64_t left = deadline - xb_mono_ms();
            if (left <= 0) { f->refs--; XB_MUTEX_UNLOCK(&c->m); return XB_FLIGHT_ERROR; }
            XB_COND_TIMEDWAIT(&c->cv, &c->m, (int)(left > 100 ? 100 : left));
        }
        xb_flight *h = (xb_flight *)xb_alloc(sizeof *h);
        h->f = f;
        if (out) *out = h; else f->refs--;
        c->stats.coalesced++;
        XB_MUTEX_UNLOCK(&c->m);
        return XB_FLIGHT_WAITER;
    }
    flight_t *f = (flight_t *)xb_alloc(sizeof *f);
    f->key = xb_strdup(key);
    f->done = false;
    f->value = f->error = NULL;
    f->error_code = 0;
    f->refs = 1;
    f->next = b->flights;
    b->flights = f;
    xb_flight *h = (xb_flight *)xb_alloc(sizeof *h);
    h->f = f;
    if (out) *out = h;
    XB_MUTEX_UNLOCK(&c->m);
    return XB_FLIGHT_OWNER;
}

void xb_flight_finish(xb_cache *c, xb_flight *handle, const char *value,
                      const char *error, int error_code)
{
    if (!c || !handle) return;
    size_t bi = bucket_of(handle->f->key);
    XB_MUTEX_LOCK(&c->m);
    flight_t *f = handle->f;
    f->value = value ? xb_strdup(value) : NULL;
    f->error = error ? xb_strdup(error) : NULL;
    f->error_code = value ? 0 : error_code;
    f->done = true;
    XB_COND_BROADCAST(&c->cv);
    XB_MUTEX_UNLOCK(&c->m);
    (void)bi;
}

int xb_flight_result(xb_flight *h, char **out_value, char **out_error,
                     int *out_code)
{
    if (!h || !h->f) return -1;
    flight_t *f = h->f;
    if (!f->done) return -1;
    if (f->value) { if (out_value) *out_value = xb_strdup(f->value); return 1; }
    if (out_error) *out_error = f->error ? xb_strdup(f->error) : NULL;
    if (out_code) *out_code = f->error_code;
    return 0;
}

void xb_flight_release(xb_flight *h)
{
    if (!h) return;
    xb_free(h);
}

/* Flight objects are freed lazily by the cache when the bucket is cleared or
 * when the cache is destroyed; refcounts keep waiters safe in the meantime. */
static void flights_reap(bucket_t *b, xb_mutex_t *m, xb_cond_t *cv)
{
    XB_MUTEX_LOCK(m);
    flight_t **pp = &b->flights;
    while (*pp) {
        flight_t *f = *pp;
        if (f->done && f->refs <= 0) {
            *pp = f->next;
            xb_free(f->key);
            xb_free(f->value);
            xb_free(f->error);
            xb_free(f);
        } else {
            pp = &f->next;
        }
    }
    XB_MUTEX_UNLOCK(m);
    (void)cv;
}

void xb_cache_clear(xb_cache *c)
{
    if (!c) return;
    XB_MUTEX_LOCK(&c->m);
    for (int i = 0; i < XB_CACHE_BUCKETS; i++) {
        entry_t *e = c->buckets[i].head;
        while (e) {
            entry_t *n = e->next;
            entry_unlink(&c->buckets[i], e);
            entry_free(c, e);
            e = n;
        }
    }
    c->entries = 0;
    c->bytes = 0;
    XB_MUTEX_UNLOCK(&c->m);
    for (int i = 0; i < XB_CACHE_BUCKETS; i++)
        flights_reap(&c->buckets[i], &c->m, &c->cv);
}

void xb_cache_free(xb_cache *c)
{
    if (!c) return;
    xb_cache_clear(c);
    for (int i = 0; i < XB_CACHE_BUCKETS; i++) {
        flight_t *f = c->buckets[i].flights;
        while (f) {
            flight_t *n = f->next;
            xb_free(f->key); xb_free(f->value); xb_free(f->error); xb_free(f);
            f = n;
        }
    }
    XB_MUTEX_DESTROY(&c->m);
    XB_COND_DESTROY(&c->cv);
    xb_free(c);
}

void xb_cache_get_stats(xb_cache *c, xb_cache_stats *out)
{
    if (!c || !out) return;
    XB_MUTEX_LOCK(&c->m);
    *out = c->stats;
    out->entries = c->entries;
    out->bytes = c->bytes;
    XB_MUTEX_UNLOCK(&c->m);
}
