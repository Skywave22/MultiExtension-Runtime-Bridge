/* bridge/rule_engine.h — built-in rule-based source engine.
 *
 * Executes declarative extraction rules (the shape used by Legado book sources
 * and adopted by other rule-driven ecosystems). Running it in-process means a
 * rule-based extension works on every supported OS with no JavaScript runtime,
 * no JVM and no native addon installed.
 */
#ifndef BRIDGE_RULE_ENGINE_H
#define BRIDGE_RULE_ENGINE_H

#include "engine.h"
#include "json.h"

/* Register the engine in-process handler with xb_engine_new_inproc(). */
int  xb_rule_engine_inproc(void *ud, const char *method, const xb_json *params,
                           xb_job_result *res, xb_chunk_fn on_chunk, void *chunk_ud);

/* Parse a rule document (single source object, an array of sources, or a
 * wrapper with a "sources"/"bookSources" array). Returns the number of rules
 * found, or -1 with a message in `err`. The parsed rules are appended to the
 * engine's internal rule table, keyed by `source_id`. */
int  xb_rule_engine_load(const char *source_id, const char *json_text,
                         size_t len, char *err, size_t errcap);

/* Load from a file on disk. */
int  xb_rule_engine_load_file(const char *source_id, const char *path,
                              char *err, size_t errcap);

void xb_rule_engine_unload(const char *source_id);
size_t xb_rule_engine_count(void);
void xb_rule_engine_reset(void);

/* Describe a loaded source (used by extension.info). Caller frees. */
char *xb_rule_engine_describe(const char *source_id);

#endif /* BRIDGE_RULE_ENGINE_H */
