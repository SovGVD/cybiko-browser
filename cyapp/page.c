/* A page part from the gate: read, drawn, scrolled, its links focused. */
#include "app.h"

struct page pg;
long top;
u8 focus;

static u16 g16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

void page_free(void)
{
    u16 i;
    for (i = 0; i < pg.n_targets; i++)
        if (pg.t[i].value) cy_free(pg.t[i].value);
    if (pg.data) cy_free(pg.data);
    pg.data = 0;
    pg.len = 0;
    pg.n_boxes = 0;
    pg.n_targets = 0;
    pg.height = 0;
}

/* Reads the target table at p; 0 if it runs past end. */
static u8 targets(const u8 *p, const u8 *end)
{
    u16 n = *p++, i, j;
    for (i = 0; i < n && i < MAX_TARGETS; i++) {
        struct target *t = &pg.t[i];
        if (p >= end) return 0;
        t->kind = *p++;
        t->value = 0;
        t->url = 0;
        t->n_options = 0;
        if (t->kind == PT_LINK) {
            t->url = p;
            p += 1 + *p;
        } else {
            if (p + 7 > end) return 0;
            t->type = p[0];
            t->form = p[1];
            t->checked = p[2] & 1;
            t->var = g16(p + 3);
            t->value = cy_malloc(VALUE_MAX + 1);
            if (t->value) {
                u8 k, l = p[5];
                if (l > VALUE_MAX) l = VALUE_MAX;
                for (k = 0; k < l; k++) t->value[k] = (char)p[6 + k];
                t->value[l] = 0;
            }
            p += 6 + p[5];
            if (t->type == FT_SELECT) {
                t->n_options = p[0];
                t->chosen = p[1];
                t->options = p + 2;
                p += 2;
                for (j = 0; j < t->n_options; j++) { p += 1 + *p; p += 1 + *p; }
            } else if (t->type == FT_SUBMIT) {
                t->url = p;
                p += 1 + *p;
            }
        }
        pg.n_targets = i + 1;
        if (p > end) return 0;
    }
    return 1;
}

u8 page_set(u8 *data, u16 len)
{
    u16 at, table;
    u8 n;
    page_free();
    pg.data = data;
    pg.len = len;
    if (len < 10 || data[0] != 'C' || data[1] != 'B' || data[2] != 1) return 0;
    pg.more = data[3] & 1;
    pg.height = g16(data + 4);
    table = g16(data + 6);
    n = data[8] > 40 ? 40 : data[8];
    for (at = 0; at < n; at++) pg.title[at] = (char)data[9 + at];
    pg.title[n] = 0;
    if (table > len) return 0;
    at = 9 + data[8];
    while (at + 8 <= table && pg.n_boxes < MAX_BOXES) {
        u8 kind = data[at] >> 4;
        pg.box[pg.n_boxes++] = at;
        at += 8;
        if (kind == PB_TEXT || kind == PB_ALT) at += 1 + data[at];
        else if (kind == PB_PIC) at += (u16)((data[at - 4] + 3) / 4) * data[at - 3];
    }
    if (!targets(data + table, data + len)) return 0;
    if (pg.more) pg.height += 16;
    return 1;
}

/* Box i; the one after the last is the More line of a page with more. */
void page_box(u16 i, struct box *b)
{
    if (i >= pg.n_boxes) {
        b->kind = PB_MORE;
        b->font = 1;
        b->x = 0;
        b->y = pg.height - 14;
        b->w = 44;
        b->h = 10;
        b->style = S_LINK;
        b->target = 1;
        b->text = "Next part";
        b->len = 9;
        return;
    }
    {
        const u8 *p = pg.data + pg.box[i];
        b->kind = p[0] >> 4;
        b->font = p[0] & 15;
        b->x = p[1];
        b->y = g16(p + 2);
        b->w = p[4];
        b->h = p[5];
        b->style = p[6];
        b->target = p[7];
        b->len = 0;
        b->text = "";
        b->pix = p + 8;
        if (b->kind == PB_TEXT || b->kind == PB_ALT) {
            b->len = p[8];
            b->text = (const char *)p + 9;
        }
    }
}

static u16 n_all(void) { return pg.n_boxes + (pg.more ? 1 : 0); }

void field_text(const struct target *t, char *out, int cap)
{
    int i;
    out[0] = 0;
    switch (t->type) {
    case FT_CHECKBOX:
    case FT_RADIO:
        return;
    case FT_PASSWORD:
        for (i = 0; t->value && t->value[i] && i < cap - 1; i++) out[i] = '*';
        out[i] = 0;
        return;
    case FT_SELECT: {
        const u8 *p = t->options;
        u8 k;
        for (k = 0; k < t->n_options; k++) {
            const u8 *label = p + 1 + *p;
            if (k == t->chosen) {
                int n = *label < cap - 1 ? *label : cap - 1;
                for (i = 0; i < n; i++) out[i] = (char)label[1 + i];
                out[n] = 0;
                return;
            }
            p = label + 1 + *label;
        }
        return;
    }
    default:
        if (t->value) scpy(out, t->value, cap);
    }
}

static void draw_field(const struct box *b, int x, int y)
{
    const struct target *t = b->target ? &pg.t[b->target - 1] : 0;
    int w = b->w, h = b->h, f = big ? 2 : 0, room, len;
    char s[VALUE_MAX + 1];
    const char *p = s;
    if (!t || t->kind != PT_FIELD) { fb_frame(x, y, w, h, LIGHT); return; }
    field_text(t, s, sizeof s);
    switch (t->type) {
    case FT_CHECKBOX:
    case FT_RADIO:
        fb_frame(x, y, w, h, BLACK);
        if (t->type == FT_RADIO) {
            fb_fill(x, y, 1, 1, WHITE); fb_fill(x + w - 1, y, 1, 1, WHITE);
            fb_fill(x, y + h - 1, 1, 1, WHITE); fb_fill(x + w - 1, y + h - 1, 1, 1, WHITE);
        }
        if (t->checked) fb_fill(x + 2, y + 2, w - 4, h - 4, BLACK);
        return;
    case FT_SUBMIT:
    case FT_RESET:
        fb_fill(x + 1, y, w - 2, h, LIGHT);
        fb_fill(x + 1, y, w - 2, 1, BLACK); fb_fill(x + 1, y + h - 1, w - 2, 1, BLACK);
        fb_fill(x, y + 1, 1, h - 2, BLACK); fb_fill(x + w - 1, y + 1, 1, h - 2, BLACK);
        text(f, x + 4, y + 2, s, slen(s), BLACK);
        return;
    }
    fb_frame(x, y, w, h, BLACK);
    room = w - 4;
    if (t->type == FT_SELECT) {
        int ax = x + w - 8, ay = y + h / 2 - 1;
        room -= 8;
        fb_fill(ax - 2, y + 1, 1, h - 2, DARK);
        fb_fill(ax, ay, 5, 1, BLACK); fb_fill(ax + 1, ay + 1, 3, 1, BLACK); fb_fill(ax + 2, ay + 2, 1, 1, BLACK);
    }
    /* as much as fits; a text field shows its end */
    len = slen(s);
    if (t->type != FT_SELECT)
        while (len && text_w(f, p, len) > room) { p++; len--; }
    else
        while (len && text_w(f, p, len) > room) len--;
    text(f, x + 2, y + 2, p, len, BLACK);
}

void page_draw(void)
{
    u16 i, n = n_all();
    struct box b;
    fb_fill(0, VIEW_Y, SCR_W, VIEW_H, WHITE);
    for (i = 0; i < n; i++) {
        long yy;
        int x, y;
        page_box(i, &b);
        yy = b.y - top;
        if (yy + b.h <= 0) continue;
        if (yy >= VIEW_H + 16) break;
        if (yy >= VIEW_H) continue;
        x = b.x + 1;
        y = (int)yy + VIEW_Y;
        switch (b.kind) {
        case PB_TEXT:
        case PB_MORE:
            text(b.font, x, y, b.text, b.len, BLACK);
            if (b.style & S_LINK) {
                int len = b.len;
                while (len && b.text[len - 1] == ' ') len--;
                fb_fill(x, y + font_h(b.font), text_w(b.font, b.text, len), 1, DARK);
            }
            break;
        case PB_ALT:
            text(0, x, y, b.text, b.len, b.target ? BLACK : DARK);
            break;
        case PB_PIC:
            fb_picture(x, y, b.w, b.h, b.pix);
            break;
        case PB_IMG:
            fb_frame(x, y, b.w, b.h, LIGHT);
            if (b.target) fb_fill(x, y + b.h - 1, b.w, 1, DARK);
            break;
        case PB_HR:
            fb_fill(x, y + 2, b.w, 1, DARK);
            break;
        case PB_FIELD:
            draw_field(&b, x, y);
            break;
        }
        if (focus && b.target == focus) {
            if (b.kind == PB_FIELD) fb_frame(x - 1, y - 1, b.w + 2, b.h + 2, BLACK);
            else fb_invert(x, y, b.w, b.h);
        }
    }
    /* the scroll bar */
    fb_fill(SCR_W - SCROLL_W, VIEW_Y, SCROLL_W, VIEW_H, LIGHT);
    if (pg.height > VIEW_H) {
        long th = (long)VIEW_H * VIEW_H / pg.height, ty;
        if (th < 4) th = 4;
        ty = (VIEW_H - th) * top / (pg.height - VIEW_H);
        if (ty > VIEW_H - th) ty = VIEW_H - th;
        fb_fill(SCR_W - SCROLL_W, VIEW_Y + (int)ty, SCROLL_W, (int)th, BLACK);
    } else {
        fb_fill(SCR_W - SCROLL_W, VIEW_Y, SCROLL_W, VIEW_H, BLACK);
    }
}

static long max_top(void)
{
    long m = pg.height - VIEW_H;
    return m > 0 ? m : 0;
}

void page_scroll_to(long y)
{
    if (y > max_top()) y = max_top();
    if (y < 0) y = 0;
    top = y;
}

/* the first and last box of target t */
static u8 span(u8 t, u16 *first, u16 *last)
{
    u16 i, n = n_all();
    u8 found = 0;
    struct box b;
    for (i = 0; i < n; i++) {
        page_box(i, &b);
        if (b.target != t) continue;
        if (!found) *first = i;
        *last = i;
        found = 1;
    }
    return found;
}

static u8 visible(const struct box *b)
{
    return b->y + b->h > top && b->y < top + VIEW_H;
}

static void drop_hidden_focus(void)
{
    u16 first, last;
    struct box b;
    if (!focus || !span(focus, &first, &last)) { focus = 0; return; }
    page_box(first, &b);
    if (!visible(&b)) focus = 0;
}

/* the first box whose bottom is below y */
static u16 find(long y)
{
    u16 i, n = n_all();
    struct box b;
    for (i = 0; i < n; i++) {
        page_box(i, &b);
        if (b.y + b.h > y) return i;
    }
    return n;
}

void page_move_focus(int dir)
{
    u16 first = 0, last = 0, n = n_all();
    struct box b;
    u8 have = 0;
    if (focus && span(focus, &first, &last)) {
        page_box(first, &b);
        have = visible(&b);
    }
    if (dir > 0) {
        u16 i = have ? last + 1 : find(top);
        for (; i < n; i++) {
            page_box(i, &b);
            if (b.y >= top + VIEW_H) break;
            if (b.target && b.target != focus && b.y >= top) {
                focus = b.target;
                if (b.y + b.h > top + VIEW_H) page_scroll_to(b.y + b.h - VIEW_H);
                return;
            }
        }
        if (top >= max_top()) return;
        page_scroll_to(top + LINE_STEP * 2);
    } else {
        int i = have ? (int)first - 1 : (int)find(top + VIEW_H) - 1;
        for (; i >= 0; i--) {
            page_box((u16)i, &b);
            if (b.y + b.h <= top) break;
            if (b.target && b.target != focus && b.y + b.h <= top + VIEW_H) {
                u16 f0 = (u16)i, f1;
                focus = b.target;
                span(focus, &f0, &f1);
                page_box(f0, &b);
                if (b.y < top) page_scroll_to(b.y);
                return;
            }
        }
        if (top <= 0) return;
        page_scroll_to(top - LINE_STEP * 2);
    }
    drop_hidden_focus();
}

void page_scroll(int dir)
{
    page_scroll_to(top + dir * (VIEW_H - LINE_STEP));
    drop_hidden_focus();
}
