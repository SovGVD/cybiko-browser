/* The page laid out in lines for the 156-dot wide text area, and drawn. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cyb_internal.h"

#define INDENT_W 8
#define LINE_GAP 1     /* between lines */
#define PARA_GAP 4     /* between paragraphs */
#define MAX_INDENT 4

typedef struct {
    cyb_layout_t *l;
    const cyb_doc_t *d;
    const cyb_font_t *fonts;
    int32_t y;             /* top of the current line */
    int x, left;
    uint32_t line;         /* first box of the line */
    int indent;
    int pending_indent;
    bool center;
    int last_gap;          /* the gap after the last line, to not double it */
} lay_t;

static int font_of(const lay_t *s, uint8_t style)
{
    if (style & S_BIG) return F_BIG_BOLD;
    if (s->l->big) return style & S_BOLD ? F_BIG_BOLD : F_BIG;
    return style & S_BOLD ? F_SMALL_BOLD : F_SMALL;
}

static int body_h(const lay_t *s) { return s->fonts[s->l->big ? F_BIG : F_SMALL].height; }

static cyb_box_t *add_box(lay_t *s, uint8_t kind, int w, int h)
{
    cyb_layout_t *l = s->l;
    if (l->n == l->cap) {
        uint32_t n = l->cap ? l->cap * 2 : 256;
        cyb_box_t *b = realloc(l->boxes, n * sizeof *b);
        if (!b) return NULL;
        l->boxes = b;
        l->cap = n;
    }
    cyb_box_t *b = &l->boxes[l->n++];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->x = (int16_t)s->x;
    b->w = (uint16_t)w;
    b->h = (uint8_t)(h > 255 ? 255 : h);
    b->ref = -1;
    s->x += w;
    return b;
}

/* Ends the line: lines its boxes up at the bottom and moves y down. An empty
 * line takes `empty` dots. */
static void end_line(lay_t *s, int empty)
{
    cyb_layout_t *l = s->l;
    int h = 0;
    for (uint32_t i = s->line; i < l->n; i++) if (l->boxes[i].h > h) h = l->boxes[i].h;
    if (s->line == l->n) h = empty;
    else {
        int shift = 0;
        if (s->center) {
            int right = 0;
            for (uint32_t i = s->line; i < l->n; i++) {
                int r = l->boxes[i].x + l->boxes[i].w;
                if (r > right) right = r;
            }
            shift = (PAGE_W - right) / 2;
            if (shift < 0) shift = 0;
        }
        for (uint32_t i = s->line; i < l->n; i++) {
            cyb_box_t *b = &l->boxes[i];
            b->y = s->y + h - b->h;
            b->x = (int16_t)(b->x + shift);
        }
    }
    if (h) {
        s->y += h + LINE_GAP;
        s->last_gap = 0;
    }
    s->line = l->n;
    if (s->pending_indent) {
        s->indent += s->pending_indent;
        if (s->indent < 0) s->indent = 0;
        s->pending_indent = 0;
    }
    s->left = (s->indent > MAX_INDENT ? MAX_INDENT : s->indent) * INDENT_W;
    s->x = s->left;
    s->center = false;
}

static bool line_empty(const lay_t *s) { return s->line == s->l->n; }

/* Room for w more dots on this line? Starts a new one if not. */
static void need(lay_t *s, int w)
{
    if (s->x + w > PAGE_W && !line_empty(s)) end_line(s, 0);
}

static void gap(lay_t *s, int g)
{
    if (!line_empty(s)) end_line(s, 0);
    if (s->y > 0 && g > s->last_gap) {
        s->y += g - s->last_gap;
        s->last_gap = g;
    }
}

static void text_run(lay_t *s, const cyb_item_t *it, uint32_t item)
{
    const cyb_doc_t *d = s->d;
    int fi = font_of(s, it->style);
    const cyb_font_t *f = &s->fonts[fi];
    const char *t = d->pool + it->text;
    uint32_t n = it->len, i = 0;
    int h = f->height + ((it->style & S_LINK) ? 1 : 0);
    if (line_empty(s) && (it->style & S_CENTER)) s->center = true;
    while (i < n) {
        /* a word and the spaces after it */
        uint32_t j = i;
        if (!(it->style & S_PRE)) while (j < n && t[j] == ' ' && line_empty(s) && s->x == s->left) { j++; i++; }
        while (j < n && t[j] != ' ') j++;
        uint32_t word_end = j;
        while (j < n && t[j] == ' ') j++;
        if (word_end == i && j == i) break;
        int ww = cyb_text_w(f, t + i, (int)(word_end - i));
        int sw = cyb_text_w(f, t + word_end, (int)(j - word_end));
        need(s, ww);
        if (s->x + ww > PAGE_W) {
            /* longer than a line: as many characters as fit */
            uint32_t k = i;
            int w = 0;
            while (k < word_end && s->x + w + cyb_char_w(f, (uint8_t)t[k]) <= PAGE_W) w += cyb_char_w(f, (uint8_t)t[k++]);
            if (k == i) { w = cyb_char_w(f, (uint8_t)t[k]); k++; }
            cyb_box_t *b = add_box(s, B_TEXT, w, h);
            if (!b) return;
            b->text = it->text + i; b->len = (uint16_t)(k - i); b->font = (uint8_t)fi;
            b->style = it->style; b->target = it->target; b->item = item;
            end_line(s, 0);
            i = k;
            continue;
        }
        if (s->x + ww + sw > PAGE_W) { sw = 0; j = word_end; while (j < n && t[j] == ' ') j++; }
        cyb_layout_t *l = s->l;
        cyb_box_t *prev = l->n > s->line ? &l->boxes[l->n - 1] : NULL;
        if (prev && prev->kind == B_TEXT && prev->item == item && prev->text + prev->len == it->text + i &&
            prev->x + prev->w == s->x && prev->len + (j - i) < 60000) {
            /* the same run goes on: one box */
            prev->w = (uint16_t)(prev->w + ww + sw);
            prev->len = (uint16_t)(prev->len + (sw ? j - i : word_end - i));
            s->x += ww + sw;
        } else {
            cyb_box_t *b = add_box(s, B_TEXT, ww + sw, h);
            if (!b) return;
            b->text = it->text + i;
            b->len = (uint16_t)(sw ? j - i : word_end - i);
            b->font = (uint8_t)fi; b->style = it->style; b->target = it->target; b->item = item;
        }
        i = j;
    }
}

static void field_size(const lay_t *s, const cyb_field_t *f, int *w, int *h)
{
    const cyb_font_t *font = &s->fonts[s->l->big ? F_BIG : F_SMALL];
    char text[FIELD_VALUE_MAX];
    *h = font->height + 3;
    switch (f->type) {
    case FT_CHECKBOX:
    case FT_RADIO:
        *w = 9; *h = 9;
        break;
    case FT_SUBMIT:
        cyb_field_text(s->d, f, text, sizeof text);
        *w = cyb_text_w(font, text, (int)strlen(text)) + 8;
        break;
    case FT_SELECT: {
        int widest = 20;
        const char *o = s->d->pool + f->options;
        for (int i = 0; i < f->n_options; i++) {
            o += strlen(o) + 1;                  /* the value, then the label */
            int lw = cyb_text_w(font, o, (int)strlen(o));
            if (lw > widest) widest = lw;
            o += strlen(o) + 1;
        }
        *w = widest + 12;
        break;
    }
    case FT_TEXTAREA:
        *w = PAGE_W - s->left;
        *h = font->height * 2 + 3;
        break;
    default:
        *w = f->size ? f->size * 5 + 6 : 80;
        if (*w < 30) *w = 30;
        break;
    }
    if (*w > PAGE_W - s->left) *w = PAGE_W - s->left;
}

static void picture(lay_t *s, const cyb_item_t *it, uint32_t item)
{
    const cyb_img_t *im = &s->d->imgs[it->ref];
    int room = PAGE_W - s->left;
    int w, h;
    uint8_t kind = B_IMG;
    if (im->state == IMG_DONE && im->pic.pix) {
        w = im->pic.w;
        h = im->pic.h;
    } else if (im->want_w && im->want_h && im->state != IMG_FAILED && im->state != IMG_SKIP) {
        w = im->want_w;
        h = im->want_h;
        if (w > room) { h = h * room / w; w = room; }
        if (h > CYB_IMG_MAX_H) { w = w * CYB_IMG_MAX_H / h; h = CYB_IMG_MAX_H; }
        if (w < 4) w = 4;
        if (h < 4) h = 4;
    } else {
        /* the alternative text in brackets */
        const char *alt = s->d->pool + im->alt;
        if (!*alt) { if (im->state == IMG_FAILED || im->state == IMG_SKIP) return; alt = "img"; }
        const cyb_font_t *f = &s->fonts[F_SMALL];
        char tmp[64];
        int n = snprintf(tmp, sizeof tmp, "[%s]", alt);
        if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
        w = cyb_text_w(f, tmp, n) + 2;
        if (w > room) w = room;
        h = f->height + 1;
        kind = B_ALT;
    }
    need(s, w);
    cyb_box_t *b = add_box(s, kind, w, h);
    if (!b) return;
    b->ref = it->ref;
    b->target = it->target;
    b->style = it->style;
    b->item = item;
}

void cyb_layout(cyb_layout_t *l, const cyb_doc_t *d, const cyb_font_t *fonts, bool big)
{
    l->n = 0;
    l->big = big;
    lay_t s = { .l = l, .d = d, .fonts = fonts, .last_gap = PARA_GAP };
    for (uint32_t i = 0; i < d->n_items; i++) {
        const cyb_item_t *it = &d->items[i];
        switch (it->kind) {
        case IT_TEXT:
            text_run(&s, it, i);
            break;
        case IT_BREAK:
            end_line(&s, body_h(&s));
            break;
        case IT_PARA:
            gap(&s, PARA_GAP);
            break;
        case IT_HR: {
            gap(&s, 1);
            cyb_box_t *b = add_box(&s, B_HR, PAGE_W - s.left, 5);
            if (b) b->item = i;
            end_line(&s, 0);
            break;
        }
        case IT_IMG:
            picture(&s, it, i);
            break;
        case IT_FIELD: {
            const cyb_field_t *f = &d->fields[it->ref];
            int w, h;
            field_size(&s, f, &w, &h);
            if (f->type == FT_TEXTAREA && !line_empty(&s)) end_line(&s, 0);
            need(&s, w);
            cyb_box_t *b = add_box(&s, B_FIELD, w, h);
            if (!b) break;
            b->ref = it->ref;
            b->target = it->target;
            b->item = i;
            s.x += 3;
            if (f->type == FT_TEXTAREA) end_line(&s, 0);
            break;
        }
        case IT_INDENT:
            if (line_empty(&s)) {
                s.indent += it->ref;
                if (s.indent < 0) s.indent = 0;
                s.left = (s.indent > MAX_INDENT ? MAX_INDENT : s.indent) * INDENT_W;
                s.x = s.left;
            } else s.pending_indent += it->ref;
            break;
        }
    }
    end_line(&s, 0);
    l->height = s.y + 2;
}

void cyb_layout_free(cyb_layout_t *l)
{
    free(l->boxes);
    memset(l, 0, sizeof *l);
}

uint32_t cyb_layout_find(const cyb_layout_t *l, int32_t y)
{
    uint32_t lo = 0, hi = l->n;
    while (lo < hi) {             /* boxes are in line order */
        uint32_t mid = (lo + hi) / 2;
        if (l->boxes[mid].y + l->boxes[mid].h + 16 <= y) lo = mid + 1;
        else hi = mid;
    }
    while (lo < l->n && l->boxes[lo].y + l->boxes[lo].h <= y) lo++;
    return lo;
}

/* ---- drawing ---- */

void cyb_field_text(const cyb_doc_t *d, const cyb_field_t *f, char *out, size_t cap)
{
    out[0] = 0;
    switch (f->type) {
    case FT_PASSWORD: {
        size_t n = strlen(f->value);
        if (n >= cap) n = cap - 1;
        memset(out, '*', n);
        out[n] = 0;
        break;
    }
    case FT_SUBMIT:
        snprintf(out, cap, "%s", d->pool + f->label);
        break;
    case FT_SELECT: {
        const char *o = d->pool + f->options;
        for (int i = 0; i < f->n_options; i++) {
            o += strlen(o) + 1;
            if (i == f->chosen) { snprintf(out, cap, "%s", o); break; }
            o += strlen(o) + 1;
        }
        break;
    }
    case FT_CHECKBOX:
    case FT_RADIO:
        break;
    default:
        snprintf(out, cap, "%s", f->value);
        for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    }
}

static void draw_field(uint8_t *fb, const cyb_box_t *b, const cyb_doc_t *d, const cyb_font_t *fonts,
                       bool big, int x, int y, int y0, int y1)
{
    const cyb_field_t *f = &d->fields[b->ref];
    const cyb_font_t *font = &fonts[big ? F_BIG : F_SMALL];
    char text[FIELD_VALUE_MAX];
    cyb_field_text(d, f, text, sizeof text);
    int w = b->w, h = b->h;
    /* the outline, clipped by hand to the view */
#define CLIPFILL(X, Y, W, H, C) do { int yy_ = (Y), hh_ = (H); \
        if (yy_ < y0) { hh_ -= y0 - yy_; yy_ = y0; } if (yy_ + hh_ > y1) hh_ = y1 - yy_; \
        if (hh_ > 0) fb_fill(fb, (X), yy_, (W), hh_, (C)); } while (0)
    switch (f->type) {
    case FT_CHECKBOX:
    case FT_RADIO:
        CLIPFILL(x, y, w, 1, BLACK); CLIPFILL(x, y + h - 1, w, 1, BLACK);
        CLIPFILL(x, y, 1, h, BLACK); CLIPFILL(x + w - 1, y, 1, h, BLACK);
        if (f->type == FT_RADIO) {   /* round off the corners */
            CLIPFILL(x, y, 1, 1, WHITE); CLIPFILL(x + w - 1, y, 1, 1, WHITE);
            CLIPFILL(x, y + h - 1, 1, 1, WHITE); CLIPFILL(x + w - 1, y + h - 1, 1, 1, WHITE);
        }
        if (f->checked) CLIPFILL(x + 2, y + 2, w - 4, h - 4, BLACK);
        return;
    case FT_SUBMIT:
        CLIPFILL(x + 1, y, w - 2, h, LIGHT);
        CLIPFILL(x + 1, y, w - 2, 1, BLACK); CLIPFILL(x + 1, y + h - 1, w - 2, 1, BLACK);
        CLIPFILL(x, y + 1, 1, h - 2, BLACK); CLIPFILL(x + w - 1, y + 1, 1, h - 2, BLACK);
        cyb_text(fb, font, x + 4, y + 2, text, (int)strlen(text), BLACK, y0, y1);
        return;
    default:
        break;
    }
    CLIPFILL(x, y, w, 1, BLACK); CLIPFILL(x, y + h - 1, w, 1, BLACK);
    CLIPFILL(x, y, 1, h, BLACK); CLIPFILL(x + w - 1, y, 1, h, BLACK);
    int room = w - 4;
    if (f->type == FT_SELECT) {
        room -= 8;
        int ax = x + w - 8, ay = y + h / 2 - 1;
        CLIPFILL(ax - 2, y + 1, 1, h - 2, DARK);
        CLIPFILL(ax, ay, 5, 1, BLACK); CLIPFILL(ax + 1, ay + 1, 3, 1, BLACK); CLIPFILL(ax + 2, ay + 2, 1, 1, BLACK);
    }
    /* as much of the text as fits; text fields show its end */
    int len = (int)strlen(text);
    const char *s = text;
    if (f->type != FT_SELECT)
        while (len && cyb_text_w(font, s, len) > room) { s++; len--; }
    else
        while (len && cyb_text_w(font, s, len) > room) len--;
    cyb_text(fb, font, x + 2, y + 2, s, len, BLACK, y0, y1);
#undef CLIPFILL
}

void cyb_draw_page(uint8_t *fb, const cyb_layout_t *l, const cyb_doc_t *d,
                   const cyb_font_t *fonts, int32_t top, uint16_t focus)
{
    int y0 = VIEW_Y, y1 = CYB_H;
    fb_fill(fb, 0, y0, CYB_W, y1 - y0, WHITE);
    for (uint32_t i = cyb_layout_find(l, top); i < l->n; i++) {
        const cyb_box_t *b = &l->boxes[i];
        int y = (int)(b->y - top) + y0;
        if (y >= y1 + 16) break;
        if (y >= y1) continue;
        int x = b->x + 1;
        switch (b->kind) {
        case B_TEXT: {
            const cyb_font_t *f = &fonts[b->font];
            cyb_text(fb, f, x, y, d->pool + b->text, b->len, BLACK, y0, y1);
            if (b->style & S_LINK) {
                int uy = y + f->height;
                /* not under the trailing space */
                int len = b->len;
                while (len && d->pool[b->text + (uint32_t)len - 1] == ' ') len--;
                int uw = cyb_text_w(f, d->pool + b->text, len);
                if (uy >= y0 && uy < y1) fb_fill(fb, x, uy, uw, 1, DARK);
            }
            break;
        }
        case B_IMG: {
            const cyb_img_t *im = &d->imgs[b->ref];
            if (im->state == IMG_DONE && im->pic.pix)
                fb_image(fb, x, y, im->pic.w, im->pic.h, im->pic.pix, y0, y1);
            else {
                for (int j = 0; j < b->h; j++) {
                    int yy = y + j;
                    if (yy < y0 || yy >= y1) continue;
                    if (j == 0 || j == b->h - 1) fb_fill(fb, x, yy, b->w, 1, LIGHT);
                    else { fb_pixel(fb, x, yy, LIGHT); fb_pixel(fb, x + b->w - 1, yy, LIGHT); }
                }
                const char *alt = d->pool + im->alt;
                if (*alt && b->h > fonts[F_SMALL].height + 2) {
                    int n = (int)strlen(alt);
                    while (n && cyb_text_w(&fonts[F_SMALL], alt, n) > b->w - 4) n--;
                    int cy = y + 2 > y0 ? y + 2 : y0;
                    cyb_text(fb, &fonts[F_SMALL], x + 2, y + 2, alt, n, DARK, cy, y + b->h - 1 < y1 ? y + b->h - 1 : y1);
                }
            }
            if (b->target) {     /* a picture link: a frame under it */
                int uy = y + b->h - 1;
                if (uy >= y0 && uy < y1 && !(im->state == IMG_DONE)) fb_fill(fb, x, uy, b->w, 1, DARK);
            }
            break;
        }
        case B_ALT: {
            const cyb_img_t *im = &d->imgs[b->ref];
            const char *alt = d->pool + im->alt;
            char tmp[64];
            int n = snprintf(tmp, sizeof tmp, "[%s]", *alt ? alt : "img");
            if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
            while (n > 2 && cyb_text_w(&fonts[F_SMALL], tmp, n) > b->w) n--;
            cyb_text(fb, &fonts[F_SMALL], x, y, tmp, n, b->target ? BLACK : DARK, y0, y1);
            break;
        }
        case B_HR:
            if (y + 2 >= y0 && y + 2 < y1) fb_fill(fb, x, y + 2, b->w, 1, DARK);
            break;
        case B_FIELD:
            draw_field(fb, b, d, fonts, l->big, x, y, y0, y1);
            break;
        }
        if (focus && b->target == focus && b->kind != B_FIELD) {
            int fy = y, fh = b->h;
            if (fy < y0) { fh -= y0 - fy; fy = y0; }
            if (fy + fh > y1) fh = y1 - fy;
            if (fh > 0) fb_invert(fb, x, fy, b->w, fh);
        } else if (focus && b->target == focus) {
            /* fields: a second frame around them */
            for (int j = -1; j <= b->h; j++) {
                int yy = y + j;
                if (yy < y0 || yy >= y1) continue;
                if (j == -1 || j == b->h) fb_fill(fb, x - 1, yy, b->w + 2, 1, BLACK);
                else { fb_pixel(fb, x - 1, yy, BLACK); fb_pixel(fb, x + b->w, yy, BLACK); }
            }
        }
    }
    /* the scroll bar */
    int sx = CYB_W - SCROLL_W;
    fb_fill(fb, sx, y0, SCROLL_W, y1 - y0, LIGHT);
    int vh = y1 - y0;
    if (l->height > vh) {
        int th = (int)((int64_t)vh * vh / l->height);
        if (th < 4) th = 4;
        int ty = (int)((int64_t)(vh - th) * top / (l->height - vh));
        if (ty > vh - th) ty = vh - th;
        fb_fill(fb, sx, y0 + ty, SCROLL_W, th, BLACK);
    } else fb_fill(fb, sx, y0, SCROLL_W, vh, BLACK);
}
