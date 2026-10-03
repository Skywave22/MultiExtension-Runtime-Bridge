/* bridge/engine.h — supervised worker engine.
 *
 * An engine owns N worker processes speaking XBP over stdio. It keeps a
 * round-robin assignment, a per-worker pending table, and health state. Callers
 * submit a job and either wait for the response or receive streamed chunks.
 */
#ifndef BRIDGE_ENGINE_H
#define BRIDGE_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "json.h"

typedef enum {
    XB_ENGINE_STOPPED = 0,
    XB_ENGINE_STARTING,
    XB_ENGINE_READY,
    XB_ENGINE_DEGRADED,     /* some workers down */
    XB_ENGINE_UNAVAILABLE   /* runtime missing / never started */
} xb_engine_state;

typedef struct xb_engine xb_engine;

/* Chunk callback for streaming jobs. Return false to ask the engine to stop. */
typedef bool (*xb_chunk_fn)(void *ud, int64_t seq, const xb_json *chunk);

typedef struct {
    const char  *method;       /* fully-qualified, e.g. "source.getVideoList" */
    const xb_json *params;
    int64_t      deadline_ms;  /* 0 = engine default */
    bool         stream;
    xb_chunk_fn  on_chunk;
    void        *on_chunk_ud;
    char         source_id[128];  /* optional routing hint (JS executor etc.) */
} xb_job;

typedef struct {
    bool   ok;
    char  *result;      /* malloc'd JSON text                                        */
    char  *error_msg;   /* malloc'd, when !ok                                        */
    int    error_code;
    int    engine_ms;   /* time inside the engine                                    */
} xb_job_result;

/* Create an engine. `argv` is the worker command with a literal "{}" placeholder
 * that is replaced by the worker index (so each worker gets its own pipe names).
 * Returns NULL when the engine cannot even be described (bad argv). */
xb_engine *xb_engine_new(const char *name, const char *const *argv,
                         int worker_count, int64_t start_timeout_ms,
                         int64_t restart_backoff_ms);

/* In-process engine: the handler runs on the daemon's task pool under the same
 * lifecycle contract as a worker process (deadline, cancellation, streaming).
 * This is what makes the bridge usable with no external runtime installed, and
 * it is how the built-in rule engine is hosted. */
typedef int (*xb_engine_inproc_fn)(void *ud, const char *method,
                                   const xb_json *params,
                                   xb_job_result *res,
                                   xb_chunk_fn on_chunk, void *chunk_ud);

xb_engine *xb_engine_new_inproc(const char *name, xb_engine_inproc_fn fn, void *ud);

/* Start workers. When `required` is false and the binary is missing, the engine
 * goes to XB_ENGINE_UNAVAILABLE and start still returns 0. */
int  xb_engine_start(xb_engine *e, bool required, char *err, size_t errcap);
void xb_engine_stop(xb_engine *e);
void xb_engine_free(xb_engine *e);

xb_engine_state xb_engine_state_of(const xb_engine *e);
const char     *xb_engine_name(const xb_engine *e);
const char     *xb_engine_state_name(xb_engine_state s);
/* Number of live workers / configured workers. */
void            xb_engine_worker_counts(const xb_engine *e, int *live, int *total);

/* Submit a job. Blocking: returns 0 on completion (result filled), -1 on error.
 * Thread-safe: many threads may submit concurrently. */
int xb_engine_submit(xb_engine *e, const xb_job *job, xb_job_result *res);

/* Ask the engine to abort the job identified by `job_id` (best effort). */
void xb_engine_cancel(xb_engine *e, const char *job_id);

/* Restart policy toggle; used when a worker dies repeatedly. */
void xb_engine_set_max_restarts(xb_engine *e, int n);

/* Engine-level counters for bridge.metrics. */
typedef struct {
    uint64_t jobs_submitted;
    uint64_t jobs_ok;
    uint64_t jobs_err;
    uint64_t jobs_timeout;
    uint64_t chunks_forwarded;
    uint64_t worker_restarts;
    int      live_workers;
    int      total_workers;
} xb_engine_stats;
void xb_engine_stats_of(const xb_engine *e, xb_engine_stats *out);

#endif /* BRIDGE_ENGINE_H */
