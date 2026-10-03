/* html.c — tolerant HTML parser + CSS-lite selection. */
#include "bridge/html.h"
#include "bridge/util.h"
#include "bridge/log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define XB_HTML_MAX_NODES 200000

/* ---------------------------------------------------------------- parser -- */

typedef struct {
    hdoc  *doc;
    hnode *cur;
} P;

static const char *VOID_TAGS[] = {
    "area","base","br","col","embed","hr","img","input","link","meta",
    "param","source","track","wbr", NULL
};

static bool is_void(const char *tag)
{
    for (int i = 0; VOID_TAGS[i]; i++)
        if (strcmp(tag, VOID_TAGS[i]) == 0) return true;
    return false;
}

static hnode *node_new(hdoc *d, const char *tag, size_t taglen)
{
    if (d->nodes >= XB_HTML_MAX_NODES) { d->truncated = true; return NULL; }
    hnode *n = (hnode *)xb_alloc(sizeof *n);
    n->tag = xb_strndup(tag, taglen);
    for (char *p = n->tag; *p; p++) *p = (char)tolower((unsigned char)*p);
    d->nodes++;
    return n;
}

static void node_add_attr(hnode *n, const char *name, size_t nl, const char *val, size_t vl)
{
    n->attrs = (hattr *)xb_realloc(n->attrs, sizeof(hattr) * (n->nattrs + 1));
    n->attrs[n->nattrs].name = xb_strndup(name, nl);
    for (char *p = n->attrs[n->nattrs].name; *p; p++) *p = (char)tolower((unsigned char)*p);
    n->attrs[n->nattrs].value = xb_strndup(val, vl);
    n->nattrs++;
}

static void node_append(hnode *parent, hnode *child)
{
    if (!parent || !child) return;
    parent->kids = (hnode **)xb_realloc(parent->kids, sizeof(hnode *) * (parent->nkids + 1));
    parent->kids[parent->nkids++] = child;
    child->parent = parent;
}

static hnode *text_node(hdoc *d, const char *s, size_t len)
{
    if (len == 0) return NULL;
    hnode *n = node_new(d, "#text", 5);
    if (!n) return NULL;
    n->text = xb_strndup(s, len);
    return n;
}

/* Decode the handful of entities that matter for extraction. */
static char *decode_entities(const char *s, size_t len)
{
    xb_buf b;
    xb_buf_init(&b);
    for (size_t i = 0; i < len; i++) {
        if (s[i] != '&') { xb_buf_append_byte(&b, (uint8_t)s[i]); continue; }
        size_t semi = i + 1;
        while (semi < len && semi - i < 12 && s[semi] != ';') semi++;
        if (semi >= len || s[semi] != ';') { xb_buf_append_byte(&b, (uint8_t)s[i]); continue; }
        char name[12];
        size_t nl = semi - i - 1;
        memcpy(name, s + i + 1, nl);
        name[nl] = '\0';
        if      (xb_strieq(name, "amp"))  xb_buf_append_byte(&b, '&');
        else if (xb_strieq(name, "lt"))   xb_buf_append_byte(&b, '<');
        else if (xb_strieq(name, "gt"))   xb_buf_append_byte(&b, '>');
        else if (xb_strieq(name, "quot")) xb_buf_append_byte(&b, '"');
        else if (xb_strieq(name, "apos")) xb_buf_append_byte(&b, '\'');
        else if (xb_strieq(name, "nbsp")) xb_buf_append_byte(&b, ' ');
        else if (xb_strieq(name, "#39"))  xb_buf_append_byte(&b, '\'');
        else if (name[0] == '#') {
            long cp = (name[1] == 'x' || name[1] == 'X')
                      ? strtol(name + 2, NULL, 16) : strtol(name + 1, NULL, 10);
            if (cp > 0 && cp < 0x80) {
                xb_buf_append_byte(&b, (uint8_t)cp);
            } else if (cp >= 0x80 && cp < 0x800) {
                xb_buf_append_byte(&b, (uint8_t)(0xC0 | (cp >> 6)));
                xb_buf_append_byte(&b, (uint8_t)(0x80 | (cp & 0x3F)));
            } else if (cp >= 0x800) {
                xb_buf_append_byte(&b, (uint8_t)(0xE0 | (cp >> 12)));
                xb_buf_append_byte(&b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
                xb_buf_append_byte(&b, (uint8_t)(0x80 | (cp & 0x3F)));
            }
        } else {
            xb_buf_append(&b, s + i, semi - i + 1);
        }
        i = semi;
    }
    return xb_buf_steal(&b);
}

static void skip_whitespace(const char **p, const char *end)
{
    while (*p < end && isspace((unsigned char)**p)) (*p)++;
}

hdoc *xb_html_parse(const char *html, size_t len)
{
    hdoc *d = (hdoc *)xb_alloc(sizeof *d);
    d->root = node_new(d, "#root", 5);
    P ps;
    ps.doc = d;
    ps.cur = d->root;

    const char *p = html;
    const char *end = html + len;
    const char *text_start = p;

    while (p < end) {
        if (*p != '<') { p++; continue; }

        /* Flush accumulated text. */
        if (p > text_start) {
            size_t tl = (size_t)(p - text_start);
            bool ws_only = true;
            for (size_t i = 0; i < tl && ws_only; i++)
                if (!isspace((unsigned char)text_start[i])) ws_only = false;
            if (!ws_only) {
                char *decoded = decode_entities(text_start, tl);
                hnode *t = text_node(d, decoded, strlen(decoded));
                if (t) node_append(ps.cur, t);
                xb_free(decoded);
            }
        }

        if (end - p >= 4 && strncmp(p, "<!--", 4) == 0) {
            const char *close = strstr(p + 4, "-->");
            p = close ? close + 3 : end;
            text_start = p;
            continue;
        }
        if (end - p >= 2 && (p[1] == '!' || p[1] == '?')) {
            const char *close = strchr(p, '>');
            p = close ? close + 1 : end;
            text_start = p;
            continue;
        }

        if (p[1] == '/') {
            /* closing tag */
            const char *q = p + 2;
            const char *name = q;
            while (q < end && (isalnum((unsigned char)*q) || *q == '-')) q++;
            size_t nl = (size_t)(q - name);
            if (ps.cur && ps.cur->parent && nl && strncasecmp(ps.cur->tag, name, nl) == 0
                && strlen(ps.cur->tag) == nl)
                ps.cur = ps.cur->parent;
            else {
                /* Mismatched close: walk up until we find the tag (tolerant). */
                hnode *walk = ps.cur;
                while (walk && walk->parent) {
                    if (nl && strncasecmp(walk->tag, name, nl) == 0 && strlen(walk->tag) == nl) {
                        ps.cur = walk->parent;
                        break;
                    }
                    walk = walk->parent;
                }
            }
            const char *close = strchr(p, '>');
            p = close ? close + 1 : end;
            text_start = p;
            continue;
        }

        /* opening tag */
        {
            const char *q = p + 1;
            const char *name = q;
            while (q < end && (isalnum((unsigned char)*q) || *q == '-' || *q == ':')) q++;
            size_t nl = (size_t)(q - name);
            if (nl == 0) { p++; text_start = p; continue; }

            hnode *n = node_new(d, name, nl);
            bool self_closing = false;
            for (;;) {
                skip_whitespace(&q, end);
                if (q >= end) break;
                if (*q == '>') { q++; break; }
                if (*q == '/' ) { q++; if (q < end && *q == '>') q++; self_closing = true; break; }
                const char *an = q;
                while (q < end && !isspace((unsigned char)*q) && *q != '=' && *q != '>' && *q != '/') q++;
                size_t anl = (size_t)(q - an);
                skip_whitespace(&q, end);
                const char *av = "";
                size_t avl = 0;
                if (q < end && *q == '=') {
                    q++;
                    skip_whitespace(&q, end);
                    if (q < end && (*q == '"' || *q == '\'')) {
                        char quote = *q++;
                        av = q;
                        while (q < end && *q != quote) q++;
                        avl = (size_t)(q - av);
                        if (q < end) q++;
                    } else {
                        av = q;
                        while (q < end && !isspace((unsigned char)*q) && *q != '>') q++;
                        avl = (size_t)(q - av);
                    }
                }
                if (n && anl) node_add_attr(n, an, anl, av, avl);
            }

            /* Skip raw text inside script/style so JS does not leak into text. */
            const char *body = q;
            if (n && (xb_strieq(n->tag, "script") || xb_strieq(n->tag, "style"))) {
                char closer[32];
                snprintf(closer, sizeof closer, "</%s", n->tag);
                const char *cp = q;
                while (cp < end) {
                    if (*cp == '<' && (size_t)(end - cp) > strlen(closer) &&
                        strncasecmp(cp, closer, strlen(closer)) == 0) break;
                    cp++;
                }
                body = cp;
                const char *gt = strchr(cp, '>');
                q = gt ? gt + 1 : end;
            } else if (n && is_void(n->tag)) {
                self_closing = true;
            }

            if (n) {
                node_append(ps.cur, n);
                if (!self_closing) ps.cur = n;
            }
            p = (n && (xb_strieq(n->tag, "script") || xb_strieq(n->tag, "style"))) ? q : body;
            if (p < body) p = body;
            text_start = p;
        }
    }

    if (end > text_start) {
        size_t tl = (size_t)(end - text_start);
        bool ws_only = true;
        for (size_t i = 0; i < tl && ws_only; i++)
            if (!isspace((unsigned char)text_start[i])) ws_only = false;
        if (!ws_only) {
            char *decoded = decode_entities(text_start, tl);
            hnode *t = text_node(d, decoded, strlen(decoded));
            if (t) node_append(ps.cur, t);
            xb_free(decoded);
        }
    }
    return d;
}

static void free_node(hnode *n)
{
    if (!n) return;
    for (size_t i = 0; i < n->nkids; i++) free_node(n->kids[i]);
    xb_free(n->kids);
    for (size_t i = 0; i < n->nattrs; i++) {
        xb_free(n->attrs[i].name);
        xb_free(n->attrs[i].value);
    }
    xb_free(n->attrs);
    xb_free(n->tag);
    xb_free(n->text);
    xb_free(n);
}

void xb_html_free(hdoc *d)
{
    if (!d) return;
    free_node(d->root);
    xb_free(d);
}

const char *xb_html_attr(const hnode *n, const char *name)
{
    if (!n || !name) return NULL;
    for (size_t i = 0; i < n->nattrs; i++)
        if (xb_strieq(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

static void append_text(const hnode *n, xb_buf *b)
{
    if (!n) return;
    if (n->text) { xb_buf_append_str(b, n->text); return; }
    for (size_t i = 0; i < n->nkids; i++) {
        if (i && n->kids[i]->text && n->kids[i-1]->text) xb_buf_append_byte(b, ' ');
        append_text(n->kids[i], b);
    }
}

char *xb_html_text(const hnode *n)
{
    xb_buf b;
    xb_buf_init(&b);
    append_text(n, &b);
    /* Collapse runs of whitespace: extraction rules always want clean strings. */
    char *raw = xb_buf_steal(&b);
    xb_buf out;
    xb_buf_init(&out);
    bool prev_space = false;
    for (const char *p = raw; *p; p++) {
        if (isspace((unsigned char)*p)) {
            if (!prev_space) xb_buf_append_byte(&out, ' ');
            prev_space = true;
        } else {
            xb_buf_append_byte(&out, (uint8_t)*p);
            prev_space = false;
        }
    }
    xb_free(raw);
    char *v = xb_buf_steal(&out);
    size_t vlen = strlen(v);
    while (vlen && v[vlen-1] == ' ') v[--vlen] = '\0';
    char *start = v;
    while (*start == ' ') start++;
    if (start != v) memmove(v, start, strlen(start) + 1);
    return v;
}

static void append_serialise(const hnode *n, xb_buf *b, bool self);

static void append_attrs(const hnode *n, xb_buf *b)
{
    for (size_t i = 0; i < n->nattrs; i++) {
        xb_buf_append_byte(b, ' ');
        xb_buf_append_str(b, n->attrs[i].name);
        if (n->attrs[i].value && n->attrs[i].value[0]) {
            xb_buf_append_str(b, "=\"");
            /* Escape just enough to stay well-formed. */
            for (const char *p = n->attrs[i].value; *p; p++) {
                if (*p == '"') xb_buf_append_str(b, "&quot;");
                else xb_buf_append_byte(b, (uint8_t)*p);
            }
            xb_buf_append_byte(b, '"');
        }
    }
}

static void append_serialise(const hnode *n, xb_buf *b, bool self)
{
    if (!n) return;
    if (n->text) { xb_buf_append_str(b, n->text); return; }
    if (self) {
        xb_buf_append_byte(b, '<');
        xb_buf_append_str(b, n->tag);
        append_attrs(n, b);
        xb_buf_append_byte(b, '>');
    }
    for (size_t i = 0; i < n->nkids; i++) append_serialise(n->kids[i], b, true);
    if (self) {
        xb_buf_append_str(b, "</");
        xb_buf_append_str(b, n->tag);
        xb_buf_append_byte(b, '>');
    }
}

char *xb_html_inner(const hnode *n)
{
    xb_buf b;
    xb_buf_init(&b);
    if (n) for (size_t i = 0; i < n->nkids; i++) append_serialise(n->kids[i], &b, true);
    return xb_buf_steal(&b);
}

char *xb_html_outer(const hnode *n)
{
    xb_buf b;
    xb_buf_init(&b);
    append_serialise(n, &b, true);
    return xb_buf_steal(&b);
}

/* -------------------------------------------------------------- selector -- */

typedef struct {
    char tag[64];
    char cls[64];
    char id[64];
    char attr[64];
    char attr_val[64];
    int  nth;          /* -1 = unset */
} simple_sel;

static void strndup_into(const char *s, size_t len, char *out, size_t cap)
{
    if (cap == 0) return;
    if (len >= cap) len = cap - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

static void parse_simple(const char *s, size_t len, simple_sel *out)
{
    memset(out, 0, sizeof *out);
    out->nth = -1;
    size_t i = 0;
    /* Trailing [n] index — but only when the bracket really holds a number.
     * `li[data-id="2"]` must stay an attribute predicate. */
    if (len > 2 && s[len-1] == ']') {
        size_t open = len;
        for (size_t k = len - 1; k > 0; k--) if (s[k] == '[') { open = k; break; }
        if (open < len - 1) {
            bool all_digits = open + 1 < len - 1;
            for (size_t k = open + 1; k < len - 1; k++)
                if (!isdigit((unsigned char)s[k])) all_digits = false;
            if (all_digits) {
                char num[16];
                size_t nl = len - 1 - open - 1;
                if (nl < sizeof num) {
                    memcpy(num, s + open + 1, nl);
                    num[nl] = '\0';
                    out->nth = atoi(num);
                    len = open;
                }
            }
        }
    }
    /* One pass over the simple selector: tag, then any mix of .class, #id and
     * [attr] / [attr="v"] suffixes. Order is irrelevant and repeats are last
     * wins, which is what the selector strings in rule files rely on. */
    bool tag_done = false;
    while (i < len) {
        char c = s[i];
        if (c == '.') {
            size_t j = i + 1;
            while (j < len && s[j] != '.' && s[j] != '#' && s[j] != '[') j++;
            size_t cl = j - i - 1;
            if (cl > 0 && cl < sizeof out->cls) {
                memcpy(out->cls, s + i + 1, cl);
                out->cls[cl] = '\0';
            }
            i = j;
        } else if (c == '#') {
            size_t j = i + 1;
            while (j < len && s[j] != '.' && s[j] != '#' && s[j] != '[') j++;
            size_t il = j - i - 1;
            if (il > 0 && il < sizeof out->id) {
                memcpy(out->id, s + i + 1, il);
                out->id[il] = '\0';
            }
            i = j;
        } else if (c == '[') {
            size_t j = i + 1;
            while (j < len && s[j] != ']') j++;
            char inner[160];
            size_t il = (j > i) ? j - i - 1 : 0;
            if (il < sizeof inner) {
                memcpy(inner, s + i + 1, il);
                inner[il] = '\0';
                char *eq = strchr(inner, '=');
                if (eq) {
                    *eq = '\0';
                    xb_str_lcpy(out->attr, xb_str_trim(inner), sizeof out->attr);
                    char *v = xb_str_trim(eq + 1);
                    size_t vl = strlen(v);
                    if (vl >= 2 && (v[0] == '"' || v[0] == '\'')) { v++; vl -= 2; }
                    strndup_into(v, vl, out->attr_val, sizeof out->attr_val);
                } else {
                    xb_str_lcpy(out->attr, xb_str_trim(inner), sizeof out->attr);
                }
            }
            i = (j < len) ? j + 1 : len;
        } else if (!tag_done) {
            size_t j = i;
            while (j < len && s[j] != '.' && s[j] != '#' && s[j] != '[') j++;
            size_t tl = j - i;
            if (tl < sizeof out->tag) {
                memcpy(out->tag, s + i, tl);
                out->tag[tl] = '\0';
                for (char *q = out->tag; *q; q++) *q = (char)tolower((unsigned char)*q);
            }
            tag_done = true;
            i = j;
        } else {
            i++;   /* stray character: skip rather than mis-parse */
        }
    }
}
/* html.c — tolerant HTML parser + CSS-lite selection. */
static bool tag_matches(const hnode *n, const simple_sel *s)
{
    if (!n || n->tag[0] == '#') return false;
    if (s->tag[0] && strcmp(s->tag, "*") != 0 && !xb_strieq(s->tag, n->tag)) return false;
    if (s->id[0]) {
        const char *id = xb_html_attr(n, "id");
        if (!id || !xb_streq(id, s->id)) return false;
    }
    if (s->cls[0]) {
        const char *cls = xb_html_attr(n, "class");
        if (!cls) return false;
        bool found = false;
        const char *p = cls;
        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            const char *start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            size_t l = (size_t)(p - start);
            if (l == strlen(s->cls) && strncmp(start, s->cls, l) == 0) { found = true; break; }
        }
        if (!found) return false;
    }
    if (s->attr[0]) {
        const char *v = xb_html_attr(n, s->attr);
        if (!v) return false;
        if (s->attr_val[0] && !xb_streq(v, s->attr_val)) return false;
    }
    return true;
}

typedef struct { hnode **items; size_t n; size_t cap; } nodelist;

static void nl_push(nodelist *l, hnode *n)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (hnode **)xb_realloc(l->items, sizeof(hnode *) * l->cap);
    }
    l->items[l->n++] = n;
}

static void collect_descendants(hnode *n, nodelist *out)
{
    for (size_t i = 0; i < n->nkids; i++) {
        nl_push(out, n->kids[i]);
        collect_descendants(n->kids[i], out);
    }
}

hsel *xb_select_node(const hnode *root, const char *selector)
{
    if (!root || !selector) return NULL;

    char *work = xb_strdup(selector);
    char *parts[8];
    size_t nparts = xb_str_split(work, ',', parts, 8);

    hsel *best = NULL;

    for (size_t pi = 0; pi < nparts && !best; pi++) {
        char *part = xb_str_trim(parts[pi]);
        if (!*part) continue;

        /* Extraction suffix. */
        char attr[64] = "";
        bool want_text = false, want_html = false, want_outer = false;

        /* Remove :first / :last pseudo classes (mapped to nth). */
        char *pseudo = strchr(part, ':');
        if (pseudo) {
            if (strstr(pseudo, ":first")) { /* handled via nth below */ }
            *pseudo = '\0';
        }

        /* Split the chain on whitespace into simple selectors. */
        char *steps[8];
        size_t nsteps = 0;
        {
            char *p = part;
            while (*p && nsteps < 8) {
                while (*p && isspace((unsigned char)*p)) p++;
                if (!*p) break;
                steps[nsteps++] = p;
                while (*p && !isspace((unsigned char)*p)) p++;
                if (*p) *p++ = '\0';
            }
        }
        if (nsteps == 0) continue;

        /* The last step may end with @something. */
        char *last = steps[nsteps - 1];
        char *at = strrchr(last, '@');
        if (at) {
            *at = '\0';
            xb_str_lcpy(attr, at + 1, sizeof attr);
            if (xb_strieq(attr, "text")) { want_text = true; attr[0] = '\0'; }
            else if (xb_strieq(attr, "html") || xb_strieq(attr, "innerhtml")) { want_html = true; attr[0] = '\0'; }
            else if (xb_strieq(attr, "outerhtml")) { want_outer = true; attr[0] = '\0'; }
            /* "@text@href" style chains: keep the last meaningful part. */
            char *at2 = strrchr(attr, '@');
            if (at2) memmove(attr, at2 + 1, strlen(at2 + 1) + 1);
        }

        nodelist cur = {0};
        /* Each step is matched against the descendants of the previous step's
         * results, and an `[n]` suffix narrows that step immediately, so
         * `li[1] a` picks the link inside the second list item. */
        for (size_t si = 0; si < nsteps && (si == 0 || cur.n); si++) {
            simple_sel s;
            parse_simple(steps[si], strlen(steps[si]), &s);
            nodelist next = {0};
            if (si == 0) {
                nodelist all = {0};
                collect_descendants((hnode *)root, &all);
                for (size_t i = 0; i < all.n; i++)
                    if (tag_matches(all.items[i], &s)) nl_push(&next, all.items[i]);
                xb_free(all.items);
            } else {
                for (size_t i = 0; i < cur.n; i++) {
                    nodelist kids = {0};
                    collect_descendants(cur.items[i], &kids);
                    for (size_t k = 0; k < kids.n; k++)
                        if (tag_matches(kids.items[k], &s)) nl_push(&next, kids.items[k]);
                    xb_free(kids.items);
                }
                xb_free(cur.items);
            }
            cur = next;
            if (s.nth >= 0) {
                if ((size_t)s.nth < cur.n) {
                    hnode *pick = cur.items[s.nth];
                    xb_free(cur.items);
                    cur.items = (hnode **)xb_alloc(sizeof(hnode *));
                    cur.items[0] = pick;
                    cur.n = 1;
                } else {
                    xb_free(cur.items);
                    cur.items = NULL;
                    cur.n = 0;
                }
            }
        }

        if (cur.n) {
            best = (hsel *)xb_alloc(sizeof(hsel));
            best->nodes = cur.items;
            best->n = cur.n;
            xb_str_lcpy(best->attr, attr, sizeof best->attr);
            best->want_text = want_text;
            best->want_html = want_html;
            best->want_outer = want_outer;
        } else {
            xb_free(cur.items);
        }
    }

    xb_free(work);
    return best;
}

hsel *xb_select(const hdoc *d, const char *selector)
{
    if (!d) return NULL;
    return xb_select_node(d->root, selector);
}

void xb_sel_free(hsel *s)
{
    if (!s) return;
    xb_free(s->nodes);
    xb_free(s);
}

char *xb_sel_string(const hsel *s, size_t index)
{
    if (!s || index >= s->n) return NULL;
    const hnode *n = s->nodes[index];
    if (s->want_html)  return xb_html_inner(n);
    if (s->want_outer) return xb_html_outer(n);
    if (s->want_text)  return xb_html_text(n);
    if (s->attr[0]) {
        const char *v = xb_html_attr(n, s->attr);
        return xb_strdup(v ? v : "");
    }
    if (n->tag[0] == '#') return xb_strdup(n->text ? n->text : "");
    return xb_html_text(n);
}

char *xb_sel_join(const hsel *s, const char *sep)
{
    xb_buf b;
    xb_buf_init(&b);
    for (size_t i = 0; s && i < s->n; i++) {
        char *v = xb_sel_string(s, i);
        if (i) xb_buf_append_str(&b, sep ? sep : "\n");
        if (v) xb_buf_append_str(&b, v);
        xb_free(v);
    }
    return xb_buf_steal(&b);
}
