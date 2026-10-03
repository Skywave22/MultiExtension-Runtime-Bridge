/* gen_formats.c — writes core/formats/<id>.json from the compiled registry.
 *
 * The C table is the single source of truth; these files are a mirror for hosts
 * that are not C (the Node and Python SDKs, the Android AAR, docs). Keeping the
 * generator in-tree is what makes drift impossible: `make formats` regenerates
 * them and the unit suite fails if a mirror disagrees with the table.
 *
 * Usage: gen_formats <output-dir>
 */
#include "bridge/registry.h"
#include "bridge/util.h"

#include <stdio.h>
#include <string.h>

static int write_one(const xb_format *f, const char *dir)
{
    char *body = xb_format_json(f);
    if (!body) return -1;

    char path[700];
    snprintf(path, sizeof path, "%s/%s.json", dir, f->id);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "gen_formats: cannot write %s\n", path);
        xb_free(body);
        return -1;
    }
    fprintf(fp, "%s\n", body);
    fclose(fp);
    xb_free(body);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "core/formats";
    int failures = 0;

    for (size_t i = 0; i < xb_format_count(); i++) {
        const xb_format *f = xb_format_at(i);
        if (write_one(f, dir) != 0) failures++;
    }

    fprintf(stderr, "gen_formats: wrote %zu format descriptors to %s\n",
            xb_format_count(), dir);
    return failures ? 1 : 0;
}
