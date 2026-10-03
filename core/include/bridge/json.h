/* bridge/json.h — arena-backed JSON value model, parser and canonical writer.
 *
 * Design notes:
 *  - Parsing never touches malloc per node: nodes and strings live in an arena
 *    owned by the caller, so a whole payload is released with one call.
 *  - The parser is strict (RFC 8259): no trailing commas, no unquoted keys,
 *    no NaN/Infinity, and it rejects depth > XB_JSON_MAX_DEPTH so a hostile
 *    frame cannot blow the C stack.
 *  - The writer always escapes control characters, and canonical mode sorts
 *    object keys — that is what makes cross-language cache keys stable.
 */
#ifndef BRIDGE_JSON_H
#define BRIDGE_JSON_H

#include "buf.h"

#define XB_JSON_MAX_DEPTH 64

typedef enum {
    XB_JNULL = 0,
    XB_JBOOL,
    XB_JNUM,
    XB_JSTR,
    XB_JARR,
    XB_JOBJ
} xb_json_type;

typedef struct xb_json xb_json;

struct xb_json {
    xb_json_type type;
    union {
        bool     b;
        double   num;
        struct { const char *s; size_t n; } str;
        struct { xb_json **items; size_t n; } arr;
        struct { const char **keys; size_t *klens; xb_json **vals; size_t n; } obj;
    } u;
};

typedef enum {
    XB_JSON_OK = 0,
    XB_JSON_ERR_SYNTAX,
    XB_JSON_ERR_DEPTH,
    XB_JSON_ERR_UTF8,
    XB_JSON_ERR_OOM
} xb_json_status;

const char *xb_json_strerror(xb_json_status st);

/* Parse `s` (len bytes). Returns NULL on failure and writes the byte offset of
 * the problem into *err_off (may be NULL). Allocated nodes live in `a`. */
xb_json *xb_json_parse(xb_arena *a, const char *s, size_t len,
                       size_t *err_off);

/* ------------------------------------------------------------------ read -- */

xb_json *xb_json_get(const xb_json *obj, const char *key);
const char *xb_json_type_name(xb_json_type t);

/* Typed accessors with defaults; never crash on type mismatch. */
const char *xb_json_str(const xb_json *v, const char *fallback);
double      xb_json_num(const xb_json *v, double fallback);
int64_t     xb_json_int(const xb_json *v, int64_t fallback);
bool        xb_json_bool(const xb_json *v, bool fallback);

const char *xb_json_obj_str(const xb_json *o, const char *k, const char *fb);
int64_t     xb_json_obj_int(const xb_json *o, const char *k, int64_t fb);
double      xb_json_obj_num(const xb_json *o, const char *k, double fb);
bool        xb_json_obj_bool(const xb_json *o, const char *k, bool fb);
xb_json    *xb_json_obj_get(const xb_json *o, const char *k);

size_t      xb_json_arr_len(const xb_json *v);
xb_json    *xb_json_arr_at(const xb_json *v, size_t i);

/* ----------------------------------------------------------------- write -- */

typedef struct {
    xb_buf  buf;
    int     depth;
    bool    canonical;     /* sort object keys (cache-key stability) */
    bool    pretty;
    bool    pending_key;   /* last emitted token was a key -> next token needs ':' */
} xb_jsonw;

void xb_jw_init(xb_jsonw *w);
void xb_jw_init_canonical(xb_jsonw *w);
void xb_jw_free(xb_jsonw *w);
const char *xb_jw_data(xb_jsonw *w);   /* NUL-terminated for convenience */
size_t      xb_jw_len(xb_jsonw *w);
void xb_jw_reset(xb_jsonw *w);

void xb_jw_obj_begin(xb_jsonw *w);
void xb_jw_obj_end(xb_jsonw *w);
void xb_jw_arr_begin(xb_jsonw *w);
void xb_jw_arr_end(xb_jsonw *w);
void xb_jw_key(xb_jsonw *w, const char *key);
void xb_jw_str(xb_jsonw *w, const char *s);          /* NULL -> null */
void xb_jw_strn(xb_jsonw *w, const char *s, size_t n);
void xb_jw_int(xb_jsonw *w, int64_t v);
void xb_jw_num(xb_jsonw *w, double v);
void xb_jw_bool(xb_jsonw *w, bool v);
void xb_jw_null(xb_jsonw *w);
void xb_jw_raw(xb_jsonw *w, const char *raw);        /* caller guarantees valid JSON */

/* key/value convenience — must be called inside an object. */
void xb_jw_kv_str(xb_jsonw *w, const char *k, const char *v);
void xb_jw_kv_strn(xb_jsonw *w, const char *k, const char *v, size_t n);
void xb_jw_kv_int(xb_jsonw *w, const char *k, int64_t v);
void xb_jw_kv_num(xb_jsonw *w, const char *k, double v);
void xb_jw_kv_bool(xb_jsonw *w, const char *k, bool v);
void xb_jw_kv_null(xb_jsonw *w, const char *k);

/* Re-emit an already-parsed value into a writer (canonical-aware). */
void xb_jw_value(xb_jsonw *w, const xb_json *v);

/* Serialise a parsed value to a fresh malloc'd NUL-terminated string.
 * If canonical is true, object keys are sorted. Caller frees. */
char *xb_json_dumps(const xb_json *v, bool canonical);

/* Canonical hash used for cache keys: sha256 of canonical JSON of `v`. */
void xb_json_canonical_hash(const xb_json *v, char out_hex[65]);

#endif /* BRIDGE_JSON_H */
