/* bridge.c — bridge state, method registry and the built-in method surface.
 *
 * This file is the contract: every method name documented in docs/METHODS.md is
 * registered here, and routing is decided by the method's metadata (which engine
 * serves it), never by ad-hoc string comparisons scattered through the code.
 */
#include "bridge/bridge_internal.h"
#include "bridge/server.h"
#include "bridge/registry.h"
#include "bridge/rule_engine.h"
#include "bridge/archive.h"
#include "bridge/http.h"
#include "bridge/buf.h"
#include "bridge/log.h"
#include "bridge/util.h"
#include "bridge/engine.h"
#include "bridge/cache.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define XB_MKDIR(p) _mkdir(p)
#  include <sys/stat.h>
#  define XB_STAT_ISDIR(m) ((m) & _S_IFDIR)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define XB_MKDIR(p) mkdir((p), 0755)
#  define XB_STAT_ISDIR(m) S_ISDIR(m)
#endif

#define XB_MAX_EXTENSIONS 512
#define XB_MAX_REPOS 64
#define XB_MAX_ENGINES 8

/* --------------------------------------------------------------- errors -- */

const char *xb_error_message(int code)
{
    switch (code) {
    case 0:                return "ok";
    case XB_ERR_PARSE:     return "malformed request payload";
    case XB_ERR_INVALID:   return "invalid request";
    case XB_ERR_NO_METHOD: return "method not found";
    case XB_ERR_PARAMS:    return "invalid params";
    case XB_ERR_ENGINE:    return "engine failure";
    case XB_ERR_DEADLINE:  return "deadline exceeded";
    case XB_ERR_CANCELLED: return "cancelled";
    case XB_ERR_NOT_FOUND: return "not found";
    case XB_ERR_UNAVAILABLE: return "engine unavailable";
    case XB_ERR_NETWORK:   return "network failure";
    case XB_ERR_REJECTED:  return "artifact rejected";
    case XB_ERR_LIMIT:     return "resource limit exceeded";
    default:               return "unknown error";
    }
}

bool xb_error_retryable(int code)
{
    switch (code) {
    case XB_ERR_ENGINE:
    case XB_ERR_DEADLINE:
    case XB_ERR_UNAVAILABLE:
    case XB_ERR_NETWORK:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------- registry -- */

#define XB_MAX_METHODS 128

static struct {
    xb_method_meta entries[XB_MAX_METHODS];
    xb_handler_fn  handlers[XB_MAX_METHODS];
    size_t         n;
    xb_mutex_t     m;
    bool           ready;
} MR;

static void mr_init(void)
{
    if (MR.ready) return;
    XB_MUTEX_INIT(&MR.m);
    MR.ready = true;
}

/* Metadata and handler are registered separately so callers can register in
 * either order. Both merge into the same slot keyed by name; this is why adding
 * a method never leaves a half-registered entry behind. */
static int mr_slot_locked(const char *name)
{
    for (size_t i = 0; i < MR.n; i++)
        if (MR.entries[i].name && xb_streq(MR.entries[i].name, name)) return (int)i;
    return -1;
}

int xb_register_method_meta(const xb_method_meta *meta)
{
    mr_init();
    if (!meta || !meta->name) return -1;
    XB_MUTEX_LOCK(&MR.m);
    int existing = mr_slot_locked(meta->name);
    if (existing >= 0) {
        /* Preserve a NULL summary/engine the caller did not resupply. */
        xb_method_meta merged = *meta;
        if (!merged.engine) merged.engine = MR.entries[existing].engine;
        if (!merged.summary) merged.summary = MR.entries[existing].summary;
        MR.entries[existing] = merged;
        XB_MUTEX_UNLOCK(&MR.m);
        return 0;
    }
    if (MR.n >= XB_MAX_METHODS) { XB_MUTEX_UNLOCK(&MR.m); return -1; }
    MR.entries[MR.n] = *meta;
    MR.entries[MR.n].name = xb_strdup(meta->name);
    MR.handlers[MR.n] = NULL;
    MR.n++;
    XB_MUTEX_UNLOCK(&MR.m);
    return 0;
}

bool xb_lookup_method_meta(const char *name, xb_method_meta *out)
{
    mr_init();
    XB_MUTEX_LOCK(&MR.m);
    for (size_t i = 0; i < MR.n; i++) {
        if (xb_streq(MR.entries[i].name, name)) {
            if (out) *out = MR.entries[i];
            XB_MUTEX_UNLOCK(&MR.m);
            return true;
        }
    }
    XB_MUTEX_UNLOCK(&MR.m);
    return false;
}

int xb_register_method(const char *name, xb_handler_fn fn)
{
    mr_init();
    if (!name || !fn) return -1;
    XB_MUTEX_LOCK(&MR.m);
    int existing = mr_slot_locked(name);
    if (existing >= 0) {
        MR.handlers[existing] = fn;
        XB_MUTEX_UNLOCK(&MR.m);
        return 0;
    }
    if (MR.n >= XB_MAX_METHODS) { XB_MUTEX_UNLOCK(&MR.m); return -1; }
    memset(&MR.entries[MR.n], 0, sizeof MR.entries[MR.n]);
    MR.entries[MR.n].name = xb_strdup(name);
    MR.handlers[MR.n] = fn;
    MR.n++;
    XB_MUTEX_UNLOCK(&MR.m);
    return 0;
}

/* Release the registry's own allocations. The registry is process-global, so
 * this exists for hosts that embed the library and then want a clean leak
 * report (test runners, hot-reloading shells) rather than for the daemon. */
void xb_method_registry_free(void)
{
    mr_init();
    XB_MUTEX_LOCK(&MR.m);
    for (size_t i = 0; i < MR.n; i++) {
        /* Only `name` is copied on registration; `engine` and `summary` point
         * at string literals owned by the caller. */
        xb_free((void *)MR.entries[i].name);
    }
    memset(MR.entries, 0, sizeof MR.entries);
    memset(MR.handlers, 0, sizeof MR.handlers);
    MR.n = 0;
    XB_MUTEX_UNLOCK(&MR.m);
}

xb_handler_fn xb_lookup_method(const char *name)
{
    mr_init();
    XB_MUTEX_LOCK(&MR.m);
    for (size_t i = 0; i < MR.n; i++) {
        if (MR.entries[i].name && xb_streq(MR.entries[i].name, name)) {
            xb_handler_fn fn = MR.handlers[i];
            XB_MUTEX_UNLOCK(&MR.m);
            return fn;
        }
    }
    /* The source surface is one handler behind many names; a name outside the
     * registered set is a genuine "method not found". */
    XB_MUTEX_UNLOCK(&MR.m);
    return NULL;
}

size_t xb_method_count(void)
{
    mr_init();
    XB_MUTEX_LOCK(&MR.m);
    size_t n = MR.n;
    XB_MUTEX_UNLOCK(&MR.m);
    return n;
}

char *xb_method_list_json(void)
{
    mr_init();
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_arr_begin(&w);
    XB_MUTEX_LOCK(&MR.m);
    for (size_t i = 0; i < MR.n; i++) {
        xb_jw_obj_begin(&w);
        xb_jw_kv_str(&w, "name", MR.entries[i].name);
        xb_jw_kv_str(&w, "engine", MR.entries[i].engine ? MR.entries[i].engine : "");
        xb_jw_kv_bool(&w, "streamable", MR.entries[i].streamable);
        xb_jw_kv_bool(&w, "cacheable", MR.entries[i].cacheable);
        if (MR.entries[i].summary) xb_jw_kv_str(&w, "summary", MR.entries[i].summary);
        xb_jw_obj_end(&w);
    }
    XB_MUTEX_UNLOCK(&MR.m);
    xb_jw_arr_end(&w);
    return xb_buf_steal(&w.buf);
}

/* ----------------------------------------------------------- bridge state -- */

typedef struct {
    char id[192];
    char name[192];
    char url[1024];
    char manager_id[32];
    int64_t added_ms;
} repo_entry;

typedef struct {
    char id[192];
    char name[192];
    char version[64];
    char manager_id[32];
    char format_id[32];
    char path[1024];
    char repo_url[1024];
    char kind[16];
    int64_t installed_ms;
    int64_t size;
} ext_entry;

struct xb_bridge {
    xb_config  cfg;
    xb_cache  *cache;
    xb_engine *engines[XB_MAX_ENGINES];
    size_t     engine_count;

    repo_entry repos[XB_MAX_REPOS];
    size_t     repo_count;

    ext_entry  exts[XB_MAX_EXTENSIONS];
    size_t     ext_count;

    char endpoint[320];

    xb_mutex_t lock;
    void      *conns[256];
    size_t     conn_count;
    volatile bool shutdown_requested;

    xb_server_stats *stats;

    /* Ad-hoc KV preferences for sources that have no real preference schema. */
    char kv_path[1024];
};

static xb_bridge *g_bridge = NULL;

xb_cache *xb_bridge_cache(xb_bridge *b) { return b ? b->cache : NULL; }

xb_engine *xb_bridge_engine(xb_bridge *b, const char *name)
{
    if (!b || !name) return NULL;
    for (size_t i = 0; i < b->engine_count; i++)
        if (xb_streq(xb_engine_name(b->engines[i]), name)) return b->engines[i];
    return NULL;
}

void xb_bridge_set_endpoint(xb_bridge *b, const char *uri)
{
    if (b && uri) xb_str_lcpy(b->endpoint, uri, sizeof b->endpoint);
}

const char *xb_bridge_endpoint(xb_bridge *b) { return b ? b->endpoint : ""; }

void xb_bridge_set_server_stats(xb_bridge *b, xb_server_stats *s)
{
    if (b) b->stats = s;
}

void xb_bridge_cancel(xb_bridge *b, const char *job_id)
{
    if (!b || !job_id) return;
    for (size_t i = 0; i < b->engine_count; i++)
        xb_engine_cancel(b->engines[i], job_id);
}

void xb_bridge_connection_added(xb_bridge *b, void *conn)
{
    if (!b) return;
    XB_MUTEX_LOCK(&b->lock);
    for (size_t i = 0; i < sizeof b->conns / sizeof b->conns[0]; i++) {
        if (!b->conns[i]) { b->conns[i] = conn; b->conn_count++; break; }
    }
    XB_MUTEX_UNLOCK(&b->lock);
}

void xb_bridge_connection_removed(xb_bridge *b, void *conn)
{
    if (!b) return;
    XB_MUTEX_LOCK(&b->lock);
    for (size_t i = 0; i < sizeof b->conns / sizeof b->conns[0]; i++) {
        if (b->conns[i] == conn) { b->conns[i] = NULL; if (b->conn_count) b->conn_count--; break; }
    }
    XB_MUTEX_UNLOCK(&b->lock);
}

/* Closing connections is the server's business; it owns the socket handles.
 * The bridge only asks, via this hook the server installs. */
static void (*g_close_conns)(void *) = NULL;
static void *g_close_ud = NULL;

void xb_bridge_set_close_hook(void (*fn)(void *), void *ud)
{
    g_close_conns = fn;
    g_close_ud = ud;
}

void xb_bridge_close_all_connections(xb_bridge *b)
{
    if (!b) return;
    XB_MUTEX_LOCK(&b->lock);
    size_t n = b->conn_count;
    XB_MUTEX_UNLOCK(&b->lock);
    if (n && g_close_conns) g_close_conns(g_close_ud);
}

/* ------------------------------------------------------------ persistence -- */

static void path_join(char *out, size_t cap, const char *a, const char *b)
{
    if (!a || !a[0]) { xb_str_lcpy(out, b, cap); return; }
    snprintf(out, cap, "%s%c%s", a, XB_PATHSEP, b);
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    xb_str_lcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == XB_PATHSEP || *p == '/') {
            char save = *p;
            *p = '\0';
            XB_MKDIR(tmp);
            *p = save;
        }
    }
    XB_MKDIR(tmp);
}

static bool is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return XB_STAT_ISDIR(st.st_mode) != 0;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)xb_alloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    if (out_len) *out_len = got;
    return buf;
}

static void write_file_atomic(const char *path, const char *data, size_t len)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
    remove(path);
    if (rename(tmp, path) != 0) {
        /* Windows refuses rename over an existing file; retry after unlink. */
        remove(path);
        rename(tmp, path);
    }
}

static char *state_path(xb_bridge *b, const char *name)
{
    char *out = (char *)xb_alloc(1200);
    path_join(out, 1200, b->cfg.data_dir, name);
    return out;
}

static void state_save(xb_bridge *b)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_int(&w, "version", 1);

    xb_jw_key(&w, "repos");
    xb_jw_arr_begin(&w);
    for (size_t i = 0; i < b->repo_count; i++) {
        xb_jw_obj_begin(&w);
        xb_jw_kv_str(&w, "id", b->repos[i].id);
        xb_jw_kv_str(&w, "name", b->repos[i].name);
        xb_jw_kv_str(&w, "url", b->repos[i].url);
        xb_jw_kv_str(&w, "manager_id", b->repos[i].manager_id);
        xb_jw_kv_int(&w, "added_ms", b->repos[i].added_ms);
        xb_jw_obj_end(&w);
    }
    xb_jw_arr_end(&w);

    xb_jw_key(&w, "extensions");
    xb_jw_arr_begin(&w);
    for (size_t i = 0; i < b->ext_count; i++) {
        xb_jw_obj_begin(&w);
        xb_jw_kv_str(&w, "id", b->exts[i].id);
        xb_jw_kv_str(&w, "name", b->exts[i].name);
        xb_jw_kv_str(&w, "version", b->exts[i].version);
        xb_jw_kv_str(&w, "manager_id", b->exts[i].manager_id);
        xb_jw_kv_str(&w, "format_id", b->exts[i].format_id);
        xb_jw_kv_str(&w, "path", b->exts[i].path);
        xb_jw_kv_str(&w, "repo_url", b->exts[i].repo_url);
        xb_jw_kv_str(&w, "kind", b->exts[i].kind);
        xb_jw_kv_int(&w, "installed_ms", b->exts[i].installed_ms);
        xb_jw_kv_int(&w, "size", b->exts[i].size);
        xb_jw_obj_end(&w);
    }
    xb_jw_arr_end(&w);
    xb_jw_obj_end(&w);

    char *path = state_path(b, "state.json");
    write_file_atomic(path, (const char *)w.buf.data, w.buf.len);
    xb_free(path);
    xb_jw_free(&w);
}

static void state_load(xb_bridge *b)
{
    char *path = state_path(b, "state.json");
    size_t len = 0;
    char *text = read_file(path, &len);
    xb_free(path);
    if (!text) return;

    xb_arena arena;
    xb_arena_init(&arena);
    size_t off = 0;
    xb_json *doc = xb_json_parse(&arena, text, len, &off);
    xb_free(text);
    if (!doc) { xb_arena_destroy(&arena); return; }

    xb_json *repos = xb_json_get(doc, "repos");
    for (size_t i = 0; repos && i < xb_json_arr_len(repos) && b->repo_count < XB_MAX_REPOS; i++) {
        xb_json *e = xb_json_arr_at(repos, i);
        repo_entry *r = &b->repos[b->repo_count];
        xb_str_lcpy(r->id, xb_json_obj_str(e, "id", ""), sizeof r->id);
        xb_str_lcpy(r->name, xb_json_obj_str(e, "name", ""), sizeof r->name);
        xb_str_lcpy(r->url, xb_json_obj_str(e, "url", ""), sizeof r->url);
        xb_str_lcpy(r->manager_id, xb_json_obj_str(e, "manager_id", ""), sizeof r->manager_id);
        r->added_ms = xb_json_obj_int(e, "added_ms", 0);
        b->repo_count++;
    }

    xb_json *exts = xb_json_get(doc, "extensions");
    for (size_t i = 0; exts && i < xb_json_arr_len(exts) && b->ext_count < XB_MAX_EXTENSIONS; i++) {
        xb_json *e = xb_json_arr_at(exts, i);
        ext_entry *x = &b->exts[b->ext_count];
        xb_str_lcpy(x->id, xb_json_obj_str(e, "id", ""), sizeof x->id);
        xb_str_lcpy(x->name, xb_json_obj_str(e, "name", ""), sizeof x->name);
        xb_str_lcpy(x->version, xb_json_obj_str(e, "version", ""), sizeof x->version);
        xb_str_lcpy(x->manager_id, xb_json_obj_str(e, "manager_id", ""), sizeof x->manager_id);
        xb_str_lcpy(x->format_id, xb_json_obj_str(e, "format_id", ""), sizeof x->format_id);
        xb_str_lcpy(x->path, xb_json_obj_str(e, "path", ""), sizeof x->path);
        xb_str_lcpy(x->repo_url, xb_json_obj_str(e, "repo_url", ""), sizeof x->repo_url);
        xb_str_lcpy(x->kind, xb_json_obj_str(e, "kind", ""), sizeof x->kind);
        x->installed_ms = xb_json_obj_int(e, "installed_ms", 0);
        x->size = xb_json_obj_int(e, "size", 0);
        b->ext_count++;
        /* Rule-based extensions are re-registered with the rule engine so they
         * keep working across daemon restarts without a reinstall. */
        if (xb_streq(x->format_id, "legado") && x->path[0]) {
            char err[160];
            xb_rule_engine_load_file(x->id, x->path, err, sizeof err);
        }
    }
    xb_arena_destroy(&arena);
}

/* ------------------------------------------------------------- lifecycle -- */

xb_bridge *xb_bridge_new(const xb_config *cfg)
{
    xb_bridge *b = (xb_bridge *)xb_alloc(sizeof *b);
    b->cfg = cfg ? *cfg : xb_g_config;
    XB_MUTEX_INIT(&b->lock);
    return b;
}

int xb_bridge_init(xb_bridge *b, char *err, size_t errcap)
{
    if (!b) { snprintf(err, errcap, "no bridge"); return -1; }
    g_bridge = b;

    b->cache = xb_cache_new((size_t)b->cfg.cache_entries, b->cfg.cache_bytes,
                            b->cfg.cache_ttl_ms);

    if (b->cfg.data_dir[0]) {
        mkdir_p(b->cfg.data_dir);
        char sub[1024];
        path_join(sub, sizeof sub, b->cfg.data_dir, "extensions");
        mkdir_p(sub);
        path_join(sub, sizeof sub, b->cfg.data_dir, "tools");
        mkdir_p(sub);
    }

    /* The rule engine is always available: it is compiled in. */
    xb_engine *rule = xb_engine_new_inproc("rule", xb_rule_engine_inproc, NULL);
    if (rule) {
        xb_engine_start(rule, true, err, errcap);
        b->engines[b->engine_count++] = rule;
    }

    /* Optional engines: attach to whatever runtime is present. The environment
     * variables let a host point at a specific binary (bundled Node, a JRE). */
    if (b->engine_count < XB_MAX_ENGINES) {
        const char *js_cmd = getenv("XBRIDGE_JS_WORKER");
        if (js_cmd && js_cmd[0]) {
            const char *argv[] = { js_cmd, NULL };
            xb_engine *e = xb_engine_new("js", argv, b->cfg.workers_per_engine,
                                         8000, 250);
            if (e) {
                xb_engine_start(e, false, err, errcap);
                b->engines[b->engine_count++] = e;
            }
        }
    }
    if (b->engine_count < XB_MAX_ENGINES) {
        const char *jvm_cmd = getenv("XBRIDGE_JVM_WORKER");
        if (jvm_cmd && jvm_cmd[0]) {
            const char *argv[] = { jvm_cmd, NULL };
            xb_engine *e = xb_engine_new("jvm", argv, b->cfg.workers_per_engine,
                                         20000, 500);
            if (e) {
                xb_engine_start(e, false, err, errcap);
                b->engines[b->engine_count++] = e;
            }
        }
    }

    state_load(b);
    xb_bridge_set_close_hook(NULL, NULL);

    if (b->cfg.prewarm) {
        for (size_t i = 0; i < b->engine_count; i++) {
            char e2[128];
            xb_engine_start(b->engines[i], false, e2, sizeof e2);
        }
    }
    return 0;
}

void xb_bridge_free(xb_bridge *b)
{
    if (!b) return;
    state_save(b);
    for (size_t i = 0; i < b->engine_count; i++) xb_engine_free(b->engines[i]);
    xb_cache_free(b->cache);
    xb_rule_engine_reset();
    XB_MUTEX_DESTROY(&b->lock);
    xb_free(b);
    g_bridge = NULL;
}

/* ------------------------------------------------------------- handshake -- */

void xb_bridge_engine_list_json(xb_bridge *b, xb_jsonw *w)
{
    xb_jw_arr_begin(w);
    for (size_t i = 0; b && i < b->engine_count; i++) {
        xb_engine_stats es;
        xb_engine_stats_of(b->engines[i], &es);
        xb_jw_obj_begin(w);
        xb_jw_kv_str(w, "name", xb_engine_name(b->engines[i]));
        xb_jw_kv_str(w, "status", xb_engine_state_name(xb_engine_state_of(b->engines[i])));
        xb_jw_kv_int(w, "workers", es.live_workers);
        xb_jw_kv_int(w, "workers_configured", es.total_workers);
        xb_jw_key(w, "formats");
        xb_jw_arr_begin(w);
        for (size_t f = 0; f < xb_format_count(); f++) {
            const xb_format *fmt = xb_format_at(f);
            if (xb_strieq(fmt->engine, xb_engine_name(b->engines[i])))
                xb_jw_str(w, fmt->id);
        }
        xb_jw_arr_end(w);
        xb_jw_obj_end(w);
    }
    xb_jw_arr_end(w);
}

void xb_bridge_handshake_json(xb_bridge *b, xb_jsonw *w)
{
    xb_jw_obj_begin(w);
    xb_jw_kv_str(w, "protocol", XBP_VERSION);
    xb_jw_kv_str(w, "software", XBP_SOFTWARE);
    xb_jw_kv_int(w, "protocol_version", 1);
    /* pid lets a supervisor reap the daemon and lets the SDK's spawn helper
     * detect a replacement process without a second syscall. */
    xb_jw_kv_int(w, "pid", (int64_t)xb_getpid_wrapper());
    xb_jw_kv_int(w, "started_ms", xb_now_ms());
    xb_jw_kv_int(w, "max_frame", b->cfg.max_frame);
    xb_jw_kv_int(w, "task_threads", b->cfg.task_threads);
    xb_jw_kv_int(w, "cache_entries", b->cfg.cache_entries);
    xb_jw_kv_int(w, "cache_ttl_ms", b->cfg.cache_ttl_ms);
    xb_jw_kv_str(w, "endpoint", b->endpoint);
    xb_jw_kv_str(w, "data_dir", b->cfg.data_dir);

    xb_jw_key(w, "features");
    xb_jw_arr_begin(w);
    xb_jw_str(w, "stream");
    xb_jw_str(w, "cancel");
    xb_jw_str(w, "deadline");
    xb_jw_str(w, "cache");
    xb_jw_str(w, "coalesce");
    xb_jw_str(w, "pipelining");
    xb_jw_str(w, "out-of-order-responses");
    xb_jw_str(w, "format-registry");
    xb_jw_str(w, "rule-engine");
    xb_jw_arr_end(w);

    xb_jw_key(w, "capabilities");
    xb_jw_obj_begin(w);
    xb_jw_kv_bool(w, "tls", xb_http_tls_available());
    xb_jw_kv_str(w, "tls_backend", xb_http_tls_backend());
    xb_jw_kv_int(w, "formats", (int64_t)xb_format_count());
    xb_jw_kv_int(w, "methods", (int64_t)xb_method_count());
    xb_jw_kv_int(w, "installed_extensions", (int64_t)b->ext_count);
    xb_jw_kv_int(w, "repositories", (int64_t)b->repo_count);
    xb_jw_kv_bool(w, "allow_shutdown", b->cfg.allow_shutdown);
    xb_jw_obj_end(w);

    xb_jw_key(w, "engines");
    xb_bridge_engine_list_json(b, w);
    xb_jw_obj_end(w);
}

/* -------------------------------------------------------------- helpers -- */

static ext_entry *find_ext(xb_bridge *b, const char *id)
{
    for (size_t i = 0; i < b->ext_count; i++)
        if (xb_streq(b->exts[i].id, id)) return &b->exts[i];
    return NULL;
}

static ext_entry *find_ext_by_manager(xb_bridge *b, const char *manager, const char *name)
{
    for (size_t i = 0; i < b->ext_count; i++)
        if (xb_streq(b->exts[i].manager_id, manager) && xb_streq(b->exts[i].name, name))
            return &b->exts[i];
    return NULL;
}

static void ext_json(xb_jsonw *w, const ext_entry *x)
{
    xb_jw_obj_begin(w);
    xb_jw_kv_str(w, "id", x->id);
    xb_jw_kv_str(w, "name", x->name);
    xb_jw_kv_str(w, "version", x->version);
    xb_jw_kv_str(w, "manager_id", x->manager_id);
    xb_jw_kv_str(w, "format_id", x->format_id);
    xb_jw_kv_str(w, "path", x->path);
    if (x->repo_url[0]) xb_jw_kv_str(w, "repo_url", x->repo_url);
    xb_jw_kv_str(w, "kind", x->kind);
    xb_jw_kv_int(w, "installed_ms", x->installed_ms);
    xb_jw_kv_int(w, "size", x->size);
    xb_jw_kv_bool(w, "installed", true);
    xb_jw_obj_end(w);
}

/* Extract the first meaningful version-looking token from a filename. */
static void guess_version(const char *filename, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = filename;
    int best_len = 0;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            const char *s = p;
            int dots = 0;
            while (*p && (isdigit((unsigned char)*p) || (*p == '.' && dots < 3))) {
                if (*p == '.') dots++;
                p++;
            }
            int len = (int)(p - s);
            if (len > best_len && len <= (int)cap - 1) {
                best_len = len;
                memcpy(out, s, (size_t)len);
                out[len] = '\0';
            }
        } else {
            p++;
        }
    }
}

static char *slugify(const char *name, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out[o++] = (char)c;
        else if (c == ' ' || c == '/' || c == '\\') out[o++] = '-';
    }
    out[o] = '\0';
    return out;
}

static int64_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_size;
}

/* Copy a file preserving the original name into the extension directory. */
static int copy_artifact(xb_bridge *b, const char *src, const char *manager,
                         char *dest_out, size_t dest_cap)
{
    const char *base = strrchr(src, XB_PATHSEP);
#ifdef _WIN32
    const char *base2 = strrchr(src, '/');
    if (base2 && (!base || base2 > base)) base = base2;
#endif
    base = base ? base + 1 : src;

    char dir[1024];
    snprintf(dir, sizeof dir, "%s%cextensions%c%s", b->cfg.data_dir, XB_PATHSEP, XB_PATHSEP, manager);
    mkdir_p(dir);

    char dest[1400];
    snprintf(dest, sizeof dest, "%s%c%s", dir, XB_PATHSEP, base);

    /* Same path? Nothing to do. */
    if (xb_streq(src, dest)) { xb_str_lcpy(dest_out, dest, dest_cap); return 0; }

    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dest, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    xb_str_lcpy(dest_out, dest, dest_cap);
    return 0;
}

static int install_path(xb_bridge *b, const char *path, const char *manager_hint,
                        const char *repo_url, char *err, size_t errcap,
                        ext_entry *out)
{
    if (b->ext_count >= XB_MAX_EXTENSIONS) {
        snprintf(err, errcap, "extension table is full");
        return -1;
    }
    xb_detect_result det;
    if (xb_detect_path(path, &det) != 0) {
        snprintf(err, errcap, "cannot read %.150s", path);
        return -1;
    }
    const char *manager = (det.manager_id[0] ? det.manager_id : manager_hint);
    if (!manager || !manager[0]) {
        snprintf(err, errcap, "unrecognized extension format: %.120s", path);
        return -1;
    }
    const xb_format *fmt = xb_format_by_manager(manager);

    char copied[1400] = "";
    const char *final_path = path;
    if (!is_dir(path)) {
        if (copy_artifact(b, path, manager, copied, sizeof copied) == 0)
            final_path = copied;
    }

    const char *base = strrchr(final_path, XB_PATHSEP);
    base = base ? base + 1 : final_path;

    ext_entry *x = &b->exts[b->ext_count];
    memset(x, 0, sizeof *x);
    char slug[96];
    slugify(base, slug, sizeof slug);
    {
        /* Build the id in a wider buffer first: the components come from the
         * filesystem and a long name must not silently truncate into a
         * colliding id. */
        char idbuf[320];
        snprintf(idbuf, sizeof idbuf, "%s/%s", manager, slug);
        xb_str_lcpy(x->id, idbuf, sizeof x->id);
    }
    xb_str_lcpy(x->name, base, sizeof x->name);
    xb_str_lcpy(x->manager_id, manager, sizeof x->manager_id);
    xb_str_lcpy(x->format_id, fmt ? fmt->id : manager, sizeof x->format_id);
    xb_str_lcpy(x->path, final_path, sizeof x->path);
    if (repo_url) xb_str_lcpy(x->repo_url, repo_url, sizeof x->repo_url);
    xb_str_lcpy(x->kind, xb_artifact_kind_name(det.kind), sizeof x->kind);
    x->installed_ms = xb_now_ms();
    x->size = file_size(final_path);
    guess_version(base, x->version, sizeof x->version);
    if (!x->version[0]) xb_str_lcpy(x->version, "unknown", sizeof x->version);

    /* Rule-based sources are loaded eagerly so the first request is not slower
     * than the rest, and so an invalid rule file fails at install time. */
    if (xb_streq(x->format_id, "legado")) {
        int n = xb_rule_engine_load_file(x->id, final_path, err, errcap);
        if (n < 0) {
            memset(x, 0, sizeof *x);
            return -1;
        }
        XB_INFO("installed rule source %s (%d rule(s))", x->id, n);
    }

    b->ext_count++;
    if (out) *out = *x;
    state_save(b);
    return 0;
}

static int uninstall(xb_bridge *b, const char *id, char *err, size_t errcap)
{
    for (size_t i = 0; i < b->ext_count; i++) {
        if (!xb_streq(b->exts[i].id, id)) continue;
        xb_rule_engine_unload(id);
        if (b->exts[i].path[0]) remove(b->exts[i].path);
        b->exts[i] = b->exts[b->ext_count - 1];
        b->ext_count--;
        state_save(b);
        return 0;
    }
    snprintf(err, errcap, "extension '%s' is not installed", id);
    return -1;
}

/* Route a source.* call to the engine that owns the format. */
static xb_engine *engine_for_ext(xb_bridge *b, const ext_entry *x)
{
    const xb_format *fmt = xb_format_by_manager(x->manager_id);
    const char *engine_name = fmt ? fmt->engine : "js";
    xb_engine *e = xb_bridge_engine(b, engine_name);
    if (e && xb_engine_state_of(e) != XB_ENGINE_UNAVAILABLE) return e;
    return NULL;
}

/* ----------------------------------------------------------- src: handler -- */

/* Progressive results.
 *
 * Engines that build their answer in one shot (the rule engine does) still have
 * something useful to stream: a long list is exactly what a host wants to start
 * rendering early. So a streaming request for a list-shaped method is split
 * into one chunk per item, with a final chunk carrying the summary the
 * non-streaming call would have returned. */
static void emit_result_chunks(xb_ctx *ctx, const char *result_json)
{
    if (!ctx->emit || !result_json || !result_json[0]) return;
    if (xb_streq(result_json, "null")) return;

    static const char *ARRAYS[] = { "list", "episodes", "items", "results" };

    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *root = xb_json_parse(&a, result_json, strlen(result_json), &off);
    if (!root) { xb_arena_destroy(&a); return; }

    const xb_json *arr = NULL;
    for (size_t i = 0; i < XB_ARRAY_LEN(ARRAYS) && !arr; i++) {
        const xb_json *v = xb_json_obj_get(root, ARRAYS[i]);
        if (v && v->type == XB_JARR) arr = v;
    }

    if (arr && xb_json_arr_len(arr) > 0) {
        for (size_t i = 0; i < (size_t)xb_json_arr_len(arr); i++) {
            xb_jsonw w;
            xb_jw_init(&w);
            xb_jw_obj_begin(&w);
            xb_jw_kv_int(&w, "index", (int64_t)i);
            xb_jw_key(&w, "item");
            xb_jw_value(&w, xb_json_arr_at(arr, (int)i));
            xb_jw_obj_end(&w);

            xb_arena ca;
            xb_arena_init(&ca);
            size_t coff = 0;
            xb_json *chunk = xb_json_parse(&ca, (const char *)w.buf.data, w.buf.len, &coff);
            if (chunk) ctx->emit(ctx->emit_ud, (int64_t)i, chunk);
            xb_arena_destroy(&ca);
            xb_jw_free(&w);
        }
        /* Summary last, so a client that only wants the count can stop early
         * without waiting for the whole list to be rendered. */
        xb_jsonw sum;
        xb_jw_init(&sum);
        xb_jw_obj_begin(&sum);
        xb_jw_kv_bool(&sum, "summary", true);
        xb_jw_kv_int(&sum, "count", (int64_t)xb_json_arr_len(arr));
        xb_jw_kv_bool(&sum, "has_next", xb_json_obj_bool(root, "has_next", false));
        xb_jw_kv_int(&sum, "page", xb_json_obj_int(root, "page", 1));
        xb_jw_obj_end(&sum);
        xb_arena sa;
        xb_arena_init(&sa);
        size_t soff = 0;
        xb_json *schunk = xb_json_parse(&sa, (const char *)sum.buf.data, sum.buf.len, &soff);
        if (schunk) ctx->emit(ctx->emit_ud, (int64_t)xb_json_arr_len(arr), schunk);
        xb_arena_destroy(&sa);
        xb_jw_free(&sum);
    } else {
        /* Not a list (chapter text, a detail object): one chunk with everything. */
        xb_jsonw w;
        xb_jw_init(&w);
        xb_jw_obj_begin(&w);
        xb_jw_kv_int(&w, "index", 0);
        xb_jw_key(&w, "item");
        xb_jw_value(&w, root);
        xb_jw_obj_end(&w);
        xb_arena ca;
        xb_arena_init(&ca);
        size_t coff = 0;
        xb_json *chunk = xb_json_parse(&ca, (const char *)w.buf.data, w.buf.len, &coff);
        if (chunk) ctx->emit(ctx->emit_ud, 0, chunk);
        xb_arena_destroy(&ca);
        xb_jw_free(&w);
    }
    xb_arena_destroy(&a);
}

int xb_handler_source_family(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *method = ctx->method;
    const xb_json *params = ctx->params;

    const char *source_id = xb_json_obj_str(params, "source_id", NULL);
    if (!source_id) {
        xb_jw_key(ctx->out, "error");
        xb_jw_str(ctx->out, "source_id is required");
        return XB_ERR_PARAMS;
    }
    ext_entry *x = find_ext(b, source_id);
    if (!x) {
        /* Accept a manager/name pair as a convenience. */
        const char *manager = xb_json_obj_str(params, "manager_id", NULL);
        const char *name = xb_json_obj_str(params, "name", NULL);
        if (manager && name) x = find_ext_by_manager(b, manager, name);
    }
    if (!x) return XB_ERR_NOT_FOUND;

    if (xb_streq(method, "source.methods") || xb_streq(method, "source.info")) {
        const xb_format *fmt = xb_format_by_manager(x->manager_id);
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "id", x->id);
        xb_jw_kv_str(ctx->out, "name", x->name);
        xb_jw_kv_str(ctx->out, "version", x->version);
        xb_jw_kv_str(ctx->out, "manager_id", x->manager_id);
        if (fmt) {
            xb_jw_key(ctx->out, "capabilities");
            xb_jw_arr_begin(ctx->out);
            for (unsigned bit = 0; bit < 32; bit++) {
                if (fmt->caps & (1u << bit)) {
                    const char *n = xb_cap_name(bit);
                    if (n) xb_jw_str(ctx->out, n);
                }
            }
            xb_jw_arr_end(ctx->out);
        }
        if (xb_streq(x->format_id, "legado")) {
            char *desc = xb_rule_engine_describe(x->id);
            xb_jw_key(ctx->out, "rule");
            xb_jw_raw(ctx->out, desc);
            xb_free(desc);
        }
        xb_jw_obj_end(ctx->out);
        return 0;
    }

    if (xb_str_has_prefix(method, "source.getPreference") ||
        xb_str_has_prefix(method, "source.setPreference")) {
        /* Preferences are stored per source in a small JSON sidecar. */
        char path[1200];
        snprintf(path, sizeof path, "%s%cprefs-%s.json", b->cfg.data_dir,
                 XB_PATHSEP, x->manager_id);
        char *existing = read_file(path, NULL);
        xb_arena arena;
        xb_arena_init(&arena);
        size_t off = 0;
        xb_json *doc = existing ? xb_json_parse(&arena, existing, strlen(existing), &off) : NULL;
        xb_free(existing);

        if (xb_streq(method, "source.getPreference")) {
            const char *key = xb_json_obj_str(params, "key", NULL);
            xb_jw_obj_begin(ctx->out);
            xb_jw_kv_str(ctx->out, "source_id", x->id);
            if (key) {
                const char *v = doc ? xb_json_obj_str(doc, key, "") : "";
                xb_jw_kv_str(ctx->out, "key", key);
                xb_jw_key(ctx->out, "value");
                /* Values are stored as opaque JSON so numbers/bools survive. */
                if (doc) {
                    xb_json *node = xb_json_get(doc, key);
                    if (node) xb_jw_value(ctx->out, node); else xb_jw_null(ctx->out);
                } else {
                    xb_jw_null(ctx->out);
                }
                xb_jw_kv_str(ctx->out, "default", v);
            } else {
                xb_jw_key(ctx->out, "preferences");
                if (doc) xb_jw_value(ctx->out, doc); else xb_jw_obj_begin(ctx->out), xb_jw_obj_end(ctx->out);
            }
            xb_jw_obj_end(ctx->out);
            xb_arena_destroy(&arena);
            return 0;
        }

        /* setPreference */
        const char *key = xb_json_obj_str(params, "key", NULL);
        if (!key) { xb_arena_destroy(&arena); return XB_ERR_PARAMS; }
        xb_jsonw w;
        xb_jw_init(&w);
        xb_jw_obj_begin(&w);
        if (doc) {
            for (size_t i = 0; i < doc->u.obj.n; i++) {
                if (xb_streq(doc->u.obj.keys[i], key)) continue;
                xb_jw_key(&w, doc->u.obj.keys[i]);
                xb_jw_value(&w, doc->u.obj.vals[i]);
            }
        }
        xb_jw_key(&w, key);
        xb_json *value = xb_json_obj_get(params, "value");
        if (value) xb_jw_value(&w, value); else xb_jw_null(&w);
        xb_jw_obj_end(&w);
        write_file_atomic(path, (const char *)w.buf.data, w.buf.len);
        xb_jw_free(&w);
        xb_arena_destroy(&arena);
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_bool(ctx->out, "saved", true);
        xb_jw_obj_end(ctx->out);
        return 0;
    }

    xb_engine *e = engine_for_ext(b, x);
    if (!e) {
        return XB_ERR_UNAVAILABLE;
    }

    /* Build the job. `_source` travels with the request so engines stay
     * stateless about the local installation layout. */
    xb_jsonw merged;
    xb_jw_init(&merged);
    xb_jw_obj_begin(&merged);
    if (params && params->type == XB_JOBJ) {
        for (size_t i = 0; i < params->u.obj.n; i++) {
            xb_jw_key(&merged, params->u.obj.keys[i]);
            xb_jw_value(&merged, params->u.obj.vals[i]);
        }
    }
    xb_jw_key(&merged, "_source");
    xb_jw_obj_begin(&merged);
    xb_jw_kv_str(&merged, "id", x->id);
    xb_jw_kv_str(&merged, "name", x->name);
    xb_jw_kv_str(&merged, "version", x->version);
    xb_jw_kv_str(&merged, "manager_id", x->manager_id);
    xb_jw_kv_str(&merged, "format_id", x->format_id);
    xb_jw_kv_str(&merged, "path", x->path);
    xb_jw_obj_end(&merged);
    xb_jw_obj_end(&merged);

    xb_arena job_arena;
    xb_arena_init(&job_arena);
    size_t joff = 0;
    xb_json *merged_params =
        xb_json_parse(&job_arena, (const char *)merged.buf.data, merged.buf.len, &joff);

    xb_job job;
    memset(&job, 0, sizeof job);
    job.method = method;
    job.params = merged_params;
    job.deadline_ms = ctx->deadline_ms;
    job.stream = ctx->stream;
    job.on_chunk = ctx->emit;
    job.on_chunk_ud = ctx->emit_ud;
    xb_str_lcpy(job.source_id, x->id, sizeof job.source_id);

    if (!merged_params) {
        XB_WARN("could not re-parse merged params for %.60s (offset %zu)",
                method, joff);
    }
    xb_job_result jr;
    int rc = xb_engine_submit(e, &job, &jr);

    xb_jw_free(&merged);
    xb_arena_destroy(&job_arena);

    if (rc != 0) {
        int code = jr.error_code ? jr.error_code : XB_ERR_ENGINE;
        xb_jw_key(ctx->out, "error");
        xb_jw_str(ctx->out, jr.error_msg ? jr.error_msg : "engine error");
        xb_free(jr.result);
        xb_free(jr.error_msg);
        return code;
    }

    if (!ctx->stream) {
        if (jr.result) {
            /* The engine already produced a JSON value; splice it in as-is. */
            xb_buf_append(&ctx->out->buf, jr.result, strlen(jr.result));
            ctx->out->pending_key = false;
        } else {
            xb_jw_null(ctx->out);
        }
    } else if (jr.result) {
        emit_result_chunks(ctx, jr.result);
    }
    xb_free(jr.result);
    xb_free(jr.error_msg);
    return 0;
}


/* ------------------------------------------------------- admin: handlers -- */

static int h_bridge_handshake(xb_ctx *ctx)
{
    xb_bridge_handshake_json(ctx->bridge, ctx->out);
    return 0;
}

static int h_bridge_ping(xb_ctx *ctx)
{
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_str(ctx->out, "pong", "xbridge");
    xb_jw_kv_int(ctx->out, "ts", xb_now_ms());
    xb_jw_obj_end(ctx->out);
    return 0;
}

static int h_bridge_metrics(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    xb_server_stats ss;
    xb_server_stats_get(b, &ss);
    xb_cache_stats cs;
    xb_cache_get_stats(b->cache, &cs);

    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_int(ctx->out, "uptime_ms", xb_mono_ms() - ss.started_ms);

    xb_jw_key(ctx->out, "server");
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_int(ctx->out, "connections_total", (int64_t)ss.connections_total);
    xb_jw_kv_int(ctx->out, "connections_active", (int64_t)ss.connections_active);
    xb_jw_kv_int(ctx->out, "requests_total", (int64_t)ss.requests_total);
    xb_jw_kv_int(ctx->out, "requests_ok", (int64_t)ss.requests_ok);
    xb_jw_kv_int(ctx->out, "requests_err", (int64_t)ss.requests_err);
    xb_jw_kv_int(ctx->out, "requests_timeout", (int64_t)ss.requests_timeout);
    xb_jw_kv_int(ctx->out, "protocol_errors", (int64_t)ss.protocol_errors);
    xb_jw_kv_int(ctx->out, "frames_in", (int64_t)ss.frames_in);
    xb_jw_kv_int(ctx->out, "frames_out", (int64_t)ss.frames_out);
    xb_jw_kv_int(ctx->out, "bytes_in", (int64_t)ss.bytes_in);
    xb_jw_kv_int(ctx->out, "bytes_out", (int64_t)ss.bytes_out);
    xb_jw_obj_end(ctx->out);

    xb_jw_key(ctx->out, "cache");
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_int(ctx->out, "hits", (int64_t)cs.hits);
    xb_jw_kv_int(ctx->out, "misses", (int64_t)cs.misses);
    xb_jw_kv_int(ctx->out, "puts", (int64_t)cs.puts);
    xb_jw_kv_int(ctx->out, "evictions", (int64_t)cs.evictions);
    xb_jw_kv_int(ctx->out, "expirations", (int64_t)cs.expirations);
    xb_jw_kv_int(ctx->out, "coalesced", (int64_t)cs.coalesced);
    xb_jw_kv_int(ctx->out, "entries", (int64_t)cs.entries);
    xb_jw_kv_int(ctx->out, "bytes", (int64_t)cs.bytes);
    double total = (double)(cs.hits + cs.misses);
    xb_jw_kv_num(ctx->out, "hit_rate", total > 0 ? (double)cs.hits / total : 0.0);
    xb_jw_obj_end(ctx->out);

    xb_jw_key(ctx->out, "engines");
    xb_jw_arr_begin(ctx->out);
    for (size_t i = 0; i < b->engine_count; i++) {
        xb_engine_stats es;
        xb_engine_stats_of(b->engines[i], &es);
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "name", xb_engine_name(b->engines[i]));
        xb_jw_kv_str(ctx->out, "state", xb_engine_state_name(xb_engine_state_of(b->engines[i])));
        xb_jw_kv_int(ctx->out, "jobs_submitted", (int64_t)es.jobs_submitted);
        xb_jw_kv_int(ctx->out, "jobs_ok", (int64_t)es.jobs_ok);
        xb_jw_kv_int(ctx->out, "jobs_err", (int64_t)es.jobs_err);
        xb_jw_kv_int(ctx->out, "jobs_timeout", (int64_t)es.jobs_timeout);
        xb_jw_kv_int(ctx->out, "chunks_forwarded", (int64_t)es.chunks_forwarded);
        xb_jw_kv_int(ctx->out, "worker_restarts", (int64_t)es.worker_restarts);
        xb_jw_kv_int(ctx->out, "live_workers", es.live_workers);
        xb_jw_obj_end(ctx->out);
    }
    xb_jw_arr_end(ctx->out);

    xb_jw_key(ctx->out, "memory");
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_int(ctx->out, "allocated_bytes", (int64_t)xb_total_allocated());
    xb_jw_kv_int(ctx->out, "allocated_blocks", (int64_t)xb_total_blocks());
    xb_jw_kv_int(ctx->out, "rss_bytes", (int64_t)xb_rss_bytes());
    xb_jw_kv_int(ctx->out, "peak_rss_bytes", (int64_t)xb_peak_rss_bytes());
    xb_jw_obj_end(ctx->out);

    xb_jw_obj_end(ctx->out);
    return 0;
}

/* Method introspection: what this daemon can do, and where each method is
 * served from. Hosts use it to build a capability screen and to decide whether
 * to offer a feature without probing it first. */
static int h_bridge_methods(xb_ctx *ctx)
{
    char *list = xb_method_list_json();
    const char *prefix = xb_json_obj_str(ctx->params, "prefix", NULL);
    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *arr = xb_json_parse(&a, list, strlen(list), &off);

    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_int(ctx->out, "count", (int64_t)xb_method_count());
    xb_jw_key(ctx->out, "methods");
    if (arr) {
        xb_jw_arr_begin(ctx->out);
        for (size_t i = 0; i < (size_t)xb_json_arr_len(arr); i++) {
            const xb_json *m = xb_json_arr_at(arr, (int)i);
            const char *name = xb_json_obj_str(m, "name", "");
            if (prefix && prefix[0] && strncmp(name, prefix, strlen(prefix)) != 0) continue;
            xb_jw_value(ctx->out, m);
        }
        xb_jw_arr_end(ctx->out);
    } else {
        xb_jw_raw(ctx->out, "[]");
    }
    xb_jw_obj_end(ctx->out);
    xb_arena_destroy(&a);
    xb_free(list);
    return 0;
}

static int h_bridge_log(xb_ctx *ctx)
{
    int64_t since = xb_json_obj_int(ctx->params, "since_ms", 0);
    int64_t max = xb_json_obj_int(ctx->params, "max", 200);
    char *snapshot = xb_log_snapshot_json(since, (size_t)max);
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_int(ctx->out, "count", (int64_t)xb_log_count());
    xb_jw_key(ctx->out, "entries");
    xb_jw_raw(ctx->out, snapshot);
    xb_jw_obj_end(ctx->out);
    xb_free(snapshot);
    return 0;
}

static int h_bridge_shutdown(xb_ctx *ctx)
{
    if (!ctx->bridge->cfg.allow_shutdown) {
        xb_jw_key(ctx->out, "error");
        xb_jw_str(ctx->out, "shutdown is disabled; start the daemon with --allow-shutdown");
        return XB_ERR_REJECTED;
    }
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_bool(ctx->out, "shutting_down", true);
    xb_jw_obj_end(ctx->out);
    xb_bridge *b = ctx->bridge;
    b->shutdown_requested = true;
    /* The server owns the listener, so the request goes through the public API. */
    xb_server_request_shutdown(b);
    return 0;
}

/* ------------------------------------------------------ format: handlers -- */

static int h_format_list(xb_ctx *ctx)
{
    char *all = xb_registry_json();
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_int(ctx->out, "count", (int64_t)xb_format_count());
    xb_jw_key(ctx->out, "formats");
    xb_jw_raw(ctx->out, all);
    xb_jw_obj_end(ctx->out);
    xb_free(all);
    return 0;
}

static int h_format_detect(xb_ctx *ctx)
{
    const char *path = xb_json_obj_str(ctx->params, "path", NULL);
    if (!path) return XB_ERR_PARAMS;
    xb_detect_result d;
    if (xb_detect_path(path, &d) != 0) {
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "error", "path is not readable");
        xb_jw_obj_end(ctx->out);
        return XB_ERR_NOT_FOUND;
    }
    char *j = xb_detect_json(&d);
    xb_jw_raw(ctx->out, j);
    xb_free(j);
    return 0;
}

/* ------------------------------------------------------- repo: handlers -- */

static int h_repo_list(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    xb_jw_obj_begin(ctx->out);
    xb_jw_key(ctx->out, "repos");
    xb_jw_arr_begin(ctx->out);
    for (size_t i = 0; i < b->repo_count; i++) {
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "id", b->repos[i].id);
        xb_jw_kv_str(ctx->out, "name", b->repos[i].name);
        xb_jw_kv_str(ctx->out, "url", b->repos[i].url);
        xb_jw_kv_str(ctx->out, "manager_id", b->repos[i].manager_id);
        xb_jw_kv_int(ctx->out, "added_ms", b->repos[i].added_ms);
        xb_jw_obj_end(ctx->out);
    }
    xb_jw_arr_end(ctx->out);
    xb_jw_kv_int(ctx->out, "count", (int64_t)b->repo_count);
    xb_jw_obj_end(ctx->out);
    return 0;
}

static int h_repo_add(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *url = xb_json_obj_str(ctx->params, "url", NULL);
    const char *manager = xb_json_obj_str(ctx->params, "manager_id", "aniyomi");
    const char *name = xb_json_obj_str(ctx->params, "name", NULL);
    if (!url) return XB_ERR_PARAMS;
    if (b->repo_count >= XB_MAX_REPOS) return XB_ERR_LIMIT;
    if (!xb_format_by_manager(manager)) return XB_ERR_PARAMS;

    for (size_t i = 0; i < b->repo_count; i++) {
        if (xb_streq(b->repos[i].url, url) && xb_streq(b->repos[i].manager_id, manager)) {
            xb_jw_obj_begin(ctx->out);
            xb_jw_kv_str(ctx->out, "id", b->repos[i].id);
            xb_jw_kv_bool(ctx->out, "already_present", true);
            xb_jw_obj_end(ctx->out);
            state_save(b);
            return 0;
        }
    }
    repo_entry *r = &b->repos[b->repo_count];
    xb_str_lcpy(r->manager_id, manager, sizeof r->manager_id);
    xb_str_lcpy(r->url, url, sizeof r->url);
    if (name) xb_str_lcpy(r->name, name, sizeof r->name);
    else {
        /* Derive a readable name from the URL host. */
        const char *host = strstr(url, "://");
        host = host ? host + 3 : url;
        const char *slash = strchr(host, '/');
        size_t hl = slash ? (size_t)(slash - host) : strlen(host);
        if (hl >= sizeof r->name) hl = sizeof r->name - 1;
        memcpy(r->name, host, hl);
        r->name[hl] = '\0';
    }
    char slug[160];
    slugify(r->name, slug, sizeof slug);
    snprintf(r->id, sizeof r->id, "%s:%s", manager, slug);
    r->added_ms = xb_now_ms();
    b->repo_count++;
    state_save(b);

    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_str(ctx->out, "id", r->id);
    xb_jw_kv_str(ctx->out, "name", r->name);
    xb_jw_kv_str(ctx->out, "url", r->url);
    xb_jw_kv_str(ctx->out, "manager_id", r->manager_id);
    xb_jw_obj_end(ctx->out);
    return 0;
}

static int h_repo_remove(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *id = xb_json_obj_str(ctx->params, "id", NULL);
    const char *url = xb_json_obj_str(ctx->params, "url", NULL);
    for (size_t i = 0; i < b->repo_count; i++) {
        bool match = (id && xb_streq(b->repos[i].id, id)) ||
                     (url && xb_streq(b->repos[i].url, url));
        if (!match) continue;
        char removed_id[192];
        xb_str_lcpy(removed_id, b->repos[i].id, sizeof removed_id);
        b->repos[i] = b->repos[b->repo_count - 1];
        b->repo_count--;
        state_save(b);
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "removed", removed_id);
        xb_jw_obj_end(ctx->out);
        return 0;
    }
    return XB_ERR_NOT_FOUND;
}

static int h_repo_refresh(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *id = xb_json_obj_str(ctx->params, "id", NULL);
    const char *url = xb_json_obj_str(ctx->params, "url", NULL);
    if (!url && id) {
        for (size_t i = 0; i < b->repo_count; i++)
            if (xb_streq(b->repos[i].id, id)) { url = b->repos[i].url; break; }
    }
    if (!url) return XB_ERR_PARAMS;

    xb_http_req req;
    memset(&req, 0, sizeof req);
    req.url = url;
    req.timeout_ms = 20000;
    req.max_bytes = 32u * 1024 * 1024;
    req.user_agent = "xbridge/1.0";
    xb_http_res res;
    if (xb_http_do(&req, NULL, &res) != 0) {
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "error", res.error);
        xb_jw_obj_end(ctx->out);
        return XB_ERR_NETWORK;
    }
    if (res.status >= 400) {
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_int(ctx->out, "status", res.status);
        xb_jw_obj_end(ctx->out);
        xb_http_res_free(&res);
        return XB_ERR_NETWORK;
    }

    xb_arena arena;
    xb_arena_init(&arena);
    size_t off = 0;
    xb_json *doc = xb_json_parse(&arena, res.body ? res.body : "", res.body_len, &off);
    xb_http_res_free(&res);

    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_str(ctx->out, "url", url);
    xb_jw_key(ctx->out, "available");
    xb_jw_arr_begin(ctx->out);
    size_t count = 0;
    if (doc) {
        const xb_json *arr = doc;
        if (doc->type == XB_JOBJ) {
            xb_json *inner = xb_json_get(doc, "extensions");
            if (!inner) inner = xb_json_get(doc, "list");
            if (inner) arr = inner;
        }
        for (size_t i = 0; i < xb_json_arr_len(arr); i++) {
            xb_json *e = xb_json_arr_at(arr, i);
            const char *name = xb_json_obj_str(e, "name", NULL);
            const char *apk = xb_json_obj_str(e, "apk", NULL);
            if (!apk) apk = xb_json_obj_str(e, "url", NULL);
            if (!name && !apk) continue;
            xb_jw_obj_begin(ctx->out);
            xb_jw_kv_str(ctx->out, "name", name ? name : "");
            if (apk) {
                char abs[2048];
                xb_url_resolve(url, apk, abs, sizeof abs);
                xb_jw_kv_str(ctx->out, "url", abs);
            }
            xb_jw_kv_str(ctx->out, "version", xb_json_obj_str(e, "version", ""));
            xb_jw_kv_str(ctx->out, "lang", xb_json_obj_str(e, "lang", ""));
            xb_jw_kv_bool(ctx->out, "nsfw", xb_json_obj_bool(e, "nsfw", false));
            bool installed = false;
            if (name) {
                for (size_t k = 0; k < b->ext_count; k++)
                    if (xb_streq(b->exts[k].name, name)) { installed = true; break; }
            }
            xb_jw_kv_bool(ctx->out, "installed", installed);
            xb_jw_obj_end(ctx->out);
            count++;
            if (count >= 2000) break;
        }
    }
    xb_jw_arr_end(ctx->out);
    xb_jw_kv_int(ctx->out, "count", (int64_t)count);
    xb_jw_obj_end(ctx->out);
    xb_arena_destroy(&arena);
    return 0;
}

/* -------------------------------------------------- extension: handlers -- */

static int h_extension_list(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *manager = xb_json_obj_str(ctx->params, "manager_id", NULL);
    xb_jw_obj_begin(ctx->out);
    xb_jw_key(ctx->out, "extensions");
    xb_jw_arr_begin(ctx->out);
    size_t n = 0;
    for (size_t i = 0; i < b->ext_count; i++) {
        if (manager && !xb_streq(b->exts[i].manager_id, manager)) continue;
        ext_json(ctx->out, &b->exts[i]);
        n++;
    }
    xb_jw_arr_end(ctx->out);
    xb_jw_kv_int(ctx->out, "count", (int64_t)n);
    xb_jw_key(ctx->out, "managers");
    xb_jw_arr_begin(ctx->out);
    for (size_t i = 0; i < xb_format_count(); i++) {
        const xb_format *f = xb_format_at(i);
        if (manager && !xb_strieq(f->manager_id, manager)) continue;
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "manager_id", f->manager_id);
        xb_jw_kv_str(ctx->out, "format_id", f->id);
        xb_jw_kv_str(ctx->out, "engine", f->engine);
        xb_jw_kv_str(ctx->out, "label", f->label);
        xb_jw_obj_end(ctx->out);
    }
    xb_jw_arr_end(ctx->out);
    xb_jw_obj_end(ctx->out);
    return 0;
}

static int h_extension_install(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *path = xb_json_obj_str(ctx->params, "path", NULL);
    const char *url = xb_json_obj_str(ctx->params, "url", NULL);
    const char *manager = xb_json_obj_str(ctx->params, "manager_id", NULL);
    char err[256] = "";

    if (!path && !url) return XB_ERR_PARAMS;

    char downloaded[1400] = "";
    if (!path && url) {
        /* Download first, then install from the local copy: install is
         * idempotent and the download is the only network step. */
        char tmpname[256];
        const char *base = strrchr(url, '/');
        base = base ? base + 1 : "download.bin";
        slugify(base, tmpname, sizeof tmpname);
        snprintf(downloaded, sizeof downloaded, "%s%cdownloads%c%s",
                 b->cfg.data_dir, XB_PATHSEP, XB_PATHSEP, tmpname);
        char dir[1200];
        snprintf(dir, sizeof dir, "%s%cdownloads", b->cfg.data_dir, XB_PATHSEP);
        mkdir_p(dir);

        xb_http_req req;
        memset(&req, 0, sizeof req);
        req.url = url;
        req.timeout_ms = 60000;
        req.max_bytes = 128u * 1024 * 1024;
        req.user_agent = "xbridge/1.0";
        xb_http_res res;
        if (xb_http_do(&req, NULL, &res) != 0) {
            snprintf(err, sizeof err, "download failed: %.180s", res.error);
            return XB_ERR_NETWORK;
        }
        if (res.status >= 400) {
            snprintf(err, sizeof err, "download returned HTTP %d", res.status);
            xb_http_res_free(&res);
            return XB_ERR_NETWORK;
        }
        write_file_atomic(downloaded, res.body ? res.body : "", res.body_len);
        xb_http_res_free(&res);
        path = downloaded;
    }

    ext_entry installed;
    if (install_path(b, path, manager, url, err, sizeof err, &installed) != 0) {
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "error", err);
        xb_jw_obj_end(ctx->out);
        return XB_ERR_REJECTED;
    }
    ext_json(ctx->out, &installed);
    return 0;
}

static int h_extension_uninstall(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *id = xb_json_obj_str(ctx->params, "id", NULL);
    char err[256];
    if (!id) return XB_ERR_PARAMS;
    if (uninstall(b, id, err, sizeof err) != 0) {
        xb_jw_obj_begin(ctx->out);
        xb_jw_kv_str(ctx->out, "error", err);
        xb_jw_obj_end(ctx->out);
        return XB_ERR_NOT_FOUND;
    }
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_str(ctx->out, "removed", id);
    xb_jw_obj_end(ctx->out);
    return 0;
}

static int h_extension_info(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *id = xb_json_obj_str(ctx->params, "id", NULL);
    if (!id) return XB_ERR_PARAMS;
    ext_entry *x = find_ext(b, id);
    if (!x) return XB_ERR_NOT_FOUND;
    ext_json(ctx->out, x);
    if (xb_streq(x->format_id, "legado")) {
        char *desc = xb_rule_engine_describe(x->id);
        xb_jw_key(ctx->out, "rule");
        xb_jw_raw(ctx->out, desc);
        xb_free(desc);
    }
    return 0;
}

static int h_extension_update(xb_ctx *ctx)
{
    /* Updates need a repository index; without one the honest answer is that
     * nothing is pending, not a fabricated success. */
    xb_bridge *b = ctx->bridge;
    (void)b;
    xb_jw_obj_begin(ctx->out);
    xb_jw_key(ctx->out, "updated");
    xb_jw_arr_begin(ctx->out);
    xb_jw_arr_end(ctx->out);
    xb_jw_kv_int(ctx->out, "count", 0);
    xb_jw_kv_str(ctx->out, "note",
                 "no repository index is configured; add one with repo.add and refresh it first");
    xb_jw_obj_end(ctx->out);
    return 0;
}

/* ------------------------------------------------- torrserver: handlers -- */

static int h_torrserver_status(xb_ctx *ctx)
{
    xb_bridge *b = ctx->bridge;
    const char *port = getenv("XBRIDGE_TORRSERVER_PORT");
    xb_jw_obj_begin(ctx->out);
    xb_jw_kv_bool(ctx->out, "installed", false);
    xb_jw_kv_bool(ctx->out, "running", false);
    xb_jw_kv_str(ctx->out, "note",
                 "the torrent addon is provided by a native worker; set XBRIDGE_TORRSERVER_BIN "
                 "to point at one and restart the daemon");
    if (port) xb_jw_kv_str(ctx->out, "port", port);
    xb_jw_obj_end(ctx->out);
    (void)b;
    return 0;
}

/* ----------------------------------------------------------- registration -- */

int xb_register_builtin_methods(void)
{
    /* Registration is idempotent-ish: it runs once per process. */
    static bool done = false;
    if (done) return 0;
    done = true;

#define REG(name, fn, eng, streamable, cacheable, summary)         \
    do {                                                            \
        xb_method_meta m = { name, eng, streamable, cacheable, summary }; \
        xb_register_method_meta(&m);                                \
        xb_register_method(name, fn);                               \
    } while (0)

    REG("bridge.handshake", h_bridge_handshake, "", false, false, "capabilities");
    REG("bridge.ping",      h_bridge_ping,      "", false, false, "liveness");
    REG("bridge.metrics",   h_bridge_metrics,   "", false, false, "counters and cache stats");
    REG("bridge.methods",   h_bridge_methods,   "", false, false, "method introspection");
    REG("bridge.log",       h_bridge_log,       "", false, false, "recent log lines");
    REG("bridge.shutdown",  h_bridge_shutdown,  "", false, false, "graceful stop");

    REG("format.list",      h_format_list,      "", false, true,  "supported extension formats");
    REG("format.detect",    h_format_detect,    "", false, true,  "classify an artifact");

    REG("repo.list",        h_repo_list,        "", false, false, "configured repositories");
    REG("repo.add",         h_repo_add,         "", false, false, "add a repository");
    REG("repo.remove",      h_repo_remove,      "", false, false, "remove a repository");
    REG("repo.refresh",     h_repo_refresh,     "", false, false, "fetch a repository index");

    REG("extension.list",   h_extension_list,   "", false, false, "installed extensions");
    REG("extension.install",h_extension_install,"", false, false, "install from path or url");
    REG("extension.uninstall", h_extension_uninstall, "", false, false, "remove an extension");
    REG("extension.info",   h_extension_info,   "", false, false, "manifest of one extension");
    REG("extension.update", h_extension_update, "", false, false, "update installed extensions");

    REG("torrserver.status", h_torrserver_status, "native", false, false, "torrent addon status");

    /* The unified source surface shares a single handler; metadata is registered
     * per method so routing and streaming flags stay declarative. */
    static const char *SOURCE_METHODS[] = {
        "source.methods", "source.info",
        "source.getPopular", "source.getLatestUpdates", "source.search",
        "source.getDetail", "source.getVideoList", "source.getVideoListStream",
        "source.getPageList", "source.getNovelContent",
        "source.getPreference", "source.setPreference", NULL
    };
    for (int i = 0; SOURCE_METHODS[i]; i++) {
        bool streamable = xb_streq(SOURCE_METHODS[i], "source.getVideoListStream");
        xb_method_meta m = { SOURCE_METHODS[i], "", streamable, false, "unified source method" };
        xb_register_method_meta(&m);
        xb_register_method(SOURCE_METHODS[i], xb_handler_source_family);
    }
#undef REG
    return 0;
}
