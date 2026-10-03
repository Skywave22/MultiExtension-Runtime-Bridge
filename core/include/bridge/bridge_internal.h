/* bridge_internal.h — interfaces shared between bridge.c and server.c only. */
#ifndef BRIDGE_INTERNAL_H
#define BRIDGE_INTERNAL_H

#include "server.h"
#include "json.h"
#include "engine.h"

/* Error helpers (bridge.c). */
const char *xb_error_message(int code);
bool        xb_error_retryable(int code);

/* Method metadata: which engine serves a method, and whether it can stream.
 * The registry is the single place that decides routing, so adding a method
 * never means touching the dispatcher. */
typedef struct {
    const char *name;
    const char *engine;      /* "js" | "jvm" | "rule" | "native" | "" (in-core) */
    bool        streamable;
    bool        cacheable;
    const char *summary;
} xb_method_meta;

int  xb_register_method_meta(const xb_method_meta *meta);
void xb_method_registry_free(void);
bool xb_lookup_method_meta(const char *name, xb_method_meta *out);

/* Bridge state accessors used by the server. */
int   xb_handler_source_family(xb_ctx *ctx);
void  xb_bridge_set_close_hook(void (*fn)(void *), void *ud);
void  xb_bridge_set_server_stats(xb_bridge *b, xb_server_stats *s);
void  xb_bridge_handshake_json(xb_bridge *b, xb_jsonw *w);
void  xb_bridge_engine_list_json(xb_bridge *b, xb_jsonw *w);
void  xb_bridge_set_endpoint(xb_bridge *b, const char *uri);
const char *xb_bridge_endpoint(xb_bridge *b);
void  xb_bridge_cancel(xb_bridge *b, const char *job_id);
void  xb_bridge_connection_added(xb_bridge *b, void *conn);
void  xb_bridge_connection_removed(xb_bridge *b, void *conn);
void  xb_bridge_close_all_connections(xb_bridge *b);

/* Server counters. */
void xb_server_stats_inc_frames_in(uint64_t n);
void xb_server_stats_inc_frames_out(uint64_t n);
void xb_server_stats_inc_bytes_in(uint64_t n);
void xb_server_stats_inc_bytes_out(uint64_t n);
void xb_server_stats_inc_connections(int delta);
void xb_server_stats_inc_connections_total(uint64_t n);
void xb_server_stats_inc_connections_active(int delta);
void xb_server_stats_inc_errors(void);
void xb_server_stats_inc_timeouts(void);
void xb_server_stats_inc_requests(int kind);   /* 1 = ok, 2 = error */
void xb_server_stats_attach(xb_bridge *b);

int64_t xb_getpid_wrapper(void);

#endif /* BRIDGE_INTERNAL_H */
