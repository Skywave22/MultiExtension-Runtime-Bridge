/* bridge/http.h — HTTP/1.1 client for extensions.
 *
 * Deliberately small: enough to fetch and post pages like an extension runtime
 * needs, with cookies, redirects and chunked decoding. TLS is delegated (see
 * docs/ARCHITECTURE.md): when the daemon has no TLS provider it reports
 * `https` as an unavailable capability instead of silently failing requests.
 */
#ifndef BRIDGE_HTTP_H
#define BRIDGE_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "buf.h"

typedef struct {
    const char *method;       /* "GET", "POST" ... default GET */
    const char *url;
    const char *body;
    size_t      body_len;
    const char *content_type;
    const char *user_agent;
    const char **headers;     /* NULL-terminated "Name: value" list (or NULL) */
    int         timeout_ms;
    int         max_redirects;
    size_t      max_bytes;    /* hard cap on the response body */
    bool        follow_redirects;
} xb_http_req;

typedef struct {
    int    status;
    char  *body;              /* malloc'd, NUL-terminated */
    size_t body_len;
    char   content_type[128];
    char   final_url[1024];
    char   error[256];
} xb_http_res;

/* Simple cookie jar shared by one source's requests. */
typedef struct xb_cookies xb_cookies;
xb_cookies *xb_cookies_new(void);
void        xb_cookies_free(xb_cookies *c);
void        xb_cookies_set(xb_cookies *c, const char *url, const char *set_cookie_header);
const char *xb_cookies_header(xb_cookies *c, const char *url);  /* internal buffer */
int         xb_cookies_count(xb_cookies *c);

/* Perform a request. Returns 0 when a response was received (check r->status);
 * -1 on transport failure (see r->error). `jar` may be NULL. */
int xb_http_do(const xb_http_req *req, xb_cookies *jar, xb_http_res *r);
void xb_http_res_free(xb_http_res *r);

/* URL helpers used by the rule engine and tests. */
bool xb_url_is_absolute(const char *url);
/* Resolve `ref` against `base` into `out`. Handles //host, /path, ../ and ?q. */
void xb_url_resolve(const char *base, const char *ref, char *out, size_t cap);
void xb_url_encode(const char *in, char *out, size_t cap);
/* Percent-decode in place. */
void xb_url_decode(char *s);

/* Capability probe: can this build actually speak https:// ? */
bool xb_http_tls_available(void);
const char *xb_http_tls_backend(void);

#endif /* BRIDGE_HTTP_H */
