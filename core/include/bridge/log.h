/* bridge/log.h — structured logging with an in-memory ring buffer.
 *
 * Every line is also parseable JSON so hosts can forward logs untouched.
 * The ring buffer backs the `bridge.log` streaming method, which is what makes
 * "why is this extension slow?" answerable from the host app.
 */
#ifndef BRIDGE_LOG_H
#define BRIDGE_LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    XB_LOG_TRACE = 0,
    XB_LOG_DEBUG,
    XB_LOG_INFO,
    XB_LOG_WARN,
    XB_LOG_ERROR,
    XB_LOG_FATAL
} xb_log_level;

typedef struct {
    xb_log_level level;
    char        *msg;          /* malloc'd JSON object text */
    int64_t      ts_ms;
} xb_log_record;

void xb_log_init(xb_log_level min_level, bool to_stderr, bool ring_enabled);
void xb_log_shutdown(void);
void xb_log_set_level(xb_log_level lvl);
xb_log_level xb_log_get_level(void);
bool xb_log_parse_level(const char *s, xb_log_level *out);

void xb_log(xb_log_level lvl, const char *fmt, ...);

/* Structured variant: emits {"ts":..,"level":..,"msg":..,"data":{...}} where
 * `fields_json` is a pre-serialised JSON object (or NULL). */
void xb_log_fields(xb_log_level lvl, const char *fields_json,
                   const char *fmt, ...);

/* Snapshot the ring buffer as a JSON array string (caller frees).
 * `since_ms` <= 0 means "everything held". `max` <= 0 means "no limit". */
char *xb_log_snapshot_json(int64_t since_ms, size_t max);
size_t xb_log_count(void);

#define XB_TRACE(...) xb_log(XB_LOG_TRACE, __VA_ARGS__)
#define XB_DEBUG(...) xb_log(XB_LOG_DEBUG, __VA_ARGS__)
#define XB_INFO(...)  xb_log(XB_LOG_INFO,  __VA_ARGS__)
#define XB_WARN(...)  xb_log(XB_LOG_WARN,  __VA_ARGS__)
#define XB_ERR(...)   xb_log(XB_LOG_ERROR, __VA_ARGS__)
#define XB_FATAL(...) do { xb_log(XB_LOG_FATAL, __VA_ARGS__); } while (0)

#endif /* BRIDGE_LOG_H */
