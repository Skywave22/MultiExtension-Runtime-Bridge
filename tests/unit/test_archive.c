/* test_archive.c — artifact classification: what *is* this file, really?
 *
 * The claim under test is that detection reads content, not the extension: an
 * `.apk` that is really a Kotatsu jar, a `.js` that is a Mangayomi module, a
 * directory of loose LnReader files. The zip fixtures are written by the test
 * itself (stored entries only) so the repository does not carry binaries.
 */
#include "xbtest.h"
#include "bridge/archive.h"
#include "bridge/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

XB_SUITE("archive");

/* ------------------------------------------------------------ zip writer -- */

typedef struct {
    const char *name;
    const char *data;
} zent;

static void w16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
}

static void w32(FILE *f, unsigned long v)
{
    w16(f, (unsigned)(v & 0xfffful));
    w16(f, (unsigned)((v >> 16) & 0xfffful));
}

/* A minimal but real zip: local headers, central directory, EOCD. `method` is
 * written into both headers but data is only emitted for stored (0) entries,
 * which is what lets us build a "deflated" entry that a reader must refuse. */
static int zip_make(const char *path, const zent *e, size_t n, unsigned method)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned long *off = (unsigned long *)calloc(n ? n : 1, sizeof *off);
    if (!off) { fclose(f); return -1; }

    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(e[i].data), nl = strlen(e[i].name);
        off[i] = (unsigned long)ftell(f);
        w32(f, 0x04034b50); w16(f, 20); w16(f, 0); w16(f, method);
        w16(f, 0); w16(f, 0);
        w32(f, 0); w32(f, (unsigned long)len); w32(f, (unsigned long)len);
        w16(f, (unsigned)nl); w16(f, 0);
        fwrite(e[i].name, 1, nl, f);
        if (method == 0 && len) fwrite(e[i].data, 1, len, f);
    }

    unsigned long cd_start = (unsigned long)ftell(f);
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(e[i].data), nl = strlen(e[i].name);
        w32(f, 0x02014b50); w16(f, 20); w16(f, 20); w16(f, 0); w16(f, method);
        w16(f, 0); w16(f, 0); w32(f, 0);
        w32(f, (unsigned long)len); w32(f, (unsigned long)len);
        w16(f, (unsigned)nl); w16(f, 0); w16(f, 0);
        w16(f, 0); w16(f, 0); w32(f, 0); w32(f, off[i]);
        fwrite(e[i].name, 1, nl, f);
    }
    unsigned long cd_end = (unsigned long)ftell(f);

    w32(f, 0x06054b50); w16(f, 0); w16(f, 0);
    w16(f, (unsigned)n); w16(f, (unsigned)n);
    w32(f, cd_end - cd_start); w32(f, cd_start);
    w16(f, 0);

    fclose(f);
    free(off);
    return 0;
}

static void write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    if (!f) { XB_CHECK(0); return; }
    fwrite(data, 1, strlen(data), f);
    fclose(f);
}

static void rm_rf(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* nothing useful to do in a test */ }
}

/* ------------------------------------------------------------ magic bytes -- */

XB_TEST(sniff_magic_classifies_the_first_bytes)
{
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("PK\003\004rest", 8), XB_ART_ZIP);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("PK\005\006", 4), XB_ART_ZIP);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("\x1f\x8b\x08\x00", 4), XB_ART_TAR_GZ);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("\x89PNG\r\n\x1a\n", 8), XB_ART_BINARY);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("\x7f" "ELF\002\001\001", 7), XB_ART_BINARY);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("MZ\x90\x00", 4), XB_ART_BINARY);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("  \n\t{\"a\":1}", 12), XB_ART_JSON);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("  \n\t// comment", 14), XB_ART_JS);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("module.exports = {};", 20), XB_ART_JS);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("", 0), XB_ART_UNKNOWN);
    XB_CHECK_EQ_INT(xb_artifact_sniff_magic("\x01\x02\x03", 3), XB_ART_UNKNOWN);

    XB_CHECK_EQ_STR(xb_artifact_kind_name(XB_ART_APK), "apk");
    XB_CHECK_EQ_STR(xb_artifact_kind_name(XB_ART_JAR), "jar");
    XB_CHECK_EQ_STR(xb_artifact_kind_name(XB_ART_DIR), "dir");
    XB_CHECK_EQ_STR(xb_artifact_kind_name(XB_ART_TAR_GZ), "tar.gz");
    XB_CHECK_EQ_STR(xb_artifact_kind_name(XB_ART_ZIP), "zip");
    XB_CHECK_EQ_STR(xb_artifact_kind_name((xb_artifact_kind)999), "unknown");
}

/* ---------------------------------------------------------------- zip --- */

XB_TEST(zip_listing_reads_a_real_central_directory)
{
    char dir[] = "/tmp/xbtest-arc-XXXXXX";
    if (!mkdtemp(dir)) { XB_CHECK(0); return; }
    char path[256];
    snprintf(path, sizeof path, "%s/plugin.zip", dir);

    const zent entries[] = {
        { "AndroidManifest.xml", "<manifest/>" },
        { "classes.dex", "dex" },
        { "com/lagradost/cloudstream3/MainAPI.class", "cafebabe" },
        { "assets/info.json", "{\"name\":\"demo\"}" },
    };
    XB_CHECK_EQ_INT(zip_make(path, entries, XB_ARRAY_LEN(entries), 0), 0);

    xb_zip_listing l;
    XB_CHECK_EQ_INT(xb_zip_list(path, &l), 0);
    XB_CHECK_EQ_INT(l.entry_count, 4);
    XB_CHECK(!l.truncated);
    XB_CHECK_EQ_INT(l.entries[0].size, strlen("<manifest/>"));
    XB_CHECK_EQ_INT(l.entries[0].compressed, strlen("<manifest/>"));

    XB_CHECK(xb_zip_listing_has(&l, "AndroidManifest.xml"));
    XB_CHECK(xb_zip_listing_has(&l, "classes*.dex"));
    XB_CHECK(xb_zip_listing_has(&l, "com/lagradost/*/MainAPI.class"));
    XB_CHECK(xb_zip_listing_has(&l, "assets/????.json"));
    XB_CHECK(!xb_zip_listing_has(&l, "META-INF/services/*"));
    XB_CHECK(!xb_zip_listing_has(&l, "classes*.jar"));
    XB_CHECK_EQ_STR(xb_zip_listing_find(&l, "*.class"),
                    "com/lagradost/cloudstream3/MainAPI.class");
    XB_CHECK(xb_zip_listing_find(&l, "nothing*") == NULL);
    xb_zip_listing_free(&l);

    /* Extraction: stored entries come back, everything else is refused. */
    char *data = NULL;
    size_t len = 0;
    XB_CHECK_EQ_INT(xb_zip_read_entry(path, "assets/info.json", &data, &len, 4096), 0);
    XB_CHECK_EQ_INT(len, strlen("{\"name\":\"demo\"}"));
    if (data) XB_CHECK_EQ_STR(data, "{\"name\":\"demo\"}");
    xb_free(data);
    data = NULL;
    XB_CHECK_EQ_INT(xb_zip_read_entry(path, "AndroidManifest.xml", &data, &len, 4), -2);
    XB_CHECK_EQ_INT(xb_zip_read_entry(path, "no/such/entry", &data, &len, 4096), -1);

    /* A deflated entry: refused with -3, never half-read. */
    char dpath[256];
    snprintf(dpath, sizeof dpath, "%s/deflated.zip", dir);
    const zent one[] = { { "META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n" } };
    XB_CHECK_EQ_INT(zip_make(dpath, one, 1, 8), 0);
    XB_CHECK_EQ_INT(xb_zip_read_entry(dpath, "META-INF/MANIFEST.MF",
                                      &data, &len, 4096), -3);

    /* A file that is not a zip at all: an error, not a crash. */
    char junk[256];
    snprintf(junk, sizeof junk, "%s/junk.zip", dir);
    write_file(junk, "this is not a zip, it is just a sentence");
    xb_zip_listing bad;
    XB_CHECK(xb_zip_list(junk, &bad) != 0);
    XB_CHECK(bad.error[0] != '\0');
    XB_CHECK_EQ_INT(bad.entry_count, 0);
    XB_CHECK(xb_zip_list("/nonexistent/definitely/missing.zip", &bad) != 0);
    XB_CHECK(xb_zip_read_entry(junk, "x", &data, &len, 16) != 0);

    rm_rf(dir);
}

XB_TEST(zip_entry_count_is_capped)
{
    enum { N = XB_ARCHIVE_MAX_ENTRIES + 128 };
    char dir[] = "/tmp/xbtest-arc-XXXXXX";
    if (!mkdtemp(dir)) { XB_CHECK(0); return; }
    char path[256];
    snprintf(path, sizeof path, "%s/many.zip", dir);

    zent *entries = (zent *)calloc(N, sizeof *entries);
    char **names = (char **)calloc(N, sizeof *names);
    for (int i = 0; i < N; i++) {
        char *nm = (char *)calloc(32, 1);
        snprintf(nm, 32, "dir%d/file%d.txt", i, i);
        names[i] = nm;
        entries[i].name = nm;
        entries[i].data = "";
    }
    XB_CHECK_EQ_INT(zip_make(path, entries, N, 0), 0);

    xb_zip_listing l;
    XB_CHECK_EQ_INT(xb_zip_list(path, &l), 0);
    XB_CHECK(l.truncated);                       /* capped, and it says so */
    XB_CHECK_EQ_INT(l.entry_count, XB_ARCHIVE_MAX_ENTRIES);
    xb_zip_listing_free(&l);

    for (int i = 0; i < N; i++) free(names[i]);
    free(names);
    free(entries);
    rm_rf(dir);
}

/* ------------------------------------------------------------- detection -- */

XB_TEST(detect_reads_content_not_the_extension)
{
    char dir[] = "/tmp/xbtest-arc-XXXXXX";
    if (!mkdtemp(dir)) { XB_CHECK(0); return; }
    xb_detect_result d;

    /* CloudStream plugin shipped as a .apk. */
    {
        char p[256];
        snprintf(p, sizeof p, "%s/cloudstream.jar", dir);   /* extension lies */
        const zent e[] = {
            { "AndroidManifest.xml", "<manifest package=\"com.lagradost\"/>" },
            { "classes.dex", "dex" },
            { "com/lagradost/cloudstream3/MainAPI.class", "cafebabe" },
            { "com/lagradost/cloudstream3/plugins/TestPlugin.class", "cafebabe" },
        };
        XB_CHECK_EQ_INT(zip_make(p, e, XB_ARRAY_LEN(e), 0), 0);
        XB_CHECK_EQ_INT(xb_detect_path(p, &d), 0);
        XB_CHECK_EQ_INT(d.kind, XB_ART_APK);        /* AndroidManifest.xml wins */
        XB_CHECK_EQ_STR(d.format_id, "cloudstream");
        XB_CHECK_EQ_STR(d.manager_id, "cloudstream");
        XB_CHECK(d.confidence >= 90);
        XB_CHECK(strstr(d.evidence, "cloudstream") != NULL);
        XB_CHECK_EQ_STR(d.entry_point, "classes.dex");
    }

    /* Aniyomi extension jar. */
    {
        char p[256];
        snprintf(p, sizeof p, "%s/aniyomi.apk", dir);
        const zent e[] = {
            { "META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n" },
            { "META-INF/services/eu.kanade.tachiyomi.AnimeSourceFactory", "x" },
            { "eu/kanade/tachiyomi/animesource/AnimeSource.class", "cafebabe" },
        };
        XB_CHECK_EQ_INT(zip_make(p, e, XB_ARRAY_LEN(e), 0), 0);
        XB_CHECK_EQ_INT(xb_detect_path(p, &d), 0);
        /* The name says .apk but there is no AndroidManifest.xml, so it is a
         * jar — and the ecosystem is still recognised. */
        XB_CHECK_EQ_INT(d.kind, XB_ART_JAR);
        XB_CHECK_EQ_STR(d.format_id, "aniyomi");
        XB_CHECK(d.confidence >= 90);
    }

    /* Kotatsu parsers jar, correctly named. */
    {
        char p[256];
        snprintf(p, sizeof p, "%s/parsers.jar", dir);
        const zent e[] = {
            { "META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n" },
            { "org/koitharu/kotatsu/parsers/site/madara/MadaraParser.kt", "x" },
        };
        XB_CHECK_EQ_INT(zip_make(p, e, XB_ARRAY_LEN(e), 0), 0);
        XB_CHECK_EQ_INT(xb_detect_path(p, &d), 0);
        XB_CHECK_EQ_INT(d.kind, XB_ART_JAR);
        XB_CHECK_EQ_STR(d.format_id, "kotatsu");
    }

    /* A zip with nothing recognisable in it: classified, honestly scored. */
    {
        char p[256];
        snprintf(p, sizeof p, "%s/mystery.zip", dir);
        const zent e[] = { { "readme.txt", "hello" } };
        XB_CHECK_EQ_INT(zip_make(p, e, 1, 0), 0);
        XB_CHECK_EQ_INT(xb_detect_path(p, &d), 0);
        XB_CHECK_EQ_INT(d.kind, XB_ART_ZIP);
        XB_CHECK_EQ_INT(d.confidence, 0);
        XB_CHECK_EQ_STR(d.format_id, "");
        XB_CHECK(strstr(d.evidence, "no known ecosystem") != NULL);
    }

    /* Loose files: a Mangayomi-style module and a Legado rule file. */
    {
        char p[256];
        snprintf(p, sizeof p, "%s/source.js", dir);
        write_file(p, "class extends MProvider { registerProvider(this); }\n");
        XB_CHECK_EQ_INT(xb_detect_path(p, &d), 0);
        XB_CHECK_EQ_INT(d.kind, XB_ART_JS);
        XB_CHECK_EQ_STR(d.format_id, "mangayomi");

        snprintf(p, sizeof p, "%s/rules.json", dir);
        write_file(p, "[{\"bookSourceUrl\":\"https://example.tld\",\"ruleSearch\":{}}]");
        XB_CHECK_EQ_INT(xb_detect_path(p, &d), 0);
        XB_CHECK_EQ_INT(d.kind, XB_ART_JSON);
        XB_CHECK_EQ_STR(d.format_id, "legado");
        XB_CHECK(d.confidence >= 80);

        /* A directory of loose files, detected as one artifact. */
        char sub[256];
        snprintf(sub, sizeof sub, "%s/bundle", dir);
        XB_CHECK_EQ_INT(mkdir(sub, 0700), 0);
        char f1[300], f2[300];
        snprintf(f1, sizeof f1, "%s/manifest.json", sub);
        snprintf(f2, sizeof f2, "%s/plugin.js", sub);
        write_file(f1, "{\"id\":\"demo\"}");
        write_file(f2, "module.exports = {};");
        XB_CHECK_EQ_INT(xb_detect_path(sub, &d), 0);
        XB_CHECK_EQ_INT(d.kind, XB_ART_DIR);
        XB_CHECK_EQ_STR(d.format_id, "lnreader");
        XB_CHECK_EQ_STR(d.entry_point, "plugin.js");
    }

    /* Native addons and gzip streams are recognised for what they are. */
    {
        char p[256];
        snprintf(p, sizeof p, "%s/TorrServer.bin", dir);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite("\x7f" "ELF\002\001\001", 1, 7, f); fclose(f); }
        XB_CHECK_EQ_INT(xb_detect_path(p, &d), 0);
        XB_CHECK_EQ_INT(d.kind, XB_ART_BINARY);
        XB_CHECK_EQ_STR(d.format_id, "torrserver");

        snprintf(p, sizeof p, "%s/archive.tar.gz", dir);
        f = fopen(p, "wb");
        if (f) { fwrite("\x1f\x8b\x08\x00rest", 1, 8, f); fclose(f); }
        XB_CHECK_EQ_INT(xb_detect_path(p, &d), 0);
        XB_CHECK_EQ_INT(d.kind, XB_ART_TAR_GZ);
    }

    /* A path that is not there is an error, not a guess. */
    XB_CHECK(xb_detect_path("/nonexistent/nope.xyz", &d) != 0);
    rm_rf(dir);
}

XB_TEST(detect_json_is_machine_readable)
{
    xb_detect_result d;
    memset(&d, 0, sizeof d);
    d.kind = XB_ART_APK;
    xb_str_lcpy(d.format_id, "cloudstream", sizeof d.format_id);
    xb_str_lcpy(d.manager_id, "cloudstream", sizeof d.manager_id);
    d.confidence = 95;
    xb_str_lcpy(d.entry_point, "classes.dex", sizeof d.entry_point);
    xb_str_lcpy(d.evidence, "found com/lagradost in the archive", sizeof d.evidence);

    char *json = xb_detect_json(&d);
    XB_CHECK(json != NULL);
    if (json) {
        XB_CHECK(strstr(json, "\"format\":\"cloudstream\"") != NULL);
        XB_CHECK(strstr(json, "\"manager_id\":\"cloudstream\"") != NULL);
        XB_CHECK(strstr(json, "\"artifact_kind\":\"apk\"") != NULL);
        XB_CHECK(strstr(json, "\"confidence\":95") != NULL);
        XB_CHECK(strstr(json, "\"entry_point\":\"classes.dex\"") != NULL);
        xb_free(json);
    }
}
