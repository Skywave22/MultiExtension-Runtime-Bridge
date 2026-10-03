/* proc.c — child process spawn/supervise, PATH lookup, env helpers. */
#include "bridge/proc.h"
#include "bridge/util.h"
#include "bridge/log.h"
#include "bridge/json.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <errno.h>
extern char **environ;
#endif

struct xb_process {
#ifdef _WIN32
    PROCESS_INFORMATION pi;
    HANDLE in_w, out_r, err_r;
    HANDLE job;
#else
    pid_t pid;
    int   in_w, out_r, err_r;
#endif
    bool  running;
    int   exit_code;
    int   stderr_pump_started;
    xb_thread_t stderr_thread;
    bool  capture_stderr;
};

/* ------------------------------------------------------------- PATH lookup */

static bool file_is_executable(const char *path)
{
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return true;
#else
    return access(path, X_OK) == 0;
#endif
}

char *xb_which(const char *name)
{
    if (!name || !name[0]) return NULL;
    if (strchr(name, XB_PATHSEP) || strchr(name, '/') || strchr(name, '\\')) {
        if (file_is_executable(name)) return xb_strdup(name);
        /* On Windows, try the usual extensions. */
#ifdef _WIN32
        static const char *EXT[] = { ".exe", ".cmd", ".bat", NULL };
        for (int i = 0; EXT[i]; i++) {
            char buf[1024];
            snprintf(buf, sizeof buf, "%s%s", name, EXT[i]);
            if (file_is_executable(buf)) return xb_strdup(buf);
        }
#endif
        return NULL;
    }
    const char *path = getenv("PATH");
    if (!path) return NULL;
    char *copy = xb_strdup(path);
    char *fields[512];
    size_t n = xb_str_split(copy, XB_PATHSEP == '/' ? ':' : ';', fields, 512);
    char *result = NULL;
    for (size_t i = 0; i < n && !result; i++) {
        char candidate[1200];
#ifdef _WIN32
        snprintf(candidate, sizeof candidate, "%s\\%s.exe", fields[i], name);
        if (file_is_executable(candidate)) { result = xb_strdup(candidate); break; }
        snprintf(candidate, sizeof candidate, "%s\\%s.cmd", fields[i], name);
        if (file_is_executable(candidate)) { result = xb_strdup(candidate); break; }
        snprintf(candidate, sizeof candidate, "%s\\%s", fields[i], name);
#else
        snprintf(candidate, sizeof candidate, "%s/%s", fields[i], name);
#endif
        if (file_is_executable(candidate)) result = xb_strdup(candidate);
    }
    xb_free(copy);
    return result;
}

/* ------------------------------------------------------------------ spawn */

#ifndef _WIN32
static XB_THREAD_FN(stderr_pump)
{
    xb_process *p = (xb_process *)ud;
    char line[512];
    size_t used = 0;
    char c;
    /* Read byte-at-a-time so partial lines do not get lost, and forward each
     * finished line into the log ring so `bridge.log` shows worker output. */
    for (;;) {
        ssize_t r = read(p->err_r, &c, 1);
        if (r <= 0) break;
        if (c == '\n') {
            line[used] = '\0';
            if (used) XB_DEBUG("[worker] %s", line);
            used = 0;
        } else if (used < sizeof line - 1) {
            line[used++] = c;
        } else {
            line[used] = '\0';
            XB_DEBUG("[worker] %s", line);
            used = 0;
        }
    }
    if (used) { line[used] = '\0'; XB_DEBUG("[worker] %s", line); }
    return NULL;
}
#else
static XB_THREAD_FN(stderr_pump)
{
    xb_process *p = (xb_process *)ud;
    char line[512];
    size_t used = 0;
    char c;
    DWORD r;
    for (;;) {
        if (!ReadFile(p->err_r, &c, 1, &r, NULL) || r == 0) break;
        if (c == '\n') {
            line[used] = '\0';
            if (used) XB_DEBUG("[worker] %s", line);
            used = 0;
        } else if (used < sizeof line - 1) {
            line[used++] = c;
        }
        if (used >= sizeof line - 1) { line[used] = '\0'; XB_DEBUG("[worker] %s", line); used = 0; }
    }
    if (used) { line[used] = '\0'; XB_DEBUG("[worker] %s", line); }
    return 0;
}
#endif

/* Windows uses a single command-line string, so arguments must be quoted and
 * escaped by hand. POSIX passes argv directly and never calls this. */
#ifdef _WIN32
static void append_quoted(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(dst);
    if (n + 2 >= cap) return;
    dst[n++] = '"';
    for (const char *p = s; *p && n + 2 < cap; p++) {
        if (*p == '"') dst[n++] = '\\';
        dst[n++] = *p;
    }
    dst[n++] = '"';
    dst[n] = '\0';
}
#endif

xb_process *xb_proc_spawn(const char *const *argv, const xb_proc_opts *opts,
                          char *err, size_t errcap)
{
    if (!argv || !argv[0]) { snprintf(err, errcap, "empty argv"); return NULL; }

    xb_process *p = (xb_process *)xb_alloc(sizeof *p);
#ifndef _WIN32
    p->pid = -1; p->in_w = -1; p->out_r = -1; p->err_r = -1;
#endif

    int stdin_pipe[2] = { -1, -1 };
    int stdout_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    bool want_stderr = opts && opts->capture_stderr;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE child_in_r = NULL, child_out_w = NULL, child_err_w = NULL;
    HANDLE parent_in_w = NULL, parent_out_r = NULL, parent_err_r = NULL;

    if (!CreatePipe(&child_in_r, &parent_in_w, &sa, 1 << 16)) { snprintf(err, errcap, "CreatePipe(in)"); xb_free(p); return NULL; }
    SetHandleInformation(parent_in_w, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&parent_out_r, &child_out_w, &sa, 1 << 16)) { snprintf(err, errcap, "CreatePipe(out)"); xb_free(p); return NULL; }
    SetHandleInformation(parent_out_r, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&parent_err_r, &child_err_w, &sa, 1 << 16)) { snprintf(err, errcap, "CreatePipe(err)"); xb_free(p); return NULL; }
    SetHandleInformation(parent_err_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_in_r;
    si.hStdOutput = child_out_w;
    si.hStdError = child_err_w;

    char cmdline[32768];
    cmdline[0] = '\0';
    for (int i = 0; argv[i]; i++) {
        if (i) append_quoted(cmdline, sizeof cmdline, " ");
        append_quoted(cmdline, sizeof cmdline, argv[i]);
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL,
                             opts && opts->cwd ? opts->cwd : NULL, &si, &pi);
    if (!ok) {
        snprintf(err, errcap, "CreateProcess failed (%lu): %s", GetLastError(), cmdline);
        xb_free(p);
        return NULL;
    }
    CloseHandle(child_in_r);
    CloseHandle(child_out_w);
    CloseHandle(child_err_w);

    /* Job object so a crashed daemon takes its workers with it. */
    p->job = CreateJobObjectA(NULL, NULL);
    if (p->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(p->job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    p->pi = pi;
    p->in_w = parent_in_w;
    p->out_r = parent_out_r;
    p->err_r = parent_err_r;
    p->capture_stderr = want_stderr;
    p->running = true;
    if (want_stderr)
        xb_thread_spawn(&p->stderr_thread, stderr_pump, p);
    return p;
#else
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        snprintf(err, errcap, "pipe(): %s", strerror(errno));
        xb_free(p);
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errcap, "fork(): %s", strerror(errno));
        xb_free(p);
        return NULL;
    }
    if (pid == 0) {
        /* child */
        dup2(stdin_pipe[0], 0);
        dup2(stdout_pipe[1], 1);
        dup2(stderr_pipe[1], 2);
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        if (opts && opts->cwd && chdir(opts->cwd) != 0) _exit(126);
        /* New process group: kill(-pid) reaches any grandchildren too. */
        setsid();
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "exec failed: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    p->pid = pid;
    p->in_w = stdin_pipe[1];
    p->out_r = stdout_pipe[0];
    p->err_r = stderr_pipe[0];
    p->capture_stderr = want_stderr;
    p->running = true;
    if (want_stderr)
        xb_thread_spawn(&p->stderr_thread, stderr_pump, p);
    return p;
#endif
}

int64_t xb_proc_pid(const xb_process *p)
{
#ifdef _WIN32
    return (int64_t)(uintptr_t)p->pi.dwProcessId;
#else
    return (int64_t)p->pid;
#endif
}

bool xb_proc_running(xb_process *p)
{
    if (!p || !p->running) return false;
#ifdef _WIN32
    DWORD code = 0;
    if (!GetExitCodeProcess(p->pi.hProcess, &code)) return false;
    if (code != STILL_ACTIVE) { p->running = false; p->exit_code = (int)code; return false; }
    return true;
#else
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == 0) return true;
    p->running = false;
    if (r == p->pid) {
        if (WIFEXITED(status)) p->exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) p->exit_code = 128 + WTERMSIG(status);
    }
    return false;
#endif
}

int xb_proc_wait(xb_process *p, int timeout_ms)
{
    if (!p) return -1;
    int64_t deadline = xb_mono_ms() + timeout_ms;
    for (;;) {
        if (!xb_proc_running(p)) return p->exit_code;
        if (xb_mono_ms() >= deadline) return -1;
        xb_sleep_ms(5);
    }
}

void xb_proc_kill(xb_process *p)
{
    if (!p) return;
#ifdef _WIN32
    TerminateProcess(p->pi.hProcess, 9);
    p->running = false;
#else
    if (p->pid > 0) {
        kill(-p->pid, SIGKILL);
        kill(p->pid, SIGKILL);
    }
    p->running = false;
#endif
}

void xb_proc_terminate(xb_process *p)
{
    if (!p) return;
#ifdef _WIN32
    xb_proc_kill(p);
#else
    if (p->pid > 0) kill(p->pid, SIGTERM);
#endif
}

int xb_proc_stdin_fd(xb_process *p)  { return p ? p->in_w : -1; }
int xb_proc_stdout_fd(xb_process *p) { return p ? p->out_r : -1; }

void xb_proc_close(xb_process *p)
{
    if (!p) return;
#ifdef _WIN32
    if (p->in_w) CloseHandle(p->in_w);
    if (p->out_r) CloseHandle(p->out_r);
    if (p->err_r) CloseHandle(p->err_r);
    if (p->pi.hProcess) CloseHandle(p->pi.hProcess);
    if (p->job) CloseHandle(p->job);
#else
    if (p->in_w >= 0) close(p->in_w);
    if (p->out_r >= 0) close(p->out_r);
    if (p->err_r >= 0) close(p->err_r);
#endif
    xb_free(p);
}

/* ------------------------------------------------------------------- env -- */

char *xb_env_get(const char *name)
{
    const char *v = getenv(name);
    return v ? xb_strdup(v) : NULL;
}

int xb_setenv(const char *k, const char *v, int overwrite)
{
#ifdef _WIN32
    return SetEnvironmentVariableA(k, v) ? 0 : -1;
#else
    return setenv(k, v, overwrite);
#endif
}

bool xb_proc_runtime_required(const char *env_name)
{
    char *v = xb_env_get(env_name);
    bool req = v && (xb_strieq(v, "1") || xb_strieq(v, "true") || xb_strieq(v, "yes"));
    xb_free(v);
    return req;
}

char *xb_proc_argv_json(void)
{
#ifdef _WIN32
    const char *cl = GetCommandLineA();
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_arr_begin(&w);
    xb_jw_str(&w, cl ? cl : "");
    xb_jw_arr_end(&w);
    return xb_buf_steal(&w.buf);
#else
    FILE *f = fopen("/proc/self/cmdline", "rb");
    xb_jsonw w;
    xb_jw_init(&w);
    xb_jw_arr_begin(&w);
    if (f) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        size_t i = 0;
        while (i < n) {
            size_t start = i;
            while (i < n && buf[i]) i++;
            xb_jw_strn(&w, buf + start, i - start);
            i++;
        }
        fclose(f);
    }
    xb_jw_arr_end(&w);
    return xb_buf_steal(&w.buf);
#endif
}
