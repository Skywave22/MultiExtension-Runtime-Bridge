/* http.c — HTTP/1.1 client, cookie jar, URL resolution. */
#include "bridge/http.h"
#include "bridge/net.h"
#include "bridge/proc.h"
#include "bridge/buf.h"
#include "bridge/util.h"
#include "bridge/log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define XB_HTTP_DEFAULT_MAX (8u * 1024 * 1024)

/* --------------------------------------------------------- TLS provider -- */

/* A TLS provider is optional. When none is compiled in, https:// requests are
 * handed to a helper binary if one exists (curl/wget), and otherwise refused
 * with a clear error. This keeps the core dependency-free while still working
 * on a normal desktop. */
bool xb_http_tls_available(void)
{
#ifdef XB_HAVE_TLS
    return true;
#else
    static int cached = -1;
    if (cached < 0) {
        char *c = xb_which("curl");
        if (c) { xb_free(c); cached = 1; }
        else {
            char *w = xb_which("wget");
            cached = w ? 1 : 0;
            xb_free(w);
        }
    }
    return cached == 1;
#endif
}

const char *xb_http_tls_backend(void)
{
#ifdef XB_HAVE_TLS
    return "builtin";
#else
    if (!xb_http_tls_available()) return "none";
    char *c = xb_which("curl");
    bool has_curl = c != NULL;
    xb_free(c);
    return has_curl ? "curl" : "wget";
#endif
}

/* ------------------------------------------------------------- URL utils -- */

bool xb_url_is_absolute(const char *url)
{
    const char *p = strstr(url, "://");
    if (!p) return false;
    for (const char *q = url; q < p; q++)
        if (!isalnum((unsigned char)*q) && *q != '+' && *q != '-' && *q != '.')
            return false;
    return p != url;
}

void xb_url_encode(const char *in, char *out, size_t cap)
{
    static const char *HEX = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else if (*p == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 0xF];
        }
    }
    out[o] = '\0';
}

void xb_url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            int hi = isdigit((unsigned char)p[1]) ? p[1] - '0' : tolower(p[1]) - 'a' + 10;
            int lo = isdigit((unsigned char)p[2]) ? p[2] - '0' : tolower(p[2]) - 'a' + 10;
            *o++ = (char)((hi << 4) | lo);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* Split into scheme://host[:port], path, query. */
typedef struct {
    char scheme[16];
    char host[256];
    int  port;
    char path[2048];
} url_parts;

static int url_split(const char *url, url_parts *p)
{
    memset(p, 0, sizeof *p);
    const char *sep = strstr(url, "://");
    if (!sep) return -1;
    size_t sl = (size_t)(sep - url);
    if (sl >= sizeof p->scheme) return -1;
    memcpy(p->scheme, url, sl);
    p->scheme[sl] = '\0';
    for (size_t i = 0; i < sl; i++) p->scheme[i] = (char)tolower((unsigned char)p->scheme[i]);

    const char *hp = sep + 3;
    const char *slash = strchr(hp, '/');
    const char *hostend = slash ? slash : hp + strlen(hp);
    const char *colon = NULL;
    for (const char *q = hp; q < hostend; q++) if (*q == ':') colon = q;
    size_t hl = (size_t)((colon ? colon : hostend) - hp);
    if (hl == 0 || hl >= sizeof p->host) return -1;
    memcpy(p->host, hp, hl);
    p->host[hl] = '\0';

    p->port = xb_streq(p->scheme, "https") ? 443 : 80;
    if (colon) p->port = atoi(colon + 1);
    xb_str_lcpy(p->path, slash ? slash : "/", sizeof p->path);
    return 0;
}

static void url_join_path(const char *base, const char *ref, char *out, size_t cap)
{
    /* `base` is the directory of the base URL (no query). */
    if (ref[0] == '/') { xb_str_lcpy(out, ref, cap); return; }
    if (ref[0] == '?') {
        xb_str_lcpy(out, base, cap);
        char *q = strchr(out, '?');
        if (q) *q = '\0';
        strncat(out, ref, cap - strlen(out) - 1);
        return;
    }
    char tmp[4096];
    xb_str_lcpy(tmp, base, sizeof tmp);
    char *q = strchr(tmp, '?');
    if (q) *q = '\0';
    char *last = strrchr(tmp, '/');
    if (last) last[1] = '\0'; else xb_str_lcpy(tmp, "/", sizeof tmp);

    char joined[4096];
    snprintf(joined, sizeof joined, "%s%s", tmp, ref);

    /* Normalise ./ and ../ segments. */
    char *segs[256];
    size_t n = xb_str_split(joined, '/', segs, 256);
    char norm[4096];
    size_t used = 0;
    norm[used++] = '/';              /* paths are always absolute here */
    for (size_t i = 0; i < n; i++) {
        const char *seg = segs[i];
        if (seg[0] == '\0' || strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) {
            /* Drop the previous segment, but never the leading slash. */
            while (used > 1 && norm[used - 1] != '/') used--;
            if (used > 1) used--;
            continue;
        }
        if (used > 1 && norm[used - 1] != '/') norm[used++] = '/';
        size_t l = strlen(seg);
        if (used + l < sizeof norm) { memcpy(norm + used, seg, l); used += l; }
    }
    if (used == 0) norm[used++] = '/';
    norm[used] = '\0';
    if (norm[0] == '\0') xb_str_lcpy(norm, "/", sizeof norm);
    xb_str_lcpy(out, norm, cap);
}

void xb_url_resolve(const char *base, const char *ref, char *out, size_t cap)
{
    if (!ref || !ref[0]) { xb_str_lcpy(out, base, cap); return; }
    if (xb_url_is_absolute(ref)) { xb_str_lcpy(out, ref, cap); return; }
    if (!base || !base[0]) { xb_str_lcpy(out, ref, cap); return; }

    url_parts bp;
    if (url_split(base, &bp) != 0) { xb_str_lcpy(out, ref, cap); return; }

    if (strncmp(ref, "//", 2) == 0) {
        snprintf(out, cap, "%s:%s", bp.scheme, ref);
        return;
    }
    char path[4096];
    url_join_path(bp.path, ref, path, sizeof path);
    bool default_port = (xb_streq(bp.scheme, "http") && bp.port == 80) ||
                        (xb_streq(bp.scheme, "https") && bp.port == 443);
    if (default_port) snprintf(out, cap, "%s://%s%s", bp.scheme, bp.host, path);
    else              snprintf(out, cap, "%s://%s:%d%s", bp.scheme, bp.host, bp.port, path);
}

/* -------------------------------------------------------------- cookies -- */

typedef struct cookie {
    char name[128];
    char value[512];
    char domain[192];
    char path[256];
    bool secure;
    struct cookie *next;
} cookie_t;

struct xb_cookies {
    cookie_t *head;
    char      hdr[4096];
    size_t    count;
};

xb_cookies *xb_cookies_new(void) { return (xb_cookies *)xb_alloc(sizeof(xb_cookies)); }

void xb_cookies_free(xb_cookies *c)
{
    if (!c) return;
    cookie_t *k = c->head;
    while (k) { cookie_t *n = k->next; xb_free(k); k = n; }
    xb_free(c);
}

static bool domain_match(const char *cookie_domain, const char *host)
{
    if (!cookie_domain[0]) return true;
    const char *d = cookie_domain;
    if (d[0] == '.') d++;
    size_t dl = strlen(d), hl = strlen(host);
    if (hl < dl) return false;
    if (!xb_strieq(host + (hl - dl), d)) return false;
    if (hl == dl) return true;
    return host[hl - dl - 1] == '.';
}

void xb_cookies_set(xb_cookies *c, const char *url, const char *set_cookie)
{
    if (!c || !set_cookie) return;
    url_parts up;
    if (url_split(url, &up) != 0) return;

    const char *p = set_cookie;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        if (linelen >= sizeof line) linelen = sizeof line - 1;
        memcpy(line, p, linelen);
        line[linelen] = '\0';

        /* Take the name=value before the first ';'. */
        char *semi = strchr(line, ';');
        cookie_t *k = (cookie_t *)xb_alloc(sizeof *k);
        if (semi) *semi = '\0';
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            xb_str_lcpy(k->name, xb_str_trim(line), sizeof k->name);
            xb_str_lcpy(k->value, xb_str_trim(eq + 1), sizeof k->value);
        }
        /* Attributes. */
        if (semi) {
            char *attr = semi + 1;
            while (attr && *attr) {
                char *next = strchr(attr, ';');
                if (next) *next = '\0';
                char *s = xb_str_trim(attr);
                if (strncasecmp(s, "domain=", 7) == 0) xb_str_lcpy(k->domain, xb_str_trim(s + 7), sizeof k->domain);
                else if (strncasecmp(s, "path=", 5) == 0) xb_str_lcpy(k->path, xb_str_trim(s + 5), sizeof k->path);
                else if (strncasecmp(s, "secure", 6) == 0) k->secure = true;
                attr = next ? next + 1 : NULL;
            }
        }
        if (k->domain[0] == '\0') xb_str_lcpy(k->domain, up.host, sizeof k->domain);

        /* Replace an existing cookie with the same name+domain. */
        cookie_t **pp = &c->head;
        bool replaced = false;
        while (*pp) {
            if (xb_streq((*pp)->name, k->name) && xb_strieq((*pp)->domain, k->domain)) {
                cookie_t *old = *pp;
                *pp = old->next;
                xb_free(old);
                replaced = true;
                break;
            }
            pp = &(*pp)->next;
        }
        k->next = c->head;
        c->head = k;
        c->count++;
        (void)replaced;

        p = eol ? eol + 1 : NULL;
        if ((size_t)(c->count) > 512) break;
    }
}

const char *xb_cookies_header(xb_cookies *c, const char *url)
{
    if (!c) return "";
    url_parts up;
    if (url_split(url, &up) != 0) return "";
    c->hdr[0] = '\0';
    size_t used = 0;
    for (cookie_t *k = c->head; k; k = k->next) {
        if (!domain_match(k->domain, up.host)) continue;
        if (k->secure && !xb_streq(up.scheme, "https")) continue;
        int n = snprintf(c->hdr + used, sizeof(c->hdr) - used, "%s%s=%s",
                         used ? "; " : "", k->name, k->value);
        if (n < 0 || (size_t)n >= sizeof(c->hdr) - used) break;
        used += (size_t)n;
    }
    return c->hdr;
}

int xb_cookies_count(xb_cookies *c)
{
    if (!c) return 0;
    int n = 0;
    for (cookie_t *k = c->head; k; k = k->next) n++;
    return n;
}

/* --------------------------------------------------------------- request -- */

static int parse_status_line(const char *line, int *status)
{
    if (strncmp(line, "HTTP/", 5) != 0) return -1;
    const char *sp = strchr(line, ' ');
    if (!sp) return -1;
    *status = atoi(sp + 1);
    return 0;
}

static char *find_header_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return (char *)(buf + i + 4);
    for (size_t i = 0; i + 1 < len; i++)
        if (buf[i] == '\n' && buf[i+1] == '\n')
            return (char *)(buf + i + 2);
    return NULL;
}

static void header_value(const char *headers, const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    size_t nl = strlen(name);
    const char *p = headers;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > nl + 1 && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = linelen - (size_t)(v - p);
            while (vl && (v[vl-1] == '\r' || v[vl-1] == ' ')) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            /* Handle repeated headers such as Set-Cookie: keep the first. */
            return;
        }
        p = eol ? eol + 1 : p + linelen;
    }
}

/* Collect every Set-Cookie line, newline separated. */
static void all_set_cookies(const char *headers, char *out, size_t cap)
{
    size_t used = 0;
    out[0] = '\0';
    const char *p = headers;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > 11 && strncasecmp(p, "set-cookie:", 11) == 0) {
            const char *v = p + 11;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = linelen - (size_t)(v - p);
            while (vl && (v[vl-1] == '\r' || v[vl-1] == ' ')) vl--;
            if (used + vl + 2 < cap) {
                if (used) out[used++] = '\n';
                memcpy(out + used, v, vl);
                used += vl;
                out[used] = '\0';
            }
        }
        p = eol ? eol + 1 : p + linelen;
    }
}

static int parse_chunked(const char *body, size_t len, xb_buf *out)
{
    size_t i = 0;
    while (i < len) {
        /* chunk size line */
        size_t start = i;
        while (i < len && body[i] != '\n') i++;
        if (i >= len) return -1;
        char sizebuf[32];
        size_t sl = i - start;
        if (sl >= sizeof sizebuf) return -1;
        memcpy(sizebuf, body + start, sl);
        sizebuf[sl] = '\0';
        char *semi = strchr(sizebuf, ';');
        if (semi) *semi = '\0';
        long chunk = strtol(sizebuf, NULL, 16);
        i++;  /* skip \n */
        if (chunk < 0) return -1;
        if (chunk == 0) return 0;
        if (i + (size_t)chunk > len) return -1;
        xb_buf_append(out, body + i, (size_t)chunk);
        i += (size_t)chunk;
        if (i < len && body[i] == '\r') i++;
        if (i < len && body[i] == '\n') i++;
    }
    return 0;
}

/* Last-resort TLS path: drive an external curl. Chosen only when the build has
 * no TLS and an https:// URL was requested. */
static int http_via_curl(const xb_http_req *req, xb_http_res *r)
{
    char *curl = xb_which("curl");
    if (!curl) {
        snprintf(r->error, sizeof r->error,
                 "https requires a TLS provider; this build has none and curl is not installed");
        return -1;
    }
    const char *argv[32];
    int n = 0;
    char timeoutbuf[32];
    snprintf(timeoutbuf, sizeof timeoutbuf, "%d", (req->timeout_ms > 0 ? req->timeout_ms : 20000) / 1000);
    argv[n++] = curl;
    argv[n++] = "-sS";
    argv[n++] = "-L";
    argv[n++] = "--max-time";
    argv[n++] = timeoutbuf;
    argv[n++] = "-w";
    argv[n++] = "\n%{http_code}";
    if (req->user_agent) { argv[n++] = "-A"; argv[n++] = req->user_agent; }
    if (req->body && req->method && xb_strieq(req->method, "POST")) {
        argv[n++] = "-X"; argv[n++] = "POST";
        argv[n++] = "--data-binary";
        argv[n++] = req->body;
    }
    for (int i = 0; req->headers && req->headers[i] && n < 26; i++) {
        argv[n++] = "-H";
        argv[n++] = req->headers[i];
    }
    argv[n++] = req->url;
    argv[n] = NULL;

    xb_proc_opts opts;
    memset(&opts, 0, sizeof opts);
    char err[128];
    xb_process *p = xb_proc_spawn(argv, &opts, err, sizeof err);
    xb_free(curl);
    if (!p) { snprintf(r->error, sizeof r->error, "curl spawn: %s", err); return -1; }

    xb_buf raw;
    xb_buf_init(&raw);
    char tmp[4096];
    int64_t got;
    while ((got = xb_read_full((xb_sock_t)(uintptr_t)xb_proc_stdout_fd(p), tmp, sizeof tmp)) > 0) {
        xb_buf_append(&raw, tmp, (size_t)got);
        if (raw.len > (req->max_bytes ? req->max_bytes : XB_HTTP_DEFAULT_MAX)) break;
    }
    xb_proc_wait(p, 20000);
    xb_proc_close(p);

    /* The trailing line is the status code. */
    char *text = xb_buf_steal(&raw);
    size_t tl = strlen(text);
    int status = 0;
    size_t cut = tl;
    for (size_t i = tl; i > 0; i--) {
        if (text[i-1] == '\n') { status = atoi(text + i); cut = i - 1; break; }
    }
    if (status == 0) {
        snprintf(r->error, sizeof r->error, "curl produced no parseable response");
        xb_free(text);
        return -1;
    }
    r->status = status;
    r->body = (char *)xb_alloc(cut + 1);
    memcpy(r->body, text, cut);
    r->body[cut] = '\0';
    r->body_len = cut;
    xb_str_lcpy(r->final_url, req->url, sizeof r->final_url);
    xb_free(text);
    return 0;
}

int xb_http_do(const xb_http_req *req, xb_cookies *jar, xb_http_res *r)
{
    memset(r, 0, sizeof *r);
    if (!req || !req->url) { snprintf(r->error, sizeof r->error, "no url"); return -1; }

    if (xb_str_has_prefix(req->url, "https://") && !xb_http_tls_available())
        return http_via_curl(req, r);
    if (xb_str_has_prefix(req->url, "https://")) {
        /* A TLS-capable build would go here; without one we still route through
         * the helper so behaviour is identical, only the client differs. */
        return http_via_curl(req, r);
    }

    url_parts up;
    if (url_split(req->url, &up) != 0) {
        snprintf(r->error, sizeof r->error, "unparseable url: %s", req->url);
        return -1;
    }

    xb_endpoint ep;
    memset(&ep, 0, sizeof ep);
    ep.kind = XB_EP_TCP;
    xb_str_lcpy(ep.host, up.host, sizeof ep.host);
    ep.port = up.port;

    char cerr[256];
    xb_sock_t s = xb_connect(&ep, req->timeout_ms > 0 ? req->timeout_ms : 15000, cerr, sizeof cerr);
    if (s == XB_SOCK_INVALID) {
        snprintf(r->error, sizeof r->error, "%s", cerr);
        return -1;
    }
    if (req->timeout_ms > 0) xb_sock_set_recv_timeout(s, req->timeout_ms);

    const char *method = req->method ? req->method : "GET";
    const char *ua = req->user_agent ? req->user_agent : "xbridge/1.0";

    xb_buf head;
    xb_buf_init(&head);
    char line[2560];
    snprintf(line, sizeof line, "%s %s HTTP/1.1\r\n", method, up.path);
    xb_buf_append_str(&head, line);
    {
        bool default_port = (xb_streq(up.scheme, "http") && up.port == 80) ||
                            (xb_streq(up.scheme, "https") && up.port == 443);
        if (default_port) snprintf(line, sizeof line, "Host: %s\r\n", up.host);
        else              snprintf(line, sizeof line, "Host: %s:%d\r\n", up.host, up.port);
        xb_buf_append_str(&head, line);
    }
    snprintf(line, sizeof line, "User-Agent: %s\r\n", ua);
    xb_buf_append_str(&head, line);
    xb_buf_append_str(&head, "Accept: */*\r\n");
    xb_buf_append_str(&head, "Accept-Encoding: identity\r\n");
    xb_buf_append_str(&head, "Connection: close\r\n");
    const char *ck = xb_cookies_header(jar, req->url);
    if (ck && ck[0]) {
        xb_buf_append_str(&head, "Cookie: ");
        xb_buf_append_str(&head, ck);
        xb_buf_append_str(&head, "\r\n");
    }
    for (int i = 0; req->headers && req->headers[i]; i++) {
        xb_buf_append_str(&head, req->headers[i]);
        xb_buf_append_str(&head, "\r\n");
    }
    if (req->body && req->body_len) {
        snprintf(line, sizeof line, "Content-Length: %zu\r\n", req->body_len);
        xb_buf_append_str(&head, line);
        xb_buf_append_str(&head, "Content-Type: ");
        xb_buf_append_str(&head, req->content_type ? req->content_type
                                                   : "application/x-www-form-urlencoded");
        xb_buf_append_str(&head, "\r\n");
    } else if (xb_strieq(method, "POST")) {
        xb_buf_append_str(&head, "Content-Length: 0\r\n");
    }
    xb_buf_append_str(&head, "\r\n");

    int rc = -1;
    size_t cap = req->max_bytes ? req->max_bytes : XB_HTTP_DEFAULT_MAX;
    xb_buf raw;
    xb_buf_init(&raw);

    if (xb_write_full(s, head.data, head.len) != (int64_t)head.len) {
        snprintf(r->error, sizeof r->error, "write failed");
        goto cleanup;
    }
    if (req->body && req->body_len &&
        xb_write_full(s, req->body, req->body_len) != (int64_t)req->body_len) {
        snprintf(r->error, sizeof r->error, "body write failed");
        goto cleanup;
    }

    {
        char tmp[16384];
        int64_t got;
        while (raw.len < cap) {
            got = xb_read_full(s, tmp, sizeof tmp);
            if (got <= 0) break;
            xb_buf_append(&raw, tmp, (size_t)got);
        }
        if (raw.len == 0) { snprintf(r->error, sizeof r->error, "empty response"); goto cleanup; }
    }

    {
        char *body_start = find_header_end((const char *)raw.data, raw.len);
        if (!body_start) {
            snprintf(r->error, sizeof r->error, "no complete HTTP header block");
            goto cleanup;
        }
        size_t head_len = (size_t)(body_start - (char *)raw.data);
        char *headers = (char *)xb_alloc(head_len + 1);
        memcpy(headers, raw.data, head_len);
        headers[head_len] = '\0';

        char statusline[256];
        header_value(headers, "HTTP", statusline, sizeof statusline); /* not a header; fallback below */
        char *first_nl = strchr(headers, '\n');
        if (first_nl) *first_nl = '\0';
        if (parse_status_line(headers, &r->status) != 0) {
            snprintf(r->error, sizeof r->error, "bad status line");
            xb_free(headers);
            goto cleanup;
        }
        if (first_nl) *first_nl = '\n';

        char te[64], clen[64];
        header_value(headers, "Transfer-Encoding", te, sizeof te);
        header_value(headers, "Content-Length", clen, sizeof clen);
        header_value(headers, "Content-Type", r->content_type, sizeof r->content_type);

        char setc[4096];
        all_set_cookies(headers, setc, sizeof setc);
        if (jar && setc[0]) xb_cookies_set(jar, req->url, setc);

        size_t body_len = raw.len - head_len;
        const char *body = body_start;
        xb_buf decoded;
        xb_buf_init(&decoded);
        if (strstr(te, "chunked")) {
            if (parse_chunked(body, body_len, &decoded) != 0 && decoded.len == 0) {
                /* Keep whatever we decoded; a truncated stream is still useful. */
                if (decoded.len == 0) xb_buf_append(&decoded, body, body_len);
            }
            r->body = xb_buf_steal(&decoded);
            r->body_len = strlen(r->body);
        } else {
            size_t want = body_len;
            if (clen[0]) {
                long declared = strtol(clen, NULL, 10);
                if (declared >= 0 && (size_t)declared < want) want = (size_t)declared;
            }
            r->body = (char *)xb_alloc(want + 1);
            memcpy(r->body, body, want);
            r->body[want] = '\0';
            r->body_len = want;
        }
        xb_buf_free(&decoded);
        xb_str_lcpy(r->final_url, req->url, sizeof r->final_url);
        xb_free(headers);
        rc = 0;
    }

cleanup:
    xb_buf_free(&raw);
    xb_buf_free(&head);
    xb_sock_close(s);
    if (rc != 0) {
        xb_free(r->body);
        r->body = NULL;
        r->body_len = 0;
    }
    return rc;
}

void xb_http_res_free(xb_http_res *r)
{
    if (!r) return;
    xb_free(r->body);
    r->body = NULL;
    r->body_len = 0;
}
