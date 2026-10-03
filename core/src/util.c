/* util.c — memory, time, hashing, strings, threading primitives. */
#include "bridge/util.h"
#include "bridge/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#  include <sys/time.h>
#endif

/* ---------------------------------------------------------------- memory -- */

static volatile uint64_t g_live_bytes = 0;
static volatile uint64_t g_live_blocks = 0;

/* Each allocation is preceded by a 16-byte header holding the size, so
 * xb_total_allocated() can report real numbers and the leak test at the end of
 * the test suite has something to assert on.
 *
 * With -DXB_ALLOC_TRACK the header grows a doubly linked list of live blocks
 * and the address that asked for them, so `xb_alloc_report()` can name the
 * guilty call site. That is debug-only: it costs 24 bytes per allocation, which
 * a latency-sensitive daemon should not pay in production builds. */
#ifdef XB_ALLOC_TRACK
#  include <execinfo.h>
#endif

typedef struct xb_hdr {
    size_t size;
    size_t magic;
#ifdef XB_ALLOC_TRACK
    void  *site;
    struct xb_hdr *next;
    struct xb_hdr *prev;
#endif
} xb_hdr;

#define XB_MAGIC 0x5875627269646765ULL /* "Xbridg e" */

#ifdef XB_ALLOC_TRACK
static xb_hdr *g_head;
static xb_mutex_t g_track_lock;
static bool g_track_lock_ready;

static void track_add(xb_hdr *h, void *site)
{
    if (!g_track_lock_ready) { XB_MUTEX_INIT(&g_track_lock); g_track_lock_ready = true; }
    h->site = site;
    XB_MUTEX_LOCK(&g_track_lock);
    h->next = g_head;
    h->prev = NULL;
    if (g_head) g_head->prev = h;
    g_head = h;
    XB_MUTEX_UNLOCK(&g_track_lock);
}

static void track_remove(xb_hdr *h)
{
    if (!g_track_lock_ready) return;
    XB_MUTEX_LOCK(&g_track_lock);
    if (h->prev) h->prev->next = h->next; else if (g_head == h) g_head = h->next;
    if (h->next) h->next->prev = h->prev;
    XB_MUTEX_UNLOCK(&g_track_lock);
}

/* Print every block still live, grouped by allocating call site. */
void xb_alloc_report(void)
{
    if (!g_track_lock_ready) { printf("xb_alloc_report: nothing tracked\n"); return; }
    printf("\n--- live allocations by call site ---\n");
    XB_MUTEX_LOCK(&g_track_lock);
    xb_hdr *seen[64];
    size_t seen_bytes[64];
    int nseen = 0;
    size_t total = 0, blocks = 0;
    for (xb_hdr *h = g_head; h; h = h->next) {
        total += h->size; blocks++;
        int found = 0;
        for (int i = 0; i < nseen; i++) {
            if (seen[i] == h->site) { seen_bytes[i] += h->size; found = 1; break; }
        }
        if (!found && nseen < 64) { seen[nseen] = h->site; seen_bytes[nseen] = h->size; nseen++; }
    }
    for (int i = 0; i < nseen; i++) {
        char **sym = backtrace_symbols(&seen[i], 1);
        printf("%10zu bytes  %s\n", seen_bytes[i], sym && sym[0] ? sym[0] : "(unknown)");
        free(sym);
    }
    printf("%10zu bytes in %zu blocks\n", total, blocks);
    XB_MUTEX_UNLOCK(&g_track_lock);
}
#endif /* XB_ALLOC_TRACK */

void *xb_alloc(size_t n)
{
    if (n == 0) n = 1;
    xb_hdr *h = (xb_hdr *)calloc(1, sizeof(xb_hdr) + n);
    if (!h) {
        xb_log(XB_LOG_FATAL, "out of memory allocating %zu bytes", n);
        abort();
    }
    h->size = n;
    h->magic = XB_MAGIC;
#ifdef XB_ALLOC_TRACK
    track_add(h, __builtin_return_address(0));
#endif
    xb_atomic_add64(&g_live_bytes, n);
    xb_atomic_add64(&g_live_blocks, 1);
    return (void *)(h + 1);
}

void *xb_realloc(void *p, size_t n)
{
    if (!p) return xb_alloc(n);
    xb_hdr *h = ((xb_hdr *)p) - 1;
    if (h->magic != XB_MAGIC) {
        xb_log(XB_LOG_FATAL, "xb_realloc on a non-xb pointer");
        abort();
    }
    size_t old = h->size;
#ifdef XB_ALLOC_TRACK
    /* realloc may move the block, so the tracker's list must be told. */
    track_remove(h);
#endif
    xb_hdr *nh = (xb_hdr *)realloc(h, sizeof(xb_hdr) + (n ? n : 1));
    if (!nh) {
        xb_log(XB_LOG_FATAL, "out of memory reallocating %zu bytes", n);
        abort();
    }
    if (n > old) memset((uint8_t *)(nh + 1) + old, 0, n - old);
    nh->size = n;
#ifdef XB_ALLOC_TRACK
    track_add(nh, __builtin_return_address(0));
#endif
    xb_atomic_add64(&g_live_bytes, (uint64_t)n - (uint64_t)old);
    return (void *)(nh + 1);
}

void xb_free(void *p)
{
    if (!p) return;
    xb_hdr *h = ((xb_hdr *)p) - 1;
    if (h->magic != XB_MAGIC) {
        xb_log(XB_LOG_FATAL, "xb_free on a non-xb pointer (double free?)");
        abort();
    }
#ifdef XB_ALLOC_TRACK
    track_remove(h);
#endif
    xb_atomic_add64(&g_live_bytes, 0 - (uint64_t)h->size);
    xb_atomic_add64(&g_live_blocks, (uint64_t)-1);
    h->magic = 0;
    free(h);
}

size_t xb_total_allocated(void) { return (size_t)g_live_bytes; }

char *xb_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = (char *)xb_alloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

size_t xb_total_blocks(void)
{
    return (size_t)g_live_blocks;
}

char *xb_strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t l = strnlen(s, n);
    char *d = (char *)xb_alloc(l + 1);
    memcpy(d, s, l);
    d[l] = '\0';
    return d;
}

/* ------------------------------------------------------------------ time -- */

int64_t xb_now_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t / 10000ULL) - 11644473600000LL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

int64_t xb_mono_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Nanosecond-resolution monotonic timestamp. Only the benchmark and the
 * frame-latency log use it; everything else works in milliseconds. */
int64_t xb_mono_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static BOOL once = FALSE;
    if (!once) { QueryPerformanceFrequency(&freq); once = TRUE; }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#else
    return xb_mono_ms() * 1000000LL;
#endif
}

void xb_sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* --------------------------------------------------------------- hashing -- */

uint64_t xb_hash64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t xb_hash_str(const char *s)
{
    return s ? xb_hash64(s, strlen(s)) : 0;
}

/* SHA-256 — public-domain style implementation written from FIPS 180-4. */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t used; } sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len = 0; c->used = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    while (n) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { sha256_block(c, c->buf); c->used = 0; }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->used != 56) sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i*8));
    sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

void xb_sha256(const void *data, size_t len, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void xb_hex(const uint8_t *in, size_t len, char *out)
{
    static const char *D = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2]   = D[(in[i] >> 4) & 0xF];
        out[i*2+1] = D[in[i] & 0xF];
    }
    out[len*2] = '\0';
}

void xb_sha256_hex(const void *data, size_t len, char out[65])
{
    uint8_t d[32];
    xb_sha256(data, len, d);
    xb_hex(d, 32, out);
}

/* ---------------------------------------------------------------- string -- */

bool xb_streq(const char *a, const char *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

bool xb_strieq(const char *a, const char *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

bool xb_str_has_suffix_ci(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return false;
    return xb_strieq(s + (ls - lf), suffix);
}

bool xb_str_has_prefix(const char *s, const char *prefix)
{
    if (!s || !prefix) return false;
    size_t lp = strlen(prefix);
    return strncmp(s, prefix, lp) == 0;
}

char *xb_str_trim(char *s)
{
    if (!s) return s;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = '\0';
    return s;
}

size_t xb_str_lcpy(char *dst, const char *src, size_t cap)
{
    if (!dst || cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

size_t xb_str_split(char *s, char sep, char **out, size_t max)
{
    size_t n = 0;
    if (!s || !out || max == 0) return 0;
    char *p = s;
    while (n < max) {
        out[n++] = p;
        char *q = strchr(p, sep);
        if (!q) break;
        *q = '\0';
        p = q + 1;
    }
    return n;
}

/* ------------------------------------------------------------- threading -- */

#ifndef _WIN32
int xb_cond_timedwait_rel(xb_cond_t *c, xb_mutex_t *m, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t ns = (int64_t)ts.tv_nsec + (int64_t)(timeout_ms % 1000) * 1000000LL;
    ts.tv_sec += timeout_ms / 1000 + (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    return pthread_cond_timedwait(c, m, &ts);
}
#endif

int xb_thread_spawn(xb_thread_t *out, XB_THREAD_RET (*fn)(void *), void *arg)
{
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (!h) return -1;
    if (out) *out = h; else CloseHandle(h);
    return 0;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return -1;
    if (out) *out = t;
    return 0;
#endif
}

void xb_thread_join(xb_thread_t t)
{
    if (!t) return;
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
}

void xb_thread_detach(xb_thread_t t)
{
    if (!t) return;
#ifdef _WIN32
    CloseHandle(t);
#else
    pthread_detach(t);
#endif
}

void xb_sem_init(xb_sem_t *s, int initial)
{
    XB_MUTEX_INIT(&s->m);
    XB_COND_INIT(&s->c);
    s->count = initial;
}

void xb_sem_destroy(xb_sem_t *s)
{
    XB_MUTEX_DESTROY(&s->m);
    XB_COND_DESTROY(&s->c);
}

void xb_sem_post(xb_sem_t *s)
{
    XB_MUTEX_LOCK(&s->m);
    s->count++;
    XB_COND_SIGNAL(&s->c);
    XB_MUTEX_UNLOCK(&s->m);
}

void xb_sem_wait(xb_sem_t *s)
{
    XB_MUTEX_LOCK(&s->m);
    while (s->count <= 0) XB_COND_WAIT(&s->c, &s->m);
    s->count--;
    XB_MUTEX_UNLOCK(&s->m);
}

uint64_t xb_atomic_add64(volatile uint64_t *p, uint64_t v)
{
#ifdef _WIN32
    return (uint64_t)InterlockedExchangeAdd64((volatile LONG64 *)p, (LONG64)v) + v;
#else
    return __atomic_fetch_add((uint64_t *)p, v, __ATOMIC_SEQ_CST) + v;
#endif
}
