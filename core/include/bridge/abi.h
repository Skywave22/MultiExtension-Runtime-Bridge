/* bridge/abi.h — the embedding ABI.
 *
 * XBP/1 over a socket is the right transport between applications. It is the
 * wrong transport when the host and the engine live in the same address space
 * and cannot afford a syscall per call — which is the situation on iOS, where
 * a second process is not an option, and inside a Flutter Android app, where
 * the reference architecture injects DEX into the host process.
 *
 * This header is that fast path. It is deliberately tiny and versioned
 * separately from the wire protocol:
 *
 *   - `xb_abi_version()` must be the first call. A host refuses to continue
 *     when the major version differs, which is what keeps a bundled extension
 *     package from calling into an incompatible library.
 *   - one `xb_abi_handle` owns its own cache, registries and engines; handles
 *     do not share mutable state, so a host can run several.
 *   - every string the library returns is owned by the caller and released
 *     with `xb_abi_free`, so hosts never need to know how the library
 *     allocates.
 *   - no call blocks indefinitely: every entry point takes a deadline in
 *     milliseconds, and 0 means "use the handle default".
 *
 * The C ABI is the interface; other languages bind to it directly (Dart FFI,
 * Swift, Kotlin/JNI, C#). Nothing here allocates on a hot path or throws.
 */
#ifndef BRIDGE_ABI_H
#define BRIDGE_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#  define XB_ABI_EXPORT __declspec(dllexport)
#else
#  define XB_ABI_EXPORT __attribute__((visibility("default")))
#endif

/* Bump XB_ABI_MAJOR only for a breaking change. */
#define XB_ABI_MAJOR 1
#define XB_ABI_MINOR 0
#define XB_ABI_PATCH 0

/* ------------------------------------------------------------------ types -- */

typedef struct xb_abi_handle xb_abi_handle;

/* Returned by every fallible call. 0 is success; anything else is an XBP error
 * code (see xb_error_code in bridge/server.h) so hosts share one error table
 * between the socket and the ABI. */
typedef int32_t xb_abi_status;

/* A borrowed, NUL-terminated UTF-8 string that lives as long as the handle.
 * Copy it if you need it after the next call. */
typedef const char *xb_abi_str;

typedef struct {
    /* Input. */
    const char *method;        /* "source.search" ... */
    const char *params_json;   /* JSON object; "{}" when there are no params */
    uint32_t    deadline_ms;   /* 0 = handle default */
    uint32_t    flags;         /* XB_ABI_F_* */
    /* Streaming callback. Return false to cancel the call; the call then ends
     * with XB_ERR_CANCELLED and the host may ignore any remaining chunks.
     * `chunk_json` is only valid for the duration of the callback. */
    bool      (*on_chunk)(void *user, uint32_t seq, const char *chunk_json);
    void       *on_chunk_user;

    /* Progress/interruption. Called from another thread when a stream is
     * running; returning true means "stop". May be NULL. */
    bool      (*should_stop)(void *user);

    /* Cancellation token. Implementations poll it; NULL means "never cancel".
     * A host that wants to cancel asynchronously allocates an atomic bool,
     * flips it from any thread, and passes the pointer here. */
    const volatile bool *cancel_flag;
    void       *user;
} xb_abi_request;

typedef struct {
    char        version[32];      /* XBP/1 */
    char        software[64];     /* xbridged/1.0.0 */
    uint32_t    abi_major;
    uint32_t    abi_minor;
    uint32_t    abi_patch;
    uint32_t    formats;          /* ecosystems this build knows */
    uint32_t    methods;          /* registered methods */
    uint64_t    max_frame;        /* protocol ceiling, for parity with the socket */
    bool        tls;              /* can this build speak https:// */
    const char *tls_backend;      /* borrowed, static lifetime */
} xb_abi_info;

enum {
    XB_ABI_F_STREAM   = 1u << 0,   /* emit chunks through on_chunk     */
    XB_ABI_F_CACHE    = 1u << 1,   /* allow a cached result            */
    XB_ABI_F_NO_CACHE = 1u << 2,   /* bypass the cache for this call   */
    XB_ABI_F_COALESCE = 1u << 3    /* join an identical in-flight call */
};

/* ----------------------------------------------------------------- entry --- */

/* Library version. Safe to call before anything else, including from a
 * constructor. Returns a static string such as "1.0.0". */
XB_ABI_EXPORT const char *xb_abi_version(void);

/* Fill `out` with build facts. Returns 0 on success. `abi_major` lets a host
 * refuse to run against an incompatible library. */
XB_ABI_EXPORT xb_abi_status xb_abi_get_info(xb_abi_info *out);

/* Create a handle. `config_json` may be NULL or an object such as
 *   {"data_dir":"/var/lib/xbridge","cache_entries":4096,"tls":"auto"}
 * Unknown keys are ignored so an older library can serve a newer host.
 * Never returns NULL except on allocation failure. */
XB_ABI_EXPORT xb_abi_handle *xb_abi_create(const char *config_json);

/* Release a handle and everything it owns. Safe on NULL. */
XB_ABI_EXPORT void xb_abi_destroy(xb_abi_handle *h);

/* Release process-global caches (the method registry and the JSON mirrors).
 *
 * Optional: call it when a host is finished with the library for reasons other
 * than exiting, so that a leak checker sees a clean process. After this call
 * the library is still usable — the next call re-registers what it needs —
 * but any xb_abi_str that came from a static accessor is invalid. */
XB_ABI_EXPORT void xb_abi_shutdown(void);

/* ------------------------------------------------------------------ calls -- */

/* Perform a request. On success `*out_json` points to a NUL-terminated JSON
 * document owned by the handle (valid until the next call on it, destroyed
 * with it, or released early with xb_abi_release). On failure `*out_json` is
 * NULL and the return value is the error code.
 *
 * The call never blocks longer than the deadline. */
XB_ABI_EXPORT xb_abi_status xb_abi_call(xb_abi_handle *h,
                                        const xb_abi_request *req,
                                        xb_abi_str *out_json);

/* Release the buffer returned by xb_abi_call before the next call, to cap
 * peak memory in a host that runs many large calls. Safe on NULL. */
XB_ABI_EXPORT void xb_abi_release(xb_abi_handle *h, xb_abi_str s);

/* ------------------------------------------------------------- extensions -- */

/* Install an extension file or directory. `out_json` follows the same
 * ownership rule as xb_abi_call. */
XB_ABI_EXPORT xb_abi_status xb_abi_install(xb_abi_handle *h, const char *path,
                                           xb_abi_str *out_json);

/* Classify a file without installing it. */
XB_ABI_EXPORT xb_abi_status xb_abi_detect(xb_abi_handle *h, const char *path,
                                          xb_abi_str *out_json);

/* --------------------------------------------------------------- registry -- */

/* The format registry as a JSON array; the string is static and must not be
 * freed. Cheap enough to call on every app start. */
XB_ABI_EXPORT const char *xb_abi_formats_json(void);

/* Registered method names as a JSON array; the string is static. */
XB_ABI_EXPORT const char *xb_abi_methods_json(void);

/* -------------------------------------------------------------- protocol --- */

/* The protocol version this library implements ("XBP/1"). Static. */
XB_ABI_EXPORT const char *xb_abi_protocol(void);

/* Human-readable message for an error code, matching the socket error table.
 * Static; never NULL. */
XB_ABI_EXPORT const char *xb_abi_strerror(xb_abi_status status);

/* Whether an error is worth retrying (network hiccups, deadline, engine
 * unavailability). Mirrors BridgeError.retryable in the SDKs. */
XB_ABI_EXPORT bool xb_abi_retryable(xb_abi_status status);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* BRIDGE_ABI_H */
