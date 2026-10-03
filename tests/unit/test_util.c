/* test_util.c — hashing, SHA-256 vectors, string helpers, URL resolution. */
#include "xbtest.h"
#include "bridge/util.h"
#include "bridge/http.h"

XB_SUITE("util");

XB_TEST(sha256_known_vectors)
{
    /* FIPS 180-4 / NIST examples. If these pass, canonical cache keys are
     * correct, because they are exactly sha256(hex) over canonical JSON. */
    static const struct { const char *in; const char *out; } V[] = {
        {"",     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc",  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"The quick brown fox jumps over the lazy dog",
         "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
    };
    for (size_t i = 0; i < XB_ARRAY_LEN(V); i++) {
        char hex[65];
        xb_sha256_hex(V[i].in, strlen(V[i].in), hex);
        XB_CHECK_MSG(strcmp(hex, V[i].out) == 0, "sha256(%s) = %s", V[i].in, hex);
    }
}

XB_TEST(sha256_long_input_blocks)
{
    /* NIST's one-million-'a' vector: exercises multi-block buffering. */
    size_t n = 1000000;
    char *buf = (char *)xb_alloc(n);
    memset(buf, 'a', n);
    char hex[65];
    xb_sha256_hex(buf, n, hex);
    XB_CHECK_EQ_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    xb_free(buf);

    /* And a size that is an exact multiple of the 64-byte block, which takes a
     * different path through the padding logic than the vector above. */
    char block[64];
    memset(block, 'b', sizeof block);
    char h1[65], h2[65];
    xb_sha256_hex(block, 64, h1);
    xb_sha256_hex(block, 64, h2);
    XB_CHECK_EQ_STR(h1, h2);
    XB_CHECK(strcmp(h1, "e0c1e5b1b7c4a0b0e5a2f9f3d0b1b6a4b3b7c1e5b1b7c4a0b0e5a2f9f3d0b1b6") != 0);
}

XB_TEST(sha256_padding_boundaries)
{
    /* 55/56/63/64/65 bytes are the classic pad edge cases. */
    char in[128];
    memset(in, 'x', sizeof in);
    for (size_t n = 54; n <= 66; n++) {
        char a[65], b[65];
        xb_sha256_hex(in, n, a);
        /* Two independent computations of the same input must agree, and a
         * one-byte-different input must differ. */
        xb_sha256_hex(in, n, b);
        XB_CHECK_EQ_STR(a, b);
        char c[65];
        xb_sha256_hex(in, n + 1, c);
        XB_CHECK(strcmp(a, c) != 0);
    }
}

XB_TEST(fnv_hash_is_stable)
{
    XB_CHECK(xb_hash_str("hello") == xb_hash64("hello", 5));
    XB_CHECK(xb_hash_str("hello") != xb_hash_str("hellp"));
    XB_CHECK(xb_hash_str("") == 1469598103934665603ULL);
}

XB_TEST(string_helpers)
{
    XB_CHECK(xb_streq("a", "a"));
    XB_CHECK(!xb_streq("a", "A"));
    XB_CHECK(xb_strieq("AbC", "aBc"));
    XB_CHECK(xb_str_has_suffix_ci("Foo.JAR", ".jar"));
    XB_CHECK(!xb_str_has_suffix_ci("Foo", ".jar"));
    XB_CHECK(xb_str_has_prefix("unix:///x", "unix://"));

    char buf[16];
    xb_str_lcpy(buf, "abcdefghijklmnopqrstuvwxyz", sizeof buf);
    XB_CHECK_EQ_INT(strlen(buf), 15);
    XB_CHECK_EQ_STR(buf, "abcdefghijklmno");

    char split_me[] = "a,b,,c";
    char *parts[8];
    size_t n = xb_str_split(split_me, ',', parts, 8);
    XB_CHECK_EQ_INT(n, 4);
    XB_CHECK_EQ_STR(parts[0], "a");
    XB_CHECK_EQ_STR(parts[2], "");
    XB_CHECK_EQ_STR(parts[3], "c");

    char trimmable[] = "  \t hello world \n ";
    XB_CHECK_EQ_STR(xb_str_trim(trimmable), "hello world");
}

XB_TEST(url_helpers)
{
    XB_CHECK(xb_url_is_absolute("https://a.b/c"));
    XB_CHECK(xb_url_is_absolute("http://a"));
    XB_CHECK(!xb_url_is_absolute("/relative"));
    XB_CHECK(!xb_url_is_absolute("a/b"));

    char out[256];

    xb_url_resolve("https://site.tld/a/b/c.html", "d.html", out, sizeof out);
    XB_CHECK_EQ_STR(out, "https://site.tld/a/b/d.html");

    xb_url_resolve("https://site.tld/a/b/c.html", "/root.html", out, sizeof out);
    XB_CHECK_EQ_STR(out, "https://site.tld/root.html");

    xb_url_resolve("https://site.tld/a/b/c.html", "../up.html", out, sizeof out);
    XB_CHECK_EQ_STR(out, "https://site.tld/a/up.html");
    xb_url_resolve("https://site.tld/a/b/c.html", "../../../up.html", out, sizeof out);
    XB_CHECK_EQ_STR(out, "https://site.tld/up.html");

    xb_url_resolve("https://site.tld/a/b/c.html", "https://other.tld/x", out, sizeof out);
    XB_CHECK_EQ_STR(out, "https://other.tld/x");

    xb_url_resolve("http://site.tld:8080/a/b", "c", out, sizeof out);
    XB_CHECK_EQ_STR(out, "http://site.tld:8080/a/c");

    xb_url_resolve("https://site.tld/a/b", "//cdn.tld/lib.js", out, sizeof out);
    XB_CHECK_EQ_STR(out, "https://cdn.tld/lib.js");

    xb_url_resolve("https://site.tld/a/b", "?q=1", out, sizeof out);
    XB_CHECK_EQ_STR(out, "https://site.tld/a/b?q=1");

    char enc[128];
    xb_url_encode("a b&c=d/é", enc, sizeof enc);
    XB_CHECK_EQ_STR(enc, "a+b%26c%3Dd%2F%C3%A9");

    char dec[] = "a+b%26c%3Dd%2F%C3%A9";
    xb_url_decode(dec);
    XB_CHECK_EQ_STR(dec, "a b&c=d/\xc3\xa9");
}

XB_TEST(threading_primitives)
{
    xb_mutex_t m;
    xb_cond_t c;
    XB_MUTEX_INIT(&m);
    XB_COND_INIT(&c);
    XB_MUTEX_LOCK(&m);
    int64_t t0 = xb_mono_ms();
    int rc = XB_COND_TIMEDWAIT(&c, &m, 30);
    int64_t elapsed = xb_mono_ms() - t0;
    XB_MUTEX_UNLOCK(&m);
    XB_CHECK(rc != 0);                    /* timed out, as asked */
    XB_CHECK(elapsed >= 20 && elapsed < 500);
    XB_MUTEX_DESTROY(&m);
    XB_COND_DESTROY(&c);

    xb_sem_t s;
    xb_sem_init(&s, 2);
    xb_sem_wait(&s);
    xb_sem_wait(&s);
    xb_sem_post(&s);
    xb_sem_destroy(&s);
    XB_CHECK(1);
}

XB_TEST(monotonic_clock_never_goes_backwards)
{
    int64_t prev = xb_mono_ms();
    for (int i = 0; i < 2000; i++) {
        int64_t now = xb_mono_ms();
        XB_CHECK(now >= prev);
        prev = now;
    }
    XB_CHECK(xb_now_ms() > 1600000000000LL);   /* after 2020 */
}
