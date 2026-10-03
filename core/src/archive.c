/* archive.c — zip index reader, glob matcher and artifact classification. */
#include "bridge/archive.h"
#include "bridge/util.h"
#include "bridge/log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define XB_STAT _stat64
#  define XB_STAT_STRUCT struct _stat64
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  include <dirent.h>
#  define XB_STAT stat
#  define XB_STAT_STRUCT struct stat
#endif

const char *xb_artifact_kind_name(xb_artifact_kind k)
{
    switch (k) {
    case XB_ART_UNKNOWN: return "unknown";
    case XB_ART_ZIP:     return "zip";
    case XB_ART_APK:     return "apk";
    case XB_ART_JAR:     return "jar";
    case XB_ART_JS:      return "js";
    case XB_ART_JSON:    return "json";
    case XB_ART_DIR:     return "dir";
    case XB_ART_TAR_GZ:  return "tar.gz";
    case XB_ART_BINARY:  return "binary";
    }
    return "unknown";
}

xb_artifact_kind xb_artifact_sniff_magic(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (len >= 4 && p[0] == 'P' && p[1] == 'K' &&
        ((p[2] == 3 && p[3] == 4) || (p[2] == 5 && p[3] == 6) ||
         (p[2] == 7 && p[3] == 8)))
        return XB_ART_ZIP;
    if (len >= 2 && p[0] == 0x1F && p[1] == 0x8B) return XB_ART_TAR_GZ;
    if (len >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return XB_ART_BINARY;
    if (len >= 4 && p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F')
        return XB_ART_BINARY;
    if (len >= 2 && p[0] == 'M' && p[1] == 'Z') return XB_ART_BINARY;
    /* Look at the first non-space byte for text formats. */
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
    if (i < len && p[i] == '{') return XB_ART_JSON;
    if (i < len && (isalpha(p[i]) || p[i] == '(' || p[i] == '/' || p[i] == '_')) return XB_ART_JS;
    return XB_ART_UNKNOWN;
}

/* ------------------------------------------------------------ glob match -- */

/* '*' matches any run (including '/'), '?' matches exactly one character.
 * Iterative with backtracking, no recursion, so a pathological pattern cannot
 * blow the stack. */
static bool glob_match(const char *pat, const char *str)
{
    const char *p = pat, *s = str;
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*p == '*' ) { star = p++; ss = s; continue; }
        if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) { p++; s++; continue; }
        if (star) { p = star + 1; s = ++ss; continue; }
        return false;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

bool xb_zip_listing_has(const xb_zip_listing *l, const char *glob)
{
    return xb_zip_listing_find(l, glob) != NULL;
}

const char *xb_zip_listing_find(const xb_zip_listing *l, const char *glob)
{
    if (!l || !glob) return NULL;
    for (size_t i = 0; i < l->entry_count; i++)
        if (glob_match(glob, l->entries[i].name)) return l->entries[i].name;
    return NULL;
}

/* -------------------------------------------------------------- zip index -- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

#define ZEOCD_SIG  0x06054b50u
#define ZCEN_SIG   0x02014b50u
#define ZLOC_SIG   0x04034b50u

int xb_zip_list(const char *path, xb_zip_listing *out)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(out->error, sizeof out->error, "cannot open %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(out->error, sizeof out->error, "seek failed");
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size < 22) {
        snprintf(out->error, sizeof out->error, "file too small to be a zip");
        fclose(f);
        return -1;
    }

    /* Find the End Of Central Directory record, scanning back over the comment. */
    long scan = size - 22;
    long lowest = size - 22 - 65535;
    if (lowest < 0) lowest = 0;
    uint8_t buf[22];
    long eocd_at = -1;
    while (scan >= lowest) {
        if (fseek(f, scan, SEEK_SET) == 0 &&
            fread(buf, 1, sizeof buf, f) == sizeof buf &&
            rd32(buf) == ZEOCD_SIG) {
            eocd_at = scan;
            break;
        }
        scan--;
    }
    if (eocd_at < 0) {
        snprintf(out->error, sizeof out->error, "no zip end-of-central-directory record");
        fclose(f);
        return -1;
    }

    uint16_t total = rd16(buf + 10);
    uint32_t cd_size = rd32(buf + 12);
    uint32_t cd_off = rd32(buf + 16);
    (void)cd_size;

    if (fseek(f, (long)cd_off, SEEK_SET) != 0) {
        snprintf(out->error, sizeof out->error, "cannot seek to central directory");
        fclose(f);
        return -1;
    }

    size_t cap = total < 64 ? 64 : total;
    out->entries = (xb_zip_entry *)xb_alloc(sizeof(xb_zip_entry) * cap);

    for (uint16_t i = 0; i < total; i++) {
        uint8_t h[46];
        if (fread(h, 1, sizeof h, f) != sizeof h) break;
        if (rd32(h) != ZCEN_SIG) {
            snprintf(out->error, sizeof out->error,
                     "central directory entry %u has bad signature", (unsigned)i);
            break;
        }
        uint16_t name_len = rd16(h + 28);
        uint16_t extra_len = rd16(h + 30);
        uint16_t comment_len = rd16(h + 32);
        uint32_t comp = rd32(h + 20);
        uint32_t uncomp = rd32(h + 24);

        if (name_len > 4096) {
            snprintf(out->error, sizeof out->error, "entry %u has an implausible name", (unsigned)i);
            break;
        }
        if (out->entry_count >= XB_ARCHIVE_MAX_ENTRIES) {
            out->truncated = true;
            break;
        }
        char *name = (char *)xb_alloc(name_len + 1);
        if (fread(name, 1, name_len, f) != name_len) { xb_free(name); break; }
        name[name_len] = '\0';
        if (fseek(f, extra_len + comment_len, SEEK_CUR) != 0) { xb_free(name); break; }

        out->entries[out->entry_count].name = name;
        out->entries[out->entry_count].size = uncomp;
        out->entries[out->entry_count].compressed = comp;
        out->entry_count++;
    }

    fclose(f);

    if (out->entry_count == 0) {
        snprintf(out->error, sizeof out->error, "zip contains no readable entries");
        return -1;
    }
    return 0;
}

void xb_zip_listing_free(xb_zip_listing *l)
{
    if (!l || !l->entries) return;
    for (size_t i = 0; i < l->entry_count; i++) xb_free(l->entries[i].name);
    xb_free(l->entries);
    l->entries = NULL;
    l->entry_count = 0;
}

/* ---------------------------------------------------------- entry extract -- */

int xb_zip_read_entry(const char *path, const char *name,
                      char **out_data, size_t *out_len, size_t max_bytes)
{
    *out_data = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    long scan = size - 22;
    long lowest = size - 22 - 65535;
    if (lowest < 0) lowest = 0;
    uint8_t buf[22];
    long eocd_at = -1;
    while (scan >= lowest) {
        if (fseek(f, scan, SEEK_SET) == 0 && fread(buf, 1, sizeof buf, f) == sizeof buf
            && rd32(buf) == ZEOCD_SIG) { eocd_at = scan; break; }
        scan--;
    }
    if (eocd_at < 0) { fclose(f); return -1; }

    uint16_t total = rd16(buf + 10);
    uint32_t cd_off = rd32(buf + 16);
    if (fseek(f, (long)cd_off, SEEK_SET) != 0) { fclose(f); return -1; }

    for (uint16_t i = 0; i < total; i++) {
        uint8_t h[46];
        if (fread(h, 1, sizeof h, f) != sizeof h) break;
        if (rd32(h) != ZCEN_SIG) break;
        uint16_t name_len = rd16(h + 28);
        uint16_t extra_len = rd16(h + 30);
        uint16_t comment_len = rd16(h + 32);
        uint16_t method = rd16(h + 10);
        uint32_t comp = rd32(h + 20);
        uint32_t uncomp = rd32(h + 24);
        uint32_t local_off = rd32(h + 42);
        if (name_len > 4096) break;
        char nbuf[4097];
        if (fread(nbuf, 1, name_len, f) != name_len) break;
        nbuf[name_len] = '\0';
        if (fseek(f, extra_len + comment_len, SEEK_CUR) != 0) break;

        if (strcmp(nbuf, name) != 0) continue;
        if (uncomp > max_bytes) { fclose(f); return -2; }
        if (method != 0) {
            /* Deflate: implement a tiny inflate. Entries we need (manifests,
             * small JS/JSON) are almost always stored or deflated; raw is
             * handled here, deflate by the fallback below. */
            fclose(f);
            return -3;   /* caller falls back to "not readable" */
        }
        /* Stored entry: local header then raw bytes. */
        if (fseek(f, (long)local_off, SEEK_SET) != 0) { fclose(f); return -1; }
        uint8_t lh[30];
        if (fread(lh, 1, sizeof lh, f) != sizeof lh || rd32(lh) != ZLOC_SIG) { fclose(f); return -1; }
        uint16_t lname = rd16(lh + 26);
        uint16_t lextra = rd16(lh + 28);
        if (fseek(f, (long)lname + lextra, SEEK_CUR) != 0) { fclose(f); return -1; }
        char *data = (char *)xb_alloc(uncomp + 1);
        if (fread(data, 1, uncomp, f) != uncomp) { xb_free(data); fclose(f); return -1; }
        data[uncomp] = '\0';
        *out_data = data;
        *out_len = uncomp;
        (void)comp;
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

/* -------------------------------------------------------------- detection -- */

static void evidence_add(xb_detect_result *d, const char *fmt, ...)
{
    size_t used = strlen(d->evidence);
    if (used >= sizeof d->evidence - 2) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->evidence + used, sizeof d->evidence - used, fmt, ap);
    va_end(ap);
}

static const char *basename_of(const char *path)
{
    const char *b = strrchr(path, '/');
#ifdef _WIN32
    const char *b2 = strrchr(path, '\\');
    if (b2 && (!b || b2 > b)) b = b2;
#endif
    return b ? b + 1 : path;
}

static void ext_lower(const char *path, char *out, size_t cap)
{
    const char *dot = strrchr(basename_of(path), '.');
    if (!dot) { out[0] = '\0'; return; }
    size_t n = strlen(dot);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)dot[i]);
    out[n] = '\0';
}

static void score(xb_detect_result *d, const char *id, const char *manager,
                  int confidence, const char *why)
{
    if (confidence <= d->confidence) return;
    d->confidence = confidence;
    xb_str_lcpy(d->format_id, id, sizeof d->format_id);
    xb_str_lcpy(d->manager_id, manager, sizeof d->manager_id);
    if (why) evidence_add(d, "%s%s", d->evidence[0] ? "; " : "", why);
}

int xb_detect_dir(const char *path, xb_detect_result *out)
{
    memset(out, 0, sizeof *out);
    out->kind = XB_ART_DIR;

    bool has_manifest = false, has_plugin_js = false, has_index_js = false;
    bool has_source_js = false, has_json_rule = false;
    char first_js[192] = "", first_json[192] = "";

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *n = fd.cFileName;
        if (xb_strieq(n, "manifest.json")) has_manifest = true;
        else if (xb_strieq(n, "plugin.js")) { has_plugin_js = true; xb_str_lcpy(first_js, n, sizeof first_js); }
        else if (xb_strieq(n, "index.js")) { has_index_js = true; xb_str_lcpy(first_js, n, sizeof first_js); }
        else if (xb_str_has_suffix_ci(n, ".js")) { has_source_js = true; if (!first_js[0]) xb_str_lcpy(first_js, n, sizeof first_js); }
        else if (xb_str_has_suffix_ci(n, ".json")) { has_json_rule = true; if (!first_json[0]) xb_str_lcpy(first_json, n, sizeof first_json); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (xb_strieq(de->d_name, "manifest.json")) has_manifest = true;
        else if (xb_strieq(de->d_name, "plugin.js")) { has_plugin_js = true; xb_str_lcpy(first_js, de->d_name, sizeof first_js); }
        else if (xb_strieq(de->d_name, "index.js")) { has_index_js = true; xb_str_lcpy(first_js, de->d_name, sizeof first_js); }
        else if (xb_str_has_suffix_ci(de->d_name, ".js")) { has_source_js = true; if (!first_js[0]) xb_str_lcpy(first_js, de->d_name, sizeof first_js); }
        else if (xb_str_has_suffix_ci(de->d_name, ".json")) { has_json_rule = true; if (!first_json[0]) xb_str_lcpy(first_json, de->d_name, sizeof first_json); }
    }
    closedir(dir);
#endif

    if (has_manifest && (has_plugin_js || has_index_js || has_source_js)) {
        score(out, "lnreader", "lnreader", 90, "manifest.json + JS entry point");
        xb_str_lcpy(out->entry_point, has_plugin_js ? "plugin.js" : first_js, sizeof out->entry_point);
    } else if (has_source_js) {
        score(out, "mangayomi", "mangayomi", 55, "loose .js sources");
        xb_str_lcpy(out->entry_point, first_js, sizeof out->entry_point);
    } else if (has_json_rule) {
        score(out, "legado", "legado", 50, "loose .json rule files");
        xb_str_lcpy(out->entry_point, first_json, sizeof out->entry_point);
    }
    return 0;
}

int xb_detect_path(const char *path, xb_detect_result *out)
{
    memset(out, 0, sizeof *out);
    xb_str_lcpy(out->entry_point, basename_of(path), sizeof out->entry_point);

    XB_STAT_STRUCT st;
    if (XB_STAT(path, &st) != 0) return -1;
#ifdef _WIN32
    bool is_dir = (st.st_mode & _S_IFDIR) != 0;
#else
    bool is_dir = S_ISDIR(st.st_mode);
#endif
    if (is_dir) return xb_detect_dir(path, out);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t magic[16];
    size_t got = fread(magic, 1, sizeof magic, f);
    fclose(f);
    xb_artifact_kind magic_kind = xb_artifact_sniff_magic(magic, got);

    char ext[32];
    ext_lower(path, ext, sizeof ext);
    bool ext_zip = xb_streq(ext, ".apk") || xb_streq(ext, ".jar") || xb_streq(ext, ".zip");
    bool ext_js  = xb_streq(ext, ".js") || xb_streq(ext, ".mjs") || xb_streq(ext, ".cjs");
    bool ext_json = xb_streq(ext, ".json") || xb_streq(ext, ".txt");

    if (magic_kind == XB_ART_ZIP || ext_zip) {
        xb_zip_listing zl;
        if (xb_zip_list(path, &zl) != 0) {
            out->kind = XB_ART_ZIP;
            snprintf(out->evidence, sizeof out->evidence, "zip container (%s)", zl.error);
            return 0;
        }
        /* Presence of AndroidManifest.xml means APK even if the extension lies. */
        bool is_apk = xb_zip_listing_has(&zl, "AndroidManifest.xml");
        bool is_jar = xb_zip_listing_has(&zl, "META-INF/MANIFEST.MF") ||
                      xb_zip_listing_has(&zl, "META-INF/services/*");
        out->kind = is_apk ? XB_ART_APK : (is_jar ? XB_ART_JAR : XB_ART_ZIP);
        evidence_add(out, "%s with %zu entries", xb_artifact_kind_name(out->kind), zl.entry_count);

        if (xb_zip_listing_has(&zl, "com/lagradost/cloudstream3/MainAPI.class") ||
            xb_zip_listing_has(&zl, "com/lagradost/cloudstream3/*")) {
            score(out, "cloudstream", "cloudstream", 95,
                  "found com/lagradost/cloudstream3 in the archive");
        }
        if (xb_zip_listing_has(&zl, "org/koitharu/kotatsu/parsers/*")) {
            score(out, "kotatsu", "kotatsu", 95, "found org/koitharu/kotatsu/parsers");
        }
        if (xb_zip_listing_has(&zl, "eu/kanade/tachiyomi/*") ||
            xb_zip_listing_has(&zl, "eu/kanade/tachiyomi/animesource/*") ||
            xb_zip_listing_has(&zl, "eu/kanade/tachiyomi/mangasource/*")) {
            score(out, "aniyomi", "aniyomi", 95, "found eu/kanade/tachiyomi in the archive");
        }
        if (xb_zip_listing_has(&zl, "ireader/*")) {
            score(out, "ireader", "ireader", 70, "found ireader/ package");
        }
        if (xb_zip_listing_has(&zl, "com/tsundoku/*")) {
            score(out, "tsundoku", "tsundoku", 70, "found com/tsundoku/ package");
        }
        /* An APK with an Aniyomi class but no CloudStream marker wins above. */
        const char *cl = xb_zip_listing_find(&zl, "classes*.dex");
        if (cl) xb_str_lcpy(out->entry_point, cl, sizeof out->entry_point);
        xb_zip_listing_free(&zl);
        if (out->confidence == 0) {
            score(out, "", "", 0, NULL);
            snprintf(out->evidence + strlen(out->evidence),
                     sizeof out->evidence - strlen(out->evidence),
                     "%sarchive matched no known ecosystem", out->evidence[0] ? "; " : "");
        }
        return 0;
    }

    if (magic_kind == XB_ART_TAR_GZ) {
        out->kind = XB_ART_TAR_GZ;
        snprintf(out->evidence, sizeof out->evidence, "gzip stream");
        return 0;
    }
    if (magic_kind == XB_ART_BINARY) {
        out->kind = XB_ART_BINARY;
        if (xb_str_has_prefix(basename_of(path), "TorrServer") || xb_streq(ext, ".bin"))
            score(out, "torrserver", "torrserver", 60, "native addon binary");
        else
            snprintf(out->evidence, sizeof out->evidence, "native binary");
        return 0;
    }

    if (ext_json || magic_kind == XB_ART_JSON) {
        /* Peek at the text to tell Legado rules from an LnReader manifest. */
        FILE *g = fopen(path, "rb");
        if (g) {
            char head[4096];
            size_t n = fread(head, 1, sizeof head - 1, g);
            head[n] = '\0';
            fclose(g);
            out->kind = XB_ART_JSON;
            if (strstr(head, "bookSourceUrl") || strstr(head, "ruleSearch") ||
                strstr(head, "ruleBookInfo")) {
                score(out, "legado", "legado", 85, "Legado bookSource rule keys present");
            } else if (strstr(head, "\"novel\"") && strstr(head, "\"id\"")) {
                score(out, "lnreader", "lnreader", 45, "manifest-like JSON");
            } else {
                snprintf(out->evidence, sizeof out->evidence, "json document");
            }
        }
        return 0;
    }

    if (ext_js || magic_kind == XB_ART_JS) {
        out->kind = XB_ART_JS;
        FILE *g = fopen(path, "rb");
        if (g) {
            char head[8192];
            size_t n = fread(head, 1, sizeof head - 1, g);
            head[n] = '\0';
            fclose(g);
            if (strstr(head, "class extends MProvider") || strstr(head, "MProvider") ||
                strstr(head, "registerProvider") || strstr(head, "Mangayomi")) {
                score(out, "mangayomi", "mangayomi", 80, "MProvider present");
            } else if (strstr(head, "SoraExtension") || strstr(head, "sora")) {
                score(out, "sora", "sora", 60, "Sora marker present");
            } else if (strstr(head, "cheerio") || strstr(head, "novel")) {
                score(out, "lnreader", "lnreader", 55, "LnReader-style globals");
            } else {
                snprintf(out->evidence, sizeof out->evidence, "javascript module");
            }
        }
        return 0;
    }

    out->kind = XB_ART_UNKNOWN;
    snprintf(out->evidence, sizeof out->evidence, "unrecognised artifact");
    return 0;
}

char *xb_detect_json(const xb_detect_result *d)
{
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_obj_begin(&w);
    xb_jw_kv_str(&w, "format", d->format_id);
    xb_jw_kv_str(&w, "manager_id", d->manager_id);
    xb_jw_kv_str(&w, "artifact_kind", xb_artifact_kind_name(d->kind));
    xb_jw_kv_int(&w, "confidence", d->confidence);
    xb_jw_kv_str(&w, "evidence", d->evidence);
    xb_jw_kv_str(&w, "entry_point", d->entry_point);
    xb_jw_obj_end(&w);
    return xb_buf_steal(&w.buf);
}
