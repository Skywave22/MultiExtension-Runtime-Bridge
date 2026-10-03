/* bridge/proc.h — child process supervision.
 *
 * The bridge runs each engine in a supervised worker process instead of thread
 * multiplexing a single JVM/script runtime. Consequences we rely on:
 *   - a wedged or crashed extension cannot take down the daemon or the host app;
 *   - a worker can be killed on deadline, and restarted with backoff;
 *   - workers are optional: if the runtime for a format is not installed, the
 *     daemon reports the engine as `unavailable` instead of failing.
 */
#ifndef BRIDGE_PROC_H
#define BRIDGE_PROC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct xb_process xb_process;

typedef struct {
    int   stdin_fd;
    int   stdout_fd;
    int   stderr_fd;
    bool  inherit_env;
    /* Capture stderr into the log ring instead of letting it hit the terminal. */
    bool  capture_stderr;
    const char *cwd;
} xb_proc_opts;

/* Spawn argv[0] with argv (argv must be NULL-terminated). Returns NULL on error. */
xb_process *xb_proc_spawn(const char *const *argv, const xb_proc_opts *opts,
                          char *err, size_t errcap);

int64_t xb_proc_pid(const xb_process *p);
bool    xb_proc_running(xb_process *p);
/* Wait up to timeout_ms for exit. Returns exit code, or -1 on timeout. */
int     xb_proc_wait(xb_process *p, int timeout_ms);
void    xb_proc_kill(xb_process *p);          /* SIGKILL / TerminateProcess */
void    xb_proc_terminate(xb_process *p);     /* SIGTERM, graceful */
void    xb_proc_close(xb_process *p);         /* free handles (does not kill) */
int     xb_proc_stdin_fd(xb_process *p);
int     xb_proc_stdout_fd(xb_process *p);

/* Locate an executable on PATH (or an absolute path). Caller frees.
 * On Windows also tries .exe/.cmd/.bat. */
char *xb_which(const char *name);

/* Return the argv of the current process as a JSON array string. Caller frees. */
char *xb_proc_argv_json(void);

/* Environment helpers. */
char *xb_env_get(const char *name);           /* malloc'd or NULL */
int   xb_setenv(const char *k, const char *v, int overwrite);

/* True when the daemon should treat missing optional runtimes as fatal. */
bool xb_proc_runtime_required(const char *env_name);

#endif /* BRIDGE_PROC_H */
