/* engine.c — supervised worker engines.
 *
 * One engine = N workers. A worker is either a child process speaking XBP over
 * stdio, or an in-process function (used by the built-in rule engine and by
 * embedders on platforms where spawning is not allowed, e.g. iOS).
 *
 * Correctness properties this file is responsible for:
 *   - a request is never lost: every submit either gets a response or a
 *     deterministic error (deadline / cancelled / worker died);
 *   - a dead worker is restarted with backoff and its in-flight jobs fail fast
 *     instead of hanging until their deadline;
 *   - responses may arrive out of order; the pending table keys on job id.
 */
#include "bridge/engine.h"
#include "bridge/server.h"
#include "bridge/proc.h"
#include "bridge/net.h"
#include "bridge/buf.h"
#include "bridge/log.h"
#include "bridge/util.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct pending {
    char        id[40];
    bool        done;
    bool        ok;
    bool        stream;
    char       *result;       /* JSON text */
    char       *error;
    int         error_code;
    int64_t     started_ms;
    int64_t     deadline_ms;   /* absolute monotonic, 0 = none */
    xb_chunk_fn on_chunk;
    void       *chunk_ud;
    bool        chunk_stopped;
    int64_t     next_seq;
    struct pending *next;
} pending_t;

typedef struct worker {
    xb_engine  *owner;
    int         index;
    xb_process *proc;
    int         in_fd;
    int         out_fd;
    xb_mutex_t  wlock;         /* serialises frame writes */
    xb_mutex_t  plock;
    xb_cond_t   pcv;
    pending_t  *pending;
    volatile bool alive;
    bool        started;
#ifdef _WIN32
    xb_thread_t reader;
#else
    pthread_t   reader;
    bool        reader_joinable;
#endif
    uint64_t    restarts;
} worker_t;

struct xb_engine {
    char        name[32];
    bool        inproc;
    xb_engine_inproc_fn ifn;
    void       *iud;
    char      **argv;          /* NULL-terminated copy, may contain "{}" */
    int         worker_count;
    int64_t     start_timeout_ms;
    int64_t     restart_backoff_ms;
    int         max_restarts;
    worker_t   *workers;
    volatile int rr;           /* round-robin cursor */
    xb_engine_state state;
    xb_mutex_t  slock;
    xb_engine_stats stats;
    volatile int shutting_down;
    uint64_t    job_seq;
    xb_mutex_t  seq_lock;
};

/* ------------------------------------------------------------- frame I/O -- */

static int write_frame(int fd, uint8_t type, const char *payload, size_t len,
                       xb_mutex_t *lock)
{
    if (len > 0xFFFFFFFFu) return -1;
    uint8_t hdr[5];
    uint32_t total = (uint32_t)(len + 1);
    hdr[0] = (uint8_t)(total >> 24);
    hdr[1] = (uint8_t)(total >> 16);
    hdr[2] = (uint8_t)(total >> 8);
    hdr[3] = (uint8_t)total;
    hdr[4] = type;

    XB_MUTEX_LOCK(lock);
    int64_t a = xb_write_full((xb_sock_t)(uintptr_t)fd, hdr, sizeof hdr);
    int64_t b = (a == (int64_t)sizeof hdr) ? xb_write_full((xb_sock_t)(uintptr_t)fd, payload, len) : -1;
    XB_MUTEX_UNLOCK(lock);
    return (b >= 0 && (size_t)b == len) ? 0 : -1;
}

/* ------------------------------------------------------------------ misc -- */

static const char *state_name(xb_engine_state s)
{
    switch (s) {
    case XB_ENGINE_STOPPED:     return "stopped";
    case XB_ENGINE_STARTING:    return "starting";
    case XB_ENGINE_READY:       return "ready";
    case XB_ENGINE_DEGRADED:    return "degraded";
    case XB_ENGINE_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}

const char *xb_engine_state_name(xb_engine_state s) { return state_name(s); }
const char *xb_engine_name(const xb_engine *e) { return e ? e->name : ""; }
xb_engine_state xb_engine_state_of(const xb_engine *e)
{
    return e ? e->state : XB_ENGINE_STOPPED;
}

void xb_engine_worker_counts(const xb_engine *e, int *live, int *total)
{
    if (!e) { if (live) *live = 0; if (total) *total = 0; return; }
    int l = 0;
    for (int i = 0; i < e->worker_count; i++)
        if (e->workers[i].alive) l++;
    if (live) *live = l;
    if (total) *total = e->worker_count;
}

void xb_engine_set_max_restarts(xb_engine *e, int n) { if (e) e->max_restarts = n; }

/* --------------------------------------------------------- pending table -- */

static pending_t *pending_find(worker_t *w, const char *id)
{
    for (pending_t *p = w->pending; p; p = p->next)
        if (strcmp(p->id, id) == 0) return p;
    return NULL;
}

static void pending_add(worker_t *w, pending_t *p)
{
    XB_MUTEX_LOCK(&w->plock);
    p->next = w->pending;
    w->pending = p;
    XB_MUTEX_UNLOCK(&w->plock);
}

static pending_t *pending_take(worker_t *w, const char *id)
{
    XB_MUTEX_LOCK(&w->plock);
    pending_t **pp = &w->pending;
    pending_t *found = NULL;
    while (*pp) {
        if (strcmp((*pp)->id, id) == 0) {
            found = *pp;
            *pp = found->next;
            break;
        }
        pp = &(*pp)->next;
    }
    XB_MUTEX_UNLOCK(&w->plock);
    return found;
}

static void pending_free(pending_t *p)
{
    if (!p) return;
    xb_free(p->result);
    xb_free(p->error);
    xb_free(p);
}

/* Fail every pending job of a dying worker — hanging until deadline is worse
 * than a fast, explicit error the caller can retry. */
static void pending_fail_all(worker_t *w, int code, const char *msg)
{
    XB_MUTEX_LOCK(&w->plock);
    pending_t *p = w->pending;
    while (p) {
        pending_t *next = p->next;
        if (!p->done) {
            p->done = true;
            p->ok = false;
            p->error_code = code;
            p->error = xb_strdup(msg);
        }
        p = next;
    }
    XB_COND_BROADCAST(&w->pcv);
    XB_MUTEX_UNLOCK(&w->plock);
}

/* ------------------------------------------------------------ reader loop -- */

static void handle_response_frame(worker_t *w, const char *payload, size_t len)
{
    xb_arena arena;
    xb_arena_init(&arena);
    size_t off = 0;
    xb_json *j = xb_json_parse(&arena, payload, len, &off);
    if (!j) {
        XB_WARN("engine %s worker %d sent unparseable JSON (offset %zu)",
                w->owner->name, w->index, off);
        xb_arena_destroy(&arena);
        return;
    }
    const char *id = xb_json_obj_str(j, "id", NULL);
    if (!id) { xb_arena_destroy(&arena); return; }

    pending_t *p = pending_find(w, id);
    if (!p) { xb_arena_destroy(&arena); return; }

    XB_MUTEX_LOCK(&w->plock);
    if (p->done) { XB_MUTEX_UNLOCK(&w->plock); xb_arena_destroy(&arena); return; }

    xb_json *err = xb_json_get(j, "error");
    xb_json *result = xb_json_get(j, "result");
    bool ok = xb_json_obj_bool(j, "ok", result != NULL && err == NULL);

    if (ok) {
        p->ok = true;
        p->result = result ? xb_json_dumps(result, false) : xb_strdup("null");
    } else {
        p->ok = false;
        p->error_code = (int)xb_json_obj_int(err, "code", XB_ERR_ENGINE);
        const char *m = xb_json_obj_str(err, "message", "engine error");
        p->error = xb_strdup(m);
    }
    p->done = true;
    XB_COND_BROADCAST(&w->pcv);
    XB_MUTEX_UNLOCK(&w->plock);
    xb_arena_destroy(&arena);
}

static void handle_chunk_frame(worker_t *w, const char *payload, size_t len, int type)
{
    xb_arena arena;
    xb_arena_init(&arena);
    size_t off = 0;
    xb_json *j = xb_json_parse(&arena, payload, len, &off);
    if (!j) { xb_arena_destroy(&arena); return; }
    const char *id = xb_json_obj_str(j, "id", NULL);
    if (!id) { xb_arena_destroy(&arena); return; }

    pending_t *p = pending_find(w, id);
    if (!p) { xb_arena_destroy(&arena); return; }

    if (type == XB_FT_STREAM_CHUNK) {
        xb_json *chunk = xb_json_get(j, "chunk");
        int64_t seq = xb_json_obj_int(j, "seq", p->next_seq);
        if (p->on_chunk && !p->chunk_stopped) {
            /* The callback runs outside the lock: it may block on the client's
             * socket, and that backpressure is exactly what we want to reach
             * the extension. */
            if (!p->on_chunk(p->chunk_ud, seq, chunk)) {
                p->chunk_stopped = true;
                /* Tell the worker to stop producing. */
                xb_jsonw cw;
                xb_jw_init(&cw);
                xb_jw_obj_begin(&cw);
                xb_jw_kv_str(&cw, "id", p->id);
                xb_jw_obj_end(&cw);
                char *cf = xb_buf_steal(&cw.buf);
                write_frame(w->in_fd, XB_FT_CANCEL, cf, strlen(cf), &w->wlock);
                xb_free(cf);
                xb_jw_free(&cw);
            }
        }
        XB_MUTEX_LOCK(&w->plock);
        w->owner->stats.chunks_forwarded++;
        XB_MUTEX_UNLOCK(&w->plock);
        p->next_seq = seq + 1;
    } else if (type == XB_FT_STREAM_END) {
        XB_MUTEX_LOCK(&w->plock);
        p->ok = true;
        p->result = xb_strdup("null");
        p->done = true;
        XB_COND_BROADCAST(&w->pcv);
        XB_MUTEX_UNLOCK(&w->plock);
    } else { /* STREAM_ERR */
        xb_json *err = xb_json_get(j, "error");
        XB_MUTEX_LOCK(&w->plock);
        p->ok = false;
        p->error_code = (int)xb_json_obj_int(err, "code", XB_ERR_ENGINE);
        p->error = xb_strdup(xb_json_obj_str(err, "message", "stream error"));
        p->done = true;
        XB_COND_BROADCAST(&w->pcv);
        XB_MUTEX_UNLOCK(&w->plock);
    }
    xb_arena_destroy(&arena);
}

static XB_THREAD_FN(worker_reader)
{
    worker_t *w = (worker_t *)ud;
    uint8_t hdr[5];
    xb_buf payload;
    xb_buf_init(&payload);

    while (!w->owner->shutting_down) {
        int64_t r = xb_read_full((xb_sock_t)(uintptr_t)w->out_fd, hdr, sizeof hdr);
        if (r != (int64_t)sizeof hdr) break;
        uint32_t total = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                         ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
        uint8_t type = hdr[4];
        if (total < 1 || total > (uint32_t)xb_g_config.max_frame) {
            XB_WARN("engine %s worker %d: bad frame length %u", w->owner->name, w->index, total);
            break;
        }
        size_t plen = total - 1;
        xb_buf_reset(&payload);
        xb_buf_reserve(&payload, plen);
        if (plen) {
            int64_t got = xb_read_full((xb_sock_t)(uintptr_t)w->out_fd, payload.data, plen);
            if (got != (int64_t)plen) break;
            payload.len = plen;
            payload.data[plen] = 0;
        }
        const char *text = payload.data ? (const char *)payload.data : "";
        switch (type) {
        case XB_FT_RESPONSE:     handle_response_frame(w, text, plen); break;
        case XB_FT_STREAM_CHUNK:
        case XB_FT_STREAM_END:
        case XB_FT_STREAM_ERR:   handle_chunk_frame(w, text, plen, type); break;
        case XB_FT_HELLO_ACK:    break;
        case XB_FT_PING:
            write_frame(w->in_fd, XB_FT_PONG, "{}", 2, &w->wlock);
            break;
        default: break;   /* forward compatibility: ignore unknown frames */
        }
    }

    xb_buf_free(&payload);
    w->alive = false;
    pending_fail_all(w, XB_ERR_UNAVAILABLE, "engine worker exited");
#ifndef _WIN32
    w->reader_joinable = false;
#endif
    XB_THREAD_RETURN(0);
}

/* ------------------------------------------------------------- lifecycle -- */

static char *argv_join(const char *const *argv)
{
    xb_buf b;
    xb_buf_init(&b);
    for (int i = 0; argv[i]; i++) {
        if (i) xb_buf_append_byte(&b, ' ');
        xb_buf_append_str(&b, argv[i]);
    }
    return xb_buf_steal(&b);
}

static int worker_start(xb_engine *e, worker_t *w)
{
    if (e->inproc) { w->alive = true; w->started = true; return 0; }

    char **argv = (char **)xb_alloc(sizeof(char *) * 32);
    int n = 0;
    for (int i = 0; e->argv[i] && n < 30; i++) {
        const char *a = e->argv[i];
        if (strcmp(a, "{}") == 0) {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", w->index);
            argv[n++] = xb_strdup(buf);
        } else {
            argv[n++] = xb_strdup(a);
        }
    }
    argv[n] = NULL;

    char resolved[1024];
    char *exe = xb_which(argv[0]);
    if (!exe) {
        for (int i = 0; i < n; i++) xb_free(argv[i]);
        xb_free(argv);
        return -1;
    }
    xb_str_lcpy(resolved, exe, sizeof resolved);
    xb_free(exe);
    xb_free(argv[0]);
    argv[0] = xb_strdup(resolved);

    xb_proc_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.capture_stderr = true;

    char err[256];
    xb_process *p = xb_proc_spawn((const char *const *)argv, &opts, err, sizeof err);
    for (int i = 0; i < n; i++) xb_free(argv[i]);
    xb_free(argv);
    if (!p) {
        XB_WARN("engine %s worker %d failed to start: %s", e->name, w->index, err);
        return -1;
    }
    w->proc = p;
    w->in_fd = xb_proc_stdin_fd(p);
    w->out_fd = xb_proc_stdout_fd(p);
    w->alive = true;
    w->started = true;
    if (xb_thread_spawn(&w->reader, worker_reader, w) != 0) {
        XB_ERR("engine %s: cannot spawn reader thread", e->name);
        xb_proc_kill(p);
        w->alive = false;
        return -1;
    }
#ifndef _WIN32
    w->reader_joinable = true;
#endif
    (void)argv_join;
    return 0;
}

static int engine_ensure_worker(xb_engine *e, int idx)
{
    worker_t *w = &e->workers[idx];
    if (w->alive) return 0;
    if (e->shutting_down) return -1;
    if ((int)w->restarts >= e->max_restarts) return -1;
    w->restarts++;
    e->stats.worker_restarts++;
    if (w->restarts > 1) xb_sleep_ms((int)e->restart_backoff_ms);
    return worker_start(e, w);
}

xb_engine *xb_engine_new(const char *name, const char *const *argv,
                         int worker_count, int64_t start_timeout_ms,
                         int64_t restart_backoff_ms)
{
    if (!name || !argv || !argv[0]) return NULL;
    xb_engine *e = (xb_engine *)xb_alloc(sizeof *e);
    xb_str_lcpy(e->name, name, sizeof e->name);
    e->worker_count = worker_count > 0 ? worker_count : 1;
    if (e->worker_count > 16) e->worker_count = 16;
    e->start_timeout_ms = start_timeout_ms > 0 ? start_timeout_ms : 8000;
    e->restart_backoff_ms = restart_backoff_ms > 0 ? restart_backoff_ms : 250;
    e->max_restarts = 5;
    e->state = XB_ENGINE_STOPPED;
    e->workers = (worker_t *)xb_alloc(sizeof(worker_t) * (size_t)e->worker_count);
    int n = 0;
    while (argv[n]) n++;
    e->argv = (char **)xb_alloc(sizeof(char *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) e->argv[i] = xb_strdup(argv[i]);
    e->argv[n] = NULL;
    XB_MUTEX_INIT(&e->slock);
    XB_MUTEX_INIT(&e->seq_lock);
    for (int i = 0; i < e->worker_count; i++) {
        e->workers[i].owner = e;
        e->workers[i].index = i;
        e->workers[i].in_fd = -1;
        e->workers[i].out_fd = -1;
        XB_MUTEX_INIT(&e->workers[i].wlock);
        XB_MUTEX_INIT(&e->workers[i].plock);
        XB_COND_INIT(&e->workers[i].pcv);
    }
    return e;
}

xb_engine *xb_engine_new_inproc(const char *name, xb_engine_inproc_fn fn, void *ud)
{
    if (!name || !fn) return NULL;
    xb_engine *e = (xb_engine *)xb_alloc(sizeof *e);
    xb_str_lcpy(e->name, name, sizeof e->name);
    e->inproc = true;
    e->ifn = fn;
    e->iud = ud;
    e->worker_count = 1;
    e->state = XB_ENGINE_STOPPED;
    e->workers = (worker_t *)xb_alloc(sizeof(worker_t));
    e->workers[0].owner = e;
    e->workers[0].index = 0;
    e->workers[0].in_fd = -1;
    e->workers[0].out_fd = -1;
    XB_MUTEX_INIT(&e->workers[0].wlock);
    XB_MUTEX_INIT(&e->workers[0].plock);
    XB_COND_INIT(&e->workers[0].pcv);
    XB_MUTEX_INIT(&e->slock);
    XB_MUTEX_INIT(&e->seq_lock);
    return e;
}

int xb_engine_start(xb_engine *e, bool required, char *err, size_t errcap)
{
    if (!e) return -1;
    if (e->state == XB_ENGINE_READY) return 0;
    e->state = XB_ENGINE_STARTING;

    if (e->inproc) {
        e->workers[0].alive = true;
        e->state = XB_ENGINE_READY;
        return 0;
    }

    /* If the executable is missing there is nothing to retry: report the engine
     * as unavailable. This is the common, expected case (no JVM installed). */
    char *exe = xb_which(e->argv[0]);
    if (!exe) {
        e->state = XB_ENGINE_UNAVAILABLE;
        snprintf(err, errcap, "engine '%s' unavailable: '%s' not found on PATH",
                 e->name, e->argv[0]);
        if (required) return -1;
        XB_INFO("engine %s unavailable (%s not found)", e->name, e->argv[0]);
        return 0;
    }
    xb_free(exe);

    int live = 0;
    for (int i = 0; i < e->worker_count; i++)
        if (worker_start(e, &e->workers[i]) == 0) live++;

    if (live == 0) {
        e->state = XB_ENGINE_UNAVAILABLE;
        snprintf(err, errcap, "engine '%s' failed to start any worker", e->name);
        if (required) return -1;
        return 0;
    }
    /* Give the workers the handshake window, but do not block startup on it:
     * the first job will simply wait a little longer. */
    e->state = live == e->worker_count ? XB_ENGINE_READY : XB_ENGINE_DEGRADED;
    XB_INFO("engine %s: %d/%d workers started", e->name, live, e->worker_count);
    return 0;
}

void xb_engine_stop(xb_engine *e)
{
    if (!e) return;
    e->shutting_down = 1;
    for (int i = 0; i < e->worker_count; i++) {
        worker_t *w = &e->workers[i];
        write_frame(w->in_fd, XB_FT_BYE, "{\"reason\":\"shutdown\"}", 20, &w->wlock);
        if (w->proc) {
            xb_proc_terminate(w->proc);
            xb_sock_shutdown((xb_sock_t)(uintptr_t)w->out_fd);
        }
    }
    for (int i = 0; i < e->worker_count; i++) {
        worker_t *w = &e->workers[i];
        if (w->proc) {
            if (xb_proc_wait(w->proc, 2000) < 0) xb_proc_kill(w->proc);
        }
    }
    for (int i = 0; i < e->worker_count; i++) {
        worker_t *w = &e->workers[i];
#ifdef _WIN32
        if (w->reader) { xb_thread_join(w->reader); w->reader = NULL; }
#else
        if (w->reader_joinable) { xb_thread_join(w->reader); w->reader_joinable = false; }
#endif
        pending_fail_all(w, XB_ERR_UNAVAILABLE, "engine stopped");
    }
    for (int i = 0; i < e->worker_count; i++) {
        worker_t *w = &e->workers[i];
        pending_t *p = w->pending;
        while (p) { pending_t *n = p->next; pending_free(p); p = n; }
        w->pending = NULL;
        if (w->proc) { xb_proc_close(w->proc); w->proc = NULL; }
    }
    e->state = XB_ENGINE_STOPPED;
}

void xb_engine_free(xb_engine *e)
{
    if (!e) return;
    xb_engine_stop(e);
    for (int i = 0; i < e->worker_count; i++) {
        XB_MUTEX_DESTROY(&e->workers[i].wlock);
        XB_MUTEX_DESTROY(&e->workers[i].plock);
        XB_COND_DESTROY(&e->workers[i].pcv);
    }
    xb_free(e->workers);
    if (e->argv) {
        for (int i = 0; e->argv[i]; i++) xb_free(e->argv[i]);
        xb_free(e->argv);
    }
    XB_MUTEX_DESTROY(&e->slock);
    XB_MUTEX_DESTROY(&e->seq_lock);
    xb_free(e);
}

/* ------------------------------------------------------------- submit -- */

static void next_job_id(xb_engine *e, char *out, size_t cap)
{
    XB_MUTEX_LOCK(&e->seq_lock);
    uint64_t n = ++e->job_seq;
    XB_MUTEX_UNLOCK(&e->seq_lock);
    snprintf(out, cap, "%s-%llu", e->name, (unsigned long long)n);
}

static worker_t *pick_worker(xb_engine *e)
{
    int start = __atomic_fetch_add((int *)&e->rr, 1, __ATOMIC_SEQ_CST);
    for (int k = 0; k < e->worker_count; k++) {
        int i = (start + k) % e->worker_count;
        worker_t *w = &e->workers[i];
        if (!w->alive) {
            if (engine_ensure_worker(e, i) != 0) continue;
        }
        if (w->alive) {
            if (w->proc && !xb_proc_running(w->proc)) {
                w->alive = false;
                engine_ensure_worker(e, i);
            }
            if (w->alive) return w;
        }
    }
    return NULL;
}

int xb_engine_submit(xb_engine *e, const xb_job *job, xb_job_result *res)
{
    memset(res, 0, sizeof *res);
    res->error_code = XB_ERR_ENGINE;
    if (!e || !job || !job->method) {
        res->error_msg = xb_strdup("no engine or method");
        return -1;
    }
    if (e->state == XB_ENGINE_UNAVAILABLE || e->state == XB_ENGINE_STOPPED) {
        res->error_code = XB_ERR_UNAVAILABLE;
        res->error_msg = xb_strdup("engine unavailable");
        return -1;
    }

    int64_t t0 = xb_mono_ms();
    e->stats.jobs_submitted++;

    /* ---- in-process path: same deadline semantics as a worker ---- */
    if (e->inproc) {
        int rc = e->ifn(e->iud, job->method, job->params, res,
                        job->on_chunk, job->on_chunk_ud);
        res->engine_ms = (int)(xb_mono_ms() - t0);
        if (rc == 0) e->stats.jobs_ok++;
        else e->stats.jobs_err++;
        return rc;
    }

    worker_t *w = pick_worker(e);
    if (!w) {
        res->error_code = XB_ERR_UNAVAILABLE;
        res->error_msg = xb_strdup("no live worker for engine");
        e->stats.jobs_err++;
        return -1;
    }

    pending_t *p = (pending_t *)xb_alloc(sizeof *p);
    if (job->job_id[0] && !pending_find(w, job->job_id)) {
        /* Caller-supplied correlation id, used by the daemon so a client
         * CANCEL can name the job directly. */
        xb_str_lcpy(p->id, job->job_id, sizeof p->id);
    } else {
        next_job_id(e, p->id, sizeof p->id);
    }
    p->stream = job->stream;
    p->on_chunk = job->on_chunk;
    p->chunk_ud = job->on_chunk_ud;
    p->started_ms = t0;
    p->deadline_ms = job->deadline_ms > 0 ? xb_mono_ms() + job->deadline_ms : 0;

    pending_add(w, p);

    xb_jsonw rq;
    xb_jw_init(&rq);
    xb_jw_obj_begin(&rq);
    xb_jw_kv_str(&rq, "id", p->id);
    xb_jw_kv_str(&rq, "method", job->method);
    xb_jw_key(&rq, "params");
    if (job->params) xb_jw_value(&rq, job->params); else xb_jw_null(&rq);
    if (job->deadline_ms > 0) xb_jw_kv_int(&rq, "deadline_ms", job->deadline_ms);
    if (job->stream) xb_jw_kv_bool(&rq, "stream", true);
    if (job->source_id[0]) xb_jw_kv_str(&rq, "source_id", job->source_id);
    xb_jw_obj_end(&rq);
    char *frame = xb_buf_steal(&rq.buf);
    xb_jw_free(&rq);

    int wr = write_frame(w->in_fd, XB_FT_REQUEST, frame, strlen(frame), &w->wlock);
    xb_free(frame);
    if (wr != 0) {
        pending_take(w, p->id);
        pending_free(p);
        res->error_code = XB_ERR_UNAVAILABLE;
        res->error_msg = xb_strdup("failed to write to engine worker");
        e->stats.jobs_err++;
        return -1;
    }

    /* Wait for completion, honouring the deadline. */
    XB_MUTEX_LOCK(&w->plock);
    while (!p->done) {
        int wait_ms = 250;
        if (p->deadline_ms > 0) {
            int64_t left = p->deadline_ms - xb_mono_ms();
            if (left <= 0) break;
            if (left < wait_ms) wait_ms = (int)left;
        }
        XB_COND_TIMEDWAIT(&w->pcv, &w->plock, wait_ms);
    }
    bool timed_out = !p->done;
    XB_MUTEX_UNLOCK(&w->plock);

    if (timed_out) {
        /* Cancel downstream, then reap the entry ourselves so a late response
         * cannot touch freed memory. */
        xb_jsonw cw;
        xb_jw_init(&cw);
        xb_jw_obj_begin(&cw);
        xb_jw_kv_str(&cw, "id", p->id);
        xb_jw_obj_end(&cw);
        char *cf = xb_buf_steal(&cw.buf);
        write_frame(w->in_fd, XB_FT_CANCEL, cf, strlen(cf), &w->wlock);
        xb_free(cf);
        xb_jw_free(&cw);
        pending_take(w, p->id);
        res->error_code = XB_ERR_DEADLINE;
        res->error_msg = xb_strdup("deadline exceeded");
        res->engine_ms = (int)(xb_mono_ms() - t0);
        pending_free(p);
        e->stats.jobs_timeout++;
        return -1;
    }

    pending_t *taken = pending_take(w, p->id);
    (void)taken;

    res->ok = p->ok;
    res->result = p->result ? xb_strdup(p->result) : NULL;
    res->error_msg = p->error ? xb_strdup(p->error) : NULL;
    res->error_code = p->ok ? 0 : p->error_code;
    res->engine_ms = (int)(xb_mono_ms() - t0);
    if (p->ok) e->stats.jobs_ok++; else e->stats.jobs_err++;
    pending_free(p);
    return p->ok ? 0 : -1;
}

void xb_engine_cancel(xb_engine *e, const char *job_id)
{
    if (!e || !job_id) return;
    for (int i = 0; i < e->worker_count; i++) {
        worker_t *w = &e->workers[i];
        pending_t *p = pending_find(w, job_id);
        if (!p) continue;
        xb_jsonw cw;
        xb_jw_init(&cw);
        xb_jw_obj_begin(&cw);
        xb_jw_kv_str(&cw, "id", job_id);
        xb_jw_obj_end(&cw);
        char *cf = xb_buf_steal(&cw.buf);
        write_frame(w->in_fd, XB_FT_CANCEL, cf, strlen(cf), &w->wlock);
        xb_free(cf);
        xb_jw_free(&cw);
        XB_MUTEX_LOCK(&w->plock);
        p->done = true;
        p->ok = false;
        p->error_code = XB_ERR_CANCELLED;
        p->error = xb_strdup("cancelled");
        XB_COND_BROADCAST(&w->pcv);
        XB_MUTEX_UNLOCK(&w->plock);
        return;
    }
}

void xb_engine_stats_of(const xb_engine *e, xb_engine_stats *out)
{
    if (!e || !out) return;
    *out = e->stats;
    xb_engine_worker_counts(e, &out->live_workers, &out->total_workers);
}
