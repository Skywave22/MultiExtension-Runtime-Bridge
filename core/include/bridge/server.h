/* bridge/server.h — the XBP/1 server: framing, dispatch, streaming, limits. */
#ifndef BRIDGE_SERVER_H
#define BRIDGE_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "net.h"
#include "json.h"
#include "cache.h"
#include "engine.h"
#include "registry.h"

#define XBP_VERSION       "XBP/1"
#define XBP_SOFTWARE      "xbridged/1.0.0"

/* Frame types — keep in sync with docs/PROTOCOL.md and the sdk directory. */
typedef enum {
    XB_FT_HELLO        = 0x01,
    XB_FT_HELLO_ACK    = 0x02,
    XB_FT_REQUEST      = 0x03,
    XB_FT_RESPONSE     = 0x04,
    XB_FT_STREAM_CHUNK = 0x05,
    XB_FT_STREAM_END   = 0x06,
    XB_FT_STREAM_ERR   = 0x07,
    XB_FT_CANCEL       = 0x08,
    XB_FT_PING         = 0x09,
    XB_FT_PONG         = 0x0A,
    XB_FT_ERROR        = 0x0B,
    XB_FT_BYE          = 0x0C
} xb_frame_type;

/* Error codes — stable, exposed in SDKs. */
typedef enum {
    XB_ERR_PARSE       = -32700,
    XB_ERR_INVALID     = -32600,
    XB_ERR_NO_METHOD   = -32601,
    XB_ERR_PARAMS      = -32602,
    XB_ERR_ENGINE      = -32000,
    XB_ERR_DEADLINE    = -32001,
    XB_ERR_CANCELLED   = -32002,
    XB_ERR_NOT_FOUND   = -32003,
    XB_ERR_UNAVAILABLE = -32004,
    XB_ERR_NETWORK     = -32005,
    XB_ERR_REJECTED    = -32006,
    XB_ERR_LIMIT       = -32007
} xb_error_code;

typedef struct {
    int    max_frame;
    int    workers_per_engine;
    int    task_threads;
    int    cache_entries;
    int64_t cache_ttl_ms;
    size_t cache_bytes;
    int    backlog;
    bool   allow_shutdown;
    bool   prewarm;
    bool   quiet;
    bool   json_logs;
    char   endpoint_uri[320];
    char   data_dir[512];
} xb_config;

extern xb_config xb_g_config;

/* A bridge instance bundles config + engines + cache + registry view. */
typedef struct xb_bridge xb_bridge;

xb_bridge *xb_bridge_new(const xb_config *cfg);
/* Registers the built-in engines that are present on this machine. */
int  xb_bridge_init(xb_bridge *b, char *err, size_t errcap);
void xb_bridge_free(xb_bridge *b);

xb_cache  *xb_bridge_cache(xb_bridge *b);
xb_engine *xb_bridge_engine(xb_bridge *b, const char *name);

/* Run the accept loop until shutdown. Returns process exit code. */
int xb_server_run(xb_bridge *b, char *err, size_t errcap);
void xb_server_request_shutdown(xb_bridge *b);

/* ------------------------------------------------------------- dispatch --- */

/* Method handler signature. `out` is appended to by the handler.
 * Streaming handlers call `emit` for each chunk. */
typedef struct xb_ctx {
    xb_bridge   *bridge;
    const char  *id;
    const char  *method;
    const xb_json *params;
    const xb_json *request;     /* whole request object */
    int64_t      deadline_ms;
    xb_jsonw    *out;           /* result object writer */
    /* streaming — same shape as xb_chunk_fn so handlers can hand it straight
     * to an engine without an adapter. */
    bool         stream;
    bool        (*emit)(void *ud, int64_t seq, const xb_json *chunk);
    void        *emit_ud;
    /* cancellation */
    volatile bool *cancelled;
} xb_ctx;

typedef int (*xb_handler_fn)(xb_ctx *ctx);
int xb_register_method(const char *name, xb_handler_fn fn);
xb_handler_fn xb_lookup_method(const char *name);
char *xb_method_list_json(void);
size_t xb_method_count(void);

/* Implemented in bridge.c — the unified source surface + admin methods. */
int xb_register_builtin_methods(void);

/* Metrics for `bridge.metrics`. */
typedef struct {
    uint64_t connections_total;
    uint64_t connections_active;
    uint64_t requests_total;
    uint64_t requests_ok;
    uint64_t requests_err;
    uint64_t requests_timeout;
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t protocol_errors;
    int64_t  started_ms;
} xb_server_stats;
void xb_server_stats_get(xb_bridge *b, xb_server_stats *out);

#endif /* BRIDGE_SERVER_H */
