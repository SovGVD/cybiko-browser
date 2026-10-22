/* Web pages as WML for the Cybiko's WAP browser (see cy_wml.h).
 *
 * The deck is one card with one paragraph: text, <b> and <big> for bold and
 * headings, <a> for links and <br/> for line breaks. Form fields become
 * <input> and <select> named v<field index>, and each submit button an
 * <anchor> whose <go> carries them as postfields. A page too big for one
 * deck ends with a "More..." link to its next part. */
#include <stdio.h>
#include <string.h>
#include "cy_wml.h"

#define CLOSE_ROOM 120     /* what closing the deck and a More link take */

typedef struct {
    char *p;
    size_t len, cap;       /* cap: room for the deck before its end */
    bool over;
    uint16_t link;         /* open <a>: its target + 1 */
    uint8_t style;         /* open <b>/<big> */
} out_t;

static void put(out_t *o, const char *s, size_t n)
{
    if (o->len + n > o->cap) { o->over = true; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void puts_(out_t *o, const char *s) { put(o, s, strlen(s)); }

/* Text for an element or an attribute value. */
static void esc(out_t *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n && !o->over; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '&': puts_(o, "&amp;"); break;
        case '<': puts_(o, "&lt;"); break;
        case '>': puts_(o, "&gt;"); break;
        case '"': puts_(o, "&quot;"); break;
        case '\'': puts_(o, "&apos;"); break;
        case '$': puts_(o, "$$"); break;             /* WML variables start with $ */
        default:
            if (c < 0x20) c = ' ';
            put(o, (const char *)&c, 1);
        }
    }
}

static void escs(out_t *o, const char *s) { esc(o, s, strlen(s)); }

static void close_style(out_t *o)
{
    if (o->style & S_BIG) puts_(o, "</big>");
    if (o->style & S_BOLD) puts_(o, "</b>");
    o->style = 0;
}

static void close_link(out_t *o)
{
    if (!o->link) return;
    close_style(o);
    puts_(o, "</a>");
    o->link = 0;
}

static void set_style(out_t *o, uint8_t style)
{
    style &= S_BOLD | S_BIG;
    if (style == o->style) return;
    close_style(o);
    if (style & S_BOLD) puts_(o, "<b>");
    if (style & S_BIG) puts_(o, "<big>");
    o->style = style;
}

static void open_link(out_t *o, const cyb_doc_t *d, uint16_t target, const cy_wml_links_t *l)
{
    if (o->link == target) return;
    close_link(o);
    if (!target || target > d->n_targets || d->targets[target - 1].kind != T_LINK) return;
    char url[160];
    l->link(l->user, d->pool + d->targets[target - 1].href, url, sizeof url);
    puts_(o, "<a href=\"");
    escs(o, url);
    puts_(o, "\">");
    o->link = target;
}

static void var(out_t *o, uint32_t field)
{
    char v[16];
    snprintf(v, sizeof v, "v%u", (unsigned)field);
    puts_(o, v);
}

static void option(out_t *o, const char *value, const char *label)
{
    puts_(o, "<option value=\"");
    escs(o, value);
    puts_(o, "\">");
    escs(o, *label ? label : value);
    puts_(o, "</option>");
}

/* The fields a form sends, as postfields. */
static void postfields(out_t *o, const cyb_doc_t *d, int form)
{
    for (uint32_t i = 0; i < d->n_fields; i++) {
        const cyb_field_t *f = &d->fields[i];
        if (f->form != form || f->type == FT_SUBMIT || f->type == FT_RESET || f->type == FT_HIDDEN) continue;
        if (!d->pool[f->name]) continue;
        if (f->type == FT_RADIO) {
            /* one select for the group: its first button's */
            bool first = true;
            for (uint32_t k = 0; k < i && first; k++)
                if (d->fields[k].type == FT_RADIO && d->fields[k].form == form &&
                    !strcmp(d->pool + d->fields[k].name, d->pool + f->name))
                    first = false;
            if (!first) continue;
        }
        puts_(o, "<postfield name=\"");
        var(o, i);
        puts_(o, "\" value=\"$(");
        var(o, i);
        puts_(o, ")\"/>");
    }
}

static void field(out_t *o, const cyb_doc_t *d, uint32_t i, const cy_wml_links_t *l)
{
    const cyb_field_t *f = &d->fields[i];
    switch (f->type) {
    case FT_TEXT:
    case FT_PASSWORD:
    case FT_TEXTAREA:
        puts_(o, "<input name=\"");
        var(o, i);
        puts_(o, "\" value=\"");
        escs(o, f->value);
        puts_(o, f->type == FT_PASSWORD ? "\" type=\"password\"/>" : "\"/>");
        break;
    case FT_CHECKBOX:
        puts_(o, "<select name=\"");
        var(o, i);
        puts_(o, "\" value=\"");
        escs(o, f->checked ? (*f->value ? f->value : "on") : "");
        puts_(o, "\">");
        option(o, *f->value ? f->value : "on", "Yes");
        option(o, "", "No");
        puts_(o, "</select>");
        break;
    case FT_RADIO: {
        /* the group's first button shows them all */
        const char *name = d->pool + f->name;
        for (uint32_t k = 0; k < i; k++)
            if (d->fields[k].type == FT_RADIO && d->fields[k].form == f->form && !strcmp(d->pool + d->fields[k].name, name))
                return;
        const char *chosen = "";
        for (uint32_t k = i; k < d->n_fields; k++)
            if (d->fields[k].type == FT_RADIO && d->fields[k].form == f->form && d->fields[k].checked &&
                !strcmp(d->pool + d->fields[k].name, name))
                chosen = d->fields[k].value;
        puts_(o, "<select name=\"");
        var(o, i);
        puts_(o, "\" value=\"");
        escs(o, chosen);
        puts_(o, "\">");
        for (uint32_t k = i; k < d->n_fields; k++)
            if (d->fields[k].type == FT_RADIO && d->fields[k].form == f->form && !strcmp(d->pool + d->fields[k].name, name))
                option(o, d->fields[k].value, "");
        puts_(o, "</select>");
        break;
    }
    case FT_SELECT: {
        const char *p = d->pool + f->options;
        const char *chosen = "";
        for (int k = 0; k < f->n_options; k++) {
            if (k == f->chosen) chosen = p;
            p += strlen(p) + 1;
            p += strlen(p) + 1;
        }
        puts_(o, "<select name=\"");
        var(o, i);
        puts_(o, "\" value=\"");
        escs(o, chosen);
        puts_(o, "\">");
        p = d->pool + f->options;
        for (int k = 0; k < f->n_options; k++) {
            const char *label = p + strlen(p) + 1;
            option(o, p, label);
            p = label + strlen(label) + 1;
        }
        puts_(o, "</select>");
        break;
    }
    case FT_SUBMIT: {
        if (f->form < 0) break;
        char url[160];
        l->form(l->user, d, f->form, (int)i, url, sizeof url);
        const char *label = d->pool + f->label;
        puts_(o, "<anchor>[");
        escs(o, *label ? label : "Submit");
        puts_(o, "]<go href=\"");
        escs(o, url);
        puts_(o, "\">");
        postfields(o, d, f->form);
        puts_(o, "</go></anchor>");
        break;
    }
    default:
        break;
    }
}

/* Writes item i from byte off; returns the offset reached in a text that
 * did not fit (only when nothing else is on the deck yet). */
static uint32_t item(out_t *o, const cyb_doc_t *d, uint32_t i, uint32_t off, const cy_wml_links_t *l, bool alone)
{
    const cyb_item_t *it = &d->items[i];
    switch (it->kind) {
    case IT_TEXT: {
        open_link(o, d, it->target, l);
        set_style(o, it->style);
        const char *s = d->pool + it->text + off;
        size_t n = it->len - off;
        size_t at = o->len;
        esc(o, s, n);
        if (!o->over || !alone) return it->len;
        /* a text bigger than a deck: as much as fits, cut at a space */
        o->over = false;
        o->len = at;
        size_t room = o->cap > at ? (o->cap - at) / 2 : 0;    /* escapes may double it */
        if (room > n) room = n;
        size_t cut = room;
        while (cut > room / 2 && s[cut - 1] != ' ') cut--;
        if (cut <= room / 2) cut = room;
        esc(o, s, cut);
        return off + (uint32_t)cut;
    }
    case IT_BREAK:
        close_link(o);
        puts_(o, "<br/>");
        break;
    case IT_PARA:
        close_link(o);
        close_style(o);
        if (o->len >= 6 && memcmp(o->p + o->len - 5, "<br/>", 5)) puts_(o, "<br/>");
        break;
    case IT_HR:
        close_link(o);
        puts_(o, "<br/>----------<br/>");
        break;
    case IT_IMG: {
        if (it->ref < 0 || (uint32_t)it->ref >= d->n_imgs) break;
        const char *alt = d->pool + d->imgs[it->ref].alt;
        if (!*alt) break;
        open_link(o, d, it->target, l);
        puts_(o, "[");
        escs(o, alt);
        puts_(o, "]");
        break;
    }
    case IT_FIELD:
        close_link(o);
        close_style(o);
        if (it->ref >= 0 && (uint32_t)it->ref < d->n_fields) field(o, d, (uint32_t)it->ref, l);
        break;
    default:
        break;
    }
    return it->len;
}

static void head(out_t *o, const char *title)
{
    puts_(o, "<?xml version=\"1.0\"?>\n<wml><card id=\"p\" title=\"");
    char t[48];
    snprintf(t, sizeof t, "%s", title);
    escs(o, t);
    puts_(o, "\"><p>");
}

static size_t tail(out_t *o)
{
    o->cap += CLOSE_ROOM;
    close_link(o);
    close_style(o);
    puts_(o, "</p></card></wml>");
    o->p[o->len] = 0;
    return o->len;
}

size_t cy_wml_page(const cyb_doc_t *d, cy_wml_pos_t *pos, char *out, size_t cap, const cy_wml_links_t *l)
{
    out_t o = { .p = out, .cap = cap > CLOSE_ROOM + 1 ? cap - CLOSE_ROOM - 1 : 0 };
    head(&o, d->title[0] ? d->title : "Page");
    bool any = false;
    while (pos->item < d->n_items) {
        out_t keep = o;
        uint32_t off = item(&o, d, pos->item, pos->off, l, !any);
        if (o.over) { o = keep; break; }
        any = true;
        if (off < d->items[pos->item].len && d->items[pos->item].kind == IT_TEXT) {
            pos->off = off;
            break;
        }
        pos->item++;
        pos->off = 0;
    }
    if (pos->item < d->n_items) {
        char url[160];
        l->more(l->user, *pos, url, sizeof url);
        o.cap += CLOSE_ROOM;
        close_link(&o);
        close_style(&o);
        puts_(&o, "<br/><a href=\"");
        escs(&o, url);
        puts_(&o, "\">More...</a>");
        o.cap -= CLOSE_ROOM;
    }
    return tail(&o);
}

size_t cy_wml_text(char *out, size_t cap, const char *title, const char *text)
{
    out_t o = { .p = out, .cap = cap > CLOSE_ROOM + 1 ? cap - CLOSE_ROOM - 1 : 0 };
    head(&o, title);
    escs(&o, text);
    o.over = false;
    return tail(&o);
}
