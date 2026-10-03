/* net.c — UDS / abstract UDS / named pipe / TCP endpoints behind one API. */
#include "bridge/net.h"
#include "bridge/util.h"
#include "bridge/log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#ifndef _WIN32
#  include <signal.h>
#endif

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
#  define XB_CLOSESOCK closesocket
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <pwd.h>
#  include <sys/stat.h>
#  define XB_CLOSESOCK close
#endif

#ifdef _WIN32
static int npipe_rearm(struct xb_listener *l);
#endif

struct xb_listener {
    xb_sock_t   fd;
    xb_endpoint ep;
    volatile int stop;
#ifdef _WIN32
    char        pipe_name[300];
    HANDLE      pipe;
#endif
};

void xb_net_global_init(void)
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#else
    /* A client that disappears between its request and our reply would
     * otherwise raise SIGPIPE and take the whole daemon down with it. The
     * daemon is a server: it must tolerate vanishing peers forever. Handlers
     * still see the error through send()'s return value. */
    signal(SIGPIPE, SIG_IGN);
#endif
}

void xb_net_global_shutdown(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

int xb_sock_last_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

/* ---------------------------------------------------------------- parse -- */

int xb_ep_parse(const char *uri, xb_endpoint *out)
{
    if (!uri || !out) return -1;
    memset(out, 0, sizeof *out);

    if (xb_str_has_prefix(uri, "unix-abstract://")) {
        out->kind = XB_EP_UNIX_ABSTRACT;
        xb_str_lcpy(out->path, uri + strlen("unix-abstract://"), sizeof out->path);
        return out->path[0] ? 0 : -1;
    }
    if (xb_str_has_prefix(uri, "unix://")) {
        out->kind = XB_EP_UNIX;
        const char *p = uri + strlen("unix://");
        while (*p == '/') p++;              /* unix:///a/b -> a/b is relative? no */
        /* Keep the leading slash for absolute paths: unix:///tmp/x.sock */
        const char *abs = uri + strlen("unix://");
        xb_str_lcpy(out->path, abs, sizeof out->path);
        return out->path[0] ? 0 : -1;
    }
    if (xb_str_has_prefix(uri, "npipe://")) {
        out->kind = XB_EP_NPIPE;
        xb_str_lcpy(out->path, uri + strlen("npipe://"), sizeof out->path);
        return out->path[0] ? 0 : -1;
    }
    if (xb_str_has_prefix(uri, "stdio://")) {
        out->kind = XB_EP_STDIO;
        return 0;
    }
    if (xb_str_has_prefix(uri, "tcp://")) {
        const char *hp = uri + strlen("tcp://");
        const char *colon = strrchr(hp, ':');
        if (!colon) return -1;
        size_t hl = (size_t)(colon - hp);
        if (hl >= sizeof out->host) hl = sizeof out->host - 1;
        memcpy(out->host, hp, hl);
        out->host[hl] = '\0';
        if (hl == 0) xb_str_lcpy(out->host, "127.0.0.1", sizeof out->host);
        out->port = atoi(colon + 1);
        if (out->port < 0 || out->port > 65535) return -1;
        out->kind = XB_EP_TCP;
        return 0;
    }
    /* Bare path -> unix socket or named pipe. */
#ifdef _WIN32
    out->kind = XB_EP_NPIPE;
    xb_str_lcpy(out->path, uri, sizeof out->path);
#else
    out->kind = XB_EP_UNIX;
    xb_str_lcpy(out->path, uri, sizeof out->path);
#endif
    return out->path[0] ? 0 : -1;
}

char *xb_ep_format(const xb_endpoint *ep, char *out, size_t cap)
{
    switch (ep->kind) {
    case XB_EP_UNIX:          snprintf(out, cap, "unix://%s", ep->path); break;
    case XB_EP_UNIX_ABSTRACT: snprintf(out, cap, "unix-abstract://%s", ep->path); break;
    case XB_EP_NPIPE:         snprintf(out, cap, "npipe://%s", ep->path); break;
    case XB_EP_TCP:           snprintf(out, cap, "tcp://%s:%d", ep->host, ep->port); break;
    case XB_EP_STDIO:         snprintf(out, cap, "stdio://"); break;
    }
    return out;
}

int xb_ep_default(xb_endpoint *out)
{
    memset(out, 0, sizeof *out);
#ifdef _WIN32
    out->kind = XB_EP_NPIPE;
    char user[128] = "default";
    DWORD n = sizeof user;
    if (!GetUserNameA(user, &n)) xb_str_lcpy(user, "default", sizeof user);
    snprintf(out->path, sizeof out->path, "\\\\.\\pipe\\xbridge-%s", user);
    return 0;
#else
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] == '/') {
        out->kind = XB_EP_UNIX;
        snprintf(out->path, sizeof out->path, "%s/xbridge.sock", xdg);
        return 0;
    }
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] != '/') tmp = "/tmp";
    char user[64];
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name) xb_str_lcpy(user, pw->pw_name, sizeof user);
    else snprintf(user, sizeof user, "%u", (unsigned)getuid());
    /* Hash the uid into the name so two users never collide on a shared /tmp. */
    uint64_t h = xb_hash64(&(uid_t){getuid()}, sizeof(uid_t));
    if (strlen(tmp) + strlen(user) + 32 < sizeof out->path) {
        out->kind = XB_EP_UNIX;
        snprintf(out->path, sizeof out->path, "%s/xbridge-%s-%04llx.sock",
                 tmp, user, (unsigned long long)(h & 0xFFFF));
    } else {
        out->kind = XB_EP_UNIX_ABSTRACT;
        snprintf(out->path, sizeof out->path, "xbridge-%s-%04llx",
                 user, (unsigned long long)(h & 0xFFFF));
    }
    return 0;
#endif
}

/* --------------------------------------------------------------- server -- */

xb_listener *xb_listen(const xb_endpoint *ep, int backlog, char *err, size_t errcap)
{
    xb_listener *l = (xb_listener *)xb_alloc(sizeof *l);
    l->fd = XB_SOCK_INVALID;
    l->ep = *ep;
    if (backlog <= 0) backlog = 128;

    switch (ep->kind) {
    case XB_EP_UNIX:
    case XB_EP_UNIX_ABSTRACT: {
#ifndef _WIN32
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { snprintf(err, errcap, "socket(AF_UNIX): %s", strerror(errno)); xb_free(l); return NULL; }
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        socklen_t len;
        if (ep->kind == XB_EP_UNIX_ABSTRACT) {
            sa.sun_path[0] = '\0';
            xb_str_lcpy(sa.sun_path + 1, ep->path, sizeof sa.sun_path - 1);
            len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(ep->path));
        } else {
            xb_str_lcpy(sa.sun_path, ep->path, sizeof sa.sun_path);
            unlink(sa.sun_path);
            len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(sa.sun_path) + 1);
        }
        if (bind(fd, (struct sockaddr *)&sa, len) != 0) {
            snprintf(err, errcap, "bind %s: %s", ep->path, strerror(errno));
            close(fd); xb_free(l); return NULL;
        }
        if (listen(fd, backlog) != 0) {
            snprintf(err, errcap, "listen: %s", strerror(errno));
            close(fd); xb_free(l); return NULL;
        }
        /* Owner-only: the daemon may hold credentials/cookies for extensions. */
        if (ep->kind == XB_EP_UNIX) chmod(sa.sun_path, 0600);
        l->fd = fd;
        return l;
#else
        snprintf(err, errcap, "unix sockets are not supported on Windows; use npipe:// or tcp://");
        xb_free(l); return NULL;
#endif
    }
    case XB_EP_NPIPE: {
#ifdef _WIN32
        char name[300];
        if (xb_str_has_prefix(ep->path, "\\\\.\\pipe\\")) xb_str_lcpy(name, ep->path, sizeof name);
        else snprintf(name, sizeof name, "\\\\.\\pipe\\%s", ep->path);
        xb_str_lcpy(l->pipe_name, name, sizeof l->pipe_name);
        l->pipe = INVALID_HANDLE_VALUE;
        if (npipe_rearm(l) != 0) {
            snprintf(err, errcap, "CreateNamedPipe: %lu", GetLastError());
            xb_free(l); return NULL;
        }
        l->fd = XB_SOCK_INVALID;
        return l;
#else
        snprintf(err, errcap, "named pipes are Windows-only");
        xb_free(l); return NULL;
#endif
    }
    case XB_EP_TCP: {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { snprintf(err, errcap, "socket: %s", strerror(errno)); xb_free(l); return NULL; }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)ep->port);
        if (!ep->host[0] || xb_streq(ep->host, "localhost")) sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else sa.sin_addr.s_addr = inet_addr(ep->host);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
            snprintf(err, errcap, "bind tcp: %s", strerror(errno));
            XB_CLOSESOCK(fd); xb_free(l); return NULL;
        }
        if (listen(fd, backlog) != 0) {
            snprintf(err, errcap, "listen: %s", strerror(errno));
            XB_CLOSESOCK(fd); xb_free(l); return NULL;
        }
        socklen_t sl = sizeof sa;
        getsockname(fd, (struct sockaddr *)&sa, &sl);
        l->ep.port = ntohs(sa.sin_port);
        xb_str_lcpy(l->ep.host, "127.0.0.1", sizeof l->ep.host);
        l->fd = fd;
        return l;
    }
    case XB_EP_STDIO:
        snprintf(err, errcap, "stdio is not a listening endpoint");
        xb_free(l); return NULL;
    }
    xb_free(l);
    return NULL;
}

const xb_endpoint *xb_listener_endpoint(const xb_listener *l) { return &l->ep; }

/* Helper: create (or re-create) the listening pipe instance. */
#ifdef _WIN32
static int npipe_rearm(struct xb_listener *l)
{
    if (l->pipe && l->pipe != INVALID_HANDLE_VALUE) CloseHandle(l->pipe);
    l->pipe = CreateNamedPipeA(
        l->pipe_name,
        PIPE_ACCESS_INBOUND | PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, NULL);
    return (l->pipe == INVALID_HANDLE_VALUE) ? -1 : 0;
}
#endif

xb_sock_t xb_listener_accept(xb_listener *l)
{
    if (l->stop) return XB_SOCK_INVALID;
#ifdef _WIN32
    if (l->ep.kind == XB_EP_NPIPE) {
        for (;;) {
            if (l->stop) return XB_SOCK_INVALID;
            BOOL ok = ConnectNamedPipe(l->pipe, NULL);
            DWORD e = ok ? 0 : GetLastError();
            if (e == ERROR_PIPE_CONNECTED) {
                HANDLE h = l->pipe;
                npipe_rearm(l);       /* ready for the next client */
                return (xb_sock_t)(uintptr_t)h;
            }
            if (e == ERROR_NO_DATA || e == ERROR_PIPE_LISTENING) {
                Sleep(20);
                continue;
            }
            if (l->stop) return XB_SOCK_INVALID;
            if (npipe_rearm(l) != 0) { Sleep(50); }
        }
    }
#endif
    for (;;) {
        xb_sock_t c = accept(l->fd, NULL, NULL);
        if (c != XB_SOCK_INVALID) return c;
        int e = xb_sock_last_error();
#ifdef _WIN32
        if (e == WSAEINTR) continue;
#else
        if (e == EINTR) continue;
#endif
        if (l->stop) return XB_SOCK_INVALID;
        return XB_SOCK_INVALID;
    }
}

/* Ask a blocked accept() to return. Implemented by making one throwaway
 * connection to our own endpoint, which is the only way that works for every
 * transport (UNIX socket, abstract socket, named pipe and TCP). */
void xb_listener_wake(xb_listener *l, char *err, size_t errcap)
{
    l->stop = true;
    if (l->ep.kind == XB_EP_NPIPE) {
        /* ConnectNamedPipe is unblocked by the client handle appearing. */
#ifdef _WIN32
        HANDLE h = CreateFileA(l->pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#endif
        return;
    }
    xb_sock_t s = xb_connect(&l->ep, 500, err, errcap);
    if (s != XB_SOCK_INVALID) xb_sock_close(s);
}

/* Call only after the accept thread has joined. */
void xb_listener_close(xb_listener *l)
{
    if (!l) return;
#ifdef _WIN32
    if (l->ep.kind == XB_EP_NPIPE) {
        if (l->pipe && l->pipe != INVALID_HANDLE_VALUE) CloseHandle(l->pipe);
        l->pipe = NULL;
    }
#endif
    if (l->fd != XB_SOCK_INVALID) {
#ifndef _WIN32
        if (l->ep.kind == XB_EP_UNIX && l->ep.path[0]) unlink(l->ep.path);
#endif
        XB_CLOSESOCK(l->fd);
    }
    l->fd = XB_SOCK_INVALID;
    xb_free(l);
}

/* --------------------------------------------------------------- client -- */

xb_sock_t xb_connect(const xb_endpoint *ep, int timeout_ms, char *err, size_t errcap)
{
    (void)timeout_ms;
    switch (ep->kind) {
    case XB_EP_UNIX:
    case XB_EP_UNIX_ABSTRACT: {
#ifndef _WIN32
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { snprintf(err, errcap, "socket: %s", strerror(errno)); return XB_SOCK_INVALID; }
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        socklen_t len;
        if (ep->kind == XB_EP_UNIX_ABSTRACT) {
            sa.sun_path[0] = '\0';
            xb_str_lcpy(sa.sun_path + 1, ep->path, sizeof sa.sun_path - 1);
            len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(ep->path));
        } else {
            xb_str_lcpy(sa.sun_path, ep->path, sizeof sa.sun_path);
            len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(sa.sun_path) + 1);
        }
        if (connect(fd, (struct sockaddr *)&sa, len) != 0) {
            snprintf(err, errcap, "connect %s: %s", ep->path, strerror(errno));
            close(fd);
            return XB_SOCK_INVALID;
        }
        return fd;
#else
        snprintf(err, errcap, "unix sockets unsupported on Windows");
        return XB_SOCK_INVALID;
#endif
    }
    case XB_EP_NPIPE: {
#ifdef _WIN32
        char name[300];
        if (xb_str_has_prefix(ep->path, "\\\\.\\pipe\\")) xb_str_lcpy(name, ep->path, sizeof name);
        else snprintf(name, sizeof name, "\\\\.\\pipe\\%s", ep->path);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            snprintf(err, errcap, "CreateFile(pipe): %lu", GetLastError());
            return XB_SOCK_INVALID;
        }
        return (xb_sock_t)(uintptr_t)h;
#else
        snprintf(err, errcap, "named pipes are Windows-only");
        return XB_SOCK_INVALID;
#endif
    }
    case XB_EP_TCP: {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { snprintf(err, errcap, "socket: %s", strerror(errno)); return XB_SOCK_INVALID; }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)ep->port);
        sa.sin_addr.s_addr = (!ep->host[0] || xb_streq(ep->host, "localhost"))
                             ? htonl(INADDR_LOOPBACK) : inet_addr(ep->host);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
            snprintf(err, errcap, "connect tcp %s:%d: %s", ep->host, ep->port, strerror(errno));
            XB_CLOSESOCK(fd);
            return XB_SOCK_INVALID;
        }
        return fd;
    }
    case XB_EP_STDIO:
        snprintf(err, errcap, "stdio connect is handled by the caller");
        return XB_SOCK_INVALID;
    }
    return XB_SOCK_INVALID;
}

/* --------------------------------------------------------------------- I/O */

int64_t xb_read_full(xb_sock_t s, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
#ifdef _WIN32
        DWORD r = 0;
        if (!ReadFile((HANDLE)(uintptr_t)s, p + got, (DWORD)(n - got), &r, NULL)) {
            DWORD e = GetLastError();
            if (e == ERROR_MORE_DATA) { /* message-mode pipe: partial read */ }
            else return got ? (int64_t)got : -1;
        }
        if (r == 0) return (int64_t)got;
        got += (size_t)r;
#else
        ssize_t r = read(s, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return got ? (int64_t)got : -1;
        }
        if (r == 0) return (int64_t)got;
        got += (size_t)r;
#endif
    }
    return (int64_t)got;
}

int64_t xb_write_full(xb_sock_t s, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
#ifdef _WIN32
        DWORD w = 0;
        if (!WriteFile((HANDLE)(uintptr_t)s, p + sent, (DWORD)(n - sent), &w, NULL))
            return sent ? (int64_t)sent : -1;
        if (w == 0) return (int64_t)sent;
        sent += (size_t)w;
#else
        /* MSG_NOSIGNAL keeps a dead peer from killing the process even if a
         * host embedding the core reinstalls the default SIGPIPE handler.
         * Pipes (the stdio endpoint) report ENOTSOCK, so fall back to write. */
        ssize_t w;
#  ifdef MSG_NOSIGNAL
        w = send(s, p + sent, n - sent, MSG_NOSIGNAL);
        if (w < 0 && (errno == ENOTSOCK || errno == EOPNOTSUPP)) w = write(s, p + sent, n - sent);
#  else
        w = write(s, p + sent, n - sent);
#  endif
        if (w < 0) {
            if (errno == EINTR) continue;
            return sent ? (int64_t)sent : -1;
        }
        sent += (size_t)w;
#endif
    }
    return (int64_t)sent;
}

void xb_sock_close(xb_sock_t s)
{
    if (s == XB_SOCK_INVALID) return;
#ifdef _WIN32
    CloseHandle((HANDLE)(uintptr_t)s);
#else
    close(s);
#endif
}

void xb_sock_shutdown(xb_sock_t s)
{
    if (s == XB_SOCK_INVALID) return;
#ifdef _WIN32
    if (GetFileType((HANDLE)(uintptr_t)s) == FILE_TYPE_PIPE)
        CancelIo((HANDLE)(uintptr_t)s);
    else
        shutdown((SOCKET)s, SD_BOTH);
#else
    shutdown(s, SHUT_RDWR);
#endif
}

int xb_sock_set_nodelay(xb_sock_t s)
{
#ifdef _WIN32
    if (GetFileType((HANDLE)(uintptr_t)s) != FILE_TYPE_PIPE) {
        BOOL one = 1;
        return setsockopt((SOCKET)s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    }
    return 0;
#else
    int one = 1;
    if (s < 0) return -1;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#endif
}

int xb_sock_set_recv_timeout(xb_sock_t s, int ms)
{
#ifdef _WIN32
    if (GetFileType((HANDLE)(uintptr_t)s) == FILE_TYPE_PIPE) return 0;
    DWORD t = (DWORD)ms;
    return setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

/* ---------------------------------------------------------- wakeup pipes --
 *
 * Kept for embedders that multiplex many connections on one thread (select on
 * POSIX; a loopback socket pair on Windows, where select() cannot see pipes). */

int xb_wakeup_pair(xb_sock_t out[2])
{
#ifndef _WIN32
    int fds[2];
    if (pipe(fds) != 0) return -1;
    out[0] = fds[0];
    out[1] = fds[1];
    return 0;
#else
    xb_endpoint ep;
    memset(&ep, 0, sizeof ep);
    ep.kind = XB_EP_TCP;
    xb_str_lcpy(ep.host, "127.0.0.1", sizeof ep.host);
    char err[128];
    xb_listener *l = xb_listen(&ep, 4, err, sizeof err);
    if (!l) return -1;
    const xb_endpoint *bound = xb_listener_endpoint(l);
    xb_sock_t c = xb_connect(bound, 1000, err, sizeof err);
    if (c == XB_SOCK_INVALID) { xb_listener_wake(l, err, sizeof err); xb_listener_close(l); return -1; }
    xb_sock_t a = xb_listener_accept(l);
    xb_listener_wake(l, err, sizeof err);
    xb_listener_close(l);
    if (a == XB_SOCK_INVALID) { xb_sock_close(c); return -1; }
    out[0] = c;
    out[1] = a;
    return 0;
#endif
}

void xb_wakeup_signal(xb_sock_t *pair)
{
    if (!pair) return;
    uint8_t b = 1;
#ifdef _WIN32
    DWORD w = 0;
    WriteFile((HANDLE)(uintptr_t)pair[0], &b, 1, &w, NULL);
#else
    ssize_t r = write(pair[0], &b, 1);
    (void)r;
#endif
}

bool xb_is_wakeup(xb_sock_t s, xb_sock_t *pair)
{
    if (!pair) return false;
    return s == pair[1];
}
