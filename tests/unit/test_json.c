/* test_json.c — parser, canonical writer, escaping, limits. */
#include "xbtest.h"
#include "bridge/json.h"
#include "bridge/buf.h"
#include "bridge/util.h"

XB_SUITE("json");

static xb_json *parse(xb_arena *a, const char *s)
{
    size_t off = 0;
    return xb_json_parse(a, s, strlen(s), &off);
}

XB_TEST(parse_primitives)
{
    xb_arena a;
    xb_arena_init(&a);
    XB_CHECK_EQ_INT(xb_json_num(parse(&a, "42"), -1), 42);
    XB_CHECK_NEAR(xb_json_num(parse(&a, "-7.5"), 0), -7.5, 1e-9);
    XB_CHECK(xb_json_bool(parse(&a, "true"), false));
    XB_CHECK(!xb_json_bool(parse(&a, "false"), true));
    XB_CHECK_EQ_STR(xb_json_str(parse(&a, "\"hi\""), NULL), "hi");
    xb_json *nul = parse(&a, "null");
    XB_CHECK(nul != NULL && nul->type == XB_JNULL);
    xb_arena_destroy(&a);
}

XB_TEST(parse_rejects_malformed)
{
    xb_arena a;
    xb_arena_init(&a);
    const char *bad[] = {
        "", "{", "[", "{\"a\":}", "{\"a\" 1}", "[1,]", "{'a':1}", "01", "1.",
        "tru", "\"unterminated", "{\"a\":1} trailing", "+", ".5", "1e", "-", NULL
    };
    for (int i = 0; bad[i]; i++) {
        xb_arena tmp;
        xb_arena_init(&tmp);
        xb_json *v = parse(&tmp, bad[i]);
        XB_CHECK_MSG(v == NULL, "input %d (%s) should be rejected", i, bad[i]);
        xb_arena_destroy(&tmp);
    }
    xb_arena_destroy(&a);
}

XB_TEST(parse_depth_limit)
{
    /* A hostile frame must not exhaust the C stack. */
    xb_arena a;
    xb_arena_init(&a);
    char deep[512];
    for (int i = 0; i < 300; i++) deep[i] = '[';
    deep[300] = '\0';
    size_t off = 0;
    XB_CHECK(xb_json_parse(&a, deep, 300, &off) == NULL);
    xb_arena_destroy(&a);
}

XB_TEST(parse_nested_and_typed_access)
{
    xb_arena a;
    xb_arena_init(&a);
    const char *doc = "{\"a\":{\"b\":[1,2,{\"c\":\"deep\"}]},\"n\":3.5,\"f\":true}";
    xb_json *v = parse(&a, doc);
    XB_CHECK(v != NULL);
    xb_json *arr = xb_json_obj_get(xb_json_obj_get(v, "a"), "b");
    XB_CHECK_EQ_INT(xb_json_arr_len(arr), 3);
    XB_CHECK_EQ_STR(xb_json_obj_str(xb_json_arr_at(arr, 2), "c", ""), "deep");
    XB_CHECK_NEAR(xb_json_obj_num(v, "n", 0), 3.5, 1e-9);
    XB_CHECK(xb_json_obj_bool(v, "f", false));
    XB_CHECK_EQ_INT(xb_json_obj_int(v, "missing", -9), -9);
    xb_arena_destroy(&a);
}

XB_TEST(string_escapes_roundtrip)
{
    xb_arena a;
    xb_arena_init(&a);
    const char *doc = "{\"s\":\"tab\\tnl\\nquote\\\"slash\\\\unicode\\u00e9\\u4e2d\"}";
    xb_json *v = parse(&a, doc);
    XB_CHECK(v != NULL);
    const char *s = xb_json_obj_str(v, "s", "");
    XB_CHECK(strstr(s, "tab\t") != NULL);
    XB_CHECK(strstr(s, "\n") != NULL);
    XB_CHECK(strstr(s, "\"") != NULL);
    XB_CHECK(strstr(s, "\\") != NULL);
    XB_CHECK(strstr(s, "\xc3\xa9") != NULL);       /* é */
    XB_CHECK(strstr(s, "\xe4\xb8\xad") != NULL);   /* 中 */
    xb_arena_destroy(&a);
}

XB_TEST(surrogate_pair_decodes)
{
    xb_arena a;
    xb_arena_init(&a);
    xb_json *v = parse(&a, "{\"e\":\"\\ud83d\\ude00\"}");   /* U+1F600 */
    XB_CHECK(v != NULL);
    const char *s = xb_json_obj_str(v, "e", "");
    XB_CHECK_EQ_STR(s, "\xf0\x9f\x98\x80");
    xb_arena_destroy(&a);
}

XB_TEST(writer_escapes_control_chars)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "k", "a\nb\tc\"d\\e");
    xb_jw_obj_end(&w);
    const char *out = xb_jw_data(&w);
    XB_CHECK_EQ_STR(out, "{\"k\":\"a\\nb\\tc\\\"d\\\\e\"}");
    /* and it must re-parse */
    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *rt = xb_json_parse(&a, out, strlen(out), &off);
    XB_CHECK(rt != NULL);
    XB_CHECK_EQ_STR(xb_json_obj_str(rt, "k", ""), "a\nb\tc\"d\\e");
    xb_arena_destroy(&a);
    xb_jw_free(&w);
}

XB_TEST(canonical_sorts_keys)
{
    xb_arena a;
    xb_arena_init(&a);
    xb_json *v = parse(&a, "{\"z\":1,\"a\":2,\"m\":{\"y\":1,\"b\":2}}");
    char *canon = xb_json_dumps(v, true);
    XB_CHECK_EQ_STR(canon, "{\"a\":2,\"m\":{\"b\":2,\"y\":1},\"z\":1}");
    char *plain = xb_json_dumps(v, false);
    XB_CHECK_EQ_STR(plain, "{\"z\":1,\"a\":2,\"m\":{\"y\":1,\"b\":2}}");
    xb_free(canon);
    xb_free(plain);
    xb_arena_destroy(&a);
}

XB_TEST(canonical_hash_is_key_order_independent)
{
    /* This property is what makes cross-language cache keys agree. */
    xb_arena a1, a2;
    xb_arena_init(&a1);
    xb_arena_init(&a2);
    char h1[65], h2[65];
    xb_json_canonical_hash(parse(&a1, "{\"a\":1,\"b\":[2,3],\"c\":{\"d\":4,\"e\":5}}"), h1);
    xb_json_canonical_hash(parse(&a2, "{\"c\":{\"e\":5,\"d\":4},\"b\":[2,3],\"a\":1}"), h2);
    XB_CHECK_EQ_STR(h1, h2);
    XB_CHECK_EQ_INT(strlen(h1), 64);

    xb_arena a3;
    xb_arena_init(&a3);
    char h3[65];
    xb_json_canonical_hash(parse(&a3, "{\"a\":1,\"b\":[2,4],\"c\":{\"d\":4,\"e\":5}}"), h3);
    XB_CHECK(strcmp(h1, h3) != 0);

    /* Array order must matter. */
    xb_arena a4, a5;
    xb_arena_init(&a4);
    xb_arena_init(&a5);
    char h4[65], h5[65];
    xb_json_canonical_hash(parse(&a4, "[1,2]"), h4);
    xb_json_canonical_hash(parse(&a5, "[2,1]"), h5);
    XB_CHECK(strcmp(h4, h5) != 0);

    xb_arena_destroy(&a1); xb_arena_destroy(&a2);
    xb_arena_destroy(&a3); xb_arena_destroy(&a4); xb_arena_destroy(&a5);
}

XB_TEST(writer_number_roundtrip)
{
    const double values[] = { 0, 1, -1, 0.5, 1e10, -3.25, 1.0 / 3.0, 123456789.123456 };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        xb_jsonw w;
        xb_jw_init(&w);
        xb_jw_num(&w, values[i]);
        xb_arena a;
        xb_arena_init(&a);
        size_t off = 0;
        const char *text = xb_jw_data(&w);
        xb_json *rt = xb_json_parse(&a, text, strlen(text), &off);
        XB_CHECK_MSG(rt != NULL, "re-parse of %.17g failed", values[i]);
        if (rt) XB_CHECK_NEAR(xb_json_num(rt, 0), values[i], 1e-12);
        xb_arena_destroy(&a);
        xb_jw_free(&w);
    }
}

XB_TEST(writer_nested_structures)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "id", "a1");
    xb_jw_key(&w, "list");
    xb_jw_arr_begin(&w);
    for (int i = 0; i < 3; i++) {
        xb_jw_obj_begin(&w);
        xb_jw_kv_int(&w, "i", i);
        xb_jw_kv_bool(&w, "even", i % 2 == 0);
        xb_jw_kv_null(&w, "extra");
        xb_jw_obj_end(&w);
    }
    xb_jw_arr_end(&w);
    xb_jw_kv_int(&w, "n", 3);
    xb_jw_obj_end(&w);

    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *rt = xb_json_parse(&a, xb_jw_data(&w), xb_jw_len(&w), &off);
    XB_CHECK(rt != NULL);
    XB_CHECK_EQ_INT(xb_json_arr_len(xb_json_obj_get(rt, "list")), 3);
    XB_CHECK_EQ_INT(xb_json_obj_int(xb_json_arr_at(xb_json_obj_get(rt, "list"), 2), "i", -1), 2);
    XB_CHECK_EQ_INT(xb_json_obj_int(rt, "n", 0), 3);
    xb_arena_destroy(&a);
    xb_jw_free(&w);
}

XB_TEST(empty_containers)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_key(&w, "a"); xb_jw_arr_begin(&w); xb_jw_arr_end(&w);
    xb_jw_key(&w, "o"); xb_jw_obj_begin(&w); xb_jw_obj_end(&w);
    xb_jw_obj_end(&w);
    XB_CHECK_EQ_STR(xb_jw_data(&w), "{\"a\":[],\"o\":{}}");
    xb_jw_free(&w);
}

XB_TEST(empty_object_key_is_not_special)
{
    /* Regression guard: the separator logic must not treat an empty nested
     * object as "still inside the key". */
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "k", "");
    xb_jw_kv_str(&w, "k2", "v");
    xb_jw_obj_end(&w);
    XB_CHECK_EQ_STR(xb_jw_data(&w), "{\"k\":\"\",\"k2\":\"v\"}");
    xb_jw_free(&w);
}
