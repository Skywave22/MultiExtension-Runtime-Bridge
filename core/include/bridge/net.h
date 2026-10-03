/* bridge/net.h — endpoint abstraction over UDS / abstract UDS / named pipe / TCP.
 *
 * One API, four transports, so the daemon has exactly one accept loop and the
 * SDKs have exactly one connect helper per platform.
 */
#ifndef BRIDGE_NET_H
#define BRIDGE_NET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
  typedef uintptr_t xb_sock_t;
#else
  typedef int xb_sock_t;
#endif

#define XB_SOCK_INVALID ((xb_sock_t)-1)

typedef enum {
    XB_EP_UNIX = 0,        /* unix:///path/to.sock        (Linux/macOS/BSD) */
    XB_EP_UNIX_ABSTRACT,   /* unix-abstract://name        (Linux only)       */
    XB_EP_NPIPE,           /* npipe://\\.\pipe\name       (Windows)          */
    XB_EP_TCP,             /* tcp://host:port             (everywhere)       */
    XB_EP_STDIO            /* stdio://                    (engines/inproc)   */
} xb_ep_kind;

typedef struct {
    xb_ep_kind kind;
    char       path[256];      /* unix path or pipe name */
    char       host[64];
    int        port;
} xb_endpoint;

/* Parse "unix:///x.sock", "unix-abstract://name", "npipe://name",
 * "tcp://127.0.0.1:7777". Returns 0 on success. */
int xb_ep_parse(const char *uri, xb_endpoint *out);

/* Render back to a URI in `out` (cap >= 320). Returns out. */
char *xb_ep_format(const xb_endpoint *ep, char *out, size_t cap);

/* Default endpoint for this platform/user, ready to be bound. */
int xb_ep_default(xb_endpoint *out);

/* --- server side --- */
typedef struct xb_listener xb_listener;
xb_listener *xb_listen(const xb_endpoint *ep, int backlog, char *err, size_t errcap);
/* Actual bound endpoint (port 0 in TCP becomes the chosen port). */
const xb_endpoint *xb_listener_endpoint(const xb_listener *l);
/* Blocking accept. Returns XB_SOCK_INVALID when the listener is closed.
 * For named pipes a fresh pipe instance is armed before returning, so the
 * caller does not have to recreate anything. */
xb_sock_t xb_listener_accept(xb_listener *l);
/* Unblock a thread sitting in xb_listener_accept(). Safe to call from another
 * thread. The listener must be closed only after the accept thread joined. */
void xb_listener_wake(xb_listener *l, char *err, size_t errcap);
/* Frees the listener. Call after the accept thread has joined. */
void xb_listener_close(xb_listener *l);

/* --- client side --- */
xb_sock_t xb_connect(const xb_endpoint *ep, int timeout_ms, char *err, size_t errcap);

/* --- I/O (loop until complete; returns bytes, 0 on EOF, -1 on error) --- */
int64_t xb_read_full(xb_sock_t s, void *buf, size_t n);
int64_t xb_write_full(xb_sock_t s, const void *buf, size_t n);

void xb_sock_close(xb_sock_t s);
void xb_sock_shutdown(xb_sock_t s);          /* stop further reads */
int  xb_sock_set_nodelay(xb_sock_t s);
int  xb_sock_set_recv_timeout(xb_sock_t s, int ms);
int  xb_sock_last_error(void);

/* Socket pair used to wake a blocked accept() during shutdown. */
int xb_wakeup_pair(xb_sock_t out[2]);
void xb_wakeup_signal(xb_sock_t *pair);
bool xb_is_wakeup(xb_sock_t s, xb_sock_t *pair);

/* Cross-platform init/teardown (WSAStartup on Windows). */
void xb_net_global_init(void);
void xb_net_global_shutdown(void);

#endif /* BRIDGE_NET_H */
