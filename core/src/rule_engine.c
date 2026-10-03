/* rule_engine.c — declarative extraction rules executed in-process.
 *
 * Rule shape (a superset of what Legado book sources use):
 *
 *   {
 *     "sourceName": "Example",
 *     "sourceUrl":  "https://example.org",
 *     "searchUrl":  "/search?q={{key}}&page={{page}}",
 *     "ruleSearch": { "bookList": "div.book", "name": "h3@text",
 *                      "author": "span.author@text", "bookUrl": "a@href",
 *                      "coverUrl": "img@src" },
 *     "ruleToc":    { "chapterList": "ul.chapters li", "chapterName": "a@text",
 *                      "chapterUrl": "a@href" },
 *     "ruleContent":{ "content": "div#content@html" }
 *   }
 *
 * Extraction strings support:
 *   selector            -> inner text of the first match
 *   selector@attr       -> attribute of the first match
 *   selector@text/html  -> text / inner html
 *   A B                 -> descendant chain
 *   {A,B,C}             -> alternatives, first non-empty wins
 *   ##regex##replacement -> regex refine (POSIX ERE when available)
 */
#include "bridge/rule_engine.h"
#include "bridge/server.h"
#include "bridge/html.h"
#include "bridge/http.h"
#include "bridge/buf.h"
#include "bridge/util.h"
#include "bridge/log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#  include <regex.h>
#  define XB_HAVE_REGEX 1
#endif

#define XB_RULE_MAX 64

typedef struct {
    char *source_id;
    char *name;
    char *base_url;
    char *search_url;
    char *search_method;
    char *search_body;

    char *search_list, *search_name, *search_author, *search_url_rule,
         *search_cover, *search_intro, *search_kind, *search_last;
    char *search_next;

    char *toc_list, *toc_name, *toc_url, *toc_next;
    char *content_rule, *content_next;

    char *info_name, *info_author, *info_cover, *info_intro, *info_toc_url;

    char *popular_url;
    char *latest_url;

    char *header;      /* extra request header, "Name: value" */
    char *charset;
    xb_cookies *jar;
} rule_source;

static rule_source *g_rules[XB_RULE_MAX];
static size_t g_rule_count = 0;
static xb_mutex_t g_lock;
static bool g_lock_ready = false;

void xb_rule_engine_reset(void);

/* ------------------------------------------------------------- helpers -- */

static void ensure_lock(void)
{
    if (!g_lock_ready) { XB_MUTEX_INIT(&g_lock); g_lock_ready = true; }
}

static char *dup_or_null(const xb_json *o, const char *k)
{
    const char *v = xb_json_obj_str(o, k, NULL);
    return v ? xb_strdup(v) : NULL;
}

/* A rule file may hold one source or many. One -> the id is the extension id.
 * Many -> ids are "ext#0", "ext#1", ... and a lookup by extension id returns the
 * first, which is what a single-source caller means. */
static rule_source *rule_find(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < g_rule_count; i++)
        if (g_rules[i] && xb_streq(g_rules[i]->source_id, id)) return g_rules[i];
    size_t idlen = strlen(id);
    for (size_t i = 0; i < g_rule_count; i++) {
        if (!g_rules[i]) continue;
        const char *sid = g_rules[i]->source_id;
        if (strncmp(sid, id, idlen) == 0 && sid[idlen] == '#') return g_rules[i];
    }
    return NULL;
}

static void rule_free(rule_source *r)
{
    if (!r) return;
    xb_free(r->source_id); xb_free(r->name); xb_free(r->base_url);
    xb_free(r->search_url); xb_free(r->search_method); xb_free(r->search_body);
    xb_free(r->search_list); xb_free(r->search_name); xb_free(r->search_author);
    xb_free(r->search_url_rule); xb_free(r->search_cover); xb_free(r->search_intro);
    xb_free(r->search_kind); xb_free(r->search_last); xb_free(r->search_next);
    xb_free(r->toc_list); xb_free(r->toc_name); xb_free(r->toc_url); xb_free(r->toc_next);
    xb_free(r->content_rule); xb_free(r->content_next);
    xb_free(r->info_name); xb_free(r->info_author); xb_free(r->info_cover);
    xb_free(r->info_intro); xb_free(r->info_toc_url);
    xb_free(r->popular_url); xb_free(r->latest_url);
    xb_free(r->header); xb_free(r->charset);
    xb_cookies_free(r->jar);
    xb_free(r);
}

/* --------------------------------------------------------- HTTP plumbing -- */

typedef struct {
    char *text;
    size_t len;
    char  url[1024];
    int   status;
} fetch_result;

/* Expand {{key}}, {{page}} and the trailing ",{...}" directive block that
 * Legado sources use for method/body/charset. */
char *xb_rule_expand(const char *template_, const char *key, int page,
                     const char *base, char **out_method, char **out_body)
{
    if (out_method) *out_method = NULL;
    if (out_body) *out_body = NULL;

    char *t = xb_strdup(template_ ? template_ : "");

    /* Directive block: url,{method},{body},{charset} */
    char *directives = NULL;
    {
        /* Find a trailing ",{" that is not inside the query string's own braces. */
        char *p = strstr(t, ",{");
        if (p) { directives = xb_strdup(p); *p = '\0'; }
    }
    if (directives) {
        char fields[4][512];
        for (int i = 0; i < 4; i++) fields[i][0] = '\0';
        const char *p = directives + 1;   /* skip the comma */
        for (int i = 0; i < 4 && *p; i++) {
            if (*p == '{') {
                const char *close = strchr(p, '}');
                if (!close) break;
                size_t l = (size_t)(close - p - 1);
                if (l >= sizeof fields[i]) l = sizeof fields[i] - 1;
                memcpy(fields[i], p + 1, l);
                fields[i][l] = '\0';
                p = close + 1;
                if (*p == ',') p++;
            } else {
                const char *comma = strchr(p, ',');
                size_t l = comma ? (size_t)(comma - p) : strlen(p);
                if (l >= sizeof fields[i]) l = sizeof fields[i] - 1;
                memcpy(fields[i], p, l);
                fields[i][l] = '\0';
                p = comma ? comma + 1 : p + l;
            }
        }
        if (fields[0][0] && out_method) *out_method = xb_strdup(fields[0]);
        if (fields[1][0] && out_body)   *out_body = xb_strdup(fields[1]);
    }
    xb_free(directives);

    /* Placeholder substitution. */
    xb_buf b;
    xb_buf_init(&b);
    for (const char *p = t; *p; ) {
        if (p[0] == '{' && p[1] == '{') {
            const char *close = strstr(p, "}}");
            if (!close) { xb_buf_append_byte(&b, (uint8_t)*p++); continue; }
            size_t nl = (size_t)(close - p - 2);
            char name[64];
            if (nl < sizeof name) {
                memcpy(name, p + 2, nl);
                name[nl] = '\0';
                if (xb_strieq(name, "key") || xb_strieq(name, "searchKey") ||
                    xb_strieq(name, "keyword")) {
                    char enc[1024];
                    xb_url_encode(key ? key : "", enc, sizeof enc);
                    xb_buf_append_str(&b, enc);
                } else if (xb_strieq(name, "page")) {
                    char nbuf[16];
                    snprintf(nbuf, sizeof nbuf, "%d", page);
                    xb_buf_append_str(&b, nbuf);
                } else if (xb_strieq(name, "base") || xb_strieq(name, "baseUrl")) {
                    xb_buf_append_str(&b, base ? base : "");
                } else {
                    xb_buf_append(&b, p, (size_t)(close + 2 - p));
                }
                p = close + 2;
                continue;
            }
        }
        if (p[0] == '{' && xb_strieq(p, "{page}")) {
            char nbuf[16];
            snprintf(nbuf, sizeof nbuf, "%d", page);
            xb_buf_append_str(&b, nbuf);
            p += 6;
            continue;
        }
        xb_buf_append_byte(&b, (uint8_t)*p++);
    }
    xb_free(t);

    char *relative = xb_buf_steal(&b);
    char *absolute = (char *)xb_alloc(2048);
    if (xb_url_is_absolute(relative)) xb_str_lcpy(absolute, relative, 2048);
    else xb_url_resolve(base, relative, absolute, 2048);
    xb_free(relative);
    return absolute;
}

static int fetch(rule_source *r, const char *url, const char *method,
                 const char *body, const char *referer, fetch_result *out)
{
    out->text = NULL;
    out->len = 0;
    out->status = 0;
    xb_str_lcpy(out->url, url, sizeof out->url);

    const char *hdrs[4];
    int nh = 0;
    if (r->header && r->header[0]) hdrs[nh++] = r->header;
    if (referer && referer[0]) {
        static char refbuf[1200];
        snprintf(refbuf, sizeof refbuf, "Referer: %s", referer);
        hdrs[nh++] = refbuf;
    }
    hdrs[nh] = NULL;

    xb_http_req req;
    memset(&req, 0, sizeof req);
    req.method = method ? method : "GET";
    req.url = url;
    req.body = body;
    req.body_len = body ? strlen(body) : 0;
    req.headers = hdrs;
    req.timeout_ms = 20000;
    req.max_bytes = 16u * 1024 * 1024;
    req.user_agent = "Mozilla/5.0 (X11; Linux x86_64) xbridge/1.0";

    xb_http_res res;
    int rc = xb_http_do(&req, r->jar, &res);
    if (rc != 0) {
        XB_DEBUG("rule fetch failed for %s: %s", url, res.error);
        return -1;
    }
    out->status = res.status;
    out->text = res.body;
    out->len = res.body_len;
    res.body = NULL;   /* take ownership */
    xb_str_lcpy(out->url, res.final_url[0] ? res.final_url : url, sizeof out->url);
    xb_http_res_free(&res);
    return 0;
}

/* -------------------------------------------------------------- extract -- */

#ifdef XB_HAVE_REGEX
static char *regex_refine(const char *input, const char *pattern, const char *repl)
{
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_ICASE) != 0) return xb_strdup(input);
    regmatch_t m[10];
    char *out = NULL;
    if (regexec(&re, input, 10, m, 0) == 0) {
        xb_buf b;
        xb_buf_init(&b);
        if (m[0].rm_so > 0) xb_buf_append(&b, input, (size_t)m[0].rm_so);
        if (repl) {
            for (const char *p = repl; *p; p++) {
                if (*p == '$' && p[1] >= '0' && p[1] <= '9') {
                    int gi = p[1] - '0';
                    if (gi < 10 && m[gi].rm_so >= 0)
                        xb_buf_append(&b, input + m[gi].rm_so,
                                      (size_t)(m[gi].rm_eo - m[gi].rm_so));
                    p++;
                } else if (*p == '\\' && p[1] == 'n') {
                    xb_buf_append_byte(&b, '\n');
                    p++;
                } else {
                    xb_buf_append_byte(&b, (uint8_t)*p);
                }
            }
        }
        if (m[0].rm_eo < (regoff_t)strlen(input))
            xb_buf_append_str(&b, input + m[0].rm_eo);
        out = xb_buf_steal(&b);
    } else {
        out = xb_strdup("");
    }
    regfree(&re);
    return out;
}
#endif

/* Split "{a,b,c}" alternatives respecting nested braces. */
static size_t split_alternatives(const char *rule, char **out, size_t max)
{
    size_t n = 0;
    const char *p = rule;
    int depth = 0;
    const char *start = rule;
    for (; *p && n < max; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                size_t l = (size_t)(p - start - 1);
                if (l > 0) out[n++] = xb_strndup(start + 1, l);
                start = p + 1;
                /* skip an optional following '}' closing the whole group */
                if (start[0] == '}') start++;
                break;
            }
        }
    }
    if (n == 0) return 0;
    return n;
}

/* Evaluate one rule expression against a node (may be NULL for document scope). */
static char *extract(const hdoc *doc, const hnode *scope, const char *rule)
{
    if (!rule || !rule[0]) return xb_strdup("");

    /* Alternative groups: {a,b} */
    if (rule[0] == '{') {
        char *alts[16];
        size_t n = split_alternatives(rule, alts, 16);
        if (n) {
            for (size_t i = 0; i < n; i++) {
                char *v = extract(doc, scope, alts[i]);
                xb_free(alts[i]);
                if (v && v[0]) {
                    for (size_t j = i + 1; j < n; j++) xb_free(alts[j]);
                    return v;
                }
                xb_free(v);
            }
            return xb_strdup("");
        }
    }

    /* Regex refine: selector##pattern##replacement */
    char *regex_part = NULL;
    char *work = xb_strdup(rule);
    char *hash = strstr(work, "##");
    if (hash) {
        regex_part = xb_strdup(hash + 2);
        *hash = '\0';
    }

    const hnode *start = scope ? scope : (doc ? doc->root : NULL);
    char *value = NULL;
    if (work[0] == '\0' || xb_streq(work, "@")) {
        /* No selector part: operate on the scope's own text. */
        value = start ? xb_html_text(start) : xb_strdup("");
    } else {
        hsel *sel = xb_select_node(start, work);
        if (sel) {
            value = xb_sel_string(sel, 0);
            xb_sel_free(sel);
        }
    }
    if (!value) value = xb_strdup("");

#ifdef XB_HAVE_REGEX
    if (regex_part) {
        char *pat = regex_part;
        char *repl = strstr(regex_part, "##");
        if (repl) { *repl = '\0'; repl += 2; }
        char *refined = regex_refine(value, pat, repl);
        xb_free(value);
        value = refined;
    }
#else
    (void)regex_part;
#endif

    xb_free(regex_part);
    xb_free(work);
    xb_str_trim(value);
    return value;
}

/* Resolve a possibly-relative URL rule result against the page URL. */
static char *extract_url(const hdoc *doc, const hnode *scope, const char *rule,
                         const char *page_url)
{
    char *raw = extract(doc, scope, rule);
    if (!raw[0]) return raw;
    char *abs = (char *)xb_alloc(2048);
    if (xb_url_is_absolute(raw)) xb_str_lcpy(abs, raw, 2048);
    else xb_url_resolve(page_url, raw, abs, 2048);
    xb_free(raw);
    return abs;
}

/* ------------------------------------------------------------ list build -- */

static void append_media(xb_jsonw *w, const char *name, const char *url,
                         const char *cover, const char *author, const char *desc)
{
    xb_jw_obj_begin(w);
    xb_jw_kv_str(w, "name", name ? name : "");
    xb_jw_kv_str(w, "url", url ? url : "");
    if (cover && cover[0]) xb_jw_kv_str(w, "cover", cover);
    if (author && author[0]) xb_jw_kv_str(w, "author", author);
    if (desc && desc[0]) xb_jw_kv_str(w, "description", desc);
    xb_jw_obj_end(w);
}

static int build_list(rule_source *r, const char *url, const char *method,
                      const char *body, const char *list_rule,
                      const char *name_rule, const char *url_rule,
                      const char *cover_rule, const char *author_rule,
                      const char *intro_rule, const char *next_rule,
                      int page, xb_job_result *res)
{
    fetch_result fr;
    if (fetch(r, url, method, body, r->base_url, &fr) != 0) {
        res->error_code = XB_ERR_NETWORK;
        res->error_msg = xb_strdup("network request failed");
        return -1;
    }
    if (fr.status >= 400) {
        char msg[128];
        snprintf(msg, sizeof msg, "upstream returned HTTP %d", fr.status);
        res->error_code = XB_ERR_NETWORK;
        res->error_msg = xb_strdup(msg);
        xb_free(fr.text);
        return -1;
    }

    hdoc *doc = xb_html_parse(fr.text ? fr.text : "", fr.len);
    hsel *items = xb_select(doc, list_rule ? list_rule : "body *");
    if (!items) items = xb_select(doc, "body");

    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_key(&w, "list");
    xb_jw_arr_begin(&w);

    size_t emitted = 0;
    if (items) {
        for (size_t i = 0; i < items->n; i++) {
            const hnode *node = items->nodes[i];
            char *name = extract(doc, node, name_rule);
            char *link = extract_url(doc, node, url_rule, fr.url);
            if (!name[0] && !link[0]) { xb_free(name); xb_free(link); continue; }
            char *cover = extract_url(doc, node, cover_rule, fr.url);
            char *author = extract(doc, node, author_rule);
            char *intro = extract(doc, node, intro_rule);
            append_media(&w, name, link, cover, author, intro);
            xb_free(name); xb_free(link); xb_free(cover); xb_free(author); xb_free(intro);
            emitted++;
        }
    }
    xb_jw_arr_end(&w);

    /* has_next: an explicit rule wins; otherwise assume there is another page
     * when the page produced a full set of results. */
    bool has_next = false;
    if (next_rule && next_rule[0]) {
        char *nxt = extract(doc, NULL, next_rule);
        has_next = nxt && nxt[0] && !xb_strieq(nxt, "false");
        xb_free(nxt);
    } else {
        has_next = emitted > 0;
    }
    xb_jw_kv_bool(&w, "has_next", has_next);
    xb_jw_kv_int(&w, "page", page);
    xb_jw_kv_int(&w, "total", (int64_t)emitted);
    xb_jw_obj_end(&w);

    xb_sel_free(items);
    xb_html_free(doc);
    xb_free(fr.text);

    res->ok = true;
    res->result = xb_buf_steal(&w.buf);
    return 0;
}

/* --------------------------------------------------------------- detail -- */

static int build_detail(rule_source *r, const char *media_url, xb_job_result *res)
{
    fetch_result fr;
    if (fetch(r, media_url, "GET", NULL, r->base_url, &fr) != 0) {
        res->error_code = XB_ERR_NETWORK;
        res->error_msg = xb_strdup("network request failed");
        return -1;
    }
    hdoc *doc = xb_html_parse(fr.text ? fr.text : "", fr.len);

    char *name = extract(doc, NULL, r->info_name);
    char *author = extract(doc, NULL, r->info_author);
    char *cover = extract_url(doc, NULL, r->info_cover, fr.url);
    char *intro = extract(doc, NULL, r->info_intro);
    char *toc_url = r->info_toc_url ? extract_url(doc, NULL, r->info_toc_url, fr.url) : NULL;

    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "name", name);
    xb_jw_kv_str(&w, "url", media_url);
    if (cover && cover[0]) xb_jw_kv_str(&w, "cover", cover);
    if (author && author[0]) xb_jw_kv_str(&w, "author", author);
    if (intro && intro[0]) xb_jw_kv_str(&w, "description", intro);
    const char *toc_page = (toc_url && toc_url[0]) ? toc_url : fr.url;

    /* Episodes/chapters from the table of contents. */
    xb_jw_key(&w, "episodes");
    xb_jw_arr_begin(&w);
    int chapter_count = 0;
    if (r->toc_list && r->toc_list[0]) {
        fetch_result tc;
        int fetched = 0;
        if (xb_streq(toc_page, fr.url)) {
            tc = fr;
            fetched = 1;
        } else {
            fetched = (fetch(r, toc_page, "GET", NULL, fr.url, &tc) == 0);
        }
        if (fetched) {
            hdoc *tdoc = xb_html_parse(tc.text ? tc.text : "", tc.len);
            hsel *chapters = xb_select(tdoc, r->toc_list);
            if (chapters) {
                for (size_t i = 0; i < chapters->n; i++) {
                    char *cn = extract(tdoc, chapters->nodes[i], r->toc_name);
                    char *cu = extract_url(tdoc, chapters->nodes[i], r->toc_url, tc.url);
                    if (!cn[0] && !cu[0]) { xb_free(cn); xb_free(cu); continue; }
                    xb_jw_obj_begin(&w);
                    xb_jw_kv_str(&w, "name", cn);
                    xb_jw_kv_str(&w, "url", cu);
                    xb_jw_kv_int(&w, "index", (int64_t)i);
                    xb_jw_obj_end(&w);
                    xb_free(cn); xb_free(cu);
                    chapter_count++;
                }
                xb_sel_free(chapters);
            }
            xb_html_free(tdoc);
            if (!fetched) { /* fallthrough */ }
            if (!xb_streq(toc_page, fr.url)) xb_free(tc.text);
        }
    }
    xb_jw_arr_end(&w);
    xb_jw_kv_int(&w, "episode_count", chapter_count);
    xb_jw_obj_end(&w);

    xb_free(name); xb_free(author); xb_free(cover); xb_free(intro); xb_free(toc_url);
    xb_html_free(doc);
    xb_free(fr.text);

    res->ok = true;
    res->result = xb_buf_steal(&w.buf);
    return 0;
}

/* ------------------------------------------------------------- chapters -- */

static int build_chapters(rule_source *r, const char *toc_url, xb_job_result *res)
{
    fetch_result fr;
    if (fetch(r, toc_url, "GET", NULL, r->base_url, &fr) != 0) {
        res->error_code = XB_ERR_NETWORK;
        res->error_msg = xb_strdup("network request failed");
        return -1;
    }
    hdoc *doc = xb_html_parse(fr.text ? fr.text : "", fr.len);
    hsel *chapters = xb_select(doc, r->toc_list ? r->toc_list : "a");

    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_key(&w, "list");
    xb_jw_arr_begin(&w);
    int n = 0;
    if (chapters) {
        for (size_t i = 0; i < chapters->n; i++) {
            char *cn = extract(doc, chapters->nodes[i], r->toc_name);
            char *cu = extract_url(doc, chapters->nodes[i], r->toc_url, fr.url);
            if (!cn[0] && !cu[0]) { xb_free(cn); xb_free(cu); continue; }
            append_media(&w, cn, cu, NULL, NULL, NULL);
            xb_free(cn); xb_free(cu);
            n++;
        }
    }
    xb_jw_arr_end(&w);
    xb_jw_kv_int(&w, "total", n);
    xb_jw_obj_end(&w);
    xb_sel_free(chapters);
    xb_html_free(doc);
    xb_free(fr.text);

    res->ok = true;
    res->result = xb_buf_steal(&w.buf);
    return 0;
}

/* -------------------------------------------------------------- content -- */

static int build_content(rule_source *r, const char *chapter_url, xb_job_result *res)
{
    fetch_result fr;
    if (fetch(r, chapter_url, "GET", NULL, r->base_url, &fr) != 0) {
        res->error_code = XB_ERR_NETWORK;
        res->error_msg = xb_strdup("network request failed");
        return -1;
    }
    hdoc *doc = xb_html_parse(fr.text ? fr.text : "", fr.len);
    char *content = extract(doc, NULL, r->content_rule ? r->content_rule : "body@html");

    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "content", content);
    xb_jw_kv_str(&w, "url", chapter_url);
    xb_jw_kv_int(&w, "length", (int64_t)strlen(content));
    if (r->content_next) {
        char *nxt = extract_url(doc, NULL, r->content_next, fr.url);
        if (nxt && nxt[0]) xb_jw_kv_str(&w, "next_url", nxt);
        xb_free(nxt);
    }
    xb_jw_obj_end(&w);

    xb_free(content);
    xb_html_free(doc);
    xb_free(fr.text);

    res->ok = true;
    res->result = xb_buf_steal(&w.buf);
    return 0;
}

/* ---------------------------------------------------------------- load -- */

static void load_rule_fields(rule_source *r, const xb_json *src)
{
    r->name = dup_or_null(src, "sourceName");
    if (!r->name) r->name = dup_or_null(src, "name");
    r->base_url = dup_or_null(src, "sourceUrl");
    if (!r->base_url) r->base_url = dup_or_null(src, "baseUrl");
    r->search_url = dup_or_null(src, "searchUrl");
    if (!r->search_url) r->search_url = dup_or_null(src, "ruleSearchUrl");
    r->search_method = dup_or_null(src, "searchMethod");
    r->search_body = dup_or_null(src, "searchBody");
    r->header = dup_or_null(src, "header");
    r->charset = dup_or_null(src, "charset");
    r->popular_url = dup_or_null(src, "rulePopularUrl");
    if (!r->popular_url) r->popular_url = dup_or_null(src, "popularUrl");
    r->latest_url = dup_or_null(src, "ruleLatestUrl");
    if (!r->latest_url) r->latest_url = dup_or_null(src, "latestUrl");

    const xb_json *s = xb_json_get(src, "ruleSearch");
    if (s) {
        r->search_list   = dup_or_null(s, "bookList");
        if (!r->search_list) r->search_list = dup_or_null(s, "list");
        r->search_name   = dup_or_null(s, "name");
        if (!r->search_name) r->search_name = dup_or_null(s, "bookName");
        r->search_author = dup_or_null(s, "author");
        r->search_url_rule = dup_or_null(s, "bookUrl");
        if (!r->search_url_rule) r->search_url_rule = dup_or_null(s, "url");
        r->search_cover  = dup_or_null(s, "coverUrl");
        r->search_intro  = dup_or_null(s, "introduce");
        r->search_kind   = dup_or_null(s, "kind");
        r->search_last   = dup_or_null(s, "lastChapter");
        r->search_next   = dup_or_null(s, "nextPage");
    }
    const xb_json *t = xb_json_get(src, "ruleToc");
    if (t) {
        r->toc_list = dup_or_null(t, "chapterList");
        r->toc_name = dup_or_null(t, "chapterName");
        r->toc_url  = dup_or_null(t, "chapterUrl");
        r->toc_next = dup_or_null(t, "nextTocUrl");
    }
    const xb_json *c = xb_json_get(src, "ruleContent");
    if (c) {
        r->content_rule = dup_or_null(c, "content");
        r->content_next = dup_or_null(c, "nextContentUrl");
    }
    const xb_json *i = xb_json_get(src, "ruleBookInfo");
    if (i) {
        r->info_name   = dup_or_null(i, "name");
        r->info_author = dup_or_null(i, "author");
        r->info_cover  = dup_or_null(i, "coverUrl");
        r->info_intro  = dup_or_null(i, "intro");
        r->info_toc_url= dup_or_null(i, "tocUrl");
    }
    if (!r->info_name)   r->info_name   = dup_or_null(src, "nameRule");
    if (!r->info_intro)  r->info_intro  = dup_or_null(src, "introRule");

    r->jar = xb_cookies_new();
}

int xb_rule_engine_load(const char *source_id, const char *json_text, size_t len,
                        char *err, size_t errcap)
{
    ensure_lock();
    xb_arena arena;
    xb_arena_init(&arena);
    size_t off = 0;
    xb_json *doc = xb_json_parse(&arena, json_text, len, &off);
    if (!doc) {
        snprintf(err, errcap, "rule file is not valid JSON (offset %zu)", off);
        xb_arena_destroy(&arena);
        return -1;
    }

    const xb_json *list = doc;
    if (doc->type == XB_JOBJ) {
        xb_json *wrap = xb_json_get(doc, "sources");
        if (!wrap) wrap = xb_json_get(doc, "bookSources");
        if (!wrap) wrap = xb_json_get(doc, "list");
        if (wrap && wrap->type == XB_JARR) list = wrap;
    }

    size_t count = 0;
    if (list->type == XB_JARR) {
        for (size_t i = 0; i < list->u.arr.n; i++) {
            const xb_json *src = list->u.arr.items[i];
            if (!src || src->type != XB_JOBJ) continue;
            ensure_lock();
            XB_MUTEX_LOCK(&g_lock);
            if (g_rule_count >= XB_RULE_MAX) {
                XB_MUTEX_UNLOCK(&g_lock);
                snprintf(err, errcap, "rule table is full (%d sources)", XB_RULE_MAX);
                xb_arena_destroy(&arena);
                return -1;
            }
            rule_source *r = (rule_source *)xb_alloc(sizeof *r);
            char sid[200];
            snprintf(sid, sizeof sid, "%s#%zu", source_id ? source_id : "rule", i);
            r->source_id = xb_strdup(sid);
            load_rule_fields(r, src);
            g_rules[g_rule_count++] = r;
            XB_MUTEX_UNLOCK(&g_lock);
            count++;
        }
    } else if (list->type == XB_JOBJ) {
        ensure_lock();
        if (g_rule_count >= XB_RULE_MAX) {
            snprintf(err, errcap, "rule table is full");
            xb_arena_destroy(&arena);
            return -1;
        }
        XB_MUTEX_LOCK(&g_lock);
        rule_source *r = (rule_source *)xb_alloc(sizeof *r);
        r->source_id = xb_strdup(source_id ? source_id : "rule");
        load_rule_fields(r, list);
        g_rules[g_rule_count++] = r;
        XB_MUTEX_UNLOCK(&g_lock);
        count = 1;
    }

    xb_arena_destroy(&arena);
    if (count == 0) {
        snprintf(err, errcap, "no source objects found in the rule file");
        return -1;
    }
    return (int)count;
}

int xb_rule_engine_load_file(const char *source_id, const char *path,
                             char *err, size_t errcap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errcap, "cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 32 * 1024 * 1024) {
        fclose(f);
        snprintf(err, errcap, "rule file size %ld is out of range", n);
        return -1;
    }
    char *buf = (char *)xb_alloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    int rc = xb_rule_engine_load(source_id, buf, got, err, errcap);
    xb_free(buf);
    return rc;
}

void xb_rule_engine_unload(const char *source_id)
{
    if (!source_id) return;
    ensure_lock();
    XB_MUTEX_LOCK(&g_lock);
    for (size_t i = 0; i < g_rule_count; i++) {
        if (g_rules[i] && xb_str_has_prefix(g_rules[i]->source_id, source_id)) {
            rule_free(g_rules[i]);
            g_rules[i] = g_rules[g_rule_count - 1];
            g_rules[g_rule_count - 1] = NULL;
            g_rule_count--;
            i--;
        }
    }
    XB_MUTEX_UNLOCK(&g_lock);
}

size_t xb_rule_engine_count(void)
{
    return g_rule_count;
}

void xb_rule_engine_reset(void)
{
    ensure_lock();
    XB_MUTEX_LOCK(&g_lock);
    for (size_t i = 0; i < g_rule_count; i++) rule_free(g_rules[i]);
    g_rule_count = 0;
    XB_MUTEX_UNLOCK(&g_lock);
}

char *xb_rule_engine_describe(const char *source_id)
{
    rule_source *r = rule_find(source_id);
    if (!r) return xb_strdup("null");
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "source_id", r->source_id);
    xb_jw_kv_str(&w, "name", r->name);
    xb_jw_kv_str(&w, "base_url", r->base_url);
    xb_jw_kv_bool(&w, "has_search", r->search_url && r->search_url[0]);
    xb_jw_kv_bool(&w, "has_toc", r->toc_list && r->toc_list[0]);
    xb_jw_kv_bool(&w, "has_content", r->content_rule && r->content_rule[0]);
    xb_jw_obj_end(&w);
    return xb_buf_steal(&w.buf);
}

/* -------------------------------------------------------------- handler -- */

static int rule_engine_run(const char *method, const xb_json *params,
                           xb_job_result *res, rule_source *r)
{
    if (xb_streq(method, "source.search")) {
        const char *query = xb_json_obj_str(params, "query", "");
        int page = (int)xb_json_obj_int(params, "page", 1);
        if (!r->search_url || !r->search_url[0]) {
            res->error_code = XB_ERR_PARAMS;
            res->error_msg = xb_strdup("this source has no searchUrl rule");
            return -1;
        }
        char *method_s = NULL, *body = NULL;
        char *url = xb_rule_expand(r->search_url, query, page, r->base_url, &method_s, &body);
        const char *m = method_s ? method_s : (r->search_method ? r->search_method : "GET");
        char *expanded_body = NULL;
        if (body) {
            expanded_body = xb_rule_expand(body, query, page, r->base_url, NULL, NULL);
        } else if (r->search_body && r->search_body[0]) {
            expanded_body = xb_rule_expand(r->search_body, query, page, r->base_url, NULL, NULL);
        }
        int rc = build_list(r, url, m, expanded_body, r->search_list, r->search_name,
                            r->search_url_rule, r->search_cover, r->search_author,
                            r->search_intro, r->search_next, page, res);
        xb_free(url); xb_free(method_s); xb_free(body); xb_free(expanded_body);
        return rc;
    }

    if (xb_streq(method, "source.getPopular") || xb_streq(method, "source.getLatestUpdates")) {
        int page = (int)xb_json_obj_int(params, "page", 1);
        const char *tpl = xb_streq(method, "source.getPopular") ? r->popular_url : r->latest_url;
        if (!tpl || !tpl[0]) {
            res->error_code = XB_ERR_PARAMS;
            res->error_msg = xb_strdup("this source has no popular/latest rule");
            return -1;
        }
        char *url = xb_rule_expand(tpl, "", page, r->base_url, NULL, NULL);
        int rc = build_list(r, url, "GET", NULL, r->search_list, r->search_name,
                            r->search_url_rule, r->search_cover, r->search_author,
                            r->search_intro, r->search_next, page, res);
        xb_free(url);
        return rc;
    }

    if (xb_streq(method, "source.getDetail")) {
        const xb_json *media = xb_json_obj_get(params, "media");
        const char *url = xb_json_obj_str(media, "url", NULL);
        if (!url) url = xb_json_obj_str(params, "url", NULL);
        if (!url) {
            res->error_code = XB_ERR_PARAMS;
            res->error_msg = xb_strdup("getDetail needs media.url");
            return -1;
        }
        return build_detail(r, url, res);
    }

    if (xb_streq(method, "source.getPageList")) {
        const xb_json *episode = xb_json_obj_get(params, "episode");
        const char *url = xb_json_obj_str(episode, "url", NULL);
        if (!url) url = xb_json_obj_str(params, "url", NULL);
        if (!url) { res->error_code = XB_ERR_PARAMS; res->error_msg = xb_strdup("getPageList needs episode.url"); return -1; }
        return build_chapters(r, url, res);
    }

    if (xb_streq(method, "source.getNovelContent")) {
        const char *url = xb_json_obj_str(params, "id", NULL);
        if (!url) url = xb_json_obj_str(params, "url", NULL);
        if (!url) { res->error_code = XB_ERR_PARAMS; res->error_msg = xb_strdup("getNovelContent needs a chapter id/url"); return -1; }
        return build_content(r, url, res);
    }

    res->error_code = XB_ERR_NO_METHOD;
    res->error_msg = xb_strdup("rule engine does not implement this method");
    return -1;
}

int xb_rule_engine_inproc(void *ud, const char *method, const xb_json *params,
                          xb_job_result *res, xb_chunk_fn on_chunk, void *chunk_ud)
{
    (void)ud; (void)on_chunk; (void)chunk_ud;
    res->ok = false;
    res->error_code = XB_ERR_ENGINE;

    const char *source_id = xb_json_obj_str(params, "source_id", NULL);
    rule_source *r = rule_find(source_id);
    rule_source *owned = NULL;

    if (!r) {
        /* The bridge may pass an inline rule object for ad-hoc evaluation, which
         * is how callers try a rule file before installing it. */
        const xb_json *inline_rule = xb_json_obj_get(params, "_rule");
        if (inline_rule && inline_rule->type == XB_JOBJ) {
            owned = (rule_source *)xb_alloc(sizeof *owned);
            owned->source_id = xb_strdup(source_id ? source_id : "inline");
            load_rule_fields(owned, inline_rule);
            r = owned;
        }
    }
    if (!r) {
        XB_DEBUG("no rule loaded for source_id='%.80s' (registered=%zu)",
                 source_id ? source_id : "(null)", g_rule_count);
        res->error_code = XB_ERR_NOT_FOUND;
        res->error_msg = xb_strdup("no rule source registered for this source_id");
        return -1;
    }

    int rc = rule_engine_run(method, params, res, r);
    if (owned) rule_free(owned);
    return rc;
}
