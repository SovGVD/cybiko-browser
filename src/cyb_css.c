/* The bits of CSS a page's text depends on: what is hidden (display:none,
 * visibility:hidden, and the usual ways of putting screen-reader text off
 * the screen), bold and centred text. Rules come from <style> blocks and
 * style attributes; a rule's selector must be one simple selector (tag,
 * .class, #id, tag.class or tag#id, in a comma list). Others, @media and
 * the like are left out, and so are linked style sheets. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cyb_internal.h"

#define RULES_MAX 512

static const char *skip_space(const char *s, const char *e)
{
    while (s < e && isspace((unsigned char)*s)) s++;
    return s;
}

/* s[0..n) trimmed and lower-cased into out. */
static void lower(char *out, size_t cap, const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[n] = 0;
}

static void effect(cyb_css_fx_t *fx, uint8_t what, bool on)
{
    if (on) { fx->set |= what; fx->clear &= (uint8_t)~what; }
    else { fx->clear |= what; fx->set &= (uint8_t)~what; }
}

cyb_css_fx_t cyb_css_decls(const char *s, size_t len)
{
    cyb_css_fx_t fx = { 0, 0 };
    const char *e = s + len;
    bool absolute = false, away = false;
    while (s < e) {
        const char *semi = memchr(s, ';', (size_t)(e - s));
        const char *end = semi ? semi : e;
        const char *colon = memchr(s, ':', (size_t)(end - s));
        if (colon) {
            char name[32], value[64];
            lower(name, sizeof name, s, (size_t)(colon - s));
            lower(value, sizeof value, colon + 1, (size_t)(end - colon - 1));
            char *imp = strstr(value, "!important");
            if (imp) {
                *imp = 0;
                lower(value, sizeof value, value, strlen(value));
            }
            if (!strcmp(name, "display")) effect(&fx, CSS_HIDE, !strcmp(value, "none"));
            else if (!strcmp(name, "visibility")) effect(&fx, CSS_HIDE, !strcmp(value, "hidden") || !strcmp(value, "collapse"));
            else if (!strcmp(name, "font-weight"))
                effect(&fx, CSS_BOLD, !strcmp(value, "bold") || !strcmp(value, "bolder") || atoi(value) >= 600);
            else if (!strcmp(name, "text-align")) effect(&fx, CSS_CENTER, !strcmp(value, "center"));
            else if (!strcmp(name, "position")) absolute = !strcmp(value, "absolute") || !strcmp(value, "fixed");
            else if ((!strcmp(name, "left") || !strcmp(name, "top")) && atoi(value) <= -999)
                away = true;                    /* pushed off the screen */
            else if (!strcmp(name, "clip") && !strncmp(value, "rect(0", 6))
                away = true;                    /* screen-reader text */
        }
        s = semi ? semi + 1 : e;
    }
    if (absolute && away) effect(&fx, CSS_HIDE, true);
    return fx;
}

/* One simple selector into r; false if it is not one. */
static bool selector(cyb_css_rule_t *r, const char *s, size_t n)
{
    char sel[96];
    lower(sel, sizeof sel, s, n);
    memset(r, 0, sizeof *r);
    if (!sel[0]) return false;
    const char *p = sel;
    size_t k = 0;
    if (*p == '*') p++;
    while (isalnum((unsigned char)*p) || *p == '-') { if (k + 1 < sizeof r->tag) r->tag[k++] = *p; p++; }
    r->tag[k] = 0;
    if (*p == '.' || *p == '#') {
        char *out = *p == '.' ? r->cls : r->id;
        const size_t cap = *p == '.' ? sizeof r->cls : sizeof r->id;
        p++;
        k = 0;
        while (isalnum((unsigned char)*p) || *p == '-' || *p == '_') { if (k + 1 < cap) out[k++] = *p; p++; }
        out[k] = 0;
        if (!k) return false;
    }
    return !*p && (r->tag[0] || r->cls[0] || r->id[0] || sel[0] == '*');
}

static void add(cyb_css_t *c, const cyb_css_rule_t *r)
{
    if (c->n == c->cap) {
        if (c->cap >= RULES_MAX) return;
        unsigned cap = c->cap ? c->cap * 2 : 32;
        cyb_css_rule_t *n = realloc(c->r, cap * sizeof *n);
        if (!n) return;
        c->r = n;
        c->cap = cap;
    }
    c->r[c->n++] = *r;
}

void cyb_css_parse(cyb_css_t *c, const char *s, size_t len)
{
    const char *e = s + len;
    while ((s = skip_space(s, e)) < e) {
        if (s + 1 < e && s[0] == '/' && s[1] == '*') {
            const char *end = s + 2;
            while (end + 1 < e && !(end[0] == '*' && end[1] == '/')) end++;
            s = end + 1 < e ? end + 2 : e;
            continue;
        }
        if (*s == '<' || *s == '-' || *s == '}') { s++; continue; }   /* <!-- --> around old styles */
        if (*s == '@') {
            /* @media, @font-face, @import...: their rules are not used */
            const char *p = s;
            while (p < e && *p != ';' && *p != '{') p++;
            if (p < e && *p == '{') {
                int depth = 0;
                for (; p < e; p++) {
                    if (*p == '{') depth++;
                    else if (*p == '}' && !--depth) { p++; break; }
                }
            } else if (p < e) {
                p++;
            }
            s = p;
            continue;
        }
        const char *open = memchr(s, '{', (size_t)(e - s));
        if (!open) break;
        const char *close = memchr(open, '}', (size_t)(e - open));
        if (!close) close = e;
        const cyb_css_fx_t fx = cyb_css_decls(open + 1, (size_t)(close - open - 1));
        if (fx.set || fx.clear) {
            for (const char *a = s; a < open;) {
                const char *comma = memchr(a, ',', (size_t)(open - a));
                const char *end = comma ? comma : open;
                cyb_css_rule_t r;
                if (selector(&r, a, (size_t)(end - a))) {
                    r.fx = fx;
                    add(c, &r);
                }
                a = comma ? comma + 1 : open;
            }
        }
        s = close < e ? close + 1 : e;
    }
}

/* Whether class list cls (space separated) has name. */
static bool has_class(const char *cls, const char *name)
{
    const size_t n = strlen(name);
    for (const char *p = cls; *p;) {
        while (*p == ' ') p++;
        size_t k = strcspn(p, " ");
        if (k == n && !strncasecmp(p, name, n)) return true;
        p += k;
    }
    return false;
}

void cyb_css_apply(const cyb_css_t *c, const char *tag, const char *cls, const char *id, uint8_t *state)
{
    for (unsigned i = 0; i < c->n; i++) {
        const cyb_css_rule_t *r = &c->r[i];
        if (r->tag[0] && strcmp(r->tag, tag)) continue;
        if (r->cls[0] && !has_class(cls, r->cls)) continue;
        if (r->id[0] && strcasecmp(r->id, id)) continue;
        *state = (uint8_t)((*state | r->fx.set) & ~r->fx.clear);
    }
}

void cyb_css_free(cyb_css_t *c)
{
    free(c->r);
    memset(c, 0, sizeof *c);
}
