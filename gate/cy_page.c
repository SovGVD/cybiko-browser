/* Laid-out pages for the Cybiko's Browser application (see cy_page.h). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy_page.h"

#define MAX_TARGETS 250
#define VALUE_MAX 80           /* the longest field value sent */
#define NEXT_ROOM 100          /* kept for the next part's link */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool over;
} out_t;

static void put(out_t *o, const void *s, size_t n)
{
    if (o->len + n > o->cap) { o->over = true; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void u8(out_t *o, unsigned v) { uint8_t b = (uint8_t)v; put(o, &b, 1); }
static void u16(out_t *o, unsigned v) { u8(o, v >> 8); u8(o, v); }

static void str(out_t *o, const char *s, size_t n)
{
    if (n > 255) n = 255;
    u8(o, (unsigned)n);
    put(o, s, n);
}

static void strz(out_t *o, const char *s, size_t most)
{
    size_t n = strlen(s);
    str(o, s, n < most ? n : most);
}

/* A form field's target: what the application needs to show it and send it. */
static void field_target(out_t *o, const cyb_doc_t *d, uint32_t i, const cy_page_links_t *k)
{
    const cyb_field_t *f = &d->fields[i];
    uint32_t var = i;
    if (f->type == FT_RADIO)    /* the group's first button names it */
        for (uint32_t j = 0; j < i; j++)
            if (d->fields[j].type == FT_RADIO && d->fields[j].form == f->form &&
                !strcmp(d->pool + d->fields[j].name, d->pool + f->name)) { var = j; break; }
    u8(o, PT_FIELD);
    u8(o, f->type);
    u8(o, f->form < 0 ? 255 : (unsigned)f->form);
    u8(o, f->checked ? 1 : 0);
    u16(o, var);
    if (f->type == FT_SUBMIT || f->type == FT_RESET) {
        char caption[FIELD_VALUE_MAX];
        cyb_field_text(d, f, caption, sizeof caption);
        strz(o, caption, 40);
    } else if (f->type == FT_CHECKBOX) {
        strz(o, *f->value ? f->value : "on", VALUE_MAX);
    } else {
        strz(o, f->value, VALUE_MAX);
    }
    if (f->type == FT_SELECT) {
        const char *p = d->pool + f->options;
        unsigned n = f->n_options > 40 ? 40 : f->n_options;
        u8(o, n);
        u8(o, f->chosen < n ? f->chosen : 0);
        for (unsigned j = 0; j < n; j++) {
            const char *label = p + strlen(p) + 1;
            strz(o, p, VALUE_MAX);
            strz(o, *label ? label : p, 40);
            p = label + strlen(label) + 1;
        }
    } else if (f->type == FT_SUBMIT) {
        char url[160] = "";
        if (f->form >= 0) k->form(k->user, d, f->form, (int)i, url, sizeof url);
        strz(o, url, 255);
    }
}

static void target(out_t *o, const cyb_doc_t *d, uint16_t t, const cy_page_links_t *k)
{
    const cyb_target_t *tg = &d->targets[t - 1];
    if (tg->kind == T_FIELD && tg->field < d->n_fields) {
        field_target(o, d, tg->field, k);
        return;
    }
    char url[160];
    k->link(k->user, d->pool + tg->href, url, sizeof url);
    u8(o, PT_LINK);
    strz(o, url, 255);
}

/* A box's text: B_TEXT's own, B_ALT's picture's in brackets. */
static void box_text(out_t *o, const cyb_doc_t *d, const cyb_box_t *b)
{
    if (b->kind == B_TEXT) {
        str(o, d->pool + b->text, b->len);
        return;
    }
    const char *alt = b->ref >= 0 && (uint32_t)b->ref < d->n_imgs ? d->pool + d->imgs[b->ref].alt : "";
    char tmp[64];
    int n = snprintf(tmp, sizeof tmp, "[%s]", *alt ? alt : "img");
    str(o, tmp, n < (int)sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void box(out_t *o, const cyb_doc_t *d, const cyb_box_t *b, int32_t top, uint8_t t)
{
    static const uint8_t kinds[] = { [B_TEXT] = PB_TEXT, [B_IMG] = PB_IMG, [B_FIELD] = PB_FIELD,
                                     [B_HR] = PB_HR, [B_ALT] = PB_ALT };
    int x = b->x < 0 ? 0 : b->x > 255 ? 255 : b->x;
    const cyb_pic_t *pic = NULL;
    if (b->kind == B_IMG && b->ref >= 0 && (uint32_t)b->ref < d->n_imgs && d->imgs[b->ref].state == IMG_DONE &&
        d->imgs[b->ref].pic.pix && d->imgs[b->ref].pic.w == b->w && d->imgs[b->ref].pic.h == b->h && b->w <= 255)
        pic = &d->imgs[b->ref].pic;
    u8(o, (unsigned)(pic ? PB_PIC : kinds[b->kind]) << 4 | (b->kind == B_ALT ? F_SMALL : b->font));
    u8(o, (unsigned)x);
    u16(o, (unsigned)(b->y - top));
    u8(o, b->w > 255 ? 255 : b->w);
    u8(o, b->h);
    u8(o, b->style);
    u8(o, t);
    if (b->kind == B_TEXT || b->kind == B_ALT) box_text(o, d, b);
    if (pic) put(o, pic->pix, (size_t)(pic->w + 3) / 4 * pic->h);
}

static bool blank(const cyb_doc_t *d, const cyb_box_t *b)
{
    for (uint16_t i = 0; i < b->len; i++)
        if (d->pool[b->text + i] != ' ') return false;
    return true;
}

/* A text box that goes on where the box just written (at `last`) ends, in
 * the same font, style and target, joins it. */
static bool join(out_t *o, size_t last, const cyb_doc_t *d, const cyb_box_t *b, int32_t from, uint8_t t)
{
    uint8_t *p = o->p + last;
    if (b->kind != B_TEXT || p[0] != (PB_TEXT << 4 | b->font) || p[6] != b->style || p[7] != t ||
        p[1] + p[4] != b->x || (p[2] << 8 | p[3]) != b->y - from || p[4] + b->w > 255 || p[8] + b->len > 255)
        return false;
    put(o, d->pool + b->text, b->len);
    if (o->over) return true;
    p[4] = (uint8_t)(p[4] + b->w);
    p[8] = (uint8_t)(p[8] + b->len);
    return true;
}

/* The part being written: its boxes in out, its targets in table (the
 * page's target behind each, from part target 2 on). */
typedef struct {
    out_t o, table;
    size_t room;               /* for both together */
    uint16_t doc_target[MAX_TARGETS];
    unsigned n_targets;        /* (target 1 is the next part's link) */
    unsigned n_boxes;
} part_t;

/* The part target for page target t, added to the table if new; 0 when
 * there is no room for it. */
static uint8_t part_target(part_t *pt, const cyb_doc_t *d, uint16_t t, const cy_page_links_t *k)
{
    unsigned j;
    for (j = 1; j < pt->n_targets && pt->doc_target[j] != t; j++) ;
    if (j < pt->n_targets) return (uint8_t)(j + 1);
    if (j == MAX_TARGETS) return 0;
    out_t keep = pt->table;
    target(&pt->table, d, t, k);
    if (pt->table.over || pt->o.len + pt->table.len > pt->room) { pt->table = keep; return 0; }
    pt->doc_target[pt->n_targets++] = t;
    return (uint8_t)(j + 1);
}

/* Writes the boxes from y = from up to (not at) end; returns where it had
 * to stop: end, or the line of the box that did not fit. */
static int32_t boxes(part_t *pt, const cyb_doc_t *d, const cyb_layout_t *l, int32_t from, int32_t end,
                     const cy_page_links_t *k)
{
    out_t *o = &pt->o;
    size_t last = 0;            /* the last text box written, 0 none */
    for (uint32_t i = cyb_layout_find(l, from); i < l->n; i++) {
        const cyb_box_t *b = &l->boxes[i];
        if (b->y < from) continue;
        if (b->y >= end) break;
        const int32_t cut = b->y > from ? b->y : b->y + 1;
        if (b->y - from > 65000) return cut;
        uint8_t t = 0;
        if (b->target && b->target <= d->n_targets && !(t = part_target(pt, d, b->target, k))) return cut;
        out_t keep = *o;
        o->cap = pt->room - pt->table.len;
        if (last && join(o, last, d, b, from, t)) {
            if (o->over) { *o = keep; return cut; }
            continue;
        }
        if (b->kind == B_TEXT && !(b->style & S_LINK) && blank(d, b)) continue;
        if (pt->n_boxes == CY_PAGE_BOXES) return cut;
        pt->n_boxes++;
        box(o, d, b, from, t);
        if (o->over) { *o = keep; return cut; }
        last = b->kind == B_TEXT ? keep.len : 0;
    }
    return end;
}

size_t cy_page_part(const cyb_doc_t *d, const cyb_layout_t *l, int32_t *top, uint8_t *out, size_t cap,
                    const cy_page_links_t *k)
{
    part_t *pt = calloc(1, sizeof *pt);
    uint8_t *table = malloc(cap);
    if (!pt || !table || cap < 300) { free(pt); free(table); return 0; }
    pt->o = (out_t){ .p = out, .cap = cap };
    pt->table = (out_t){ .p = table, .cap = cap };
    pt->room = cap - NEXT_ROOM;
    pt->n_targets = 1;
    out_t *o = &pt->o;
    const int32_t from = *top;
    put(o, "CB\1", 3);
    u8(o, 0);
    u16(o, 0);                  /* the height and the table, filled in below */
    u16(o, 0);
    strz(o, d->title[0] ? d->title : "Page", 40);
    const out_t head = *o;
    int32_t end = boxes(pt, d, l, from, INT32_MAX, k);
    if (end != INT32_MAX) {
        /* the cut line's boxes that fitted go in the next part instead (the
         * targets stay numbered as they are) */
        *o = head;
        pt->n_boxes = 0;
        boxes(pt, d, l, from, end, k);
    }
    const bool more = end < l->height;
    o->cap = cap;
    const size_t at = o->len;
    char url[NEXT_ROOM - 4] = "";
    if (more) k->more(k->user, end, url, sizeof url);
    u8(o, pt->n_targets);
    u8(o, PT_LINK);
    strz(o, url, 255);
    put(o, table, pt->table.len);
    int32_t h = (more ? end : l->height) - from;
    if (h < 0) h = 0;
    if (h > 65535) h = 65535;
    out[3] = more ? 1 : 0;
    out[4] = (uint8_t)(h >> 8);
    out[5] = (uint8_t)h;
    out[6] = (uint8_t)(at >> 8);
    out[7] = (uint8_t)at;
    *top = more ? end : l->height;
    size_t len = o->over ? 0 : o->len;
    free(table);
    free(pt);
    return len;
}
