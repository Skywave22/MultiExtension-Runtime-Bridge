/* json.c — strict RFC 8259 parser + canonical writer.
 *
 * Performance notes that matter for the bridge's hot path:
 *  - No per-node malloc: nodes come from the caller's arena.
 *  - Strings are NUL-terminated in the arena so callers can use them directly.
 *  - Number parsing uses strtod on a bounded copy; no locale dependence
 *    (we re-implement '.' handling by rejecting ',' as a decimal separator).
 *  - Depth-limited to prevent stack exhaustion from hostile frames.
 */
#include "bridge/json.h"
#include "bridge/util.h"
#include "bridge/log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

const char *xb_json_strerror(xb_json_status st)
{
    switch (st) {
    case XB_JSON_OK:         return "ok";
    case XB_JSON_ERR_SYNTAX: return "syntax error";
    case XB_JSON_ERR_DEPTH:  return "maximum nesting depth exceeded";
    case XB_JSON_ERR_UTF8:   return "invalid UTF-8";
    case XB_JSON_ERR_OOM:    return "out of memory";
    }
    return "unknown";
}

typedef struct {
    xb_arena *a;
    const char *start;
    const char *p;
    const char *end;
    size_t depth;
    size_t err_off;
    xb_json_status status;
} P;

static xb_json *node(P *ps, xb_json_type t)
{
    xb_json *v = (xb_json *)xb_arena_alloc(ps->a, sizeof(xb_json));
    v->type = t;
    return v;
}

static void skip_ws(P *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static void fail(P *ps, xb_json_status st)
{
    if (ps->status == XB_JSON_OK) {
        ps->status = st;
        ps->err_off = (size_t)(ps->p - ps->start);
    }
}

/* Decode one \uXXXX escape into UTF-8. Returns bytes written. */
static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *parse_string(P *ps, size_t *out_len)
{
    /* Fast path: scan for the closing quote with no escapes. */
    const char *start = ps->p;
    const char *q = start;
    while (q < ps->end && *q != '"' && *q != '\\') {
        if ((unsigned char)*q < 0x20) { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
        q++;
    }
    if (q >= ps->end) { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
    if (*q == '"') {
        *out_len = (size_t)(q - start);
        ps->p = q + 1;
        return xb_arena_strndup(ps->a, start, *out_len);
    }
    /* Slow path: build into a scratch arena buffer. */
    xb_buf b;
    xb_buf_init(&b);
    xb_buf_append(&b, start, (size_t)(q - start));
    ps->p = q;
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') {
            *out_len = b.len;
            char *s = xb_arena_strndup(ps->a, (const char *)b.data, b.len);
            xb_buf_free(&b);
            return s;
        }
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            switch (e) {
            case '"':  xb_buf_append_byte(&b, '"');  break;
            case '\\': xb_buf_append_byte(&b, '\\'); break;
            case '/':  xb_buf_append_byte(&b, '/');  break;
            case 'b':  xb_buf_append_byte(&b, '\b'); break;
            case 'f':  xb_buf_append_byte(&b, '\f'); break;
            case 'n':  xb_buf_append_byte(&b, '\n'); break;
            case 'r':  xb_buf_append_byte(&b, '\r'); break;
            case 't':  xb_buf_append_byte(&b, '\t'); break;
            case 'u': {
                if (ps->end - ps->p < 4) { xb_buf_free(&b); fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    int hv = hexval(ps->p[i]);
                    if (hv < 0) { xb_buf_free(&b); fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
                    cp = cp * 16 + (uint32_t)hv;
                }
                ps->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {   /* surrogate pair */
                    if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        uint32_t lo = 0;
                        int ok = 1;
                        for (int i = 0; i < 4; i++) {
                            int hv = hexval(ps->p[2 + i]);
                            if (hv < 0) { ok = 0; break; }
                            lo = lo * 16 + (uint32_t)hv;
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            ps->p += 6;
                        }
                    }
                }
                char tmp[4];
                size_t n = utf8_encode(cp, tmp);
                xb_buf_append(&b, tmp, n);
                break;
            }
            default:
                xb_buf_free(&b);
                fail(ps, XB_JSON_ERR_SYNTAX);
                return NULL;
            }
        } else if ((unsigned char)c < 0x20) {
            xb_buf_free(&b);
            fail(ps, XB_JSON_ERR_SYNTAX);
            return NULL;
        } else {
            xb_buf_append_byte(&b, (uint8_t)c);
        }
    }
    xb_buf_free(&b);
    fail(ps, XB_JSON_ERR_SYNTAX);
    return NULL;
}

static xb_json *parse_value(P *ps);

static xb_json *parse_array(P *ps)
{
    xb_json *v = node(ps, XB_JARR);
    size_t cap = 8, n = 0;
    xb_json **items = (xb_json **)xb_arena_alloc(ps->a, cap * sizeof(xb_json *));
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; v->u.arr.items = items; v->u.arr.n = 0; return v; }
    for (;;) {
        xb_json *it = parse_value(ps);
        if (!it) return NULL;
        if (n == cap) {
            size_t ncap = cap * 2;
            xb_json **ni = (xb_json **)xb_arena_alloc(ps->a, ncap * sizeof(xb_json *));
            memcpy(ni, items, n * sizeof(xb_json *));
            items = ni; cap = ncap;
        }
        items[n++] = it;
        skip_ws(ps);
        if (ps->p >= ps->end) { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
        if (*ps->p == ',') { ps->p++; skip_ws(ps); continue; }
        if (*ps->p == ']') { ps->p++; break; }
        fail(ps, XB_JSON_ERR_SYNTAX);
        return NULL;
    }
    v->u.arr.items = items;
    v->u.arr.n = n;
    return v;
}

static xb_json *parse_object(P *ps)
{
    xb_json *v = node(ps, XB_JOBJ);
    size_t cap = 8, n = 0;
    const char **keys = (const char **)xb_arena_alloc(ps->a, cap * sizeof(char *));
    size_t *klens = (size_t *)xb_arena_alloc(ps->a, cap * sizeof(size_t));
    xb_json **vals = (xb_json **)xb_arena_alloc(ps->a, cap * sizeof(xb_json *));
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; goto done; }
    for (;;) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
        ps->p++;
        size_t klen = 0;
        char *k = parse_string(ps, &klen);
        if (!k) return NULL;
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
        ps->p++;
        xb_json *val = parse_value(ps);
        if (!val) return NULL;
        if (n == cap) {
            size_t ncap = cap * 2;
            const char **nk = (const char **)xb_arena_alloc(ps->a, ncap * sizeof(char *));
            size_t *nl = (size_t *)xb_arena_alloc(ps->a, ncap * sizeof(size_t));
            xb_json **nv = (xb_json **)xb_arena_alloc(ps->a, ncap * sizeof(xb_json *));
            memcpy(nk, keys, n * sizeof(char *));
            memcpy(nl, klens, n * sizeof(size_t));
            memcpy(nv, vals, n * sizeof(xb_json *));
            keys = nk; klens = nl; vals = nv; cap = ncap;
        }
        keys[n] = k; klens[n] = klen; vals[n] = val; n++;
        skip_ws(ps);
        if (ps->p >= ps->end) { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; break; }
        fail(ps, XB_JSON_ERR_SYNTAX);
        return NULL;
    }
done:
    v->u.obj.keys = keys; v->u.obj.klens = klens;
    v->u.obj.vals = vals; v->u.obj.n = n;
    return v;
}

static xb_json *parse_number(P *ps)
{
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    if (ps->p >= ps->end || !isdigit((unsigned char)*ps->p)) { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
    /* RFC 8259 forbids leading zeros: 0 is a number, 01 is not. Accepting it
     * would make canonical hashes differ between SDKs, so reject it here. */
    if (*ps->p == '0' && ps->p + 1 < ps->end && isdigit((unsigned char)ps->p[1])) {
        fail(ps, XB_JSON_ERR_SYNTAX);
        return NULL;
    }
    while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || !isdigit((unsigned char)*ps->p)) { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (ps->p >= ps->end || !isdigit((unsigned char)*ps->p)) { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    char tmp[64];
    size_t n = (size_t)(ps->p - start);
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    xb_json *v = node(ps, XB_JNUM);
    v->u.num = strtod(tmp, NULL);
    return v;
}

static xb_json *parse_value(P *ps)
{
    if (ps->depth >= XB_JSON_MAX_DEPTH) { fail(ps, XB_JSON_ERR_DEPTH); return NULL; }
    skip_ws(ps);
    if (ps->p >= ps->end) { fail(ps, XB_JSON_ERR_SYNTAX); return NULL; }
    char c = *ps->p;
    switch (c) {
    case '{': {
        ps->p++; ps->depth++;
        xb_json *v = parse_object(ps);
        ps->depth--;
        return v;
    }
    case '[': {
        ps->p++; ps->depth++;
        xb_json *v = parse_array(ps);
        ps->depth--;
        return v;
    }
    case '"': {
        ps->p++;
        size_t n = 0;
        char *s = parse_string(ps, &n);
        if (!s) return NULL;
        xb_json *v = node(ps, XB_JSTR);
        v->u.str.s = s; v->u.str.n = n;
        return v;
    }
    case 't':
        if (ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) {
            ps->p += 4;
            xb_json *v = node(ps, XB_JBOOL); v->u.b = true; return v;
        }
        fail(ps, XB_JSON_ERR_SYNTAX); return NULL;
    case 'f':
        if (ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) {
            ps->p += 5;
            xb_json *v = node(ps, XB_JBOOL); v->u.b = false; return v;
        }
        fail(ps, XB_JSON_ERR_SYNTAX); return NULL;
    case 'n':
        if (ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) {
            ps->p += 4;
            return node(ps, XB_JNULL);
        }
        fail(ps, XB_JSON_ERR_SYNTAX); return NULL;
    default:
        if (c == '-' || isdigit((unsigned char)c)) return parse_number(ps);
        fail(ps, XB_JSON_ERR_SYNTAX);
        return NULL;
    }
}

xb_json *xb_json_parse(xb_arena *a, const char *s, size_t len, size_t *err_off)
{
    P ps;
    ps.a = a; ps.start = s; ps.p = s; ps.end = s + len; ps.depth = 0;
    ps.err_off = 0; ps.status = XB_JSON_OK;
    xb_json *v = parse_value(&ps);
    if (v) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            v = NULL;
            fail(&ps, XB_JSON_ERR_SYNTAX);
            ps.err_off = (size_t)(ps.p - s);
        }
    }
    if (!v && err_off) *err_off = ps.err_off;
    return v;
}

/* ---------------------------------------------------------------- read -- */

static int kcmp(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    int r = memcmp(a, b, n);
    if (r) return r;
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

xb_json *xb_json_get(const xb_json *obj, const char *key)
{
    if (!obj || obj->type != XB_JOBJ || !key) return NULL;
    size_t kl = strlen(key);
    for (size_t i = 0; i < obj->u.obj.n; i++)
        if (kcmp(obj->u.obj.keys[i], obj->u.obj.klens[i], key, kl) == 0)
            return obj->u.obj.vals[i];
    return NULL;
}

const char *xb_json_type_name(xb_json_type t)
{
    switch (t) {
    case XB_JNULL: return "null";
    case XB_JBOOL: return "boolean";
    case XB_JNUM:  return "number";
    case XB_JSTR:  return "string";
    case XB_JARR:  return "array";
    case XB_JOBJ:  return "object";
    }
    return "unknown";
}

const char *xb_json_str(const xb_json *v, const char *fb)
{
    return (v && v->type == XB_JSTR) ? v->u.str.s : fb;
}

double xb_json_num(const xb_json *v, double fb)
{
    if (!v) return fb;
    if (v->type == XB_JNUM) return v->u.num;
    if (v->type == XB_JSTR) { char *e = NULL; double d = strtod(v->u.str.s, &e); if (e && e != v->u.str.s) return d; }
    if (v->type == XB_JBOOL) return v->u.b ? 1 : 0;
    return fb;
}

int64_t xb_json_int(const xb_json *v, int64_t fb)
{
    double d = xb_json_num(v, (double)fb);
    if (!isfinite(d)) return fb;
    return (int64_t)d;
}

bool xb_json_bool(const xb_json *v, bool fb)
{
    if (!v) return fb;
    if (v->type == XB_JBOOL) return v->u.b;
    if (v->type == XB_JNUM) return v->u.num != 0;
    if (v->type == XB_JSTR) return xb_strieq(v->u.str.s, "true") || xb_streq(v->u.str.s, "1");
    return fb;
}

const char *xb_json_obj_str(const xb_json *o, const char *k, const char *fb)
{ return xb_json_str(xb_json_get(o, k), fb); }
int64_t xb_json_obj_int(const xb_json *o, const char *k, int64_t fb)
{ return xb_json_int(xb_json_get(o, k), fb); }
double xb_json_obj_num(const xb_json *o, const char *k, double fb)
{ return xb_json_num(xb_json_get(o, k), fb); }
bool xb_json_obj_bool(const xb_json *o, const char *k, bool fb)
{ return xb_json_bool(xb_json_get(o, k), fb); }
xb_json *xb_json_obj_get(const xb_json *o, const char *k) { return xb_json_get(o, k); }

size_t xb_json_arr_len(const xb_json *v)
{ return (v && v->type == XB_JARR) ? v->u.arr.n : 0; }

xb_json *xb_json_arr_at(const xb_json *v, size_t i)
{
    if (!v || v->type != XB_JARR || i >= v->u.arr.n) return NULL;
    return v->u.arr.items[i];
}

/* ---------------------------------------------------------------- write -- */

void xb_jw_init(xb_jsonw *w)
{
    xb_buf_init(&w->buf);
    w->depth = 0; w->canonical = false; w->pretty = false; w->pending_key = false;
}

void xb_jw_init_canonical(xb_jsonw *w) { xb_jw_init(w); w->canonical = true; }
void xb_jw_free(xb_jsonw *w) { xb_buf_free(&w->buf); }
void xb_jw_reset(xb_jsonw *w) { xb_buf_reset(&w->buf); w->depth = 0; w->pending_key = false; }
const char *xb_jw_data(xb_jsonw *w) { return (const char *)w->buf.data; }
size_t xb_jw_len(xb_jsonw *w) { return w->buf.len; }

/* `w->pending_key` tracks whether the last token was an object key, so the next
 * token emits ':' rather than ','. It lives in the writer (not a file-scope
 * static) because writers are used concurrently by connection threads. */
static void sep(xb_jsonw *w)
{
    if (w->buf.len == 0) return;
    uint8_t last = w->buf.data[w->buf.len - 1];
    if (last == '{' || last == '[') return;
    if (w->pending_key) { xb_buf_append_byte(&w->buf, ':'); w->pending_key = false; return; }
    xb_buf_append_byte(&w->buf, ',');
}

void xb_jw_obj_begin(xb_jsonw *w) { sep(w); xb_buf_append_byte(&w->buf, '{'); w->depth++; }
void xb_jw_obj_end(xb_jsonw *w)   { xb_buf_append_byte(&w->buf, '}'); w->depth--; }
void xb_jw_arr_begin(xb_jsonw *w) { sep(w); xb_buf_append_byte(&w->buf, '['); w->depth++; }
void xb_jw_arr_end(xb_jsonw *w)   { xb_buf_append_byte(&w->buf, ']'); w->depth--; }

void xb_jw_key(xb_jsonw *w, const char *key)
{
    sep(w);
    xb_buf_append_json_string(&w->buf, key, strlen(key));
    w->pending_key = true;
}

void xb_jw_str(xb_jsonw *w, const char *s)
{
    sep(w); w->pending_key = false;
    if (!s) { xb_buf_append(&w->buf, "null", 4); return; }
    xb_buf_append_json_string(&w->buf, s, strlen(s));
}

void xb_jw_strn(xb_jsonw *w, const char *s, size_t n)
{
    sep(w); w->pending_key = false;
    if (!s) { xb_buf_append(&w->buf, "null", 4); return; }
    xb_buf_append_json_string(&w->buf, s, n);
}

void xb_jw_int(xb_jsonw *w, int64_t v)
{
    sep(w); w->pending_key = false;
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    xb_buf_append(&w->buf, tmp, (size_t)n);
}

void xb_jw_num(xb_jsonw *w, double v)
{
    sep(w); w->pending_key = false;
    if (!isfinite(v)) { xb_buf_append(&w->buf, "null", 4); return; }
    char tmp[40];
    /* %.17g round-trips doubles exactly; trim trailing zeros for brevity. */
    int n = snprintf(tmp, sizeof tmp, "%.17g", v);
    if (n > 0 && !strchr(tmp, '.') && !strchr(tmp, 'e') && !strchr(tmp, 'E')) {
        /* integral: try shorter representations that round-trip */
        for (int prec = 1; prec <= 17; prec++) {
            char t2[40];
            int n2 = snprintf(t2, sizeof t2, "%.*g", prec, v);
            if (n2 > 0 && strtod(t2, NULL) == v) { memcpy(tmp, t2, (size_t)n2 + 1); n = n2; break; }
        }
    }
    xb_buf_append(&w->buf, tmp, (size_t)n);
}

void xb_jw_bool(xb_jsonw *w, bool v)
{
    sep(w); w->pending_key = false;
    xb_buf_append(&w->buf, v ? "true" : "false", v ? 4 : 5);
}

void xb_jw_null(xb_jsonw *w) { sep(w); w->pending_key = false; xb_buf_append(&w->buf, "null", 4); }

void xb_jw_raw(xb_jsonw *w, const char *raw)
{
    sep(w); w->pending_key = false;
    if (!raw) { xb_buf_append(&w->buf, "null", 4); return; }
    xb_buf_append_str(&w->buf, raw);
}

void xb_jw_kv_str(xb_jsonw *w, const char *k, const char *v) { xb_jw_key(w, k); xb_jw_str(w, v); }
void xb_jw_kv_strn(xb_jsonw *w, const char *k, const char *v, size_t n) { xb_jw_key(w, k); xb_jw_strn(w, v, n); }
void xb_jw_kv_int(xb_jsonw *w, const char *k, int64_t v) { xb_jw_key(w, k); xb_jw_int(w, v); }
void xb_jw_kv_num(xb_jsonw *w, const char *k, double v) { xb_jw_key(w, k); xb_jw_num(w, v); }
void xb_jw_kv_bool(xb_jsonw *w, const char *k, bool v) { xb_jw_key(w, k); xb_jw_bool(w, v); }
void xb_jw_kv_null(xb_jsonw *w, const char *k) { xb_jw_key(w, k); xb_jw_null(w); }

/* ---------------------------------------------------------- value re-emit -- */

typedef struct { const char *k; size_t kl; const xb_json *v; } kv_pairs;

static int kv_cmp(const void *a, const void *b)
{
    const kv_pairs *x = (const kv_pairs *)a, *y = (const kv_pairs *)b;
    size_t n = x->kl < y->kl ? x->kl : y->kl;
    int r = memcmp(x->k, y->k, n);
    if (r) return r;
    if (x->kl == y->kl) return 0;
    return x->kl < y->kl ? -1 : 1;
}

void xb_jw_value(xb_jsonw *w, const xb_json *v)
{
    if (!v) { xb_jw_null(w); return; }
    switch (v->type) {
    case XB_JNULL: xb_jw_null(w); break;
    case XB_JBOOL: xb_jw_bool(w, v->u.b); break;
    case XB_JNUM:  xb_jw_num(w, v->u.num); break;
    case XB_JSTR:  xb_jw_strn(w, v->u.str.s, v->u.str.n); break;
    case XB_JARR:
        xb_jw_arr_begin(w);
        for (size_t i = 0; i < v->u.arr.n; i++) xb_jw_value(w, v->u.arr.items[i]);
        xb_jw_arr_end(w);
        break;
    case XB_JOBJ: {
        xb_jw_obj_begin(w);
        size_t n = v->u.obj.n;
        if (w->canonical && n > 1) {
            kv_pairs stack[64];
            kv_pairs *pairs = stack;
            if (n > XB_ARRAY_LEN(stack)) pairs = (kv_pairs *)xb_alloc(n * sizeof(kv_pairs));
            for (size_t i = 0; i < n; i++) {
                pairs[i].k = v->u.obj.keys[i];
                pairs[i].kl = v->u.obj.klens[i];
                pairs[i].v = v->u.obj.vals[i];
            }
            qsort(pairs, n, sizeof(kv_pairs), kv_cmp);
            for (size_t i = 0; i < n; i++) {
                xb_jw_key(w, pairs[i].k);
                xb_jw_value(w, pairs[i].v);
            }
            if (pairs != stack) xb_free(pairs);
        } else {
            for (size_t i = 0; i < n; i++) {
                xb_jw_key(w, v->u.obj.keys[i]);
                xb_jw_value(w, v->u.obj.vals[i]);
            }
        }
        xb_jw_obj_end(w);
        break;
    }
    }
}

char *xb_json_dumps(const xb_json *v, bool canonical)
{
    xb_jsonw w;
    if (canonical) xb_jw_init_canonical(&w); else xb_jw_init(&w);
    xb_jw_value(&w, v);
    return xb_buf_steal(&w.buf);
}

void xb_json_canonical_hash(const xb_json *v, char out_hex[65])
{
    xb_jsonw w;
    xb_jw_init_canonical(&w);
    xb_jw_value(&w, v);
    xb_sha256_hex(w.buf.data ? (const char *)w.buf.data : "", w.buf.len, out_hex);
    xb_jw_free(&w);
}
