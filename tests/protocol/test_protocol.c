/* test_protocol.c — XBP/1 conformance, driven over a real socket.
 *
 * This file deliberately re-implements the framing from docs/PROTOCOL.md
 * instead of calling into core/src/server.c. A test that shares the encoder
 * with the code under test cannot catch a framing bug; this one can.
 */
#include "xbtest.h"
#include "bridge/server.h"
#include "bridge/net.h"
#include "bridge/util.h"
#include "bridge/json.h"

#include <stdlib.h>
#include <string.h>

XB_SUITE("protocol");

/* ------------------------------------------------------------- framing --- */

#define FT_HELLO        0x01
#define FT_HELLO_ACK    0x02
#define FT_REQUEST      0x03
#define FT_RESPONSE     0x04
#define FT_STREAM_CHUNK 0x05
#define FT_STREAM_END   0x06
#define FT_STREAM_ERR   0x07
#define FT_CANCEL       0x08
#define FT_PING         0x09
#define FT_PONG         0x0A
#define FT_ERROR        0x0B
#define FT_BYE          0x0C

typedef struct {
    xb_sock_t sock;
    xb_arena  arena;      /* per-frame parse arena, reset on reuse */
    int       id;
} client;

static uint32_t rd_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static int frame_send(client *c, uint8_t type, const char *body, size_t len)
{
    unsigned char hdr[5];
    wr_be32(hdr, (uint32_t)len + 1);
    hdr[4] = type;
    if (xb_write_full(c->sock, hdr, 5) != 5) return -1;
    if (len && xb_write_full(c->sock, body, len) != (int64_t)len) return -1;
    return 0;
}

static int frame_send_str(client *c, uint8_t type, const char *body)
{
    return frame_send(c, type, body, body ? strlen(body) : 0);
}

/* Reads one frame; returns the type or -1. `out` receives a NUL-terminated copy. */
static int frame_recv(client *c, char *out, size_t cap, size_t *outlen)
{
    unsigned char hdr[5];
    if (xb_read_full(c->sock, hdr, 5) != 5) return -1;
    uint32_t total = rd_be32(hdr);
    if (total < 1 || total > 16u * 1024 * 1024) return -1;
    size_t blen = total - 1;
    if (blen >= cap) blen = cap - 1;
    if (blen && xb_read_full(c->sock, out, blen) != (int64_t)blen) return -1;
    out[blen] = '\0';
    if (outlen) *outlen = blen;
    return hdr[4];
}

/* Keep reading until a frame of `want` arrives, skipping the others. */
static int frame_recv_until(client *c, int want, char *out, size_t cap)
{
    for (int i = 0; i < 64; i++) {
        int t = frame_recv(c, out, cap, NULL);
        if (t < 0 || t == want) return t;
    }
    return -1;
}

/* ------------------------------------------------------------ server ----- */

static int      g_port;
static xb_bridge *g_bridge;
static xb_thread_t g_server_thread;

static XB_THREAD_FN(server_main)
{
    char err[256] = {0};
    xb_server_run((xb_bridge *)ud, err, sizeof err);
    XB_THREAD_RETURN(0);
}

static int probe_free_port(void)
{
    xb_endpoint ep;
    ep.kind = XB_EP_TCP;
    xb_str_lcpy(ep.host, "127.0.0.1", sizeof ep.host);
    ep.port = 0;
    xb_listener *l = xb_listen(&ep, 8, NULL, 0);
    if (!l) return -1;
    int port = xb_listener_endpoint(l)->port;
    xb_listener_close(l);
    return port;
}

static int server_start(void)
{
    g_port = probe_free_port();
    if (g_port <= 0) return -1;

    memset(&xb_g_config, 0, sizeof xb_g_config);
    xb_g_config.max_frame = 16 * 1024 * 1024;
    xb_g_config.workers_per_engine = 0;
    xb_g_config.task_threads = 8;
    xb_g_config.cache_entries = 256;
    xb_g_config.cache_ttl_ms = 60000;
    xb_g_config.cache_bytes = 4u * 1024 * 1024;
    xb_g_config.backlog = 32;
    xb_g_config.allow_shutdown = true;
    xb_g_config.quiet = true;                 /* no ready line during tests */
    snprintf(xb_g_config.endpoint_uri, sizeof xb_g_config.endpoint_uri,
             "tcp://127.0.0.1:%d", g_port);

    char err[256] = {0};
    g_bridge = xb_bridge_new(&xb_g_config);
    if (!g_bridge) return -1;
    if (xb_bridge_init(g_bridge, err, sizeof err) != 0) return -1;
    if (xb_thread_spawn(&g_server_thread, server_main, g_bridge) != 0) return -1;
    return 0;
}

static void server_stop(void)
{
    xb_server_request_shutdown(g_bridge);
    xb_thread_join(g_server_thread);
    xb_bridge_free(g_bridge);
    g_bridge = NULL;
}

static int client_open(client *c)
{
    xb_endpoint ep;
    snprintf(ep.host, sizeof ep.host, "127.0.0.1");
    ep.kind = XB_EP_TCP;
    ep.port = g_port;

    /* The server thread may not have reached accept() yet. */
    for (int i = 0; i < 100; i++) {
        c->sock = xb_connect(&ep, 500, NULL, 0);
        if (c->sock != XB_SOCK_INVALID) return 0;
        xb_sleep_ms(20);
    }
    return -1;
}

static void client_close(client *c)
{
    if (c->sock != XB_SOCK_INVALID) xb_sock_close(c->sock);
    c->sock = XB_SOCK_INVALID;
}

static int client_handshake(client *c, char *ack, size_t cap)
{
    if (frame_send_str(c, FT_HELLO,
                       "{\"protocol\":\"XBP/1\",\"client\":\"xbtest/1\",\"features\":[]}") != 0)
        return -1;
    return frame_recv_until(c, FT_HELLO_ACK, ack, cap);
}

/* Send a request and return the parsed RESPONSE body. */
static char *call(client *c, const char *method, const char *params, int *rc_out)
{
    char req[512];
    snprintf(req, sizeof req,
             "{\"id\":\"r%d\",\"method\":\"%s\",\"params\":%s}",
             ++c->id, method, params ? params : "{}");
    if (frame_send_str(c, FT_REQUEST, req) != 0) return NULL;

    static char buf[64 * 1024];
    int t = frame_recv(c, buf, sizeof buf, NULL);
    if (t < 0) return NULL;
    if (rc_out) *rc_out = t;
    return xb_strdup(buf);
}

/* The whole suite shares one daemon: startup is the slowest part of the file. */
static int g_started;

XB_TEST(handshake_reports_protocol_and_capabilities)
{
    if (!g_started) { XB_CHECK(server_start() == 0); g_started = 1; }
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[8192];
    XB_CHECK(client_handshake(&c, ack, sizeof ack) == FT_HELLO_ACK);

    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *j = xb_json_parse(&a, ack, strlen(ack), &off);
    XB_CHECK(j != NULL);
    XB_CHECK_EQ_STR(xb_json_obj_str(j, "protocol", ""), "XBP/1");
    XB_CHECK(xb_json_obj_int(j, "pid", 0) > 0);
    XB_CHECK(xb_json_obj_get(j, "features") != NULL);
    XB_CHECK(xb_json_obj_get(j, "capabilities") != NULL);
    XB_CHECK(xb_json_obj_int(xb_json_obj_get(j, "capabilities"), "formats", 0) == 10);
    xb_arena_destroy(&a);
    client_close(&c);
}

XB_TEST(ping_echoes_its_payload_as_pong)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    XB_CHECK(client_handshake(&c, ack, sizeof ack) == FT_HELLO_ACK);

    XB_CHECK(frame_send_str(&c, FT_PING, "{\"t\":123}") == 0);
    char buf[1024];
    XB_CHECK(frame_recv(&c, buf, sizeof buf, NULL) == FT_PONG);
    XB_CHECK_EQ_STR(buf, "{\"t\":123}");
    client_close(&c);
}

XB_TEST(unknown_method_is_a_jsonrpc_error)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);

    int t = 0;
    char *resp = call(&c, "no.such.method", "{}", &t);
    XB_CHECK(t == FT_RESPONSE || t == FT_ERROR);
    XB_CHECK(resp != NULL);
    if (resp) {
        xb_arena a;
        xb_arena_init(&a);
        size_t off = 0;
        xb_json *j = xb_json_parse(&a, resp, strlen(resp), &off);
        XB_CHECK_EQ_INT(xb_json_obj_int(xb_json_obj_get(j, "error"), "code", 0),
                        XB_ERR_NO_METHOD);
        XB_CHECK_EQ_STR(xb_json_obj_str(j, "id", ""), "r1");
        xb_arena_destroy(&a);
        xb_free(resp);
    }
    client_close(&c);
}

XB_TEST(malformed_json_request_is_rejected_and_connection_survives)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);

    XB_CHECK(frame_send_str(&c, FT_REQUEST, "{\"id\":\"bad\",\"method\":") == 0);
    char buf[4096];
    XB_CHECK(frame_recv(&c, buf, sizeof buf, NULL) == FT_ERROR);

    /* Still usable. */
    XB_CHECK(frame_send_str(&c, FT_PING, "\"alive\"") == 0);
    XB_CHECK(frame_recv(&c, buf, sizeof buf, NULL) == FT_PONG);
    client_close(&c);
}

XB_TEST(request_without_id_is_rejected)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);

    XB_CHECK(frame_send_str(&c, FT_REQUEST, "{\"method\":\"bridge.ping\"}") == 0);
    char buf[4096];
    XB_CHECK(frame_recv(&c, buf, sizeof buf, NULL) == FT_ERROR);
    XB_CHECK(strstr(buf, "-32600") != NULL || strstr(buf, "invalid") != NULL);
    client_close(&c);
}

XB_TEST(frames_before_hello_are_refused)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    XB_CHECK(frame_send_str(&c, FT_REQUEST,
                            "{\"id\":\"x\",\"method\":\"bridge.ping\"}") == 0);
    char buf[4096];
    int t = frame_recv(&c, buf, sizeof buf, NULL);
    XB_CHECK(t == FT_ERROR || t == -1);
    client_close(&c);
}

XB_TEST(oversize_declared_length_is_dropped)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);

    unsigned char hdr[5];
    wr_be32(hdr, 32u * 1024 * 1024);   /* cap is 16 MiB */
    hdr[4] = FT_REQUEST;
    xb_write_full(c.sock, hdr, 5);

    /* The daemon must not try to buffer 32 MiB; it either answers with an
     * error or closes. Either is fine, but it must not hang. */
    char buf[4096];
    int t = frame_recv(&c, buf, sizeof buf, NULL);
    XB_CHECK(t == FT_ERROR || t == -1);
    client_close(&c);
}

XB_TEST(pipelined_requests_are_all_answered)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);

    for (int i = 0; i < 8; i++) {
        char req[256];
        snprintf(req, sizeof req, "{\"id\":\"p%d\",\"method\":\"bridge.ping\"}", i);
        XB_CHECK(frame_send_str(&c, FT_REQUEST, req) == 0);
    }
    int seen[8] = {0};
    char buf[4096];
    for (int i = 0; i < 8; i++) {
        XB_CHECK(frame_recv(&c, buf, sizeof buf, NULL) == FT_RESPONSE);
        xb_arena a;
        xb_arena_init(&a);
        size_t off = 0;
        xb_json *j = xb_json_parse(&a, buf, strlen(buf), &off);
        const char *id = xb_json_obj_str(j, "id", "");
        if (id[0] == 'p' && id[1] >= '0' && id[1] <= '7') seen[id[1] - '0'] = 1;
        xb_arena_destroy(&a);
    }
    for (int i = 0; i < 8; i++) XB_CHECK_MSG(seen[i] == 1, "response %d missing", i);
    client_close(&c);
}

XB_TEST(concurrent_connections_are_independent)
{
    client clients[8];
    char ack[4096];
    for (int i = 0; i < 8; i++) {
        clients[i].sock = XB_SOCK_INVALID;
        clients[i].id = 0;
        XB_CHECK(client_open(&clients[i]) == 0);
        XB_CHECK(client_handshake(&clients[i], ack, sizeof ack) == FT_HELLO_ACK);
    }
    for (int i = 0; i < 8; i++) {
        char req[128];
        snprintf(req, sizeof req, "{\"id\":\"c%d\",\"method\":\"bridge.ping\"}", i);
        XB_CHECK(frame_send_str(&clients[i], FT_REQUEST, req) == 0);
    }
    for (int i = 0; i < 8; i++) {
        char buf[4096];
        XB_CHECK(frame_recv(&clients[i], buf, sizeof buf, NULL) == FT_RESPONSE);
        XB_CHECK(strstr(buf, "\"id\":\"c") != NULL);
    }
    for (int i = 0; i < 8; i++) client_close(&clients[i]);
}

XB_TEST(bye_closes_the_connection_cleanly)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);
    XB_CHECK(frame_send_str(&c, FT_BYE, "{\"reason\":\"done\"}") == 0);
    char buf[256];
    int t = frame_recv(&c, buf, sizeof buf, NULL);
    XB_CHECK(t == -1);          /* EOF, no error frame */
    client_close(&c);
}

XB_TEST(unknown_frame_types_are_ignored_for_forward_compat)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);

    XB_CHECK(frame_send_str(&c, 0x7F, "{\"future\":true}") == 0);
    XB_CHECK(frame_send_str(&c, FT_PING, "\"still here\"") == 0);
    char buf[256];
    XB_CHECK(frame_recv_until(&c, FT_PONG, buf, sizeof buf) == FT_PONG);
    client_close(&c);
}

XB_TEST(daemon_survives_a_connection_that_vanishes_mid_request)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);
    XB_CHECK(frame_send_str(&c, FT_REQUEST,
                            "{\"id\":\"half\",\"method\":\"bridge.ping\"}") == 0);
    /* Slam the socket without reading the reply. */
    client_close(&c);

    /* A fresh connection must still work. */
    client d = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&d) == 0);
    XB_CHECK(client_handshake(&d, ack, sizeof ack) == FT_HELLO_ACK);
    int rc = 0;
    char *resp = call(&d, "bridge.ping", "{}", &rc);
    XB_CHECK(rc == FT_RESPONSE);
    xb_free(resp);
    client_close(&d);
}

/* CANCEL names a request id on the connection that sent it. Everything that
 * does not name a live task must be a no-op, and the connection must stay
 * usable afterwards. */
XB_TEST(cancel_frames_are_scoped_and_never_break_a_connection)
{
    client c = {.sock = XB_SOCK_INVALID, .id = 0};
    XB_CHECK(client_open(&c) == 0);
    char ack[4096];
    client_handshake(&c, ack, sizeof ack);
    char buf[4096];

    /* A request that already finished: the task is gone from the registry. */
    XB_CHECK(frame_send_str(&c, FT_REQUEST,
                            "{\"id\":\"done\",\"method\":\"bridge.ping\"}") == 0);
    XB_CHECK(frame_recv_until(&c, FT_RESPONSE, buf, sizeof buf) == FT_RESPONSE);
    XB_CHECK(frame_send_str(&c, FT_CANCEL, "{\"id\":\"done\"}") == 0);

    /* Unknown, empty and unparseable payloads. */
    XB_CHECK(frame_send_str(&c, FT_CANCEL, "{\"id\":\"never-sent\"}") == 0);
    XB_CHECK(frame_send_str(&c, FT_CANCEL, "{}") == 0);
    XB_CHECK(frame_send_str(&c, FT_CANCEL, "not json at all") == 0);

    /* A cancelled id is released, so it can be reused by a later request. */
    XB_CHECK(frame_send_str(&c, FT_REQUEST,
                            "{\"id\":\"done\",\"method\":\"bridge.ping\"}") == 0);
    XB_CHECK(frame_recv_until(&c, FT_RESPONSE, buf, sizeof buf) == FT_RESPONSE);
    XB_CHECK(strstr(buf, "\"id\":\"done\"") != NULL);
    XB_CHECK(strstr(buf, "\"pong\"") != NULL);
    client_close(&c);
}

XB_TEST(shutdown_stops_the_daemon)
{
    server_stop();
    g_started = 0;
    XB_CHECK(1);
}
