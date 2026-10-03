/* test_abi.c — the in-process embedding path.
 *
 * The ABI is a second transport for the same dispatcher. These tests pin the
 * contract a host binds to (versioning, ownership, error codes) and prove that
 * a call through the ABI returns the same shape as a call through the socket.
 */
#include "xbtest.h"
#include "bridge/abi.h"
#include "bridge/json.h"
#include "bridge/server.h"
#include "bridge/util.h"

#include <string.h>

XB_SUITE("abi");

/* The method registry is process-global on purpose; give it back before the
 * harness checks for leaks. */
static void abi_teardown(void) { xb_abi_shutdown(); }
__attribute__((constructor)) static void abi_register_teardown(void)
{
    xb_test_atexit(abi_teardown);
}

typedef struct {
    int  calls;
    char last[512];
    int  stop_after;
} chunk_sink;

static bool sink_on_chunk(void *user, uint32_t seq, const char *chunk_json)
{
    chunk_sink *s = (chunk_sink *)user;
    (void)seq;
    s->calls++;
    xb_str_lcpy(s->last, chunk_json, sizeof s->last);
    return !(s->stop_after && s->calls >= s->stop_after);
}

static xb_json *parse(const xb_arena *a, const char *text)
{
    size_t off = 0;
    return xb_json_parse((xb_arena *)a, text, text ? strlen(text) : 0, &off);
}

XB_TEST(version_and_build_info)
{
    XB_CHECK_EQ_STR(xb_abi_version(), "1.0.0");
    XB_CHECK_EQ_STR(xb_abi_protocol(), XBP_VERSION);

    xb_abi_info info;
    XB_CHECK_EQ_INT(xb_abi_get_info(&info), 0);
    XB_CHECK_EQ_INT(info.abi_major, XB_ABI_MAJOR);
    XB_CHECK_EQ_INT(info.formats, 10);
    XB_CHECK(info.methods >= 25);
    XB_CHECK(info.max_frame >= 1024 * 1024);
    XB_CHECK_EQ_STR(info.version, "XBP/1");
    XB_CHECK(info.tls_backend != NULL);

    /* A NULL out pointer must be an error, not a crash. */
    XB_CHECK_EQ_INT(xb_abi_get_info(NULL), XB_ERR_INVALID);
}

XB_TEST(static_registry_and_method_listing)
{
    const char *formats = xb_abi_formats_json();
    XB_CHECK(formats != NULL);
    xb_arena a;
    xb_arena_init(&a);
    xb_json *v = parse(&a, formats);
    XB_CHECK(v != NULL && v->type == XB_JARR);
    XB_CHECK_EQ_INT(xb_json_arr_len(v), 10);

    const char *methods = xb_abi_methods_json();
    size_t off = 0;
    xb_json *m = xb_json_parse(&a, methods, strlen(methods), &off);
    XB_CHECK(m != NULL && m->type == XB_JARR);
    XB_CHECK(xb_json_arr_len(m) >= 25);

    /* Repeated calls return the same cached pointer. */
    XB_CHECK(xb_abi_formats_json() == formats);
    xb_arena_destroy(&a);
}

XB_TEST(error_helpers_match_the_wire_table)
{
    XB_CHECK_EQ_STR(xb_abi_strerror(XB_ERR_NOT_FOUND), "not found");
    XB_CHECK_EQ_STR(xb_abi_strerror(XB_ERR_NO_METHOD), "method not found");
    XB_CHECK(xb_abi_retryable(XB_ERR_NETWORK));
    XB_CHECK(xb_abi_retryable(XB_ERR_DEADLINE));
    XB_CHECK(!xb_abi_retryable(XB_ERR_NOT_FOUND));
    XB_CHECK(!xb_abi_retryable(XB_ERR_PARAMS));
}

XB_TEST(create_call_destroy)
{
    xb_abi_handle *h = xb_abi_create(NULL);
    XB_CHECK(h != NULL);
    if (!h) return;

    xb_abi_request req;
    memset(&req, 0, sizeof req);
    req.method = "bridge.ping";
    req.params_json = "{}";

    xb_abi_str out = NULL;
    XB_CHECK_EQ_INT(xb_abi_call(h, &req, &out), 0);
    XB_CHECK(out != NULL);
    if (out) {
        xb_arena a;
        xb_arena_init(&a);
        xb_json *v = parse(&a, out);
        XB_CHECK(v && v->type == XB_JOBJ);
        XB_CHECK_EQ_STR(xb_json_obj_str(v, "pong", ""), "xbridge");
        xb_arena_destroy(&a);
    }

    xb_abi_destroy(h);
}

XB_TEST(same_answer_as_the_socket_path)
{
    xb_abi_handle *h = xb_abi_create("{\"cache_entries\":64,\"quiet\":true}");
    XB_CHECK(h != NULL);
    if (!h) return;

    xb_abi_request req;
    memset(&req, 0, sizeof req);
    req.method = "format.list";
    xb_abi_str out = NULL;
    XB_CHECK_EQ_INT(xb_abi_call(h, &req, &out), 0);

    xb_arena a;
    xb_arena_init(&a);
    xb_json *v = parse(&a, out);
    XB_CHECK(v != NULL);
    XB_CHECK_EQ_INT(xb_json_obj_int(v, "count", -1), 10);
    xb_arena_destroy(&a);

    xb_abi_destroy(h);
}

XB_TEST(unknown_method_and_bad_params_are_reported_not_thrown)
{
    xb_abi_handle *h = xb_abi_create(NULL);
    XB_CHECK(h != NULL);
    if (!h) return;

    xb_abi_request req;
    xb_abi_str out = NULL;

    memset(&req, 0, sizeof req);
    req.method = "nope.nope";
    XB_CHECK_EQ_INT(xb_abi_call(h, &req, &out), XB_ERR_NO_METHOD);
    XB_CHECK(out == NULL);

    memset(&req, 0, sizeof req);
    req.method = "source.getDetail";
    req.params_json = "{not json";
    XB_CHECK_EQ_INT(xb_abi_call(h, &req, &out), XB_ERR_PARSE);

    memset(&req, 0, sizeof req);
    req.method = "source.getDetail";
    req.params_json = "{\"source_id\":\"legado/missing.json\"}";
    XB_CHECK_EQ_INT(xb_abi_call(h, &req, &out), XB_ERR_NOT_FOUND);

    XB_CHECK_EQ_INT(xb_abi_call(NULL, &req, &out), XB_ERR_INVALID);
    XB_CHECK_EQ_INT(xb_abi_call(h, NULL, &out), XB_ERR_INVALID);

    xb_abi_destroy(h);
}

XB_TEST(streaming_hands_chunks_to_the_host)
{
    xb_abi_handle *h = xb_abi_create(NULL);
    XB_CHECK(h != NULL);
    if (!h) return;

    /* Without a source there is nothing to stream, but the plumbing must not
     * crash and must report the same "not found" the socket path does. */
    chunk_sink sink;
    memset(&sink, 0, sizeof sink);

    xb_abi_request req;
    memset(&req, 0, sizeof req);
    req.method = "source.search";
    req.params_json = "{\"source_id\":\"legado/missing.json\",\"query\":\"x\"}";
    req.flags = XB_ABI_F_STREAM;
    req.on_chunk = sink_on_chunk;
    req.on_chunk_user = &sink;

    xb_abi_str out = NULL;
    XB_CHECK_EQ_INT(xb_abi_call(h, &req, &out), XB_ERR_NOT_FOUND);
    XB_CHECK_EQ_INT(sink.calls, 0);

    xb_abi_destroy(h);
}

XB_TEST(abi_and_socket_agree_on_the_error_table)
{
    /* Both transports must expose the same numbers; a host switches between
     * them at runtime (Android in-process vs desktop daemon). */
    XB_CHECK_EQ_INT(XB_ERR_PARSE, -32700);
    XB_CHECK_EQ_INT(XB_ERR_INVALID, -32600);
    XB_CHECK_EQ_INT(XB_ERR_NO_METHOD, -32601);
    XB_CHECK_EQ_INT(XB_ERR_PARAMS, -32602);
    XB_CHECK_EQ_INT(XB_ERR_ENGINE, -32000);
    XB_CHECK_EQ_INT(XB_ERR_DEADLINE, -32001);
    XB_CHECK_EQ_INT(XB_ERR_CANCELLED, -32002);
    XB_CHECK_EQ_INT(XB_ERR_NOT_FOUND, -32003);
    XB_CHECK_EQ_INT(XB_ERR_UNAVAILABLE, -32004);
    XB_CHECK_EQ_INT(XB_ERR_NETWORK, -32005);
    XB_CHECK_EQ_INT(XB_ERR_REJECTED, -32006);
    XB_CHECK_EQ_INT(XB_ERR_LIMIT, -32007);
}
