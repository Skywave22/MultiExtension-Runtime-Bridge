/* bridge/html.h — small tolerant HTML parser + CSS-lite selector engine.
 *
 * This is what lets rule-based extensions (Legado and friends) run with no
 * third-party dependency and no bundled browser. It is intentionally a subset:
 * tags, attributes, text, nesting, void elements — everything an extraction
 * rule needs, nothing a layout engine needs.
 */
#ifndef BRIDGE_HTML_H
#define BRIDGE_HTML_H

#include <stddef.h>
#include <stdbool.h>
#include "buf.h"

typedef struct hnode hnode;

typedef struct {
    char *name;
    char *value;      /* "" for boolean attributes */
} hattr;

struct hnode {
    char    *tag;          /* lowercase; "#text" for text nodes; "#root" for the root */
    char    *text;         /* text content for #text, otherwise NULL */
    hattr   *attrs;
    size_t   nattrs;
    hnode  **kids;
    size_t   nkids;
    hnode   *parent;
};

typedef struct {
    hnode  *root;
    size_t  nodes;
    bool    truncated;
} hdoc;

hdoc    *xb_html_parse(const char *html, size_t len);
void     xb_html_free(hdoc *d);

/* Serialise a node's inner HTML (or outer HTML with include_self). */
char    *xb_html_inner(const hnode *n);
char    *xb_html_outer(const hnode *n);
/* Text content with tags stripped and whitespace collapsed. */
char    *xb_html_text(const hnode *n);
const char *xb_html_attr(const hnode *n, const char *name);

/* ------------------------------------------------------------ selectors -- */

/* A selector list is comma separated; the first part that matches anything wins.
 * Supported per part:
 *   tag, .class, #id, *, tag.class, tag#id, [attr], [attr=value]
 *   descendant combinator (whitespace)
 *   :first, [n] (n-th match, 0-based), @attr / @text / @html / @outerhtml suffix
 */
typedef struct {
    hnode **nodes;
    size_t  n;
    char    attr[64];      /* extraction requested by the selector, or "" */
    bool    want_text;
    bool    want_html;
    bool    want_outer;
} hsel;

/* Run a selector over the document. Returns NULL when nothing matched.
 * The caller frees with xb_sel_free(). */
hsel *xb_select(const hdoc *d, const char *selector);
void  xb_sel_free(hsel *s);

/* Extract one string from a selection: the requested attribute, else text. */
char *xb_sel_string(const hsel *s, size_t index);
/* All matches joined by `sep`. */
char *xb_sel_join(const hsel *s, const char *sep);

/* Convenience: select on a node subtree rather than the whole document. */
hsel *xb_select_node(const hnode *n, const char *selector);

#endif /* BRIDGE_HTML_H */
