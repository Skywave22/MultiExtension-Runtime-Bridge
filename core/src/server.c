/* server.c — XBP/1 accept loop, framing, dispatch and streaming.
 *
 * Concurrency model (one paragraph, because it is the whole performance story):
 *
 *   accept thread ──► one reader thread per connection
 *                        │  parses frames, never blocks on work
 *                        ├─► task pool (N threads) ─► engine submit ─► worker process
 *                        └─► CANCEL / PING / BYE answered inline
 *
 * The reader thread stays responsive, so a client can cancel, pipeline and
 * stream on a single connection. Responses are written under a per-connection
 * lock and may interleave — the client correlates by id, which is why the
 * protocol does not require ordering.
 */
#include "bridge/server.h"
#include "bridge/bridge_internal.h"
#include "bridge/buf.h"
#include "bridge/log.h"
#include "bridge/util.h"
#include "bridge/registry.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* ---------------------------------------------------------------- config -- */

xb_config xb_g_config = {
    .max_frame = 16 * 1024 * 1024,
    .workers_per_engine = 2,
    .task_threads = 16,
    .cache_entries = 4096,
    .cache_ttl_ms = 30000,
    .cache_bytes = 64u * 1024 * 1024,
    .backlog = 128,
    .allow_shutdown = false,
    .prewarm = false,
    .quiet = false,
    .json_logs = true,
};

/* ------------------------------------------------------------------ task -- */

typedef struct task {
    struct conn *conn;
    char        *request_json;   /* owned copy of the frame payload */
    char         id[68];
    char         method[96];
    int64_t      deadline_ms;
    bool         stream;
    struct task *next;
} task_t;

typedef struct conn {
    xb_bridge  *bridge;
    xb_sock_t   fd;
    xb_mutex_t  wlock;        /* serialises frame writes */
    xb_mutex_t  lock;
    xb_cond_t   cv;
    int         inflight;      /* tasks still running for this connection */
    bool        closing;       /* reader loop finished; no new tasks accepted */
    bool        hello_done;
    uint64_t    id_counter;    /* last id seen, for duplicate detection */
    char        peer[128];
    struct conn *next;
} conn_t;

static void conn_write(conn_t *c, uint8_t type, const char *payload, size_t len)
{
    if (len > 0xFFFFFFFFu) return;
    uint8_t hdr[5];
    uint32_t total = (uint32_t)(len + 1);
    hdr[0] = (uint8_t)(total >> 24);
    hdr[1] = (uint8_t)(total >> 16);
    hdr[2] = (uint8_t)(total >> 8);
    hdr[3] = (uint8_t)total;
    hdr[4] = type;

    XB_MUTEX_LOCK(&c->wlock);
    int64_t a = xb_write_full(c->fd, hdr, sizeof hdr);
    if (a == (int64_t)sizeof hdr && len)
        xb_write_full(c->fd, payload, len);
    XB_MUTEX_UNLOCK(&c->wlock);

    xb_server_stats_inc_frames_out(1);
    xb_server_stats_inc_bytes_out((uint64_t)len + 5);
}

static void conn_write_json(conn_t *c, uint8_t type, xb_jsonw *w)
{
    conn_write(c, type, (const char *)(w->buf.data ? w->buf.data : (const uint8_t *)""),
               w->buf.len);
}

/* ------------------------------------------------------------ task pool -- */

static struct {
    task_t     *head;
    task_t     *tail;
    xb_mutex_t  m;
    xb_cond_t   cv;
    int         threads;
    bool        started;
    volatile int stop;
    xb_thread_t *ids;
} POOL;

static XB_THREAD_FN(pool_worker);

static void pool_init(int threads)
{
    if (POOL.started) return;
    POOL.threads = threads > 0 ? threads : 16;
    if (POOL.threads > 256) POOL.threads = 256;
    XB_MUTEX_INIT(&POOL.m);
    XB_COND_INIT(&POOL.cv);
    POOL.ids = (xb_thread_t *)xb_alloc(sizeof(xb_thread_t) * (size_t)POOL.threads);
    for (int i = 0; i < POOL.threads; i++)
        xb_thread_spawn(&POOL.ids[i], pool_worker, NULL);
    POOL.started = true;
}

static void pool_shutdown(void)
{
    if (!POOL.started) return;
    POOL.stop = 1;
    XB_MUTEX_LOCK(&POOL.m);
    XB_COND_BROADCAST(&POOL.cv);
    XB_MUTEX_UNLOCK(&POOL.m);
#ifndef _WIN32
    for (int i = 0; i < POOL.threads; i++) xb_thread_join(POOL.ids[i]);
#else
#endif
    xb_free(POOL.ids);
    XB_MUTEX_DESTROY(&POOL.m);
    XB_COND_DESTROY(&POOL.cv);
    POOL.started = false;
}

static bool pool_submit(task_t *t)
{
    XB_MUTEX_LOCK(&POOL.m);
    if (POOL.stop) { XB_MUTEX_UNLOCK(&POOL.m); return false; }
    t->next = NULL;
    if (POOL.tail) POOL.tail->next = t; else POOL.head = t;
    POOL.tail = t;
    XB_COND_SIGNAL(&POOL.cv);
    XB_MUTEX_UNLOCK(&POOL.m);
    return true;
}

static task_t *pool_take(int wait_ms)
{
    XB_MUTEX_LOCK(&POOL.m);
    while (!POOL.head && !POOL.stop) {
        if (wait_ms < 0) XB_COND_WAIT(&POOL.cv, &POOL.m);
        else if (XB_COND_TIMEDWAIT(&POOL.cv, &POOL.m, wait_ms) != 0) break;
    }
    task_t *t = POOL.head;
    if (t) {
        POOL.head = t->next;
        if (!POOL.head) POOL.tail = NULL;
        t->next = NULL;
    }
    XB_MUTEX_UNLOCK(&POOL.m);
    return t;
}

/* ------------------------------------------------------- request execution -- */

static void conn_inflight_done(conn_t *c)
{
    XB_MUTEX_LOCK(&c->lock);
    if (c->inflight > 0) c->inflight--;
    XB_COND_BROADCAST(&c->cv);
    XB_MUTEX_UNLOCK(&c->lock);
}

/* Emit callback handed to method handlers for streaming methods. */
typedef struct {
    conn_t *conn;
    const char *id;
    int64_t seq;
} emit_ctx;

static bool stream_emit(void *ud, int64_t seq, const xb_json *chunk)
{
    emit_ctx *e = (emit_ctx *)ud;
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "id", e->id);
    xb_jw_kv_int(&w, "seq", seq);
    xb_jw_key(&w, "chunk");
    if (chunk) xb_jw_value(&w, chunk); else xb_jw_null(&w);
    xb_jw_obj_end(&w);
    conn_write_json(e->conn, XB_FT_STREAM_CHUNK, &w);
    xb_jw_free(&w);
    e->seq++;
    return true;
}

static void send_response(conn_t *c, const char *id, int rc, xb_jsonw *out,
                          int64_t t0, const char *engine_name, bool cached,
                          bool coalesced)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "id", id);

    if (rc == 0) {
        xb_jw_kv_bool(&w, "ok", true);
        xb_jw_key(&w, "result");
        /* Handlers fill `out` with the result value itself. Route it through
         * xb_jw_raw so the writer's key/separator state stays consistent — a
         * direct byte append here is what produces `"result":{}:"meta"`. */
        if (out && out->buf.len > 0) {
            xb_jw_raw(&w, (const char *)out->buf.data);
        } else {
            xb_jw_null(&w);
        }
    } else {
        xb_jw_kv_bool(&w, "ok", false);
        xb_jw_key(&w, "error");
        xb_jw_obj_begin(&w);
        xb_jw_kv_int(&w, "code", rc);
        xb_jw_kv_str(&w, "message", xb_error_message(rc));
        xb_jw_kv_bool(&w, "retryable", xb_error_retryable(rc));
        xb_jw_obj_end(&w);
    }

    xb_jw_key(&w, "meta");
    xb_jw_obj_begin(&w);
    xb_jw_kv_int(&w, "ms", xb_mono_ms() - t0);
    xb_jw_kv_bool(&w, "cached", cached);
    xb_jw_kv_bool(&w, "coalesced", coalesced);
    if (engine_name && engine_name[0]) xb_jw_kv_str(&w, "engine", engine_name);
    xb_jw_obj_end(&w);

    xb_jw_obj_end(&w);
    conn_write_json(c, XB_FT_RESPONSE, &w);
    xb_jw_free(&w);

    xb_server_stats_inc_requests(rc == 0 ? 1 : 2);
    if (rc == XB_ERR_DEADLINE) xb_server_stats_inc_timeouts();
}

/* Cache policy per method family. Returns TTL in ms, 0 = not cacheable. */
static int64_t method_ttl(const char *method)
{
    if (xb_str_has_prefix(method, "bridge."))      return 0;
    if (xb_str_has_prefix(method, "format."))      return 60000;
    if (xb_str_has_prefix(method, "extension."))   return 0;
    if (xb_str_has_prefix(method, "repo."))        return 0;
    if (xb_str_has_prefix(method, "torrserver."))  return 0;
    if (xb_str_has_prefix(method, "source.getDetail"))       return 120000;
    if (xb_str_has_prefix(method, "source.getPopular"))      return 60000;
    if (xb_str_has_prefix(method, "source.getLatestUpdates"))return 30000;
    if (xb_str_has_prefix(method, "source.search"))          return 15000;
    if (xb_str_has_prefix(method, "source.getPageList"))     return 300000;
    if (xb_str_has_prefix(method, "source.getNovelContent")) return 300000;
    if (xb_str_has_prefix(method, "source.getVideoList"))    return 20000;
    return 0;
}

static void run_task(task_t *t)
{
    conn_t *c = t->conn;
    int64_t t0 = xb_mono_ms();

    xb_arena arena;
    xb_arena_init(&arena);
    size_t off = 0;
    xb_json *req = xb_json_parse(&arena, t->request_json, strlen(t->request_json), &off);
    if (!req || req->type != XB_JOBJ) {
        xb_jsonw w;
        xb_jw_init(&w);
        xb_ctx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.bridge = c->bridge;
        ctx.id = t->id;
        ctx.out = &w;
        (void)ctx;
        xb_jw_free(&w);
        send_response(c, t->id, XB_ERR_PARSE, NULL, t0, NULL, false, false);
        xb_arena_destroy(&arena);
        free(t->request_json);
        free(t);
        conn_inflight_done(c);
        return;
    }

    const xb_json *params = xb_json_get(req, "params");
    bool stream = xb_json_obj_bool(req, "stream", false);

    /* ---- cache ------------------------------------------------------- */
    int64_t req_ttl = method_ttl(t->method);
    const xb_json *cache_spec = xb_json_get(req, "cache");
    if (cache_spec) {
        if (cache_spec->type == XB_JNULL) req_ttl = 0;
        else {
            xb_json *ttl_node = xb_json_get(cache_spec, "ttl_ms");
            if (ttl_node) req_ttl = xb_json_int(ttl_node, req_ttl);
        }
    }
    bool coalesce = xb_json_obj_bool(req, "coalesce", true);

    char key[80] = "";
    if (!stream && req_ttl > 0) {
        xb_json_canonical_hash(params ? params : req, key);
        /* Include the method so two methods with identical params differ. */
        char fuller[160];
        snprintf(fuller, sizeof fuller, "%s:%s", t->method, key);
        xb_str_lcpy(key, fuller, sizeof key);
    }

    if (key[0]) {
        char *cached = xb_cache_get(xb_bridge_cache(c->bridge), key);
        if (cached) {
            xb_jsonw w;
            xb_jw_init(&w);
            xb_jw_raw(&w, cached);
            send_response(c, t->id, 0, &w, t0, NULL, true, false);
            xb_jw_free(&w);
            xb_free(cached);
            xb_arena_destroy(&arena);
            free(t->request_json);
            free(t);
            conn_inflight_done(c);
            return;
        }
    }

    /* ---- single-flight coalescing ------------------------------------ */
    xb_flight *flight = NULL;
    bool i_am_owner = true;
    if (key[0] && coalesce) {
        xb_flight_role role = xb_flight_begin(xb_bridge_cache(c->bridge), key,
                                              t->deadline_ms > 0 ? t->deadline_ms : 60000,
                                              &flight);
        if (role == XB_FLIGHT_WAITER) {
            char *value = NULL, *errmsg = NULL;
            int rr = xb_flight_result(flight, &value, &errmsg);
            if (rr == 1 && value) {
                xb_jsonw w;
                xb_jw_init(&w);
                xb_jw_raw(&w, value);
                send_response(c, t->id, 0, &w, t0, NULL, false, true);
                xb_jw_free(&w);
            } else {
                send_response(c, t->id, XB_ERR_ENGINE, NULL, t0, NULL, false, true);
            }
            xb_free(value);
            xb_free(errmsg);
            xb_flight_release(flight);
            xb_arena_destroy(&arena);
            free(t->request_json);
            free(t);
            conn_inflight_done(c);
            return;
        }
        i_am_owner = (role == XB_FLIGHT_OWNER);
    }

    /* ---- execute ----------------------------------------------------- */
    xb_jsonw out;
    xb_jw_init(&out);

    emit_ctx ec;
    ec.conn = c;
    ec.id = t->id;
    ec.seq = 0;

    volatile bool cancelled = false;
    xb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.bridge = c->bridge;
    ctx.id = t->id;
    ctx.method = t->method;
    ctx.params = params;
    ctx.request = req;
    ctx.deadline_ms = t->deadline_ms;
    ctx.out = &out;
    ctx.stream = stream;
    ctx.emit = stream_emit;
    ctx.emit_ud = &ec;
    ctx.cancelled = &cancelled;

    xb_handler_fn fn = xb_lookup_method(t->method);
    int rc;
    const char *engine_name = NULL;
    if (!fn) {
        rc = XB_ERR_NO_METHOD;
    } else {
        xb_method_meta meta;
        xb_lookup_method_meta(t->method, &meta);
        engine_name = meta.engine;
        if (stream && !meta.streamable && !meta.name) {
            rc = XB_ERR_PARAMS;
        } else {
            rc = fn(&ctx);
        }
    }

    if (stream) {
        /* A streaming request never gets a RESPONSE frame; it is terminated by
         * STREAM_END or STREAM_ERR, so a client that drops mid-stream cannot
         * mistake a truncated stream for a complete one. */
        xb_jsonw term;
        xb_jw_init(&term);
        xb_jw_obj_begin(&term);
        xb_jw_kv_str(&term, "id", t->id);
        if (rc == 0) {
            xb_jw_kv_bool(&term, "done", true);
            xb_jw_kv_int(&term, "count", ec.seq);
            xb_jw_key(&term, "meta");
            xb_jw_obj_begin(&term);
            xb_jw_kv_int(&term, "ms", xb_mono_ms() - t0);
            xb_jw_obj_end(&term);
            xb_jw_obj_end(&term);
            conn_write_json(c, XB_FT_STREAM_END, &term);
        } else {
            xb_jw_key(&term, "error");
            xb_jw_obj_begin(&term);
            xb_jw_kv_int(&term, "code", rc);
            xb_jw_kv_str(&term, "message", xb_error_message(rc));
            xb_jw_obj_end(&term);
            xb_jw_obj_end(&term);
            conn_write_json(c, XB_FT_STREAM_ERR, &term);
        }
        xb_jw_free(&term);
        xb_server_stats_inc_requests(rc == 0 ? 1 : 2);
    } else {
        send_response(c, t->id, rc, &out, t0, engine_name, false, false);
        if (rc == 0 && key[0] && flight) {
            /* Publish for waiters and store in the cache while the value is hot. */
            char *text = (char *)(out.buf.data ? out.buf.data : (const uint8_t *)"null");
            if (i_am_owner) xb_flight_finish(xb_bridge_cache(c->bridge), flight, text, NULL);
            xb_cache_put(xb_bridge_cache(c->bridge), key, text, req_ttl);
        } else if (key[0] && flight && i_am_owner) {
            xb_flight_finish(xb_bridge_cache(c->bridge), flight, NULL,
                             xb_error_message(rc));
        }
    }

    if (flight) xb_flight_release(flight);
    xb_jw_free(&out);
    xb_arena_destroy(&arena);
    free(t->request_json);
    free(t);
    conn_inflight_done(c);
}

static XB_THREAD_FN(pool_worker)
{
    (void)ud;
    for (;;) {
        task_t *t = pool_take(200);
        if (!t) {
            if (POOL.stop) break;
            continue;
        }
        run_task(t);
    }
    XB_THREAD_RETURN(0);
}

/* ------------------------------------------------------------ connection -- */

static void send_protocol_error(conn_t *c, int code, const char *msg)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_int(&w, "code", code);
    xb_jw_kv_str(&w, "message", msg);
    xb_jw_obj_end(&w);
    conn_write_json(c, XB_FT_ERROR, &w);
    xb_jw_free(&w);
    xb_server_stats_inc_errors();
}

static void handle_hello(conn_t *c, const xb_json *j)
{
    const char *proto = xb_json_obj_str(j, "protocol", XBP_VERSION);
    if (strcmp(proto, XBP_VERSION) != 0) {
        send_protocol_error(c, XB_ERR_INVALID, "unsupported protocol version");
        return;
    }
    xb_jsonw w;
    xb_jw_init(&w);
    xb_bridge_handshake_json(c->bridge, &w);
    conn_write_json(c, XB_FT_HELLO_ACK, &w);
    xb_jw_free(&w);
    c->hello_done = true;
    xb_server_stats_inc_connections_active(1);
}

static void handle_request_frame(conn_t *c, const char *payload, size_t len)
{
    /* Parse just enough to route. Parsing twice (here and in the task) is a
     * deliberate trade: the reader thread must stay cheap so cancellation and
     * pipelining keep working under load. */
    xb_arena arena;
    xb_arena_init(&arena);
    size_t off = 0;
    xb_json *j = xb_json_parse(&arena, payload, len, &off);
    if (!j || j->type != XB_JOBJ) {
        send_protocol_error(c, XB_ERR_PARSE, "malformed request frame");
        xb_arena_destroy(&arena);
        return;
    }
    const char *id = xb_json_obj_str(j, "id", NULL);
    const char *method = xb_json_obj_str(j, "method", NULL);
    if (!id || !id[0] || !method || !method[0]) {
        send_protocol_error(c, XB_ERR_INVALID, "request needs a non-empty id and method");
        xb_arena_destroy(&arena);
        return;
    }
    if (strlen(id) > 64 || strlen(method) > 95) {
        send_protocol_error(c, XB_ERR_INVALID, "id or method too long");
        xb_arena_destroy(&arena);
        return;
    }
    if (!c->hello_done) {
        send_protocol_error(c, XB_ERR_INVALID, "HELLO required before requests");
        xb_arena_destroy(&arena);
        return;
    }
    if (c->closing) {
        xb_arena_destroy(&arena);
        return;
    }

    task_t *t = (task_t *)calloc(1, sizeof *t);
    t->conn = c;
    t->request_json = (char *)malloc(len + 1);
    memcpy(t->request_json, payload, len);
    t->request_json[len] = '\0';
    xb_str_lcpy(t->id, id, sizeof t->id);
    xb_str_lcpy(t->method, method, sizeof t->method);
    t->deadline_ms = xb_json_obj_int(j, "deadline_ms", 0);
    t->stream = xb_json_obj_bool(j, "stream", false);

    XB_MUTEX_LOCK(&c->lock);
    c->inflight++;
    XB_MUTEX_UNLOCK(&c->lock);

    if (!pool_submit(t)) {
        free(t->request_json);
        free(t);
        conn_inflight_done(c);
        send_protocol_error(c, XB_ERR_UNAVAILABLE, "server is shutting down");
    }
    xb_arena_destroy(&arena);
}

static void handle_cancel_frame(conn_t *c, const char *payload, size_t len)
{
    xb_arena arena;
    xb_arena_init(&arena);
    size_t off = 0;
    xb_json *j = xb_json_parse(&arena, payload, len, &off);
    const char *id = j ? xb_json_obj_str(j, "id", NULL) : NULL;
    if (id) xb_bridge_cancel(c->bridge, id);
    xb_arena_destroy(&arena);
}

/* --- active connection registry (used by the shutdown path) --- */
static struct {
    conn_t *list[512];
    size_t  n;
    xb_mutex_t m;
    bool    ready;
} CONNS;

static void conns_init(void)
{
    if (CONNS.ready) return;
    XB_MUTEX_INIT(&CONNS.m);
    CONNS.ready = true;
}

static void conns_track(conn_t *c, bool add)
{
    conns_init();
    XB_MUTEX_LOCK(&CONNS.m);
    if (add) {
        for (size_t i = 0; i < 512; i++)
            if (!CONNS.list[i]) { CONNS.list[i] = c; CONNS.n++; break; }
    } else {
        for (size_t i = 0; i < 512; i++)
            if (CONNS.list[i] == c) { CONNS.list[i] = NULL; if (CONNS.n) CONNS.n--; break; }
    }
    XB_MUTEX_UNLOCK(&CONNS.m);
}

static void close_all_connections(void *ud)
{
    (void)ud;
    conns_init();
    XB_MUTEX_LOCK(&CONNS.m);
    for (size_t i = 0; i < 512; i++) {
        conn_t *c = CONNS.list[i];
        if (c && c->fd != XB_SOCK_INVALID) xb_sock_shutdown(c->fd);
    }
    XB_MUTEX_UNLOCK(&CONNS.m);
}

static XB_THREAD_FN(connection_thread)
{
    conn_t *c = (conn_t *)ud;
    uint8_t hdr[5];
    xb_buf payload;
    xb_buf_init(&payload);

    for (;;) {
        int64_t r = xb_read_full(c->fd, hdr, sizeof hdr);
        if (r != (int64_t)sizeof hdr) break;
        uint32_t total = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                         ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
        uint8_t type = hdr[4];
        if (total < 1 || total > (uint32_t)xb_g_config.max_frame) {
            send_protocol_error(c, XB_ERR_LIMIT, "frame exceeds max_frame");
            break;
        }
        size_t plen = total - 1;
        xb_buf_reset(&payload);
        if (plen) {
            xb_buf_reserve(&payload, plen);
            int64_t got = xb_read_full(c->fd, payload.data, plen);
            if (got != (int64_t)plen) break;
            payload.len = plen;
            payload.data[plen] = 0;
        }
        const char *text = payload.data ? (const char *)payload.data : "";
        xb_server_stats_inc_frames_in(1);
        xb_server_stats_inc_bytes_in(plen + 5);

        switch (type) {
        case XB_FT_HELLO: {
            xb_arena arena;
            xb_arena_init(&arena);
            size_t off = 0;
            xb_json *j = xb_json_parse(&arena, text, plen, &off);
            handle_hello(c, j ? j : NULL);
            xb_arena_destroy(&arena);
            break;
        }
        case XB_FT_REQUEST:
            handle_request_frame(c, text, plen);
            break;
        case XB_FT_CANCEL:
            handle_cancel_frame(c, text, plen);
            break;
        case XB_FT_PING:
            conn_write(c, XB_FT_PONG, text, plen);
            break;
        case XB_FT_BYE:
            goto done;
        default:
            /* Unknown frame types are ignored on purpose: that is what lets a
             * newer client talk to an older daemon within XBP/1. */
            break;
        }
    }

done:
    XB_MUTEX_LOCK(&c->lock);
    c->closing = true;
    while (c->inflight > 0) XB_COND_WAIT(&c->cv, &c->lock);
    XB_MUTEX_UNLOCK(&c->lock);

    xb_sock_close(c->fd);
    c->fd = XB_SOCK_INVALID;
    xb_buf_free(&payload);
    xb_server_stats_inc_connections_active(-1);
    xb_bridge_connection_removed(c->bridge, c);
    conns_track(c, false);
    XB_MUTEX_DESTROY(&c->wlock);
    XB_MUTEX_DESTROY(&c->lock);
    XB_COND_DESTROY(&c->cv);
    xb_free(c);
    XB_THREAD_RETURN(0);
}

/* --------------------------------------------------------------- running -- */

typedef struct {
    xb_bridge *bridge;
    xb_listener *listener;
    xb_mutex_t m;
    xb_cond_t  cv;
    bool shutdown;
} run_state;

static run_state RS;

static XB_THREAD_FN(accept_loop)
{
    xb_listener *l = (xb_listener *)ud;
    for (;;) {
        xb_sock_t fd = xb_listener_accept(l);
        if (fd == XB_SOCK_INVALID) break;

        xb_server_stats_inc_connections_total(1);

        conn_t *c = (conn_t *)xb_alloc(sizeof *c);
        c->bridge = RS.bridge;
        c->fd = fd;
        XB_MUTEX_INIT(&c->wlock);
        XB_MUTEX_INIT(&c->lock);
        XB_COND_INIT(&c->cv);
        xb_sock_set_nodelay(fd);
        xb_bridge_connection_added(RS.bridge, c);
        conns_track(c, true);

        xb_thread_t th;
        if (xb_thread_spawn(&th, connection_thread, c) != 0) {
            xb_sock_close(fd);
            xb_bridge_connection_removed(RS.bridge, c);
            xb_free(c);
            xb_server_stats_inc_connections_active(-1);
        }
    }
    XB_THREAD_RETURN(0);
}

void xb_server_request_shutdown(xb_bridge *b)
{
    (void)b;
    XB_MUTEX_LOCK(&RS.m);
    RS.shutdown = true;
    XB_COND_BROADCAST(&RS.cv);
    XB_MUTEX_UNLOCK(&RS.m);
    if (RS.listener) {
        char err[128];
        xb_listener_wake(RS.listener, err, sizeof err);
    }
}

int xb_server_run(xb_bridge *b, char *err, size_t errcap)
{
    xb_endpoint ep;
    if (xb_g_config.endpoint_uri[0]) {
        if (xb_ep_parse(xb_g_config.endpoint_uri, &ep) != 0) {
            snprintf(err, errcap, "cannot parse endpoint '%s'", xb_g_config.endpoint_uri);
            return 1;
        }
    } else if (xb_ep_default(&ep) != 0) {
        snprintf(err, errcap, "cannot determine a default endpoint");
        return 1;
    }

    xb_listener *l = xb_listen(&ep, xb_g_config.backlog, err, errcap);
    if (!l) return 1;

    RS.bridge = b;
    RS.listener = l;
    XB_MUTEX_INIT(&RS.m);
    XB_COND_INIT(&RS.cv);
    RS.shutdown = false;

    char uri[320];
    xb_ep_format(xb_listener_endpoint(l), uri, sizeof uri);
    xb_bridge_set_endpoint(b, uri);
    xb_server_stats_attach(b);
    xb_bridge_set_close_hook(close_all_connections, NULL);

    /* The ready line is the contract with process supervisors and with the
     * SDK's spawn helper: exactly one JSON line, then the socket is live. */
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "event", "ready");
    xb_jw_kv_str(&w, "protocol", XBP_VERSION);
    xb_jw_kv_str(&w, "version", "1.0.0");
    xb_jw_kv_str(&w, "endpoint", uri);
    xb_jw_kv_int(&w, "pid", (int64_t)xb_getpid_wrapper());
    xb_jw_key(&w, "engines");
    xb_bridge_engine_list_json(b, &w);
    xb_jw_obj_end(&w);
    /* The ready line is the contract with process supervisors and the SDK's
     * spawn helper. `--quiet` suppresses it for embedders and tests that get the
     * endpoint from xb_bridge_endpoint() instead. */
    if (!xb_g_config.quiet) {
        printf("%s\n", (const char *)w.buf.data);
        fflush(stdout);
    }
    xb_jw_free(&w);

    pool_init(xb_g_config.task_threads);

    xb_thread_t accepter;
    if (xb_thread_spawn(&accepter, accept_loop, l) != 0) {
        snprintf(err, errcap, "cannot start accept thread");
        pool_shutdown();
        xb_listener_close(l);
        return 1;
    }

    XB_INFO("xbridged listening on %s", uri);

    XB_MUTEX_LOCK(&RS.m);
    while (!RS.shutdown) XB_COND_WAIT(&RS.cv, &RS.m);
    XB_MUTEX_UNLOCK(&RS.m);

    /* Unblock accept, then wait for the loop to finish before freeing it. */
    char werr[128];
    xb_listener_wake(l, werr, sizeof werr);
#ifndef _WIN32
    xb_thread_join(accepter);
#else
#endif

    /* Give in-flight work a moment, then stop everything deterministically. */
    XB_INFO("shutting down: draining in-flight requests");
    xb_sleep_ms(50);
    pool_shutdown();
    xb_bridge_close_all_connections(b);
    xb_listener_close(l);
    XB_MUTEX_DESTROY(&RS.m);
    XB_COND_DESTROY(&RS.cv);
    return 0;
}

/* ------------------------------------------------------------ statistics -- */

static xb_server_stats STATS = { 0 };

void xb_server_stats_inc_frames_in(uint64_t n)      { xb_atomic_add64(&STATS.frames_in, n); }
void xb_server_stats_inc_frames_out(uint64_t n)     { xb_atomic_add64(&STATS.frames_out, n); }
void xb_server_stats_inc_bytes_in(uint64_t n)       { xb_atomic_add64(&STATS.bytes_in, n); }
void xb_server_stats_inc_bytes_out(uint64_t n)      { xb_atomic_add64(&STATS.bytes_out, n); }
void xb_server_stats_inc_connections_total(uint64_t n) { xb_atomic_add64(&STATS.connections_total, n); }
void xb_server_stats_inc_errors(void)               { xb_atomic_add64(&STATS.protocol_errors, 1); }
void xb_server_stats_inc_timeouts(void)             { xb_atomic_add64(&STATS.requests_timeout, 1); }

void xb_server_stats_inc_connections_active(int delta)
{
    if (delta >= 0) xb_atomic_add64(&STATS.connections_active, (uint64_t)delta);
    else {
        /* saturating decrement */
        for (;;) {
            uint64_t cur = STATS.connections_active;
            if (cur == 0) break;
            uint64_t next = cur - 1;
            if (__atomic_compare_exchange_n(&STATS.connections_active, &cur, next,
                                            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                break;
        }
    }
}

void xb_server_stats_inc_requests(int kind)
{
    xb_atomic_add64(&STATS.requests_total, 1);
    if (kind == 1) xb_atomic_add64(&STATS.requests_ok, 1);
    else           xb_atomic_add64(&STATS.requests_err, 1);
}

void xb_server_stats_attach(xb_bridge *b)
{
    STATS.started_ms = xb_mono_ms();
    xb_bridge_set_server_stats(b, &STATS);
}

void xb_server_stats_get(xb_bridge *b, xb_server_stats *out)
{
    (void)b;
    *out = STATS;
    out->started_ms = STATS.started_ms;
}

int64_t xb_getpid_wrapper(void)
{
#ifdef _WIN32
    return (int64_t)GetCurrentProcessId();
#else
    return (int64_t)getpid();
#endif
}
