/* HTML to the browser's document: a list of text runs, breaks, pictures and
 * form fields, with the links and fields as focus targets.
 *
 * A forgiving tokenizer, not a full HTML parser: tags open and close styles
 * as they come, block tags make breaks, and scripts, styles and the like
 * are skipped. Text is turned into the font's Latin-1 as it is read. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cyb_internal.h"

uint32_t cyb_utf8_at(const char *s, int *len);

#define ATTR_MAX 12
#define VAL_MAX 2048
#define MAX_DEPTH 8

/* ---- growing arrays ---- */

static bool grow(void **p, uint32_t *cap, uint32_t need, size_t size)
{
    if (need <= *cap) return true;
    uint32_t n = *cap ? *cap : 64;
    while (n < need) n *= 2;
    void *q = realloc(*p, n * size);
    if (!q) return false;
    *p = q;
    *cap = n;
    return true;
}
#define GROW(d, arr, n, cap) grow((void **)&(d)->arr, &(d)->cap, (d)->n + 1, sizeof *(d)->arr)

void cyb_doc_free(cyb_doc_t *d)
{
    free(d->pool);
    free(d->items);
    free(d->targets);
    free(d->fields);
    free(d->forms);
    for (uint32_t i = 0; i < d->n_imgs; i++) free(d->imgs[i].pic.pix);
    free(d->imgs);
    memset(d, 0, sizeof *d);
}

static bool pool_put(cyb_doc_t *d, const char *s, size_t len)
{
    if (!grow((void **)&d->pool, &d->pool_cap, d->pool_len + (uint32_t)len + 1, 1)) {
        d->oom = true;
        return false;
    }
    memcpy(d->pool + d->pool_len, s, len);
    d->pool_len += (uint32_t)len;
    d->pool[d->pool_len] = 0;
    return true;
}

uint32_t cyb_doc_str(cyb_doc_t *d, const char *s, size_t len)
{
    uint32_t at = d->pool_len;
    /* the string and its NUL (pool_put keeps one after the end) */
    if (!pool_put(d, s, len) || !pool_put(d, "", 1)) return POOL_NONE;
    return at;
}

/* ---- the builder ---- */

typedef struct {
    cyb_doc_t *d;
    int cs;
    char base[1024];
    uint8_t style;
    int bold, big, pre, center;
    uint16_t link;             /* current link target + 1 */
    bool run_open;             /* an IT_TEXT item is being added to */
    uint32_t run_item;
    bool space;                /* a space is due before the next word */
    bool line_start;           /* nothing on the line yet */
    int gap;                   /* 2: a paragraph gap is due, 1: a break */
    int skip;                  /* inside script, style, ... */
    char skip_tag[12];
    bool in_title;
    bool in_select;
    bool pre_start;            /* just after <pre>: a new line there is dropped */
    int form;                  /* current form, -1 none */
    int list[MAX_DEPTH];       /* ol counters (-1 for ul) */
    int lists;
    int indent;
    int cells;                 /* cells so far in this table row */
    /* <select>, <textarea>, <button>: text goes here instead */
    int capture;               /* 0 none, 1 option, 2 textarea, 3 button */
    char cap_buf[FIELD_VALUE_MAX];
    size_t cap_len;
    int field;                 /* the field being captured */
    char opt_value[FIELD_VALUE_MAX];
    bool opt_selected, opt_has_value;
    char refresh[1024];
} builder_t;

static void close_run(builder_t *b) { b->run_open = false; }

static uint8_t cur_style(const builder_t *b)
{
    uint8_t s = 0;
    if (b->bold) s |= S_BOLD;
    if (b->big) s |= S_BIG | S_BOLD;
    if (b->pre) s |= S_PRE;
    if (b->link) s |= S_LINK;
    if (b->center) s |= S_CENTER;
    return s;
}

static cyb_item_t *add_item(builder_t *b, uint8_t kind)
{
    cyb_doc_t *d = b->d;
    if (d->oom || !GROW(d, items, n_items, cap_items)) { d->oom = true; return NULL; }
    cyb_item_t *it = &d->items[d->n_items++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->style = cur_style(b);
    it->target = b->link;
    return it;
}

/* Breaks are only added when something comes after them. */
static void flush_gap(builder_t *b)
{
    if (!b->gap) return;
    /* a line break where the line is still empty adds nothing */
    if (b->d->n_items && (b->gap >= 2 || !b->line_start)) add_item(b, b->gap >= 2 ? IT_PARA : IT_BREAK);
    b->gap = 0;
    b->line_start = true;
    b->space = false;
}

static void block(builder_t *b, int gap)
{
    close_run(b);
    if (gap > b->gap) b->gap = gap;
    b->space = false;
}

/* A space that is due goes out now, outside any link (before a field, a
 * picture or a new link). */
static void put_space(builder_t *b)
{
    bool due = b->space && !b->line_start;
    b->space = false;
    if (!due) return;
    cyb_doc_t *d = b->d;
    cyb_item_t *run = b->run_open ? &d->items[b->run_item] : NULL;
    if (!run || run->text + run->len != d->pool_len) {
        uint16_t link = b->link;
        b->link = 0;
        run = add_item(b, IT_TEXT);
        b->link = link;
        if (!run) return;
        run->text = d->pool_len;
    }
    if (pool_put(d, " ", 1)) run->len++;
    close_run(b);
}

/* Text in Latin-1 that goes onto the page. */
static void emit(builder_t *b, const char *s, size_t len)
{
    cyb_doc_t *d = b->d;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (b->pre && b->pre_start && (c == '\n' || c == '\r')) {
            if (c == '\n') b->pre_start = false;
            continue;
        }
        b->pre_start = false;
        if (b->pre) {
            if (c == '\n') { close_run(b); flush_gap(b); add_item(b, IT_BREAK); b->line_start = true; continue; }
            if (c == '\r') continue;
            if (c == '\t') c = ' ';
        } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!b->line_start) b->space = true;
            continue;
        }
        flush_gap(b);
        uint8_t st = cur_style(b);
        cyb_item_t *run = b->run_open ? &d->items[b->run_item] : NULL;
        if (!run || run->style != st || run->target != b->link || run->text + run->len != d->pool_len) {
            put_space(b);
            cyb_item_t *it = add_item(b, IT_TEXT);
            if (!it) return;
            it->text = d->pool_len;
            b->run_item = d->n_items - 1;
            b->run_open = true;
        }
        if (b->space) {
            pool_put(d, " ", 1);
            d->items[b->run_item].len++;
            b->space = false;
        }
        if (!pool_put(d, &c, 1)) return;
        d->items[b->run_item].len++;
        b->line_start = false;
    }
}

/* ---- characters ---- */

/* Decode one character of the page at *p into Latin-1 (out, returns its
 * length), moving *p on. Entities are decoded too. */
static int next_char(builder_t *b, const char **p, const char *end, char *out, bool entities)
{
    const char *s = *p;
    uint32_t cp;
    if (*s == '&' && entities) {
        const char *q = s + 1;
        if (*q == '#') {
            q++;
            int hex = *q == 'x' || *q == 'X';
            if (hex) q++;
            cp = 0;
            const char *dig = q;
            while (q < end && (hex ? isxdigit((unsigned char)*q) : isdigit((unsigned char)*q)) && q - dig < 8) {
                cp = cp * (hex ? 16 : 10) + (uint32_t)(isdigit((unsigned char)*q) ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10));
                q++;
            }
            if (q > dig) {
                if (q < end && *q == ';') q++;
                *p = q;
                if (cp >= 0x80 && cp < 0xa0) cp = cyb_cs_byte(CS_LATIN1, (uint8_t)cp);
                return cyb_latin1(cp, out);
            }
        } else {
            const char *name = q;
            while (q < end && isalnum((unsigned char)*q) && q - name < 10) q++;
            uint32_t e = cyb_entity(name, (int)(q - name));
            if (e) {
                if (q < end && *q == ';') q++;
                *p = q;
                return cyb_latin1(e, out);
            }
        }
    }
    uint8_t c = (uint8_t)*s;
    if (c < 0x80) { *p = s + 1; out[0] = (char)c; return c == 0 ? 0 : 1; }
    if (b->cs == CS_UTF8) {
        int n;
        if (c >= 0xc0 && s + 1 < end) {
            cp = cyb_utf8_at(s, &n);
            if (s + n > end) n = (int)(end - s);
        } else { cp = '?'; n = 1; }
        *p = s + n;
        return cyb_latin1(cp, out);
    }
    *p = s + 1;
    return cyb_latin1(cyb_cs_byte(b->cs, c), out);
}

/* An attribute value decoded to Latin-1 (for text) or kept as bytes with
 * the entities decoded (for URLs: then non-ASCII ones become UTF-8). */
static void decode_value(builder_t *b, const char *s, size_t len, char *out, size_t cap, bool url)
{
    const char *p = s, *end = s + len;
    size_t o = 0;
    while (p < end && o + 8 < cap) {
        if (url) {
            if (*p == '&') {
                char tmp[8];
                const char *q = p;
                builder_t plain = *b;
                plain.cs = CS_LATIN1;
                int n = next_char(&plain, &q, end, tmp, true);
                if (q != p + 1 || n != 1 || tmp[0] != '&') {
                    /* an entity: put its character as UTF-8 */
                    for (int i = 0; i < n; i++) {
                        uint8_t c = (uint8_t)tmp[i];
                        if (c < 0x80) out[o++] = (char)c;
                        else { out[o++] = (char)(0xc0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3f)); }
                    }
                    p = q;
                    continue;
                }
            }
            out[o++] = *p++;
        } else {
            char tmp[8];
            int n = next_char(b, &p, end, tmp, true);
            for (int i = 0; i < n && o + 1 < cap; i++) out[o++] = tmp[i] == '\n' || tmp[i] == '\t' ? ' ' : tmp[i];
        }
    }
    out[o] = 0;
}

/* ---- tags ---- */

typedef struct {
    char name[16];
    bool close, self_close;
    int n;
    const char *an[ATTR_MAX], *av[ATTR_MAX];
    size_t anl[ATTR_MAX], avl[ATTR_MAX];
} tag_t;

static bool tag_is(const tag_t *t, const char *name) { return !strcmp(t->name, name); }

static bool attr(const tag_t *t, const char *name, const char **v, size_t *vl)
{
    size_t n = strlen(name);
    for (int i = 0; i < t->n; i++)
        if (t->anl[i] == n && !strncasecmp(t->an[i], name, n)) { *v = t->av[i]; *vl = t->avl[i]; return true; }
    return false;
}

static bool attr_text(builder_t *b, const tag_t *t, const char *name, char *out, size_t cap)
{
    const char *v;
    size_t vl;
    out[0] = 0;
    if (!attr(t, name, &v, &vl)) return false;
    decode_value(b, v, vl, out, cap, false);
    return true;
}

static bool attr_url(builder_t *b, const tag_t *t, const char *name, char *out, size_t cap)
{
    const char *v;
    size_t vl;
    char raw[VAL_MAX];
    if (!attr(t, name, &v, &vl)) return false;
    decode_value(b, v, vl, raw, sizeof raw, true);
    return cyb_url_resolve(b->base, raw, out, cap);
}

static int attr_int(const tag_t *t, const char *name)
{
    const char *v;
    size_t vl;
    if (!attr(t, name, &v, &vl)) return 0;
    char tmp[12];
    snprintf(tmp, sizeof tmp, "%.*s", (int)(vl < 11 ? vl : 11), v);
    if (strchr(tmp, '%')) return 0;
    return atoi(tmp);
}

/* Parse the tag starting after '<'; returns the end (after '>'). */
static const char *parse_tag(const char *p, const char *end, tag_t *t)
{
    memset(t, 0, sizeof *t);
    if (p < end && *p == '/') { t->close = true; p++; }
    size_t n = 0;
    while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == ':')) {
        if (n + 1 < sizeof t->name) t->name[n++] = (char)tolower((unsigned char)*p);
        p++;
    }
    t->name[n] = 0;
    while (p < end && *p != '>') {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (*p == '/') { t->self_close = true; p++; continue; }
        const char *an = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/') p++;
        size_t anl = (size_t)(p - an);
        while (p < end && isspace((unsigned char)*p)) p++;
        const char *av = "";
        size_t avl = 0;
        if (p < end && *p == '=') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                av = p;
                while (p < end && *p != q) p++;
                avl = (size_t)(p - av);
                if (p < end) p++;
            } else {
                av = p;
                while (p < end && !isspace((unsigned char)*p) && *p != '>') p++;
                avl = (size_t)(p - av);
            }
        }
        if (anl == 0) { if (p < end && *p != '>') p++; continue; }
        if (t->n < ATTR_MAX) {
            t->an[t->n] = an; t->anl[t->n] = anl;
            t->av[t->n] = av; t->avl[t->n] = avl;
            t->n++;
        }
    }
    return p < end ? p + 1 : end;
}

static int add_target(builder_t *b, uint8_t kind, uint32_t href, int field)
{
    cyb_doc_t *d = b->d;
    if (d->oom || !GROW(d, targets, n_targets, cap_targets) || d->n_targets >= 0xfffe) return 0;
    cyb_target_t *t = &d->targets[d->n_targets++];
    t->kind = kind;
    t->href = href;
    t->field = (uint16_t)field;
    return (int)d->n_targets;
}

static int add_field(builder_t *b, uint8_t type, const tag_t *t)
{
    cyb_doc_t *d = b->d;
    if (d->oom || !GROW(d, fields, n_fields, cap_fields)) return -1;
    cyb_field_t *f = &d->fields[d->n_fields];
    memset(f, 0, sizeof *f);
    f->type = type;
    f->form = (int16_t)b->form;
    char tmp[FIELD_VALUE_MAX];
    attr_text(b, t, "name", tmp, sizeof tmp);
    f->name = cyb_doc_str(d, tmp, strlen(tmp));
    if (type == FT_HIDDEN) {
        /* kept as the page's bytes: sent back as they came */
        const char *v;
        size_t vl;
        if (attr(t, "value", &v, &vl)) decode_value(b, v, vl, f->value, sizeof f->value, true);
    } else {
        attr_text(b, t, "value", f->value, sizeof f->value);
    }
    const char *v;
    size_t vl;
    f->checked = attr(t, "checked", &v, &vl);
    f->size = (uint16_t)attr_int(t, "size");
    return (int)d->n_fields++;
}

static void field_item(builder_t *b, int field)
{
    if (field < 0) return;
    flush_gap(b);
    put_space(b);
    close_run(b);
    int tg = add_target(b, T_FIELD, POOL_NONE, field);
    cyb_item_t *it = add_item(b, IT_FIELD);
    if (!it) return;
    it->ref = (int16_t)field;
    it->target = (uint16_t)tg;
    it->style &= (uint8_t)~S_LINK;
    b->line_start = false;
    b->space = true;
}

static void start_form(builder_t *b, const tag_t *t)
{
    cyb_doc_t *d = b->d;
    if (d->oom || !GROW(d, forms, n_forms, cap_forms)) return;
    cyb_form_t *f = &d->forms[d->n_forms];
    char url[1024];
    if (!attr_url(b, t, "action", url, sizeof url)) snprintf(url, sizeof url, "%s", b->base);
    f->action = cyb_doc_str(d, url, strlen(url));
    const char *v;
    size_t vl;
    f->post = attr(t, "method", &v, &vl) && vl == 4 && !strncasecmp(v, "post", 4);
    b->form = (int)d->n_forms++;
}

static void image(builder_t *b, const tag_t *t)
{
    cyb_doc_t *d = b->d;
    char url[1024], alt[128];
    int w = attr_int(t, "width"), h = attr_int(t, "height");
    if ((w > 0 && w <= 2) || (h > 0 && h <= 2)) return;     /* spacers, trackers */
    if (!attr_url(b, t, "src", url, sizeof url) || (strncasecmp(url, "http", 4) && strncasecmp(url, "file:", 5))) url[0] = 0;
    attr_text(b, t, "alt", alt, sizeof alt);
    if (!url[0] && !alt[0]) return;
    if (d->oom || !GROW(d, imgs, n_imgs, cap_imgs)) return;
    cyb_img_t *im = &d->imgs[d->n_imgs];
    memset(im, 0, sizeof *im);
    im->src = url[0] ? cyb_doc_str(d, url, strlen(url)) : POOL_NONE;
    im->alt = cyb_doc_str(d, alt, strlen(alt));
    im->want_w = (uint16_t)(w > 0 ? w : 0);
    im->want_h = (uint16_t)(h > 0 ? h : 0);
    im->state = url[0] ? IMG_WAIT : IMG_SKIP;
    flush_gap(b);
    put_space(b);
    close_run(b);
    cyb_item_t *it = add_item(b, IT_IMG);
    if (!it) return;
    it->ref = (int16_t)d->n_imgs++;
    b->line_start = false;
}

static void list_bullet(builder_t *b)
{
    char tmp[12];
    if (b->lists > 0 && b->list[b->lists - 1] >= 0)
        snprintf(tmp, sizeof tmp, "%d. ", ++b->list[b->lists - 1]);
    else snprintf(tmp, sizeof tmp, "* ");
    flush_gap(b);
    int saved = b->link;
    b->link = 0;
    emit(b, tmp, strlen(tmp));
    close_run(b);
    b->link = (uint16_t)saved;
}

static void indent(builder_t *b, int dir)
{
    if (dir > 0) flush_gap(b);
    cyb_item_t *it = add_item(b, IT_INDENT);
    if (!it) return;
    it->ref = (int16_t)dir;
    b->indent += dir;
}

static void skip_until(builder_t *b, const char *name)
{
    b->skip++;
    snprintf(b->skip_tag, sizeof b->skip_tag, "%s", name);
}

static void capture_end(builder_t *b)
{
    cyb_doc_t *d = b->d;
    b->cap_buf[b->cap_len] = 0;
    if (b->capture == 1 && b->field >= 0) {             /* an <option> */
        cyb_field_t *f = &d->fields[b->field];
        char *label = b->cap_buf;
        while (*label == ' ') label++;
        size_t ll = strlen(label);
        while (ll && label[ll - 1] == ' ') ll--;
        const char *value = b->opt_has_value ? b->opt_value : label;
        uint32_t at = cyb_doc_str(d, value, strlen(value));
        cyb_doc_str(d, label, ll);
        if (!f->n_options) f->options = at;
        if (b->opt_selected) f->chosen = f->n_options;
        f->n_options++;
        b->capture = 0;
        b->cap_len = 0;
        return;
    }
    if (b->capture == 2 && b->field >= 0)               /* <textarea> */
        snprintf(d->fields[b->field].value, FIELD_VALUE_MAX, "%s", b->cap_buf);
    if (b->capture == 3 && b->field >= 0) {             /* <button> */
        char *s = b->cap_buf;
        while (*s == ' ') s++;
        d->fields[b->field].label = cyb_doc_str(d, s, strlen(s));
        field_item(b, b->field);
    }
    b->capture = 0;
    b->cap_len = 0;
}

static void on_tag(builder_t *b, const tag_t *t)
{
    cyb_doc_t *d = b->d;
    const char *n = t->name;
    if (b->skip) {
        if (t->close && !strcmp(n, b->skip_tag) && !--b->skip) b->skip_tag[0] = 0;
        else if (!t->close && !strcmp(n, b->skip_tag) && strcmp(n, "script") && strcmp(n, "style")) b->skip++;
        return;
    }
    /* inside <select>, only options count */
    if (b->in_select && !tag_is(t, "select")) {
        if (tag_is(t, "option")) {
            if (b->capture == 1) capture_end(b);
            if (!t->close) {
                b->capture = 1;
                b->cap_len = 0;
                b->opt_has_value = attr_text(b, t, "value", b->opt_value, sizeof b->opt_value);
                const char *v;
                size_t vl;
                b->opt_selected = attr(t, "selected", &v, &vl);
            }
            return;
        }
        return;
    }
    if (!strcmp(n, "script") || !strcmp(n, "style") || !strcmp(n, "template") ||
        !strcmp(n, "svg") || !strcmp(n, "math") || !strcmp(n, "iframe") || !strcmp(n, "object") ||
        !strcmp(n, "canvas") || !strcmp(n, "video") || !strcmp(n, "audio") || !strcmp(n, "map")) {
        if (!t->close && !t->self_close) skip_until(b, n);
        return;
    }
    if (!strcmp(n, "title")) { b->in_title = !t->close; return; }
    if (!strcmp(n, "base") && !t->close) {
        char url[1024];
        if (attr_url(b, t, "href", url, sizeof url)) snprintf(b->base, sizeof b->base, "%s", url);
        return;
    }
    if (!strcmp(n, "meta") && !t->close) {
        char tmp[256];
        if (attr_text(b, t, "charset", tmp, sizeof tmp)) return;   /* sniffed already */
        attr_text(b, t, "http-equiv", tmp, sizeof tmp);
        if (!strcasecmp(tmp, "refresh")) {
            char c[1024];
            attr_text(b, t, "content", c, sizeof c);
            char *u = strcasestr(c, "url=");
            if (atoi(c) <= 5 && u) {
                u += 4;
                if (*u == '\'' || *u == '"') { u++; u[strcspn(u, "'\"")] = 0; }
                cyb_url_resolve(b->base, u, b->refresh, sizeof b->refresh);
            }
        }
        return;
    }
    if (!strcmp(n, "head") || !strcmp(n, "link")) return;
    if (!strcmp(n, "br")) { close_run(b); flush_gap(b); add_item(b, IT_BREAK); b->line_start = true; b->space = false; return; }
    if (!strcmp(n, "hr")) { block(b, 2); flush_gap(b); add_item(b, IT_HR); b->gap = 1; return; }
    if (!strcmp(n, "img")) { image(b, t); return; }
    if (!strcmp(n, "p") || !strcmp(n, "table") || !strcmp(n, "dl") || !strcmp(n, "figure") ||
        !strcmp(n, "address") || !strcmp(n, "fieldset")) {
        block(b, 2);
        if (!strcmp(n, "table")) b->cells = 0;
        return;
    }
    if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]) {
        block(b, 2);
        if (n[1] <= '2') b->big += t->close ? (b->big ? -1 : 0) : 1;
        else b->bold += t->close ? (b->bold ? -1 : 0) : 1;
        return;
    }
    if (!strcmp(n, "ul") || !strcmp(n, "ol") || !strcmp(n, "menu")) {
        block(b, b->lists ? 1 : 2);
        if (!t->close) {
            if (b->lists < MAX_DEPTH) b->list[b->lists] = n[0] == 'o' ? 0 : -1;
            b->lists++;
            if (b->indent < 3) indent(b, 1);
        } else if (b->lists) {
            b->lists--;
            if (b->indent > 0 && b->lists < 3) indent(b, -1);
        }
        return;
    }
    if (!strcmp(n, "blockquote") || !strcmp(n, "dd")) {
        block(b, !strcmp(n, "dd") ? 1 : 2);
        if (!t->close && b->indent < 3) indent(b, 1);
        else if (t->close && b->indent > 0) indent(b, -1);
        return;
    }
    if (!strcmp(n, "li")) {
        block(b, 1);
        if (!t->close) list_bullet(b);
        return;
    }
    if (!strcmp(n, "pre") || !strcmp(n, "listing") || !strcmp(n, "xmp")) {
        block(b, 2);
        b->pre += t->close ? (b->pre ? -1 : 0) : 1;
        b->pre_start = !t->close;
        return;
    }
    if (!strcmp(n, "center")) { block(b, 1); b->center += t->close ? (b->center ? -1 : 0) : 1; return; }
    if (!strcmp(n, "div") || !strcmp(n, "section") || !strcmp(n, "article") || !strcmp(n, "header") ||
        !strcmp(n, "footer") || !strcmp(n, "nav") || !strcmp(n, "main") || !strcmp(n, "aside") ||
        !strcmp(n, "dt") || !strcmp(n, "figcaption") || !strcmp(n, "details") ||
        !strcmp(n, "summary") || !strcmp(n, "caption") || !strcmp(n, "body") || !strcmp(n, "noscript") ||
        !strcmp(n, "legend") || !strcmp(n, "tbody") || !strcmp(n, "thead") || !strcmp(n, "tfoot")) {
        block(b, 1);
        return;
    }
    if (!strcmp(n, "tr")) { block(b, 1); b->cells = 0; return; }
    if (!strcmp(n, "td") || !strcmp(n, "th")) {
        if (!t->close) {
            if (b->cells++ > 0 && !b->line_start) b->space = true;
            if (n[1] == 'h') b->bold++;
        } else if (n[1] == 'h' && b->bold) b->bold--;
        return;
    }
    if (!strcmp(n, "b") || !strcmp(n, "strong")) { b->bold += t->close ? (b->bold ? -1 : 0) : 1; return; }
    if (!strcmp(n, "a")) {
        close_run(b);
        if (t->close) { b->link = 0; return; }
        char url[1024];
        b->link = 0;
        if (attr_url(b, t, "href", url, sizeof url)) {
            uint32_t at = cyb_doc_str(d, url, strlen(url));
            b->link = (uint16_t)add_target(b, T_LINK, at, 0);
        }
        return;
    }
    if (!strcmp(n, "form")) {
        block(b, 1);
        if (!t->close) start_form(b, t);
        else b->form = -1;
        return;
    }
    if (!strcmp(n, "input") && !t->close) {
        char type[16];
        attr_text(b, t, "type", type, sizeof type);
        uint8_t ft = FT_TEXT;
        if (!strcasecmp(type, "hidden")) ft = FT_HIDDEN;
        else if (!strcasecmp(type, "password")) ft = FT_PASSWORD;
        else if (!strcasecmp(type, "submit") || !strcasecmp(type, "image") || !strcasecmp(type, "button")) ft = FT_SUBMIT;
        else if (!strcasecmp(type, "checkbox")) ft = FT_CHECKBOX;
        else if (!strcasecmp(type, "radio")) ft = FT_RADIO;
        else if (!strcasecmp(type, "reset") || !strcasecmp(type, "file")) return;
        if (ft == FT_SUBMIT && !strcasecmp(type, "button") && b->form < 0) return;
        int f = add_field(b, ft, t);
        if (f < 0) return;
        if (ft == FT_SUBMIT) {
            cyb_field_t *fl = &d->fields[f];
            const char *label = fl->value[0] ? fl->value : "Submit";
            char alt[64];
            if (!strcasecmp(type, "image") && attr_text(b, t, "alt", alt, sizeof alt) && alt[0]) label = alt;
            fl->label = cyb_doc_str(d, label, strlen(label));
            if (!strcasecmp(type, "image")) fl->value[0] = 0;
        }
        if (ft != FT_HIDDEN) field_item(b, f);
        return;
    }
    if (!strcmp(n, "button")) {
        if (t->close) { if (b->capture == 3) capture_end(b); return; }
        char type[16];
        attr_text(b, t, "type", type, sizeof type);
        if (!strcasecmp(type, "button") || !strcasecmp(type, "reset") || b->form < 0) return;
        int f = add_field(b, FT_SUBMIT, t);
        if (f < 0) return;
        b->field = f;
        b->capture = 3;
        b->cap_len = 0;
        return;
    }
    if (!strcmp(n, "textarea")) {
        if (t->close) { if (b->capture == 2) capture_end(b); return; }
        int f = add_field(b, FT_TEXTAREA, t);
        if (f < 0) return;
        b->field = f;
        b->capture = 2;
        b->cap_len = 0;
        field_item(b, f);
        return;
    }
    if (!strcmp(n, "select")) {
        if (t->close) {
            if (b->capture == 1) capture_end(b);
            b->in_select = false;
            if (b->field >= 0 && d->fields[b->field].type == FT_SELECT) {
                if (d->fields[b->field].n_options) field_item(b, b->field);
                b->field = -1;
            }
            return;
        }
        int f = add_field(b, FT_SELECT, t);
        b->field = f;
        b->capture = 0;
        b->in_select = f >= 0;
        return;
    }
}

static void on_text(builder_t *b, const char *s, size_t len)
{
    if (b->skip) return;
    const char *p = s, *end = s + len;
    while (p < end) {
        char out[8];
        int n = next_char(b, &p, end, out, true);
        if (b->in_title) {
            size_t tl = strlen(b->d->title);
            for (int i = 0; i < n && tl + 1 < sizeof b->d->title; i++) {
                char c = out[i] == '\n' || out[i] == '\t' || out[i] == '\r' ? ' ' : out[i];
                if (c == ' ' && (tl == 0 || b->d->title[tl - 1] == ' ')) continue;
                b->d->title[tl++] = c;
                b->d->title[tl] = 0;
            }
            continue;
        }
        if (b->capture) {
            for (int i = 0; i < n; i++) {
                char c = out[i];
                if (b->capture != 2 && (c == '\n' || c == '\t' || c == '\r')) c = ' ';
                if (c == ' ' && b->capture != 2 && b->cap_len && b->cap_buf[b->cap_len - 1] == ' ') continue;
                if (b->cap_len + 1 < sizeof b->cap_buf) b->cap_buf[b->cap_len++] = c;
            }
            continue;
        }
        if (b->in_select) continue;
        emit(b, out, (size_t)n);
    }
}

/* ---- the page's character set ---- */

static int sniff_charset(const char *src, size_t len)
{
    if (len >= 3 && !memcmp(src, "\xef\xbb\xbf", 3)) return CS_UTF8;
    size_t lim = len < 4096 ? len : 4096;
    for (size_t i = 0; i + 8 < lim; i++) {
        if (strncasecmp(src + i, "charset", 7)) continue;
        size_t j = i + 7;
        while (j < lim && (src[j] == ' ' || src[j] == '=' || src[j] == '"' || src[j] == '\'')) j++;
        char name[24];
        size_t k = 0;
        while (j < lim && k + 1 < sizeof name && (isalnum((unsigned char)src[j]) || src[j] == '-' || src[j] == '_'))
            name[k++] = src[j++];
        name[k] = 0;
        int cs = cyb_charset(name);
        if (cs >= 0) return cs;
    }
    /* no label: UTF-8 if every byte over 0x7f is part of a valid sequence */
    size_t bad = 0, good = 0;
    for (size_t i = 0; i < len && i < 65536; i++) {
        uint8_t c = (uint8_t)src[i];
        if (c < 0x80) continue;
        int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
        if (n < 0 || i + (size_t)n >= len) { bad++; continue; }
        bool ok = true;
        for (int k = 1; k <= n; k++) if (((uint8_t)src[i + (size_t)k] & 0xc0) != 0x80) ok = false;
        if (ok) { good++; i += (size_t)n; } else bad++;
    }
    return bad > good / 8 ? CS_LATIN1 : CS_UTF8;
}

void cyb_html_parse(cyb_doc_t *d, const char *src, size_t len, const char *base_url,
                    int cs, bool plain)
{
    builder_t *b = calloc(1, sizeof *b);
    if (!b) { d->oom = true; return; }
    b->d = d;
    b->cs = cs >= 0 ? cs : sniff_charset(src, len);
    d->utf8 = b->cs == CS_UTF8;
    snprintf(b->base, sizeof b->base, "%s", base_url ? base_url : "");
    b->form = -1;
    b->field = -1;
    b->line_start = true;
    if (!d->pool_len) pool_put(d, "", 1);     /* offset 0: the empty string */
    if (plain) {
        b->pre = 1;
        builder_t *pb = b;
        const char *p = src, *end = src + len;
        while (p < end) {
            char out[8];
            int n = next_char(pb, &p, end, out, false);
            emit(pb, out, (size_t)n);
        }
        free(b);
        return;
    }
    const char *p = src, *end = src + len, *text = src;
    while (p < end && !d->oom) {
        if (*p != '<') { p++; continue; }
        /* a tag starts with < and a letter, / or ! */
        char c = p + 1 < end ? p[1] : 0;
        if (!(isalpha((unsigned char)c) || c == '/' || c == '!' || c == '?')) { p++; continue; }
        if (b->skip && c != '/') { p++; continue; }
        if (p > text) on_text(b, text, (size_t)(p - text));
        if (c == '!' && p + 3 < end && p[2] == '-' && p[3] == '-') {
            const char *e = p + 4;
            while (e + 2 < end && !(e[0] == '-' && e[1] == '-' && e[2] == '>')) e++;
            p = text = e + 2 < end ? e + 3 : end;
            continue;
        }
        if (c == '!' || c == '?') {
            const char *e = memchr(p, '>', (size_t)(end - p));
            p = text = e ? e + 1 : end;
            continue;
        }
        tag_t t;
        const char *after = parse_tag(p + 1, end, &t);
        if (b->skip && !(t.close && !strcmp(t.name, b->skip_tag))) { p++; text = p - 1; continue; }
        on_tag(b, &t);
        p = text = after;
    }
    if (p > text && text < end) on_text(b, text, (size_t)(end - text));
    if (b->capture) capture_end(b);
    if (b->refresh[0] && d->n_targets < 3) {
        /* a page that only sends the browser on: its first link */
        uint32_t at = cyb_doc_str(d, b->refresh, strlen(b->refresh));
        builder_t *bb = b;
        bb->link = 0;
        bb->gap = 2;
        flush_gap(bb);
        int tg = add_target(bb, T_LINK, at, 0);
        bb->link = (uint16_t)tg;
        emit(bb, "(continue)", 10);
        close_run(bb);
    }
    free(b);
}
