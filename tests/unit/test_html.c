/* test_html.c — tolerant parsing, selector matching, extraction rules. */
#include "xbtest.h"
#include "bridge/html.h"
#include "bridge/util.h"

XB_SUITE("html");

static const char *DOC =
    "<!doctype html><html><head><title>T</title>"
    "<script>var x = '<div class=\"fake\">';</script></head><body>"
    "<div id=\"wrap\" class=\"outer\">"
    "  <ul class=\"list\">"
    "    <li class=\"item\" data-id=\"1\"><a href=\"/a\">First &amp; best</a><span class=\"n\">1</span></li>"
    "    <li class=\"item\" data-id=\"2\"><a href=\"/b\">Second</a><span class=\"n\">2</span></li>"
    "    <li class=\"item\" data-id=\"3\"><a href=\"/c\">Third</a><span class=\"n\">3</span></li>"
    "  </ul>"
    "  <div class=\"content\"><p>Line one</p><p>Line <b>two</b></p></div>"
    "  <img src=\"/img.jpg\" alt=\"cover\" />"
    "</div></body></html>";

/* Helper: evaluate one selector and return the first string. */
static char *sel1(const char *s)
{
    hdoc *d = xb_html_parse(DOC, strlen(DOC));
    hsel *sel = xb_select(d, s);
    char *out = sel ? xb_sel_string(sel, 0) : NULL;
    xb_sel_free(sel);
    xb_html_free(d);
    return out;
}

static size_t sel_count(const char *s)
{
    hdoc *d = xb_html_parse(DOC, strlen(DOC));
    hsel *sel = xb_select(d, s);
    size_t n = sel ? sel->n : 0;
    xb_sel_free(sel);
    xb_html_free(d);
    return n;
}

XB_TEST(parse_and_count)
{
    XB_CHECK_EQ_INT(sel_count("li.item"), 3);
    XB_CHECK_EQ_INT(sel_count("ul li"), 3);
    XB_CHECK_EQ_INT(sel_count("#wrap"), 1);
    XB_CHECK_EQ_INT(sel_count(".missing"), 0);
    XB_CHECK_EQ_INT(sel_count("p"), 2);
}

XB_TEST(script_content_is_not_parsed_as_markup)
{
    /* The fake <div class="fake"> inside <script> must not match. */
    XB_CHECK_EQ_INT(sel_count(".fake"), 0);
    XB_CHECK_EQ_INT(sel_count("div.fake"), 0);
}

XB_TEST(descendant_combinator)
{
    XB_CHECK_EQ_INT(sel_count("#wrap ul.list li.item"), 3);
    XB_CHECK_EQ_INT(sel_count("#wrap .content p"), 2);
    XB_CHECK_EQ_INT(sel_count("ul li a"), 3);
}

XB_TEST(attribute_selection)
{
    XB_CHECK_EQ_INT(sel_count("li[data-id=\"2\"]"), 1);
    XB_CHECK_EQ_INT(sel_count("img[alt]"), 1);
    XB_CHECK_EQ_INT(sel_count("li[data-id=\"9\"]"), 0);
}

XB_TEST(attribute_extraction)
{
    char *v = sel1("li.item a@href");
    XB_CHECK_EQ_STR(v, "/a");
    xb_free(v);

    v = sel1("img@src");
    XB_CHECK_EQ_STR(v, "/img.jpg");
    xb_free(v);

    v = sel1("li[data-id=\"3\"] a@href");
    XB_CHECK_EQ_STR(v, "/c");
    xb_free(v);
}

XB_TEST(text_extraction_and_entities)
{
    char *v = sel1("li.item a@text");
    XB_CHECK_EQ_STR(v, "First & best");    /* &amp; decoded */
    xb_free(v);

    v = sel1("li[data-id=\"2\"] a");
    XB_CHECK_EQ_STR(v, "Second");
    xb_free(v);

    /* Whitespace collapses across child elements. */
    v = sel1("div.content");
    XB_CHECK(v != NULL);
    XB_CHECK(strstr(v, "Line one") != NULL);
    XB_CHECK(strstr(v, "Line two") != NULL);
    XB_CHECK(strstr(v, "  ") == NULL);
    xb_free(v);
}

XB_TEST(html_extraction)
{
    char *v = sel1("div.content@html");
    XB_CHECK(v != NULL);
    XB_CHECK(strstr(v, "<p>Line one</p>") != NULL);
    XB_CHECK(strstr(v, "<b>two</b>") != NULL);
    XB_CHECK(strstr(v, "div.content") == NULL);   /* inner, not outer */
    xb_free(v);
}

XB_TEST(index_selector)
{
    char *v = sel1("li.item[1] a@href");
    XB_CHECK_EQ_STR(v, "/b");
    xb_free(v);

    v = sel1("li.item[2] a@href");
    XB_CHECK_EQ_STR(v, "/c");
    xb_free(v);

    v = sel1("li.item[7] a@href");
    XB_CHECK(v == NULL);   /* out of range is a miss, not a crash */
    xb_free(v);
}

XB_TEST(malformed_html_does_not_crash)
{
    const char *cases[] = {
        "<div><p>unclosed",
        "<div>",
        "plain text",
        "",
        "<a href='unquoted>x",
        "<div class=\"a\" class=\"b\">x</div>",
        "<!-- comment --><p>after</p>",
        "<br><hr><img src=x>",
        "<<<>>>",
        "<div><div><div><div>deep</div></div></div></div>",
    };
    for (size_t i = 0; i < XB_ARRAY_LEN(cases); i++) {
        hdoc *d = xb_html_parse(cases[i], strlen(cases[i]));
        XB_CHECK(d != NULL);
        hsel *s = xb_select(d, "div, p, a, img, br");
        if (s) xb_sel_free(s);
        xb_html_free(d);
    }
}

XB_TEST(deep_nesting_is_bounded)
{
    size_t n = 5000;
    char *html = (char *)xb_alloc(n * 5 * 2 + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) o += (size_t)snprintf(html + o, 6, "<div>");
    html[o] = '\0';
    hdoc *d = xb_html_parse(html, o);
    XB_CHECK(d != NULL);
    xb_html_free(d);
    xb_free(html);
}

XB_TEST(selector_alternatives_and_whitespace)
{
    /* Comma-separated selectors: the first that matches anything wins. */
    XB_CHECK_EQ_INT(sel_count("nope, li.item"), 3);
    XB_CHECK_EQ_INT(sel_count("  li.item  "), 3);
    XB_CHECK_EQ_INT(sel_count("table, #wrap"), 1);
}

XB_TEST(wildcard_and_tag_only)
{
    XB_CHECK(sel_count("*") > 10);
    XB_CHECK_EQ_INT(sel_count("body *"), 16);
}
