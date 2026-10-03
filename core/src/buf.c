/* buf.c — byte buffer + arena. */
#include "bridge/buf.h"
#include "bridge/util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void xb_buf_init(xb_buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
void xb_buf_free(xb_buf *b) { xb_free(b->data); b->data = NULL; b->len = b->cap = 0; }
void xb_buf_reset(xb_buf *b) { b->len = 0; if (b->data) b->data[0] = 0; }

void xb_buf_reserve(xb_buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t want = b->len + extra + 1;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < want) cap *= 2;
    b->data = (uint8_t *)xb_realloc(b->data, cap);
    b->cap = cap;
}

void xb_buf_append(xb_buf *b, const void *p, size_t n)
{
    if (!n) return;
    xb_buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

void xb_buf_append_byte(xb_buf *b, uint8_t c)
{
    xb_buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

void xb_buf_append_str(xb_buf *b, const char *s)
{
    if (s) xb_buf_append(b, s, strlen(s));
}

void xb_buf_append_json_string(xb_buf *b, const char *s, size_t n)
{
    xb_buf_append_byte(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  xb_buf_append(b, "\\\"", 2); break;
        case '\\': xb_buf_append(b, "\\\\", 2); break;
        case '\n': xb_buf_append(b, "\\n", 2); break;
        case '\r': xb_buf_append(b, "\\r", 2); break;
        case '\t': xb_buf_append(b, "\\t", 2); break;
        case '\b': xb_buf_append(b, "\\b", 2); break;
        case '\f': xb_buf_append(b, "\\f", 2); break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", c);
                xb_buf_append(b, tmp, 6);
            } else {
                xb_buf_append_byte(b, c);
            }
        }
    }
    xb_buf_append_byte(b, '"');
}

char *xb_buf_steal(xb_buf *b)
{
    char *p = (char *)b->data;
    if (!p) p = xb_strdup("");
    b->data = NULL; b->len = b->cap = 0;
    return p;
}

/* --------------------------------------------------------------- arena --- */

#define XB_ARENA_BLOCK 65536

void xb_arena_init(xb_arena *a) { a->head = NULL; a->total = 0; }

void xb_arena_destroy(xb_arena *a)
{
    xb_arena_block *b = a->head;
    while (b) {
        xb_arena_block *n = b->next;
        xb_free(b);
        b = n;
    }
    a->head = NULL;
    a->total = 0;
}

void *xb_arena_alloc(xb_arena *a, size_t n)
{
    n = (n + 7u) & ~(size_t)7u;   /* 8-byte alignment */
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > XB_ARENA_BLOCK ? n : XB_ARENA_BLOCK;
        xb_arena_block *blk = (xb_arena_block *)xb_alloc(sizeof(xb_arena_block) + cap);
        blk->cap = cap;
        blk->used = 0;
        blk->next = a->head;
        a->head = blk;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    a->total += n;
    return p;   /* xb_alloc zeroes, so fresh block memory is zeroed */
}

char *xb_arena_strndup(xb_arena *a, const char *s, size_t n)
{
    char *d = (char *)xb_arena_alloc(a, n + 1);
    if (n) memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

size_t xb_arena_bytes(const xb_arena *a) { return a->total; }
