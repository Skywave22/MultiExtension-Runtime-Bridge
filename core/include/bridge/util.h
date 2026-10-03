/* bridge/util.h — portable primitives, no dependencies beyond libc.
 *
 * Part of xbridged. Clean-room implementation, see docs/CLEANROOM.md.
 */
#ifndef BRIDGE_UTIL_H
#define BRIDGE_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef _WIN32
#  define XB_EXPORT __declspec(dllexport)
#  define XB_PATHSEP '\\'
#else
#  define XB_EXPORT __attribute__((visibility("default")))
#  define XB_PATHSEP '/'
#endif

#define XB_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define XB_UNUSED(x) ((void)(x))

/* ---------------------------------------------------------------- memory -- */

void *xb_alloc(size_t n);            /* zeroed; aborts on OOM (daemon policy) */
void *xb_realloc(void *p, size_t n);
void  xb_free(void *p);
char *xb_strdup(const char *s);
char *xb_strndup(const char *s, size_t n);
size_t xb_total_allocated(void);     /* live bytes, for metrics/leak tests */
size_t xb_total_blocks(void);        /* live allocation count */

#ifdef XB_ALLOC_TRACK
/* Debug builds only: prints live blocks grouped by allocating call site. */
void xb_alloc_report(void);
#endif

/* ------------------------------------------------------------------ time -- */

int64_t xb_now_ms(void);
int64_t xb_mono_ms(void);            /* monotonic, never jumps */
int64_t xb_mono_ns(void);            /* same clock, nanosecond resolution */
void    xb_sleep_ms(int ms);

/* ------------------------------------------------------------------ misc -- */

uint64_t xb_hash64(const void *data, size_t len);      /* FNV-1a 64 */
uint64_t xb_hash_str(const char *s);
void     xb_sha256(const void *data, size_t len, uint8_t out[32]);
void     xb_sha256_hex(const void *data, size_t len, char out[65]);

/* Lowercase-hex of arbitrary bytes. out must hold 2*len+1 bytes. */
void xb_hex(const uint8_t *in, size_t len, char *out);

/* ------------------------------------------------------------ case/trim -- */

bool      xb_streq(const char *a, const char *b);
bool      xb_strieq(const char *a, const char *b);
bool      xb_str_has_suffix_ci(const char *s, const char *suffix);
bool      xb_str_has_prefix(const char *s, const char *prefix);
char     *xb_str_trim(char *s);                     /* in-place, returns s */
size_t    xb_str_lcpy(char *dst, const char *src, size_t cap); /* always NUL */

/* Split `s` on `sep`, writing up to `max` pointers into `out`. Mutates `s`.
 * Returns the number of fields. Empty fields are kept. */
size_t xb_str_split(char *s, char sep, char **out, size_t max);

/* ------------------------------------------------------------ threading -- */

#ifdef _WIN32
#  include <windows.h>
typedef CRITICAL_SECTION   xb_mutex_t;
typedef CONDITION_VARIABLE xb_cond_t;
typedef HANDLE             xb_thread_t;
#  define XB_THREAD_RET DWORD WINAPI
#  define XB_MUTEX_INIT(m)  InitializeCriticalSection(m)
#  define XB_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#  define XB_MUTEX_LOCK(m)  EnterCriticalSection(m)
#  define XB_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#  define XB_COND_INIT(c)   InitializeConditionVariable(c)
#  define XB_COND_DESTROY(c) ((void)0)
#  define XB_COND_WAIT(c, m) SleepConditionVariableCS((c), (m), INFINITE)
#  define XB_COND_TIMEDWAIT(c, m, ms) \
        (SleepConditionVariableCS((c), (m), (DWORD)(ms)) ? 0 : 1)
#  define XB_COND_SIGNAL(c)   WakeConditionVariable(c)
#  define XB_COND_BROADCAST(c) WakeAllConditionVariable(c)
#else
#  include <pthread.h>
typedef pthread_mutex_t xb_mutex_t;
typedef pthread_cond_t  xb_cond_t;
typedef pthread_t       xb_thread_t;
#  define XB_THREAD_RET void *
#  define XB_MUTEX_INIT(m)   pthread_mutex_init((m), NULL)
#  define XB_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#  define XB_MUTEX_LOCK(m)   pthread_mutex_lock(m)
#  define XB_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#  define XB_COND_INIT(c)    pthread_cond_init((c), NULL)
#  define XB_COND_DESTROY(c) pthread_cond_destroy(c)
#  define XB_COND_WAIT(c, m) pthread_cond_wait((c), (m))
#  define XB_COND_TIMEDWAIT(c, m, ms) xb_cond_timedwait_rel((c), (m), (ms))
#  define XB_COND_SIGNAL(c)    pthread_cond_signal(c)
#  define XB_COND_BROADCAST(c) pthread_cond_broadcast(c)
#endif

/* Portable relative timed wait. Returns 0 on signal, non-zero on timeout. */
int xb_cond_timedwait_rel(xb_cond_t *c, xb_mutex_t *m, int timeout_ms);

/* Portable thread entry/return spellings. `(XB_THREAD_RET)0` is not valid
 * syntax with a calling convention attached, so use these instead. */
#ifdef _WIN32
#  define XB_THREAD_FN(name)   DWORD WINAPI name(void *ud)
#  define XB_THREAD_RETURN(v)  return (DWORD)(v)
#else
#  define XB_THREAD_FN(name)   void *name(void *ud)
#  define XB_THREAD_RETURN(v)  return (void *)(v)
#endif

/* Spawn a *joinable* thread running `fn(arg)`. Returns 0 on success.
 *
 * Joinable is the default because almost every thread in this daemon owns
 * memory that outlives the spawn point (engine readers, pool workers, the
 * accept loop), and shutdown has to be able to wait for them. Fire-and-forget
 * threads must say so explicitly with xb_thread_detach(). */
int  xb_thread_spawn(xb_thread_t *out, XB_THREAD_RET (*fn)(void *), void *arg);
/* Wait for a spawned thread to finish and release its handle. */
void xb_thread_join(xb_thread_t t);
/* Release the handle without waiting. */
void xb_thread_detach(xb_thread_t t);

/* Simple counting semaphore built on mutex+cond (no POSIX sem dependency). */
typedef struct {
    xb_mutex_t m;
    xb_cond_t  c;
    int        count;
} xb_sem_t;
void xb_sem_init(xb_sem_t *s, int initial);
void xb_sem_destroy(xb_sem_t *s);
void xb_sem_post(xb_sem_t *s);
void xb_sem_wait(xb_sem_t *s);

/* Single-writer spin-free helper used by the profiler/tests. */
uint64_t xb_atomic_add64(volatile uint64_t *p, uint64_t v);

#endif /* BRIDGE_UTIL_H */
