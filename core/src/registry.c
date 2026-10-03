/* registry.c — the declarative format table.
 *
 * Each row says what an ecosystem's artifacts look like and what they can do.
 * It is intentionally data, not code: hosts read it (format.list), tests assert
 * on it, and adding an ecosystem means adding a row, not a new code path.
 *
 * Mirrored verbatim under core/formats so non-C hosts see the same truth;
 * `tests/unit/test_registry.c` fails the build if the two ever diverge.
 */
#include "bridge/registry.h"
#include "bridge/json.h"
#include "bridge/buf.h"
#include "bridge/util.h"

#include <string.h>

const char *xb_cap_name(unsigned bit_index)
{
    static const char *NAMES[] = {
        "anime", "manga", "novel", "video", "torrent", "search", "popular",
        "latest", "detail", "pages", "prefs", "filters", "image_proxy"
    };
    if (bit_index >= XB_ARRAY_LEN(NAMES)) return NULL;
    return NAMES[bit_index];
}

/* Shared capability bundles, to keep the table readable. */
#define CAP_READ_MANGA  (XB_CAP_MANGA|XB_CAP_SEARCH|XB_CAP_POPULAR|XB_CAP_LATEST|XB_CAP_DETAIL|XB_CAP_PAGES|XB_CAP_PREFS|XB_CAP_FILTERS)
#define CAP_READ_ANIME  (XB_CAP_ANIME|XB_CAP_VIDEO|XB_CAP_SEARCH|XB_CAP_POPULAR|XB_CAP_LATEST|XB_CAP_DETAIL|XB_CAP_PREFS|XB_CAP_FILTERS)
#define CAP_READ_NOVEL  (XB_CAP_NOVEL|XB_CAP_SEARCH|XB_CAP_POPULAR|XB_CAP_LATEST|XB_CAP_DETAIL|XB_CAP_PREFS|XB_CAP_FILTERS)

static const xb_format FORMATS[] = {
    {
        "aniyomi", "Aniyomi", "aniyomi",
        ".apk,.jar",
        "classes.dex;AndroidManifest.xml;eu/kanade/tachiyomi",
        "jvm",
        "eu.kanade.tachiyomi.animesource.AnimeSource",
        CAP_READ_ANIME | CAP_READ_MANGA | XB_CAP_IMAGE_PROXY,
        XB_PLAT_ALL,
        "Tachiyomi-compatible sources. The extension class implements AnimeSource, "
        "MangaSource or both; the loader reads sources from sourceFactory()."
    },
    {
        "cloudstream", "CloudStream", "cloudstream",
        ".jar,.cs",
        "com/lagradost/cloudstream3;META-INF/services",
        "jvm",
        "com.lagradost.cloudstream3.MainAPI",
        XB_CAP_VIDEO | XB_CAP_ANIME | XB_CAP_MANGA | XB_CAP_SEARCH | XB_CAP_POPULAR
            | XB_CAP_LATEST | XB_CAP_DETAIL | XB_CAP_PREFS | XB_CAP_FILTERS | XB_CAP_IMAGE_PROXY,
        XB_PLAT_ALL,
        "Plugins extending MainAPI. Android-API surface is shimmed by the JVM worker "
        "so the same jar runs on desktop."
    },
    {
        "kotatsu", "Kotatsu", "kotatsu",
        ".jar,.apk",
        "org/koitharu/kotatsu/parsers",
        "jvm",
        "org.koitharu.kotatsu.parsers.MangaParser",
        CAP_READ_MANGA | XB_CAP_IMAGE_PROXY,
        XB_PLAT_ALL,
        "kotatsu-parsers bundles. Sources are discovered from the parser factory "
        "listed in META-INF/services."
    },
    {
        "legado", "Legado", "legado",
        ".json,.txt",
        "bookSourceUrl;ruleSearch",
        "rule",
        "",
        CAP_READ_NOVEL,
        XB_PLAT_ALL,
        "Legado book-source rule files. Interpreted by the built-in rule engine, so "
        "no JavaScript runtime is required."
    },
    {
        "lnreader", "LnReader", "lnreader",
        ".js,.json",
        "plugin.js;manifest.json;novel",
        "js",
        "",
        CAP_READ_NOVEL,
        XB_PLAT_ALL,
        "plugin.js plus manifest.json. Executed with a QuickJS-compatible host "
        "providing fetch, cheerio and the LnReader globals."
    },
    {
        "mangayomi", "Mangayomi", "mangayomi",
        ".js,.dart,.json",
        "MProvider;anime;manga;novel",
        "js",
        "",
        XB_CAP_ANIME | XB_CAP_MANGA | XB_CAP_NOVEL | XB_CAP_VIDEO | XB_CAP_SEARCH
            | XB_CAP_POPULAR | XB_CAP_LATEST | XB_CAP_DETAIL | XB_CAP_PAGES
            | XB_CAP_PREFS | XB_CAP_FILTERS,
        XB_PLAT_ALL,
        "Mangayomi source bundles. Both the JavaScript and the Dart-flavoured "
        "bundles are accepted; the Dart variant is translated by the js worker."
    },
    {
        "sora", "Sora", "sora",
        ".js,.json",
        "SoraExtension;module.exports",
        "js",
        "",
        XB_CAP_ANIME | XB_CAP_MANGA | XB_CAP_VIDEO | XB_CAP_SEARCH | XB_CAP_POPULAR
            | XB_CAP_LATEST | XB_CAP_DETAIL | XB_CAP_PREFS,
        XB_PLAT_ALL,
        "Sora modules exporting an extension object with a fixed method set."
    },
    {
        "tsundoku", "Tsundoku", "tsundoku",
        ".apk,.jar",
        "com/tsundoku;AndroidManifest.xml",
        "jvm",
        "com.tsundoku.source.Source",
        CAP_READ_NOVEL | CAP_READ_MANGA,
        XB_PLAT_ALL,
        "Tsundoku novel and manga sources, run on the same JVM worker as Aniyomi."
    },
    {
        "ireader", "iReader", "ireader",
        ".apk,.jar",
        "ireader;AndroidManifest.xml",
        "jvm",
        "ireader.source.Source",
        CAP_READ_NOVEL,
        XB_PLAT_ALL,
        "iReader novel sources."
    },
    {
        "torrserver", "TorrServer", "torrserver",
        ".bin,.exe,el",
        "",
        "native",
        "",
        XB_CAP_TORRENT | XB_CAP_VIDEO,
        XB_PLAT_ALL,
        "TorrServer addon. Started as a supervised native process; on platforms "
        "where a static build is not available the daemon reports it unavailable "
        "rather than failing the whole bridge."
    }
};

size_t xb_format_count(void) { return XB_ARRAY_LEN(FORMATS); }

const xb_format *xb_format_at(size_t i)
{
    return i < XB_ARRAY_LEN(FORMATS) ? &FORMATS[i] : NULL;
}

const xb_format *xb_format_by_id(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < XB_ARRAY_LEN(FORMATS); i++)
        if (xb_strieq(FORMATS[i].id, id)) return &FORMATS[i];
    return NULL;
}

const xb_format *xb_format_by_manager(const char *manager_id)
{
    if (!manager_id) return NULL;
    for (size_t i = 0; i < XB_ARRAY_LEN(FORMATS); i++)
        if (xb_strieq(FORMATS[i].manager_id, manager_id)) return &FORMATS[i];
    return NULL;
}

static void write_str_list(xb_jsonw *w, const char *key, const char *csv)
{
    xb_jw_key(w, key);
    xb_jw_arr_begin(w);
    if (csv && csv[0]) {
        char tmp[256];
        xb_str_lcpy(tmp, csv, sizeof tmp);
        char *fields[16];
        size_t n = xb_str_split(tmp, ',', fields, 16);
        for (size_t i = 0; i < n; i++) {
            char *f = xb_str_trim(fields[i]);
            if (*f) xb_jw_str(w, f);
        }
    }
    xb_jw_arr_end(w);
}

char *xb_format_json(const xb_format *f)
{
    if (!f) return xb_strdup("null");
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "id", f->id);
    xb_jw_kv_str(&w, "label", f->label);
    xb_jw_kv_str(&w, "manager_id", f->manager_id);
    xb_jw_kv_str(&w, "engine", f->engine);
    if (f->ext_class_hint && f->ext_class_hint[0])
        xb_jw_kv_str(&w, "extension_class", f->ext_class_hint);
    write_str_list(&w, "artifact_kinds", f->artifact_kinds);
    write_str_list(&w, "identify_markers",
                   (f->identify_globs && f->identify_globs[0]) ? f->identify_globs : "");

    xb_jw_key(&w, "capabilities");
    xb_jw_arr_begin(&w);
    for (unsigned b = 0; b < 32; b++) {
        if (f->caps & (1u << b)) {
            const char *n = xb_cap_name(b);
            if (n) xb_jw_str(&w, n);
        }
    }
    xb_jw_arr_end(&w);

    xb_jw_key(&w, "platforms");
    xb_jw_arr_begin(&w);
    if (f->platforms & XB_PLAT_ANDROID) xb_jw_str(&w, "android");
    if (f->platforms & XB_PLAT_IOS)     xb_jw_str(&w, "ios");
    if (f->platforms & XB_PLAT_WINDOWS) xb_jw_str(&w, "windows");
    if (f->platforms & XB_PLAT_MACOS)   xb_jw_str(&w, "macos");
    if (f->platforms & XB_PLAT_LINUX)   xb_jw_str(&w, "linux");
    xb_jw_arr_end(&w);

    xb_jw_kv_str(&w, "notes", f->notes);
    xb_jw_obj_end(&w);
    return xb_buf_steal(&w.buf);
}

char *xb_registry_json(void)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_arr_begin(&w);
    for (size_t i = 0; i < XB_ARRAY_LEN(FORMATS); i++) {
        char *one = xb_format_json(&FORMATS[i]);
        xb_jw_raw(&w, one);
        xb_free(one);
    }
    xb_jw_arr_end(&w);
    return xb_buf_steal(&w.buf);
}
