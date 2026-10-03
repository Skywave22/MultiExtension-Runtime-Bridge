/* log.c — rate-limited structured logging into stderr and a ring buffer. */
#include "bridge/log.h"
#include "bridge/util.h"
#include "bridge/json.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define XB_LOG_RING_DEFAULT 2048

static struct {
    xb_log_level level;
    bool         to_stderr;
    bool         ring_on;
    xb_log_record *ring;
    size_t       ring_cap;
    size_t       ring_len;   /* total ever pushed */
    xb_mutex_t   m;
    bool         inited;
} L;

static const char *level_name(xb_log_level l)
{
    switch (l) {
    case XB_LOG_TRACE: return "trace";
    case XB_LOG_DEBUG: return "debug";
    case XB_LOG_INFO:  return "info";
    case XB_LOG_WARN:  return "warn";
    case XB_LOG_ERROR: return "error";
    case XB_LOG_FATAL: return "fatal";
    }
    return "info";
}

void xb_log_init(xb_log_level min_level, bool to_stderr, bool ring_enabled)
{
    if (L.inited) { L.level = min_level; return; }
    L.level = min_level;
    L.to_stderr = to_stderr;
    L.ring_on = ring_enabled;
    L.ring_cap = XB_LOG_RING_DEFAULT;
    L.ring = (xb_log_record *)xb_alloc(sizeof(xb_log_record) * L.ring_cap);
    L.ring_len = 0;
    XB_MUTEX_INIT(&L.m);
    L.inited = true;
}

void xb_log_shutdown(void)
{
    if (!L.inited) return;
    for (size_t i = 0; i < L.ring_cap; i++) {
        if (L.ring[i].msg) xb_free(L.ring[i].msg);
    }
    xb_free(L.ring);
    L.ring = NULL;
    XB_MUTEX_DESTROY(&L.m);
    L.inited = false;
}

void xb_log_set_level(xb_log_level lvl) { L.level = lvl; }
xb_log_level xb_log_get_level(void) { return L.level; }

bool xb_log_parse_level(const char *s, xb_log_level *out)
{
    static const struct { const char *n; xb_log_level l; } T[] = {
        {"trace", XB_LOG_TRACE}, {"debug", XB_LOG_DEBUG}, {"info", XB_LOG_INFO},
        {"warn", XB_LOG_WARN}, {"warning", XB_LOG_WARN}, {"error", XB_LOG_ERROR},
        {"fatal", XB_LOG_FATAL}, {"off", (xb_log_level)99}
    };
    for (size_t i = 0; i < XB_ARRAY_LEN(T); i++) {
        if (xb_strieq(s, T[i].n)) { if (out) *out = T[i].l; return true; }
    }
    return false;
}

static void ring_push(const char *json_line, int64_t ts)
{
    if (!L.ring_on || !L.ring) return;
    size_t idx = L.ring_len % L.ring_cap;
    if (L.ring[idx].msg) xb_free(L.ring[idx].msg);
    L.ring[idx].msg = xb_strdup(json_line);
    L.ring[idx].ts_ms = ts;
    L.ring[idx].level = L.level;
    L.ring_len++;
}

void xb_log(xb_log_level lvl, const char *fmt, ...)
{
    if (lvl < L.level && lvl < XB_LOG_FATAL) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    int64_t ts = xb_now_ms();

    /* Build the JSON line with the same writer used elsewhere. */
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_int(&w, "ts", ts);
    xb_jw_key(&w, "level");
    xb_jw_str(&w, level_name(lvl));
    xb_jw_kv_str(&w, "msg", msg);
    xb_jw_obj_end(&w);
    char *line = xb_buf_steal(&w.buf);

    if (L.to_stderr) {
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }
    if (L.inited) {
        XB_MUTEX_LOCK(&L.m);
        ring_push(line, ts);
        XB_MUTEX_UNLOCK(&L.m);
    }
    xb_jw_free(&w);
    xb_free(line);
}

void xb_log_fields(xb_log_level lvl, const char *fields_json, const char *fmt, ...)
{
    if (lvl < L.level && lvl < XB_LOG_FATAL) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int64_t ts = xb_now_ms();

    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_int(&w, "ts", ts);
    xb_jw_key(&w, "level");
    xb_jw_str(&w, level_name(lvl));
    xb_jw_kv_str(&w, "msg", msg);
    if (fields_json && fields_json[0] == '{') {
        xb_jw_key(&w, "data");
        xb_jw_raw(&w, fields_json);
    }
    xb_jw_obj_end(&w);
    char *line = xb_buf_steal(&w.buf);

    if (L.to_stderr) { fprintf(stderr, "%s\n", line); fflush(stderr); }
    if (L.inited) {
        XB_MUTEX_LOCK(&L.m);
        ring_push(line, ts);
        XB_MUTEX_UNLOCK(&L.m);
    }
    xb_jw_free(&w);
    xb_free(line);
}

char *xb_log_snapshot_json(int64_t since_ms, size_t max)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_arr_begin(&w);
    if (!L.inited) { xb_jw_arr_end(&w); return xb_buf_steal(&w.buf); }

    XB_MUTEX_LOCK(&L.m);
    size_t total = L.ring_len;
    size_t avail = total < L.ring_cap ? total : L.ring_cap;
    size_t emitted = 0;
    for (size_t k = 0; k < avail; k++) {
        /* oldest first */
        size_t seq = total - avail + k;
        size_t idx = seq % L.ring_cap;
        if (!L.ring[idx].msg) continue;
        if (since_ms > 0 && L.ring[idx].ts_ms < since_ms) continue;
        if (max > 0 && emitted >= max) break;
        xb_jw_raw(&w, L.ring[idx].msg);
        emitted++;
    }
    XB_MUTEX_UNLOCK(&L.m);

    xb_jw_arr_end(&w);
    return xb_buf_steal(&w.buf);
}

size_t xb_log_count(void)
{
    if (!L.inited) return 0;
    size_t total = L.ring_len;
    return total < L.ring_cap ? total : L.ring_cap;
}
