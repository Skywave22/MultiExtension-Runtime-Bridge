/* abi.c — the in-process embedding ABI declared in bridge/abi.h.
 *
 * This is the same dispatcher, cache and engines the socket server uses; it
 * simply skips the socket. Keeping one implementation is the whole point: a
 * host that embeds the library must get the same answers, the same errors and
 * the same caching as a host that talks to the daemon, or the two transports
 * would drift apart.
 */
#include "bridge/abi.h"
#include "bridge/server.h"
#include "bridge/bridge_internal.h"
#include "bridge/json.h"
#include "bridge/buf.h"
#include "bridge/util.h"
#include "bridge/log.h"
#include "bridge/registry.h"
#include "bridge/cache.h"
#include "bridge/http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct xb_abi_handle {
    xb_bridge *bridge;
    xb_buf     result;      /* last xb_abi_call output */
    int64_t    default_deadline_ms;
    bool       owns_bridge;
};

/* Method registration is global and idempotent; do it once per process. */
static xb_mutex_t g_abi_lock;
static bool g_abi_registered;

static void ensure_registered(void)
{
    XB_MUTEX_INIT(&g_abi_lock);
    XB_MUTEX_LOCK(&g_abi_lock);
    if (!g_abi_registered) {
        xb_register_builtin_methods();
        g_abi_registered = true;
    }
    XB_MUTEX_UNLOCK(&g_abi_lock);
}

const char *xb_abi_version(void)
{
    return "1.0.0";
}

const char *xb_abi_protocol(void)
{
    return XBP_VERSION;
}

const char *xb_abi_strerror(xb_abi_status status)
{
    return xb_error_message(status);
}

bool xb_abi_retryable(xb_abi_status status)
{
    return xb_error_retryable(status);
}

/* Process-wide JSON mirrors. Built once, cheap to hand out, released by
 * xb_abi_shutdown() so a host that is finished with the library can prove it
 * left nothing behind. */
static struct {
    char       *formats;
    char       *methods;
    xb_mutex_t  m;
    bool        ready;
} G;

static void g_init(void)
{
    if (!G.ready) { XB_MUTEX_INIT(&G.m); G.ready = true; }
}

const char *xb_abi_formats_json(void)
{
    g_init();
    XB_MUTEX_LOCK(&G.m);
    if (!G.formats) G.formats = xb_registry_json();
    char *out = G.formats;
    XB_MUTEX_UNLOCK(&G.m);
    return out ? out : "[]";
}

const char *xb_abi_methods_json(void)
{
    ensure_registered();
    g_init();
    XB_MUTEX_LOCK(&G.m);
    if (!G.methods) G.methods = xb_method_list_json();
    char *out = G.methods;
    XB_MUTEX_UNLOCK(&G.m);
    return out ? out : "[]";
}

xb_abi_status xb_abi_get_info(xb_abi_info *out)
{
    if (!out) return XB_ERR_INVALID;
    ensure_registered();
    memset(out, 0, sizeof *out);
    xb_str_lcpy(out->version, XBP_VERSION, sizeof out->version);
    xb_str_lcpy(out->software, XBP_SOFTWARE, sizeof out->software);
    out->abi_major = XB_ABI_MAJOR;
    out->abi_minor = XB_ABI_MINOR;
    out->abi_patch = XB_ABI_PATCH;
    out->formats = (uint32_t)xb_format_count();
    out->methods = (uint32_t)xb_method_count();
    out->max_frame = (uint64_t)xb_g_config.max_frame;
    out->tls = xb_http_tls_available();
    out->tls_backend = xb_http_tls_backend();
    return 0;
}

/* ------------------------------------------------------------- lifecycle -- */

static void apply_config_json(const char *json)
{
    if (!json || !json[0]) return;
    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *cfg = xb_json_parse(&a, json, strlen(json), &off);
    if (cfg && cfg->type == XB_JOBJ) {
        const char *dir = xb_json_obj_str(cfg, "data_dir", NULL);
        if (dir) xb_str_lcpy(xb_g_config.data_dir, dir, sizeof xb_g_config.data_dir);
        int64_t entries = xb_json_obj_int(cfg, "cache_entries", 0);
        if (entries > 0) xb_g_config.cache_entries = (int)entries;
        int64_t ttl = xb_json_obj_int(cfg, "cache_ttl_ms", 0);
        if (ttl >= 0 && xb_json_obj_get(cfg, "cache_ttl_ms")) xb_g_config.cache_ttl_ms = ttl;
        int64_t threads = xb_json_obj_int(cfg, "task_threads", 0);
        if (threads > 0) xb_g_config.task_threads = (int)threads;
        xb_g_config.quiet = xb_json_obj_bool(cfg, "quiet", true);
    } else if (cfg) {
        XB_WARN("abi: config_json is not an object; using defaults");
    }
    xb_arena_destroy(&a);
}

xb_abi_handle *xb_abi_create(const char *config_json)
{
    ensure_registered();
    apply_config_json(config_json);

    xb_abi_handle *h = (xb_abi_handle *)xb_alloc(sizeof *h);
    char err[256] = {0};
    h->default_deadline_ms = 30000;
    xb_buf_init(&h->result);

    h->bridge = xb_bridge_new(&xb_g_config);
    if (!h->bridge || xb_bridge_init(h->bridge, err, sizeof err) != 0) {
        XB_ERR("abi: cannot initialise bridge: %s", err);
        if (h->bridge) xb_bridge_free(h->bridge);
        xb_buf_free(&h->result);
        xb_free(h);
        return NULL;
    }
    h->owns_bridge = true;
    return h;
}

void xb_abi_destroy(xb_abi_handle *h)
{
    if (!h) return;
    if (h->owns_bridge && h->bridge) xb_bridge_free(h->bridge);
    xb_buf_free(&h->result);
    xb_free(h);
}

void xb_abi_shutdown(void)
{
    xb_method_registry_free();
    g_init();
    XB_MUTEX_LOCK(&G.m);
    xb_free(G.formats); G.formats = NULL;
    xb_free(G.methods); G.methods = NULL;
    XB_MUTEX_UNLOCK(&G.m);

    XB_MUTEX_LOCK(&g_abi_lock);
    g_abi_registered = false;
    XB_MUTEX_UNLOCK(&g_abi_lock);
}

void xb_abi_release(xb_abi_handle *h, xb_abi_str s)
{
    (void)s;
    if (!h) return;
    if (h->result.data) h->result.len = 0;
}

/* ----------------------------------------------------------------- calls -- */

/* The ABI hands chunks to the host as JSON text; the internal ctx wants parsed
 * values. Parse into a scratch arena per chunk: chunks are small and a stream
 * can run for a long time, so nothing may accumulate. */
typedef struct {
    bool      (*on_chunk)(void *user, uint32_t seq, const char *chunk_json);
    void       *user;
} abi_chunk_shim;

static bool abi_emit(void *ud, int64_t seq, const xb_json *chunk)
{
    abi_chunk_shim *shim = (abi_chunk_shim *)ud;
    if (!shim || !shim->on_chunk) return true;

    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_value(&w, chunk);
    bool keep_going = shim->on_chunk(shim->user, (uint32_t)seq,
                                     (const char *)w.buf.data);
    xb_jw_free(&w);
    return keep_going;
}

xb_abi_status xb_abi_call(xb_abi_handle *h, const xb_abi_request *req,
                          xb_abi_str *out_json)
{
    if (!h || !req || !req->method) return XB_ERR_INVALID;
    if (out_json) *out_json = NULL;

    xb_handler_fn fn = xb_lookup_method(req->method);
    if (!fn) return XB_ERR_NO_METHOD;

    /* Params are parsed into an arena that lives for the whole call, because
     * handlers keep pointers into the parsed tree. */
    xb_arena *arena = (xb_arena *)xb_alloc(sizeof *arena);
    xb_arena_init(arena);
    xb_json *params = NULL;
    if (req->params_json && req->params_json[0]) {
        size_t off = 0;
        params = xb_json_parse(arena, req->params_json, strlen(req->params_json), &off);
        if (!params) {
            xb_arena_destroy(arena);
            xb_free(arena);
            return XB_ERR_PARSE;
        }
    }

    /* Handlers write a complete JSON value themselves (the dispatcher that
     * wraps results in {"id","ok","result"} lives in the socket server), so the
     * writer starts empty and the handler's value is the whole answer. */
    xb_jsonw w;
    xb_jw_init(&w);

    int64_t deadline = xb_mono_ms() +
        (req->deadline_ms ? (int64_t)req->deadline_ms : h->default_deadline_ms);

    xb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.bridge = h->bridge;
    ctx.id = "abi";
    ctx.method = req->method;
    ctx.params = params;
    ctx.out = &w;
    ctx.deadline_ms = deadline;
    abi_chunk_shim shim;
    shim.on_chunk = req->on_chunk;
    shim.user = req->on_chunk_user;

    ctx.stream = (req->flags & XB_ABI_F_STREAM) != 0;
    ctx.emit = req->on_chunk ? abi_emit : NULL;
    ctx.emit_ud = &shim;
    ctx.cancelled = (volatile bool *)req->cancel_flag;

    int rc = fn(&ctx);
    if (req->should_stop && req->should_stop(req->user)) rc = XB_ERR_CANCELLED;

    xb_arena_destroy(arena);
    xb_free(arena);

    char *text = (w.buf.data && w.buf.len) ? xb_buf_steal(&w.buf) : xb_strdup("null");
    xb_jw_free(&w);

    if (rc != 0) {
        xb_free(text);
        return rc;
    }

    /* The caller's string must outlive this call, so park it in the handle. */
    xb_buf_reset(&h->result);
    xb_buf_append(&h->result, text, strlen(text));
    xb_free(text);
    if (out_json) *out_json = (xb_abi_str)h->result.data;
    return 0;
}

xb_abi_status xb_abi_install(xb_abi_handle *h, const char *path, xb_abi_str *out_json)
{
    if (!path || !path[0]) return XB_ERR_INVALID;
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "path", path);
    xb_jw_obj_end(&w);
    char *params = xb_buf_steal(&w.buf);
    xb_jw_free(&w);

    xb_abi_request req;
    memset(&req, 0, sizeof req);
    req.method = "extension.install";
    req.params_json = params;
    req.deadline_ms = 60000;
    xb_abi_status st = xb_abi_call(h, &req, out_json);
    xb_free(params);
    return st;
}

xb_abi_status xb_abi_detect(xb_abi_handle *h, const char *path, xb_abi_str *out_json)
{
    if (!path || !path[0]) return XB_ERR_INVALID;
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "path", path);
    xb_jw_obj_end(&w);
    char *params = xb_buf_steal(&w.buf);
    xb_jw_free(&w);

    xb_abi_request req;
    memset(&req, 0, sizeof req);
    req.method = "format.detect";
    req.params_json = params;
    req.deadline_ms = 15000;
    xb_abi_status st = xb_abi_call(h, &req, out_json);
    xb_free(params);
    return st;
}
