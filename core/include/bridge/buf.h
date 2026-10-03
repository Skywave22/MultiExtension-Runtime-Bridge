/* bridge/buf.h — growable byte buffer and arena allocator. */
#ifndef BRIDGE_BUF_H
#define BRIDGE_BUF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------- byte buffer -- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} xb_buf;

void   xb_buf_init(xb_buf *b);
void   xb_buf_free(xb_buf *b);
void   xb_buf_reset(xb_buf *b);
void   xb_buf_reserve(xb_buf *b, size_t extra);
void   xb_buf_append(xb_buf *b, const void *p, size_t n);
void   xb_buf_append_byte(xb_buf *b, uint8_t c);
void   xb_buf_append_str(xb_buf *b, const char *s);
/* Append a JSON-escaped string body (no surrounding quotes). Escapes
 * ", \, control chars, and emits \u for < 0x20. UTF-8 passes through. */
void   xb_buf_append_json_string(xb_buf *b, const char *s, size_t n);
/* Take ownership of the bytes; caller must free() them. Buffer is reset. */
char  *xb_buf_steal(xb_buf *b);

/* --------------------------------------------------------------- arena --- */

/* Bump allocator: many small allocations, freed in one shot. Used by the JSON
 * parser so a 4 MiB payload costs one malloc, not megabytes of linked nodes. */
typedef struct xb_arena_block {
    struct xb_arena_block *next;
    size_t used;
    size_t cap;
    uint8_t data[];
} xb_arena_block;

typedef struct {
    xb_arena_block *head;
    size_t          total;
} xb_arena;

void  xb_arena_init(xb_arena *a);
void  xb_arena_destroy(xb_arena *a);
void *xb_arena_alloc(xb_arena *a, size_t n);          /* 8-byte aligned, zeroed */
char *xb_arena_strndup(xb_arena *a, const char *s, size_t n);
size_t xb_arena_bytes(const xb_arena *a);

#endif /* BRIDGE_BUF_H */
