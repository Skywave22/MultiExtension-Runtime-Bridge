/* main.c — xbridged entry point: configuration, engines, signals.
 *
 * The daemon is deliberately a single self-contained binary. Everything optional
 * (a JS runtime, a JVM, TorrServer) is *discovered*, reported in the handshake,
 * and skipped when absent — starting the bridge never fails because a
 * third-party runtime is missing.
 */
#include "bridge/bridge_internal.h"
#include "bridge/server.h"
#include "bridge/log.h"
#include "bridge/net.h"
#include "bridge/util.h"
#include "bridge/registry.h"
#include "bridge/proc.h"
#include "bridge/http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <signal.h>
#  include <unistd.h>
#endif

static xb_bridge *g_bridge = NULL;

#ifndef _WIN32
static void on_signal(int sig)
{
    (void)sig;
    if (g_bridge) xb_server_request_shutdown(g_bridge);
}
#endif

static void usage(FILE *out)
{
    fprintf(out,
        "xbridged — Multi-Extension Runtime Bridge daemon\n"
        "\n"
        "Usage: xbridged [options]\n"
        "\n"
        "  --endpoint <uri>      unix:///path, unix-abstract://name,\n"
        "                        npipe://name, tcp://host:port  (default: per-user)\n"
        "  --data-dir <path>     where state, extensions and downloads live\n"
        "  --log-level <lvl>     trace|debug|info|warn|error  (default: info)\n"
        "  --max-frame <bytes>   protocol frame ceiling          (default: 16777216)\n"
        "  --task-threads <n>    request executor threads        (default: 16)\n"
        "  --workers <n>         worker processes per engine     (default: 2)\n"
        "  --cache-entries <n>   response cache size             (default: 4096)\n"
        "  --no-cache            disable the response cache\n"
        "  --prewarm             start engines before the first request\n"
        "  --allow-shutdown      permit the bridge.shutdown method\n"
        "  --quiet               do not write logs to stderr\n"
        "  --print-endpoint      print the resolved endpoint URI and exit\n"
        "  --list-formats        print the format registry as JSON and exit\n"
        "  --methods             print the method table as JSON and exit\n"
        "  --version             print version information and exit\n"
        "  --help                this text\n"
        "\n"
        "Environment:\n"
        "  XBRIDGE_JS_WORKER   path to a JS engine worker (optional)\n"
        "  XBRIDGE_JVM_WORKER  path to a JVM engine worker (optional)\n"
        "\n"
        "On startup the daemon prints exactly one JSON line:\n"
        "  {\"event\":\"ready\",\"protocol\":\"XBP/1\",\"endpoint\":\"unix:///...\", ...}\n");
}

static const char *default_data_dir(void)
{
    static char buf[512];
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0]) { snprintf(buf, sizeof buf, "%s\\xbridge", appdata); return buf; }
    snprintf(buf, sizeof buf, ".\\xbridge");
#else
    const char *home = getenv("HOME");
    if (home && home[0]) { snprintf(buf, sizeof buf, "%s/.local/share/xbridge", home); return buf; }
    snprintf(buf, sizeof buf, "/tmp/xbridge-data");
#endif
    return buf;
}

static int parse_int_arg(const char *s, int fallback)
{
    if (!s || !s[0]) return fallback;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return fallback;
    return (int)v;
}

int main(int argc, char **argv)
{
    xb_log_level level = XB_LOG_INFO;
    bool quiet = false;
    bool to_stderr = true;
    xb_net_global_init();

    xb_str_lcpy(xb_g_config.data_dir, default_data_dir(), sizeof xb_g_config.data_dir);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (xb_streq(a, "--help") || xb_streq(a, "-h")) { usage(stdout); return 0; }
        if (xb_streq(a, "--version")) {
            printf("xbridged 1.0.0 (%s)\n", XBP_VERSION);
            printf("compiled %s %s\n", __DATE__, __TIME__);
            return 0;
        }
        if (xb_streq(a, "--list-formats")) { printf("%s\n", xb_registry_json()); return 0; }
        if (xb_streq(a, "--endpoint") && next) { xb_str_lcpy(xb_g_config.endpoint_uri, next, sizeof xb_g_config.endpoint_uri); i++; continue; }
        if (xb_streq(a, "--data-dir") && next) { xb_str_lcpy(xb_g_config.data_dir, next, sizeof xb_g_config.data_dir); i++; continue; }
        if (xb_streq(a, "--log-level") && next) {
            if (!xb_log_parse_level(next, &level)) {
                fprintf(stderr, "unknown log level '%s'\n", next);
                return 2;
            }
            i++;
            continue;
        }
        if (xb_streq(a, "--max-frame") && next) { xb_g_config.max_frame = parse_int_arg(next, xb_g_config.max_frame); i++; continue; }
        if (xb_streq(a, "--task-threads") && next) { xb_g_config.task_threads = parse_int_arg(next, xb_g_config.task_threads); i++; continue; }
        if (xb_streq(a, "--workers") && next) { xb_g_config.workers_per_engine = parse_int_arg(next, xb_g_config.workers_per_engine); i++; continue; }
        if (xb_streq(a, "--cache-entries") && next) { xb_g_config.cache_entries = parse_int_arg(next, xb_g_config.cache_entries); i++; continue; }
        if (xb_streq(a, "--no-cache")) { xb_g_config.cache_entries = 1; xb_g_config.cache_bytes = 1024; xb_g_config.cache_ttl_ms = 0; continue; }
        if (xb_streq(a, "--prewarm")) { xb_g_config.prewarm = true; continue; }
        if (xb_streq(a, "--allow-shutdown")) { xb_g_config.allow_shutdown = true; continue; }
        if (xb_streq(a, "--quiet")) { quiet = true; to_stderr = false; xb_g_config.quiet = true; continue; }
        if (xb_streq(a, "--print-endpoint")) {
            xb_endpoint ep;
            if (xb_g_config.endpoint_uri[0]) {
                if (xb_ep_parse(xb_g_config.endpoint_uri, &ep) != 0) return 2;
            } else {
                xb_ep_default(&ep);
            }
            char uri[320];
            printf("%s\n", xb_ep_format(&ep, uri, sizeof uri));
            return 0;
        }
        fprintf(stderr, "unknown or incomplete option: %s\n", a);
        usage(stderr);
        return 2;
    }

    if (xb_g_config.max_frame < 1024) xb_g_config.max_frame = 1024;
    if (xb_g_config.task_threads < 1) xb_g_config.task_threads = 1;
    if (xb_g_config.task_threads > 512) xb_g_config.task_threads = 512;

    /* The ready line is the machine-readable contract; logs go to stderr. */
    (void)quiet;
    xb_log_init(level, to_stderr, true);

    xb_register_builtin_methods();

    if (xb_streq(argv[argc - 1], "--methods") || (argc > 1 && xb_streq(argv[1], "--methods"))) {
        printf("%s\n", xb_method_list_json());
        return 0;
    }

    xb_bridge *b = xb_bridge_new(&xb_g_config);
    g_bridge = b;

    char err[512] = "";
    if (xb_bridge_init(b, err, sizeof err) != 0) {
        fprintf(stderr, "xbridged: %s\n", err);
        return 1;
    }

#ifndef _WIN32
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
#endif

    int rc = xb_server_run(b, err, sizeof err);
    if (rc != 0) fprintf(stderr, "xbridged: %s\n", err);

    xb_bridge_free(b);
    g_bridge = NULL;
    xb_log_shutdown();
    xb_net_global_shutdown();
    return rc;
}
