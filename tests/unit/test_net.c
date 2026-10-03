/* test_net.c — endpoints, transports, listener lifecycle.
 *
 * The transports are the one part of the bridge that cannot be exercised by
 * the protocol suite (which always runs over a socket the test itself created),
 * so the edge cases live here: URI parsing, EOF semantics, the socket-file
 * ownership rule, and the shutdown wake-up path.
 */
#include "xbtest.h"
#include "bridge/net.h"
#include "bridge/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

XB_SUITE("net");

XB_TEST(endpoint_parses_every_uri_form)
{
    xb_endpoint ep;
    char out[320];

    XB_CHECK_EQ_INT(xb_ep_parse("unix:///tmp/x.sock", &ep), 0);
    XB_CHECK_EQ_INT(ep.kind, XB_EP_UNIX);
    XB_CHECK_EQ_STR(ep.path, "/tmp/x.sock");
    XB_CHECK_EQ_STR(xb_ep_format(&ep, out, sizeof out), "unix:///tmp/x.sock");

    XB_CHECK_EQ_INT(xb_ep_parse("unix-abstract://xbridge-test", &ep), 0);
    XB_CHECK_EQ_INT(ep.kind, XB_EP_UNIX_ABSTRACT);
    XB_CHECK_EQ_STR(ep.path, "xbridge-test");
    XB_CHECK_EQ_STR(xb_ep_format(&ep, out, sizeof out), "unix-abstract://xbridge-test");

    XB_CHECK_EQ_INT(xb_ep_parse("npipe://xbridge-test", &ep), 0);
    XB_CHECK_EQ_INT(ep.kind, XB_EP_NPIPE);
    XB_CHECK_EQ_STR(xb_ep_format(&ep, out, sizeof out), "npipe://xbridge-test");

    XB_CHECK_EQ_INT(xb_ep_parse("tcp://127.0.0.1:7777", &ep), 0);
    XB_CHECK_EQ_INT(ep.kind, XB_EP_TCP);
    XB_CHECK_EQ_INT(ep.port, 7777);
    XB_CHECK_EQ_STR(ep.host, "127.0.0.1");
    XB_CHECK_EQ_STR(xb_ep_format(&ep, out, sizeof out), "tcp://127.0.0.1:7777");

    /* tcp://:7777 means "loopback", which is what a host that only cares about
     * the port writes. */
    XB_CHECK_EQ_INT(xb_ep_parse("tcp://:4096", &ep), 0);
    XB_CHECK_EQ_STR(ep.host, "127.0.0.1");
    XB_CHECK_EQ_INT(ep.port, 4096);

    XB_CHECK_EQ_INT(xb_ep_parse("stdio://", &ep), 0);
    XB_CHECK_EQ_INT(ep.kind, XB_EP_STDIO);

    /* A bare path is a unix socket (a pipe name on Windows). */
    XB_CHECK_EQ_INT(xb_ep_parse("/run/xbridge.sock", &ep), 0);
    XB_CHECK_EQ_INT(ep.kind, XB_EP_UNIX);
    XB_CHECK_EQ_STR(ep.path, "/run/xbridge.sock");

    XB_CHECK(xb_ep_parse("", &ep) != 0);
    XB_CHECK(xb_ep_parse(NULL, &ep) != 0);
    XB_CHECK(xb_ep_parse("tcp://127.0.0.1:99999", &ep) != 0);   /* port range */
    XB_CHECK(xb_ep_parse("tcp://127.0.0.1", &ep) != 0);         /* no port */
    XB_CHECK(xb_ep_parse("tcp://127.0.0.1:-1", &ep) != 0);
}

XB_TEST(default_endpoint_is_per_user_and_usable)
{
    xb_endpoint ep;
    char out[320];
    XB_CHECK(xb_ep_default(&ep) == 0);
    XB_CHECK(ep.kind == XB_EP_UNIX || ep.kind == XB_EP_UNIX_ABSTRACT ||
             ep.kind == XB_EP_NPIPE);
    XB_CHECK(ep.path[0] != '\0');
    XB_CHECK(strlen(xb_ep_format(&ep, out, sizeof out)) > 8);

    /* Round-trip: what we format must parse back to the same endpoint. */
    xb_endpoint again;
    XB_CHECK_EQ_INT(xb_ep_parse(out, &again), 0);
    XB_CHECK_EQ_INT(again.kind, ep.kind);
    XB_CHECK_EQ_STR(again.path, ep.path);
}

/* ------------------------------------------------------------------ tcp -- */

XB_TEST(tcp_listener_round_trip_and_eof)
{
    xb_endpoint ep;
    memset(&ep, 0, sizeof ep);
    ep.kind = XB_EP_TCP;
    xb_str_lcpy(ep.host, "127.0.0.1", sizeof ep.host);
    ep.port = 0;                       /* let the kernel choose */

    char err[256] = {0};
    xb_listener *l = xb_listen(&ep, 8, err, sizeof err);
    XB_CHECK_MSG(l != NULL, "listen failed: %s", err);
    if (!l) return;

    const xb_endpoint *bound = xb_listener_endpoint(l);
    XB_CHECK(bound->port > 0);
    XB_CHECK_EQ_STR(bound->host, "127.0.0.1");

    xb_sock_t c = xb_connect(bound, 2000, err, sizeof err);
    XB_CHECK_MSG(c != XB_SOCK_INVALID, "connect failed: %s", err);
    xb_sock_t s = xb_listener_accept(l);
    XB_CHECK(s != XB_SOCK_INVALID);
    if (c != XB_SOCK_INVALID && s != XB_SOCK_INVALID) {
        XB_CHECK_EQ_INT(xb_sock_set_nodelay(s), 0);
        XB_CHECK_EQ_INT(xb_write_full(c, "hello", 5), 5);
        char buf[16] = {0};
        XB_CHECK_EQ_INT(xb_read_full(s, buf, 5), 5);
        XB_CHECK_EQ_STR(buf, "hello");

        /* Server -> client in the other direction. */
        XB_CHECK_EQ_INT(xb_write_full(s, "world", 5), 5);
        memset(buf, 0, sizeof buf);
        XB_CHECK_EQ_INT(xb_read_full(c, buf, 5), 5);
        XB_CHECK_EQ_STR(buf, "world");

        /* A closed peer is EOF (0), not an error, so readers can tell the
         * difference between "done" and "broken". */
        xb_sock_close(c);
        XB_CHECK_EQ_INT(xb_read_full(s, buf, 1), 0);
        xb_sock_close(s);
    } else {
        if (c != XB_SOCK_INVALID) xb_sock_close(c);
        if (s != XB_SOCK_INVALID) xb_sock_close(s);
    }

    /* Waking a live listener makes accept() return "stop", which is how the
     * accept loop ends. */
    xb_listener_wake(l, err, sizeof err);
    XB_CHECK(xb_listener_accept(l) == XB_SOCK_INVALID);
    xb_listener_close(l);
}

XB_TEST(socket_receive_timeout_is_honoured)
{
    xb_endpoint ep;
    memset(&ep, 0, sizeof ep);
    ep.kind = XB_EP_TCP;
    xb_str_lcpy(ep.host, "127.0.0.1", sizeof ep.host);
    ep.port = 0;

    char err[256] = {0};
    xb_listener *l = xb_listen(&ep, 4, err, sizeof err);
    XB_CHECK(l != NULL);
    if (!l) return;
    xb_sock_t c = xb_connect(xb_listener_endpoint(l), 2000, err, sizeof err);
    xb_sock_t s = (c != XB_SOCK_INVALID) ? xb_listener_accept(l) : XB_SOCK_INVALID;
    XB_CHECK(c != XB_SOCK_INVALID && s != XB_SOCK_INVALID);
    if (c != XB_SOCK_INVALID && s != XB_SOCK_INVALID) {
        XB_CHECK_EQ_INT(xb_sock_set_recv_timeout(s, 100), 0);
        char b = 0;
        int64_t t0 = xb_mono_ms();
        int64_t rc = xb_read_full(s, &b, 1);
        int64_t took = xb_mono_ms() - t0;
        XB_CHECK(rc <= 0);                       /* timeout, not a byte */
        XB_CHECK_MSG(took < 1500, "recv timeout took %lld ms", (long long)took);
    }
    if (c != XB_SOCK_INVALID) xb_sock_close(c);
    if (s != XB_SOCK_INVALID) xb_sock_close(s);
    xb_listener_wake(l, err, sizeof err);
    xb_listener_accept(l);
    xb_listener_close(l);
}

/* ----------------------------------------------------------------- unix -- */

static int make_tmp_dir(char *out, size_t cap)
{
    snprintf(out, cap, "/tmp/xbtest-net-XXXXXX");
    return mkdtemp(out) != NULL;
}

XB_TEST(unix_listener_creates_a_private_socket_and_removes_it)
{
#ifndef _WIN32
    char dir[128];
    if (!make_tmp_dir(dir, sizeof dir)) { XB_CHECK(0); return; }

    char path[256];
    snprintf(path, sizeof path, "%s/x.sock", dir);

    xb_endpoint ep;
    memset(&ep, 0, sizeof ep);
    ep.kind = XB_EP_UNIX;
    xb_str_lcpy(ep.path, path, sizeof ep.path);

    char err[256] = {0};
    xb_listener *l = xb_listen(&ep, 8, err, sizeof err);
    XB_CHECK_MSG(l != NULL, "unix listen failed: %s", err);
    if (l) {
        struct stat st;
        XB_CHECK(stat(path, &st) == 0);
        /* Owner-only: the daemon can hold credentials for extensions. */
        XB_CHECK_EQ_INT(st.st_mode & 0777, 0600);

        xb_sock_t c = xb_connect(&ep, 1000, err, sizeof err);
        XB_CHECK(c != XB_SOCK_INVALID);
        xb_sock_t s = xb_listener_accept(l);
        XB_CHECK(s != XB_SOCK_INVALID);
        if (c != XB_SOCK_INVALID && s != XB_SOCK_INVALID) {
            XB_CHECK_EQ_INT(xb_write_full(c, "ping", 4), 4);
            char buf[8] = {0};
            XB_CHECK_EQ_INT(xb_read_full(s, buf, 4), 4);
            XB_CHECK_EQ_STR(buf, "ping");
        }
        if (c != XB_SOCK_INVALID) xb_sock_close(c);
        if (s != XB_SOCK_INVALID) xb_sock_close(s);

        xb_listener_wake(l, err, sizeof err);
        xb_listener_accept(l);
        xb_listener_close(l);
        XB_CHECK(stat(path, &st) != 0);      /* cleaned up after ourselves */
    }
    rmdir(dir);
#endif
}

/* A restarting daemon must not delete the socket a successor has bound: it
 * only unlinks the path if it still is the file it created. */
XB_TEST(listener_close_spares_a_successors_socket)
{
#ifndef _WIN32
    char dir[128];
    if (!make_tmp_dir(dir, sizeof dir)) { XB_CHECK(0); return; }

    char path[256];
    snprintf(path, sizeof path, "%s/x.sock", dir);

    xb_endpoint ep;
    memset(&ep, 0, sizeof ep);
    ep.kind = XB_EP_UNIX;
    xb_str_lcpy(ep.path, path, sizeof ep.path);

    char err[256] = {0};
    xb_listener *old = xb_listen(&ep, 8, err, sizeof err);
    XB_CHECK(old != NULL);
    if (!old) { rmdir(dir); return; }

    /* Another daemon takes over the path. */
    unlink(path);
    xb_listener *fresh = xb_listen(&ep, 8, err, sizeof err);
    XB_CHECK(fresh != NULL);
    if (fresh) {
        struct stat before;
        XB_CHECK(stat(path, &before) == 0);

        xb_listener_wake(old, err, sizeof err);
        xb_listener_accept(old);
        xb_listener_close(old);

        struct stat after;
        XB_CHECK_MSG(stat(path, &after) == 0,
                     "the departing daemon unlinked its successor's socket");
        XB_CHECK_EQ_INT(after.st_ino, before.st_ino);

        /* The real owner still removes it. */
        xb_listener_wake(fresh, err, sizeof err);
        xb_listener_accept(fresh);
        xb_listener_close(fresh);
        XB_CHECK(stat(path, &after) != 0);
    } else {
        xb_listener_wake(old, err, sizeof err);
        xb_listener_accept(old);
        xb_listener_close(old);
    }
    rmdir(dir);
#endif
}

/* ------------------------------------------------------- accept wake-up -- */

static volatile int g_accept_done;
static xb_sock_t    g_accept_sock;

static XB_THREAD_FN(accept_in_thread)
{
    xb_listener *l = (xb_listener *)ud;
    g_accept_sock = xb_listener_accept(l);
    g_accept_done = 1;
    XB_THREAD_RETURN(0);
}

/* xb_listener_wake() unblocks accept() by connecting to our own endpoint. When
 * that connection cannot arrive — the socket file was replaced while we were
 * shutting down — the accept thread must still return, or the daemon hangs in
 * shutdown forever. This is the regression test for that hang. */
XB_TEST(listener_wake_is_not_lost_when_the_wake_connect_cannot_arrive)
{
#ifndef _WIN32
    char dir[128];
    if (!make_tmp_dir(dir, sizeof dir)) { XB_CHECK(0); return; }

    char path[256];
    snprintf(path, sizeof path, "%s/x.sock", dir);

    xb_endpoint ep;
    memset(&ep, 0, sizeof ep);
    ep.kind = XB_EP_UNIX;
    xb_str_lcpy(ep.path, path, sizeof ep.path);

    char err[256] = {0};
    xb_listener *l = xb_listen(&ep, 8, err, sizeof err);
    XB_CHECK(l != NULL);
    if (!l) { rmdir(dir); return; }

    xb_thread_t th;
    g_accept_done = 0;
    g_accept_sock = XB_SOCK_INVALID;
    XB_CHECK(xb_thread_spawn(&th, accept_in_thread, l) == 0);
    xb_sleep_ms(100);                 /* let it block in accept() */

    unlink(path);                     /* the wake connection can never arrive */
    xb_listener_wake(l, err, sizeof err);

    for (int i = 0; i < 40 && !g_accept_done; i++) xb_sleep_ms(50);
    XB_CHECK_MSG(g_accept_done, "accept() never returned after the wake");
    if (g_accept_done) {
        XB_CHECK(g_accept_sock == XB_SOCK_INVALID);
        xb_thread_join(th);
    }
    xb_listener_close(l);
    rmdir(dir);
#endif
}

XB_TEST(wakeup_pair_carries_one_signal)
{
    xb_sock_t pair[2];
    XB_CHECK_EQ_INT(xb_wakeup_pair(pair), 0);
    XB_CHECK(!xb_is_wakeup(pair[0], pair));
    XB_CHECK(xb_is_wakeup(pair[1], pair));

    xb_wakeup_signal(pair);
    char b = 0;
    XB_CHECK_EQ_INT(xb_read_full(pair[1], &b, 1), 1);
    XB_CHECK_EQ_INT(b, 1);

    /* Nothing else was written. */
    XB_CHECK_EQ_INT(xb_sock_set_recv_timeout(pair[1], 100), 0);
    XB_CHECK(xb_read_full(pair[1], &b, 1) <= 0);

    xb_sock_close(pair[0]);
    xb_sock_close(pair[1]);
}
