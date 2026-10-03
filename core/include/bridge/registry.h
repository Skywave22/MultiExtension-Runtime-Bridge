/* bridge/registry.h — declarative extension-format registry.
 *
 * The reference implementation encodes "what a Kotatsu extension is" in Dart
 * code, once per platform. Here a format is *data*: an entry describing which
 * files an artifact contains, how to recognise it, which manager id it maps to,
 * which methods it can serve, which engine runs it, and what a valid artifact
 * even looks like (content hashes, signatures, size caps).
 *
 * The built-in entries live in core/src/registry.c and are mirrored in
 * the core/formats directory so non-C hosts and tests can read the same truth.
 */
#ifndef BRIDGE_REGISTRY_H
#define BRIDGE_REGISTRY_H

#include <stddef.h>
#include <stdbool.h>

/* Capability bits (bit positions, stable across versions). */
#define XB_CAP_ANIME        (1u << 0)
#define XB_CAP_MANGA        (1u << 1)
#define XB_CAP_NOVEL        (1u << 2)
#define XB_CAP_VIDEO        (1u << 3)
#define XB_CAP_TORRENT      (1u << 4)
#define XB_CAP_SEARCH       (1u << 5)
#define XB_CAP_POPULAR      (1u << 6)
#define XB_CAP_LATEST       (1u << 7)
#define XB_CAP_DETAIL       (1u << 8)
#define XB_CAP_PAGES        (1u << 9)
#define XB_CAP_PREFS        (1u << 10)
#define XB_CAP_FILTERS      (1u << 11)
#define XB_CAP_IMAGE_PROXY  (1u << 12)

typedef enum {
    XB_PLAT_ANDROID = 1u << 0,
    XB_PLAT_IOS     = 1u << 1,
    XB_PLAT_WINDOWS = 1u << 2,
    XB_PLAT_MACOS   = 1u << 3,
    XB_PLAT_LINUX   = 1u << 4
} xb_platform_bit;

#define XB_PLAT_ALL (XB_PLAT_ANDROID|XB_PLAT_IOS|XB_PLAT_WINDOWS|XB_PLAT_MACOS|XB_PLAT_LINUX)

typedef struct {
    const char *id;              /* "aniyomi"                       */
    const char *label;           /* "Aniyomi"                       */
    const char *manager_id;      /* id used by the unified API      */
    const char *artifact_kinds;  /* ".apk,.jar"                     */
    const char *identify_globs;  /* "classes.dex;AndroidManifest.xml" (inside artifact) */
    const char *engine;          /* "jvm" | "js" | "rule" | "native" */
    const char *ext_class_hint;  /* e.g. "eu.kanade.tachiyomi.animesource.AnimeSource" */
    unsigned    caps;
    unsigned    platforms;
    const char *notes;
} xb_format;

/* Built-in table. */
size_t            xb_format_count(void);
const xb_format  *xb_format_at(size_t i);
const xb_format  *xb_format_by_id(const char *id);
const xb_format  *xb_format_by_manager(const char *manager_id);

/* Capability bit names, for JSON output. */
const char *xb_cap_name(unsigned bit_index);

/* Serialise the whole registry as a JSON array. Caller frees. */
char *xb_registry_json(void);

/* Serialise one format. Caller frees. */
char *xb_format_json(const xb_format *f);

#endif /* BRIDGE_REGISTRY_H */
