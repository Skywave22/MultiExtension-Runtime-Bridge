/* bench.c — micro-benchmarks for the hot paths, printed as a table.
 *
 * Every number is measured on the machine that runs `make bench`; nothing is
 * hardcoded. The point is to keep the per-operation cost visible so a change
 * that makes the daemon slower is noticed immediately.
 *
 * Usage: bench [--json] [--iterations N]
 */
#include "bridge/json.h"
#include "bridge/html.h"
#include "bridge/cache.h"
#include "bridge/registry.h"
#include "bridge/archive.h"
#include "bridge/util.h"
#include "bridge/buf.h"
#include "bridge/http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------- timing ----- */

typedef struct {
    const char *name;
    const char *unit;
    double      ops_per_sec;
    double      us_per_op;
    double      mb_per_sec;
    size_t      bytes;         /* bytes per op, for throughput columns */
} bench_row;

static bench_row g_rows[64];
static int g_nrows;

static int64_t g_iterations = 0;   /* 0 = auto-calibrate */
static bool    g_json_out = false;

static void row_add(const char *name, const char *unit, int64_t ops,
                    int64_t elapsed_ns, size_t bytes_per_op)
{
    if (g_nrows >= (int)XB_ARRAY_LEN(g_rows)) return;
    bench_row *r = &g_rows[g_nrows++];
    r->name = name;
    r->unit = unit;
    r->ops_per_sec = ops > 0 ? (double)ops * 1e9 / (double)elapsed_ns : 0;
    r->us_per_op   = ops > 0 ? (double)elapsed_ns / 1000.0 / (double)ops : 0;
    r->mb_per_sec  = (ops > 0 && bytes_per_op)
                   ? (double)ops * (double)bytes_per_op * 1e9 / (double)elapsed_ns / (1024.0 * 1024.0)
                   : 0;
    r->bytes = bytes_per_op;
}

/* Runs `fn` with an increasing count until it takes at least ~150 ms, so slow
 * and fast benchmarks are both measured with the same statistical quality. */
static void calibrate(const char *name, const char *unit, size_t bytes_per_op,
                      void (*fn)(int64_t))
{
    int64_t n = g_iterations ? g_iterations : 1000;
    for (int attempt = 0; attempt < 12; attempt++) {
        int64_t t0 = xb_mono_ns();
        fn(n);
        int64_t dt = xb_mono_ns() - t0;
        if (g_iterations || dt >= 150 * 1000 * 1000 || attempt == 11) {
            row_add(name, unit, n, dt, bytes_per_op);
            return;
        }
        /* Scale to ~250 ms, at least double, to converge quickly. */
        double factor = dt > 0 ? (250.0e6 / (double)dt) : 2.0;
        if (factor < 2.0) factor = 2.0;
        if (factor > 100.0) factor = 100.0;
        n = (int64_t)((double)n * factor) + 1;
    }
}

/* ------------------------------------------------------------- payloads --- */

static const char *JSON_SMALL =
    "{\"id\":\"42\",\"method\":\"source.search\",\"params\":{\"source_id\":\"legado/x\",\"query\":\"solo\"}}";

static char *JSON_LARGE;

static const char *HTML_DOC;

static void json_build_large(void)
{
    xb_buf b;
    xb_buf_init(&b);
    xb_buf_append(&b, "[", 1);
    for (int i = 0; i < 200; i++) {
        char tmp[512];
        snprintf(tmp, sizeof tmp,
                 "{\"name\":\"Book %d\",\"url\":\"https://example.tld/book/%d\","
                 "\"cover\":\"https://cdn.example.tld/c/%d.jpg\","
                 "\"author\":\"Author %d\",\"description\":\"A description that is "
                 "long enough to be realistic for a search result payload.\"}", i, i, i, i);
        if (i) xb_buf_append(&b, ",", 1);
        xb_buf_append(&b, tmp, strlen(tmp));
    }
    xb_buf_append(&b, "]", 1);
    JSON_LARGE = (char *)b.data;
}

static char *build_html(void)
{
    xb_buf b;
    xb_buf_init(&b);
    const char *head =
        "<!doctype html><html><head><title>Chapter list</title></head><body>"
        "<div class=\"wrap\"><ul class=\"chapters\">";
    xb_buf_append(&b, head, strlen(head));
    for (int i = 0; i < 500; i++) {
        char tmp[256];
        snprintf(tmp, sizeof tmp,
                 "<li class=\"chapter\" data-id=\"%d\"><a href=\"/read/%d/%d\">"
                 "Chapter %d &mdash; The Long Road</a></li>", i, 7, i, i + 1);
        xb_buf_append(&b, tmp, strlen(tmp));
    }
    const char *tail = "</ul></div></body></html>";
    xb_buf_append(&b, tail, strlen(tail));
    return (char *)b.data;
}

/* ---------------------------------------------------------------- jobs ---- */

static void b_json_parse_small(int64_t n)
{
    for (int64_t i = 0; i < n; i++) {
        xb_arena a;
        xb_arena_init(&a);
        size_t off = 0;
        volatile int ok = xb_json_parse(&a, JSON_SMALL, strlen(JSON_SMALL), &off) != NULL;
        (void)ok;
        xb_arena_destroy(&a);
    }
}

static void b_json_parse_large(int64_t n)
{
    size_t len = strlen(JSON_LARGE);
    for (int64_t i = 0; i < n; i++) {
        xb_arena a;
        xb_arena_init(&a);
        size_t off = 0;
        volatile int ok = xb_json_parse(&a, JSON_LARGE, len, &off) != NULL;
        (void)ok;
        xb_arena_destroy(&a);
    }
}

static void b_json_serialize(int64_t n)
{
    for (int64_t i = 0; i < n; i++) {
        xb_arena a;
        xb_arena_init(&a);
        size_t off = 0;
        xb_json *j = xb_json_parse(&a, JSON_LARGE, strlen(JSON_LARGE), &off);
        xb_jsonw w;
        xb_jw_init(&w);
        xb_jw_value(&w, j);
        volatile size_t wrote = xb_jw_len(&w);
        (void)wrote;
        xb_jw_free(&w);
        xb_arena_destroy(&a);
    }
}

static void b_json_canonical_hash(int64_t n)
{
    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *j = xb_json_parse(&a, JSON_SMALL, strlen(JSON_SMALL), &off);
    for (int64_t i = 0; i < n; i++) {
        char hex[65];
        xb_json_canonical_hash(j, hex);
        volatile char c = hex[0];
        (void)c;
    }
    xb_arena_destroy(&a);
}

static void b_html_parse(int64_t n)
{
    size_t len = strlen(HTML_DOC);
    for (int64_t i = 0; i < n; i++) {
        hdoc *d = xb_html_parse(HTML_DOC, len);
        xb_html_free(d);
    }
}

static void b_html_select(int64_t n)
{
    size_t len = strlen(HTML_DOC);
    hdoc *d = xb_html_parse(HTML_DOC, len);
    for (int64_t i = 0; i < n; i++) {
        hsel *s = xb_select(d, "li.chapter a@href");
        xb_sel_free(s);
    }
    xb_html_free(d);
}

static void b_html_parse_and_select(int64_t n)
{
    size_t len = strlen(HTML_DOC);
    for (int64_t i = 0; i < n; i++) {
        hdoc *d = xb_html_parse(HTML_DOC, len);
        hsel *s = xb_select(d, "ul.chapters li");
        xb_sel_free(s);
        xb_html_free(d);
    }
}

static char *g_sample_path;

static void b_detect(int64_t n)
{
    for (int64_t i = 0; i < n; i++) {
        xb_detect_result r;
        volatile int ok = xb_detect_path(g_sample_path, &r) >= 0;
        (void)ok;
    }
}

static void b_sha256(int64_t n)
{
    size_t len = strlen(JSON_LARGE);
    for (int64_t i = 0; i < n; i++) {
        char hex[65];
        xb_sha256_hex(JSON_LARGE, len, hex);
    }
}

static void b_cache_put_get(int64_t n)
{
    xb_cache *c = xb_cache_new(1024, 16u * 1024 * 1024, 60000);
    for (int64_t i = 0; i < n; i++) {
        char key[32];
        snprintf(key, sizeof key, "k%lld", (long long)(i & 255));
        xb_cache_put(c, key, "{\"value\":\"cached\"}", 60000);
        char *v = xb_cache_get(c, key);
        xb_free(v);
    }
    xb_cache_free(c);
}

static void b_url_resolve(int64_t n)
{
    for (int64_t i = 0; i < n; i++) {
        char out[512];
        xb_url_resolve("https://site.tld/a/b/c/d.html?x=1", "../../img/cover.jpg", out, sizeof out);
    }
}

/* ----------------------------------------------------------------- main --- */

static void print_table(void)
{
    if (g_json_out) {
        printf("{\"benchmarks\":[");
        for (int i = 0; i < g_nrows; i++) {
            bench_row *r = &g_rows[i];
            printf("%s{\"name\":\"%s\",\"ops_per_sec\":%.0f,\"us_per_op\":%.3f"
                   ",\"mb_per_sec\":%.1f}",
                   i ? "," : "", r->name, r->ops_per_sec, r->us_per_op, r->mb_per_sec);
        }
        printf("]}\n");
        return;
    }

    printf("\n%-28s %14s %14s %12s\n", "benchmark", "ops/sec", "us/op", "MiB/s");
    printf("%-28s %14s %14s %12s\n", "----------------------------",
           "--------------", "--------------", "------------");
    for (int i = 0; i < g_nrows; i++) {
        bench_row *r = &g_rows[i];
        printf("%-28s %14.0f %14.3f", r->name, r->ops_per_sec, r->us_per_op);
        if (r->mb_per_sec > 0) printf(" %12.1f", r->mb_per_sec);
        else                   printf(" %12s", "-");
        printf("\n");
    }

    double best = 0, worst = 0;
    for (int i = 0; i < g_nrows; i++) {
        if (!best || g_rows[i].ops_per_sec > best) best = g_rows[i].ops_per_sec;
        if (!worst || g_rows[i].ops_per_sec < worst) worst = g_rows[i].ops_per_sec;
    }
    printf("\n%zu benchmarks, %.2f MiB payload, %zu-byte document\n",
           (size_t)g_nrows, strlen(JSON_LARGE) / (1024.0 * 1024.0), strlen(HTML_DOC));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) g_json_out = true;
        else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
            g_iterations = atoll(argv[++i]);
    }

    json_build_large();
    HTML_DOC = build_html();

    /* A realistic detection target: a Legado book-source file. */
    {
        const char *tmpdir = getenv("TMPDIR");
        char path[512];
        snprintf(path, sizeof path, "%s/xbridge_bench_sample.json",
                 tmpdir && *tmpdir ? tmpdir : "/tmp");
        FILE *fp = fopen(path, "wb");
        if (fp) {
            const char *sample =
                "{\"bookSourceName\":\"Bench\",\"bookSourceUrl\":\"https://example.tld\","
                "\"bookSourceGroup\":\"test\",\"ruleSearch\":{\"bookList\":\".list li\","
                "\"name\":\"a@text\",\"author\":\".author@text\",\"bookUrl\":\"a@href\"}}";
            fwrite(sample, 1, strlen(sample), fp);
            fclose(fp);
            g_sample_path = xb_strdup(path);
        }
    }

    calibrate("json.parse.small",      "req", strlen(JSON_SMALL), b_json_parse_small);
    calibrate("json.parse.large",      "doc", strlen(JSON_LARGE), b_json_parse_large);
    calibrate("json.serialize.large",  "doc", strlen(JSON_LARGE), b_json_serialize);
    calibrate("json.canonical_hash",   "req", strlen(JSON_SMALL), b_json_canonical_hash);
    calibrate("html.parse",            "doc", strlen(HTML_DOC),  b_html_parse);
    calibrate("html.select",           "sel", strlen(HTML_DOC),  b_html_select);
    calibrate("html.parse+select",     "doc", strlen(HTML_DOC),  b_html_parse_and_select);
    calibrate("format.detect",         "op",  512,               b_detect);
    calibrate("sha256",                "doc", strlen(JSON_LARGE), b_sha256);
    calibrate("cache.put+get",         "op",  16,                b_cache_put_get);
    calibrate("url.resolve",           "op",  64,                b_url_resolve);

    print_table();

    xb_free(JSON_LARGE);
    xb_free((void *)HTML_DOC);
    if (g_sample_path) {
        remove(g_sample_path);
        xb_free(g_sample_path);
    }
    return 0;
}
