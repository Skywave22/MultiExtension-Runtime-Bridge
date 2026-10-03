/* bridge/archive.h — artifact inspection: what *is* this file, really?
 *
 * Format detection must not trust the extension. A `.jar` that is really a zip
 * of Kotatsu parsers, an `.apk` with a CloudStream plugin inside, or a bare
 * `plugin.js` all have to be classified the same way on every platform, so all
 * classification lives here and is unit-tested against real fixtures.
 */
#ifndef BRIDGE_ARCHIVE_H
#define BRIDGE_ARCHIVE_H

#include <stddef.h>
#include <stdbool.h>
#include "json.h"

/* Cap on the number of central-directory entries we will walk; a malicious zip
 * with millions of entries must not become a memory-exhaustion vector. */
#define XB_ARCHIVE_MAX_ENTRIES 4096
/* Cap on total uncompressed size we are willing to iterate. */
#define XB_ARCHIVE_MAX_TOTAL   (256ull * 1024 * 1024)

typedef enum {
    XB_ART_UNKNOWN = 0,
    XB_ART_ZIP,          /* any zip container */
    XB_ART_APK,          /* zip with AndroidManifest.xml */
    XB_ART_JAR,          /* zip with META-INF/MANIFEST.MF, META-INF/services */
    XB_ART_JS,           /* single JavaScript file */
    XB_ART_JSON,         /* single JSON rule file (Legado) */
    XB_ART_DIR,          /* a directory of extension files */
    XB_ART_TAR_GZ,
    XB_ART_BINARY        /* native addon (TorrServer) */
} xb_artifact_kind;

typedef struct {
    char *name;
    size_t size;
    size_t compressed;
} xb_zip_entry;

typedef struct {
    xb_artifact_kind kind;
    xb_zip_entry *entries;
    size_t        entry_count;
    bool          truncated;     /* hit XB_ARCHIVE_MAX_ENTRIES */
    char          error[256];
} xb_zip_listing;

const char *xb_artifact_kind_name(xb_artifact_kind k);

/* First 8 bytes tell us most of what we need without reading the whole file. */
xb_artifact_kind xb_artifact_sniff_magic(const void *data, size_t len);

/* Read the zip central directory (streams with fseek; no full-file buffering). */
int  xb_zip_list(const char *path, xb_zip_listing *out);
void xb_zip_listing_free(xb_zip_listing *l);

/* Extract one entry to memory. Caller frees *out_data. */
int  xb_zip_read_entry(const char *path, const char *name,
                       char **out_data, size_t *out_len, size_t max_bytes);

/* Does the listing contain any path matching `glob` (supporting '*' and '?')? */
bool xb_zip_listing_has(const xb_zip_listing *l, const char *glob);
/* Return the first matching entry name or NULL. */
const char *xb_zip_listing_find(const xb_zip_listing *l, const char *glob);

/* -------------------------------------------------------------- detection -- */

typedef struct {
    char           format_id[64];    /* e.g. "aniyomi"; "" when unknown */
    char           manager_id[64];
    xb_artifact_kind kind;
    int            confidence;       /* 0..100 */
    char           evidence[512];    /* human-readable why */
    char           entry_point[192]; /* main file, when meaningful */
} xb_detect_result;

/* Classify a file or directory. Cheap: reads only headers and the zip index. */
int xb_detect_path(const char *path, xb_detect_result *out);

/* Same, but for a directory of loose files (JS/JSON extensions). */
int xb_detect_dir(const char *path, xb_detect_result *out);

char *xb_detect_json(const xb_detect_result *d);

#endif /* BRIDGE_ARCHIVE_H */
