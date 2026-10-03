/* test_registry.c — the format table must stay internally consistent and must
 * match the JSON mirror in core/formats, which non-C hosts read. */
#include "xbtest.h"
#include "bridge/registry.h"
#include "bridge/json.h"
#include "bridge/buf.h"
#include "bridge/util.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

XB_SUITE("registry");

/* Every manager id documented in docs/METHODS.md must exist. */
static const char *EXPECTED_MANAGERS[] = {
    "aniyomi", "cloudstream", "kotatsu", "legado", "lnreader",
    "mangayomi", "sora", "tsundoku", "ireader", "torrserver", NULL
};

static const char *KNOWN_ENGINES[] = { "jvm", "js", "rule", "native", NULL };

XB_TEST(all_expected_formats_present)
{
    for (int i = 0; EXPECTED_MANAGERS[i]; i++) {
        const xb_format *f = xb_format_by_manager(EXPECTED_MANAGERS[i]);
        XB_CHECK_MSG(f != NULL, "manager '%s' is missing", EXPECTED_MANAGERS[i]);
        if (f) XB_CHECK_EQ_STR(f->manager_id, EXPECTED_MANAGERS[i]);
    }
    XB_CHECK_EQ_INT(xb_format_count(), 10);
}

XB_TEST(ids_and_managers_are_unique)
{
    for (size_t i = 0; i < xb_format_count(); i++) {
        const xb_format *a = xb_format_at(i);
        XB_CHECK(a->id[0] != '\0');
        XB_CHECK(a->label[0] != '\0');
        XB_CHECK(a->engine[0] != '\0');
        XB_CHECK(a->caps != 0);
        XB_CHECK(a->platforms != 0);
        for (size_t j = i + 1; j < xb_format_count(); j++) {
            const xb_format *b = xb_format_at(j);
            XB_CHECK_MSG(!xb_strieq(a->id, b->id), "duplicate id %s", a->id);
            XB_CHECK_MSG(!xb_strieq(a->manager_id, b->manager_id),
                         "duplicate manager_id %s", a->manager_id);
        }
    }
}

XB_TEST(every_format_names_a_known_engine)
{
    for (size_t i = 0; i < xb_format_count(); i++) {
        const xb_format *f = xb_format_at(i);
        bool known = false;
        for (int e = 0; KNOWN_ENGINES[e]; e++)
            if (xb_strieq(f->engine, KNOWN_ENGINES[e])) known = true;
        XB_CHECK_MSG(known, "%s declares unknown engine '%s'", f->id, f->engine);
    }
}

XB_TEST(capability_bits_have_names)
{
    for (size_t i = 0; i < xb_format_count(); i++) {
        const xb_format *f = xb_format_at(i);
        for (unsigned bit = 0; bit < 32; bit++) {
            if (!(f->caps & (1u << bit))) continue;
            XB_CHECK_MSG(xb_cap_name(bit) != NULL,
                         "%s sets capability bit %u which has no name", f->id, bit);
        }
    }
    XB_CHECK(xb_cap_name(0) != NULL);
    XB_CHECK(xb_cap_name(99) == NULL);
}

XB_TEST(json_shape_is_stable)
{
    char *json = xb_format_json(xb_format_by_id("aniyomi"));
    XB_CHECK(json != NULL);
    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *v = xb_json_parse(&a, json, strlen(json), &off);
    XB_CHECK(v != NULL);
    XB_CHECK_EQ_STR(xb_json_obj_str(v, "id", ""), "aniyomi");
    XB_CHECK_EQ_STR(xb_json_obj_str(v, "manager_id", ""), "aniyomi");
    XB_CHECK_EQ_STR(xb_json_obj_str(v, "engine", ""), "jvm");
    XB_CHECK(xb_json_arr_len(xb_json_obj_get(v, "capabilities")) > 0);
    XB_CHECK(xb_json_arr_len(xb_json_obj_get(v, "platforms")) == 5);
    xb_arena_destroy(&a);
    xb_free(json);
}

XB_TEST(registry_json_is_a_valid_array)
{
    char *json = xb_registry_json();
    xb_arena a;
    xb_arena_init(&a);
    size_t off = 0;
    xb_json *v = xb_json_parse(&a, json, strlen(json), &off);
    XB_CHECK(v != NULL && v->type == XB_JARR);
    XB_CHECK_EQ_INT(xb_json_arr_len(v), (int)xb_format_count());
    xb_arena_destroy(&a);
    xb_free(json);
}

/* The JSON mirror under core/formats is generated from this table; if they
 * diverge, a non-C host would route to an engine this daemon does not have. */
XB_TEST(json_mirror_matches_the_table)
{
    const char *path = getenv("XBRIDGE_FORMATS_DIR");
    char dir[512];
    if (path && path[0]) xb_str_lcpy(dir, path, sizeof dir);
    else xb_str_lcpy(dir, "core/formats", sizeof dir);

    for (size_t i = 0; i < xb_format_count(); i++) {
        const xb_format *f = xb_format_at(i);
        char file[700];
        snprintf(file, sizeof file, "%s/%s.json", dir, f->id);
        FILE *fp = fopen(file, "rb");
        XB_CHECK_MSG(fp != NULL, "missing mirror %s (run tools/gen_formats.sh)", file);
        if (!fp) continue;
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, fp);
        buf[n] = '\0';
        fclose(fp);

        xb_arena a;
        xb_arena_init(&a);
        size_t off = 0;
        xb_json *v = xb_json_parse(&a, buf, n, &off);
        XB_CHECK_MSG(v != NULL, "mirror %s is not valid JSON", file);
        if (v) {
            XB_CHECK_MSG(xb_streq(xb_json_obj_str(v, "id", ""), f->id),
                         "mirror %s has id '%s'", file, xb_json_obj_str(v, "id", ""));
            XB_CHECK_MSG(xb_streq(xb_json_obj_str(v, "engine", ""), f->engine),
                         "mirror %s engine mismatch (%s vs %s)", file,
                         xb_json_obj_str(v, "engine", ""), f->engine);
            XB_CHECK_MSG(xb_streq(xb_json_obj_str(v, "manager_id", ""), f->manager_id),
                         "mirror %s manager_id mismatch", file);
        }
        xb_arena_destroy(&a);
    }
}
