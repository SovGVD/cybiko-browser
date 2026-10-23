/* The browser itself: pages, history, keys, menus and the screen. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cyb_internal.h"
#include "cy_browser_app.h"

const uint8_t *const cyb_launcher_app = cy_browser_app;
const size_t cyb_launcher_app_size = sizeof cy_browser_app;

#define HOME_URL "about:home"
#define PAGE_MAX (768 * 1024)      /* biggest page read */
#define HISTORY 32
#define BOOKMARKS 64
#define LINE_STEP 10
#define REPEAT_DELAY 400
#define REPEAT_EVERY 90

enum { MODE_PAGE, MODE_MENU, MODE_EDIT, MODE_PICK };
enum { EDIT_URL, EDIT_FIELD };
enum { LOAD_NONE, LOAD_PAGE, LOAD_IMAGE };

typedef struct {
    char url[1024];
    int32_t top;
    uint16_t focus;
} hist_t;

typedef struct {
    char url[512];
    char title[96];
} mark_t;

/* a list shown over the page: the menu, or a select's options */
typedef struct {
    int n, sel, first;
    const char *items[48];
    char title[48];
} list_t;

struct cyb {
    cyb_net_t net;
    cyb_font_t fonts[F_COUNT];
    char data_dir[256];

    /* the page */
    char url[1024];
    cyb_doc_t doc;
    cyb_layout_t lay;
    int32_t top;
    uint16_t focus;
    char status[40];            /* a message in the title bar */
    uint32_t status_until;

    hist_t hist[HISTORY];
    int hist_n, hist_pos;       /* hist_pos: the entry shown */

    /* loading */
    int load;
    char load_url[1024];
    bool load_push;             /* add to history when it comes */
    int32_t load_top;
    uint16_t load_focus;
    uint8_t *buf;
    size_t buf_len, buf_cap;
    uint32_t img;               /* the picture being fetched */
    bool stop_images;

    /* input */
    int mode;
    bool shift, fn, shift_used, fn_used, sticky_shift;
    int held;                   /* an arrow held down, for repeats */
    uint32_t held_since, held_last, now;
    list_t list;
    int list_kind;              /* 0 menu, 1 select */
    int pick_field;
    int edit_kind, edit_field;
    char edit[FIELD_VALUE_MAX];
    int edit_len, edit_cur;
    char edit_prompt[24];

    /* settings */
    bool images, big;
    char home[512];

    mark_t marks[BOOKMARKS];
    int n_marks;

    bool closed, shown, dirty;
    bool wifi_scanning;
    uint8_t fb[CYB_FB_SIZE];
};

static void go(cyb_t *b, const char *url, bool push, const char *body);

/* ---- small things ---- */

static void say(cyb_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->status, sizeof b->status, fmt, ap);
    va_end(ap);
    b->status_until = b->now + 2500;
    b->dirty = true;
}

/* text into HTML */
typedef struct {
    char *p;
    size_t len, cap;
} sb_t;

static void sb_add(sb_t *s, const char *t, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        while (cap < s->len + n + 1) cap *= 2;
        char *p = realloc(s->p, cap);
        if (!p) return;
        s->p = p;
        s->cap = cap;
    }
    memcpy(s->p + s->len, t, n);
    s->len += n;
    s->p[s->len] = 0;
}

static void sb_printf(sb_t *s, const char *fmt, ...)
{
    char tmp[1536];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_add(s, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

#define SB_LIT(s, lit) sb_add((s), lit, sizeof lit - 1)

/* Latin-1 text escaped for an HTML page (the pages built here are Latin-1) */
static void sb_text(sb_t *s, const char *t)
{
    for (; *t; t++) {
        switch (*t) {
        case '<': SB_LIT(s, "&lt;"); break;
        case '>': SB_LIT(s, "&gt;"); break;
        case '&': SB_LIT(s, "&amp;"); break;
        case '"': SB_LIT(s, "&quot;"); break;
        default: sb_add(s, t, 1);
        }
    }
}

/* a value for a link of an about: page */
static void sb_query(sb_t *s, const char *t)
{
    char enc[512] = "";
    cyb_url_encode(enc, sizeof enc, t, false);
    sb_text(s, enc);
}

static bool query_get(const char *url, const char *key, char *out, size_t cap)
{
    const char *q = strchr(url, '?');
    size_t kl = strlen(key);
    out[0] = 0;
    while (q) {
        q++;
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            size_t n = strcspn(q + kl + 1, "&#");
            if (n >= cap) n = cap - 1;
            memcpy(out, q + kl + 1, n);
            out[n] = 0;
            cyb_url_decode(out);
            return true;
        }
        q = strchr(q, '&');
    }
    return false;
}

/* ---- settings and bookmarks, in data_dir ---- */

static void path(const cyb_t *b, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", b->data_dir, name);
}

static void load_settings(cyb_t *b)
{
    b->images = true;
    b->big = false;
    snprintf(b->home, sizeof b->home, "%s", HOME_URL);
    b->n_marks = 0;
    if (!b->data_dir[0]) return;
    char p[320], line[640];
    path(b, "settings.txt", p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *v = strchr(line, '=');
            if (!v) continue;
            *v++ = 0;
            if (!strcmp(line, "images")) b->images = atoi(v) != 0;
            else if (!strcmp(line, "big")) b->big = atoi(v) != 0;
            else if (!strcmp(line, "home") && *v) snprintf(b->home, sizeof b->home, "%s", v);
        }
        fclose(f);
    }
    path(b, "bookmarks.txt", p, sizeof p);
    f = fopen(p, "r");
    if (f) {
        while (b->n_marks < BOOKMARKS && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *t = strchr(line, '\t');
            if (t) *t++ = 0;
            if (!line[0]) continue;
            mark_t *m = &b->marks[b->n_marks++];
            snprintf(m->url, sizeof m->url, "%s", line);
            snprintf(m->title, sizeof m->title, "%s", t && *t ? t : line);
        }
        fclose(f);
    }
}

static void save_settings(cyb_t *b)
{
    if (!b->data_dir[0]) return;
    char p[320];
    path(b, "settings.txt", p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) { say(b, "Can't save settings"); return; }
    fprintf(f, "images=%d\nbig=%d\nhome=%s\n", b->images, b->big, b->home);
    fclose(f);
}

static void save_marks(cyb_t *b)
{
    if (!b->data_dir[0]) { say(b, "No folder for bookmarks"); return; }
    char p[320];
    path(b, "bookmarks.txt", p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) { say(b, "Can't save bookmarks"); return; }
    for (int i = 0; i < b->n_marks; i++) fprintf(f, "%s\t%s\n", b->marks[i].url, b->marks[i].title);
    fclose(f);
}

/* ---- the page on screen ---- */

static int32_t max_top(const cyb_t *b)
{
    int32_t m = b->lay.height - VIEW_H;
    return m > 0 ? m : 0;
}

static void scroll_to(cyb_t *b, int32_t top)
{
    if (top > max_top(b)) top = max_top(b);
    if (top < 0) top = 0;
    if (top != b->top) b->dirty = true;
    b->top = top;
}

static void relayout(cyb_t *b, bool keep_place)
{
    /* keep the first line on screen where it is */
    uint32_t anchor_item = 0;
    int32_t anchor_off = 0;
    bool anchored = false;
    if (keep_place && b->lay.n && b->top > 0) {
        uint32_t i = cyb_layout_find(&b->lay, b->top);
        if (i < b->lay.n) {
            anchor_item = b->lay.boxes[i].item;
            anchor_off = b->lay.boxes[i].y - b->top;
            anchored = true;
        }
    }
    cyb_layout(&b->lay, &b->doc, b->fonts, b->big);
    if (anchored) {
        for (uint32_t i = 0; i < b->lay.n; i++)
            if (b->lay.boxes[i].item >= anchor_item) { b->top = b->lay.boxes[i].y - anchor_off; break; }
    }
    scroll_to(b, b->top);
    b->dirty = true;
}

/* the first and last box of a target */
static bool target_span(const cyb_t *b, uint16_t t, uint32_t *first, uint32_t *last)
{
    bool found = false;
    for (uint32_t i = 0; i < b->lay.n; i++)
        if (b->lay.boxes[i].target == t) {
            if (!found) *first = i;
            *last = i;
            found = true;
        }
    return found;
}

static bool box_visible(const cyb_t *b, const cyb_box_t *x)
{
    return x->y + x->h > b->top && x->y < b->top + VIEW_H;
}

static void move_focus(cyb_t *b, int dir)
{
    const cyb_layout_t *l = &b->lay;
    uint32_t first = 0, last = 0;
    bool have = b->focus && target_span(b, b->focus, &first, &last) && box_visible(b, &l->boxes[first]);
    if (dir > 0) {
        uint32_t i = have ? last + 1 : cyb_layout_find(l, b->top);
        for (; i < l->n; i++) {
            const cyb_box_t *x = &l->boxes[i];
            if (x->y >= b->top + VIEW_H) break;
            if (x->target && x->target != b->focus && x->y >= b->top) {
                b->focus = x->target;
                if (x->y + x->h > b->top + VIEW_H) scroll_to(b, x->y + x->h - VIEW_H);
                b->dirty = true;
                return;
            }
        }
        if (b->top >= max_top(b)) return;
        scroll_to(b, b->top + LINE_STEP * 2);
    } else {
        int64_t i = have ? (int64_t)first - 1 : (int64_t)cyb_layout_find(l, b->top + VIEW_H) - 1;
        for (; i >= 0; i--) {
            const cyb_box_t *x = &l->boxes[i];
            if (x->y + x->h <= b->top) break;
            if (x->target && x->target != b->focus && x->y + x->h <= b->top + VIEW_H) {
                b->focus = x->target;
                uint32_t f0 = (uint32_t)i, f1;
                target_span(b, x->target, &f0, &f1);
                if (l->boxes[f0].y < b->top) scroll_to(b, l->boxes[f0].y);
                b->dirty = true;
                return;
            }
        }
        if (b->top <= 0) return;
        scroll_to(b, b->top - LINE_STEP * 2);
    }
    /* the focus scrolled away: drop it */
    if (b->focus && target_span(b, b->focus, &first, &last) && !box_visible(b, &l->boxes[first])) b->focus = 0;
    b->dirty = true;
}

static void page_scroll(cyb_t *b, int dir)
{
    scroll_to(b, b->top + dir * (VIEW_H - LINE_STEP));
    uint32_t first, last;
    if (b->focus && target_span(b, b->focus, &first, &last) && !box_visible(b, &b->lay.boxes[first])) b->focus = 0;
    b->dirty = true;
}

/* ---- loading ---- */

static void stop_loading(cyb_t *b)
{
    if (b->load) b->net.close(b->net.user);
    b->load = LOAD_NONE;
    b->buf_len = 0;
    b->dirty = true;
}

static void set_doc(cyb_t *b, const char *url, bool push, int32_t top, uint16_t focus)
{
    if (push) {
        /* the page we leave keeps its place */
        if (b->hist_n) {
            b->hist[b->hist_pos].top = b->top;
            b->hist[b->hist_pos].focus = b->focus;
        }
        if (b->hist_n && strcmp(b->hist[b->hist_pos].url, url)) {
            b->hist_n = b->hist_pos + 1;
            if (b->hist_n == HISTORY) {
                memmove(b->hist, b->hist + 1, (HISTORY - 1) * sizeof *b->hist);
                b->hist_n--;
            }
            b->hist_pos = b->hist_n++;
        } else if (!b->hist_n) b->hist_pos = b->hist_n++;
        snprintf(b->hist[b->hist_pos].url, sizeof b->hist[0].url, "%s", url);
    } else if (b->hist_n) {
        snprintf(b->hist[b->hist_pos].url, sizeof b->hist[0].url, "%s", url);
    }
    snprintf(b->url, sizeof b->url, "%s", url);
    b->top = 0;
    b->focus = focus;
    cyb_layout(&b->lay, &b->doc, b->fonts, b->big);
    scroll_to(b, top);
    b->img = 0;
    b->stop_images = false;
    b->mode = MODE_PAGE;
    b->dirty = true;
}

static void error_page(cyb_t *b, const char *url, const char *why)
{
    sb_t s = { 0 };
    SB_LIT(&s, "<title>Not opened</title><h2>Not opened</h2><p>");
    sb_text(&s, url);
    SB_LIT(&s, "<p>");
    sb_text(&s, why && *why ? why : "The page did not load.");
    SB_LIT(&s, "<p><a href=\"");
    sb_text(&s, url);
    SB_LIT(&s, "\">Try again</a> | <a href=\"about:home\">Home</a>");
    stop_loading(b);
    cyb_doc_free(&b->doc);
    cyb_html_parse(&b->doc, s.p, s.len, "about:error", CS_LATIN1, false);
    free(s.p);
    /* it stands for the page that failed, so Reload tries again */
    set_doc(b, url, b->load_push, 0, 0);
}

static bool type_is(const char *ct, const char *what)
{
    return ct && !strncasecmp(ct, what, strlen(what));
}

static void page_done(cyb_t *b)
{
    const char *final = b->net.final_url ? b->net.final_url(b->net.user) : NULL;
    const char *ct = b->net.content_type ? b->net.content_type(b->net.user) : NULL;
    char url[1024], ctype[96] = "";
    snprintf(url, sizeof url, "%s", final && *final ? final : b->load_url);
    if (ct) snprintf(ctype, sizeof ctype, "%s", ct);
    b->net.close(b->net.user);
    b->load = LOAD_NONE;
    const uint8_t *d = b->buf;
    size_t n = b->buf_len;
    cyb_doc_free(&b->doc);
    bool image = type_is(ctype, "image/") ||
        (n > 8 && (!memcmp(d, "\x89PNG", 4) || !memcmp(d, "GIF8", 4) || (d[0] == 0xff && d[1] == 0xd8)));
    if (image) {
        /* a picture on its own: a page with just it */
        cyb_pic_t pic;
        if (!cyb_image_decode(d, n, PAGE_W, 4 * CYB_IMG_MAX_H, &pic)) {
            error_page(b, url, "This picture's format can't be shown.");
            return;
        }
        static const char html[] = "<img src=\"x\">";
        cyb_html_parse(&b->doc, html, sizeof html - 1, url, CS_LATIN1, false);
        if (b->doc.n_imgs) {
            b->doc.imgs[0].pic = pic;
            b->doc.imgs[0].state = IMG_DONE;
        } else free(pic.pix);
        const char *slash = strrchr(url, '/');
        snprintf(b->doc.title, sizeof b->doc.title, "%s", slash && slash[1] ? slash + 1 : url);
    } else if (ctype[0] && !type_is(ctype, "text/") && !type_is(ctype, "application/xhtml") &&
               !type_is(ctype, "application/xml")) {
        char why[160];
        snprintf(why, sizeof why, "This is a %s file, which can't be shown.", ctype);
        error_page(b, url, why);
        return;
    } else {
        int cs = -1;
        const char *c = strcasestr(ctype, "charset=");
        if (c) {
            char name[24];
            snprintf(name, sizeof name, "%s", c + 8);
            name[strcspn(name, " ;\"'")] = 0;
            cs = cyb_charset(name);
        }
        bool plain = type_is(ctype, "text/plain");
        cyb_html_parse(&b->doc, (const char *)d, n, url, cs, plain);
        if (b->doc.oom) say(b, "Page too big, cut short");
    }
    b->buf_len = 0;
    set_doc(b, url, b->load_push, b->load_top, b->load_focus);
}

static bool buf_room(cyb_t *b, size_t want)
{
    if (b->buf_len + want <= b->buf_cap) return true;
    size_t cap = b->buf_cap ? b->buf_cap * 2 : 32768;
    while (cap < b->buf_len + want) cap *= 2;
    uint8_t *p = realloc(b->buf, cap + 1);
    if (!p) return false;
    b->buf = p;
    b->buf_cap = cap;
    return true;
}

/* Reads what has come; true when the response is complete. */
static int pump(cyb_t *b, size_t max)
{
    for (int round = 0; round < 8; round++) {
        if (!buf_room(b, 4096)) return CYB_EOF;
        int n = b->net.read(b->net.user, b->buf + b->buf_len, 4096);
        if (n == CYB_EOF || n == CYB_FAIL) return n;
        if (n <= 0) return 0;
        b->buf_len += (size_t)n;
        if (b->buf_len >= max) return CYB_EOF;
    }
    return 0;
}

static void next_image(cyb_t *b)
{
    if (!b->images || b->stop_images) return;
    for (; b->img < b->doc.n_imgs; b->img++) {
        cyb_img_t *im = &b->doc.imgs[b->img];
        if (im->state != IMG_WAIT) continue;
        if (!b->net.open(b->net.user, b->doc.pool + im->src, NULL, NULL, NULL)) {
            im->state = IMG_FAILED;
            continue;
        }
        im->state = IMG_LOADING;
        b->load = LOAD_IMAGE;
        b->buf_len = 0;
        return;
    }
}

static void image_done(cyb_t *b, bool ok)
{
    b->net.close(b->net.user);
    b->load = LOAD_NONE;
    cyb_img_t *im = &b->doc.imgs[b->img];
    im->state = IMG_FAILED;
    if (ok) {
        int max_w = PAGE_W;
        /* the room the picture has where it stands */
        for (uint32_t i = 0; i < b->lay.n; i++)
            if ((b->lay.boxes[i].kind == B_IMG || b->lay.boxes[i].kind == B_ALT) && b->lay.boxes[i].ref == (int16_t)b->img) {
                int left = b->lay.boxes[i].x;
                int j = (int)i;
                while (j > 0 && b->lay.boxes[j - 1].y == b->lay.boxes[i].y) j--;
                left = b->lay.boxes[j].x;
                max_w = PAGE_W - left;
                break;
            }
        if (im->want_w && im->want_w < max_w) max_w = im->want_w;
        int max_h = CYB_IMG_MAX_H;
        if (im->want_w && im->want_h) {
            int h = im->want_h * max_w / im->want_w;
            if (h > 0 && h < max_h) max_h = h;
        }
        if (cyb_image_decode(b->buf, b->buf_len, max_w, max_h, &im->pic)) im->state = IMG_DONE;
    }
    b->buf_len = 0;
    b->img++;
    relayout(b, true);
}

static void go(cyb_t *b, const char *url, bool push, const char *body)
{
    stop_loading(b);
    b->mode = MODE_PAGE;
    if (!strncasecmp(url, "about:", 6)) {
        extern void cyb_about(cyb_t * b, const char *url, bool push);
        cyb_about(b, url, push);
        return;
    }
    snprintf(b->load_url, sizeof b->load_url, "%s", url);
    b->load_push = push;
    b->load_top = 0;
    b->load_focus = 0;
    if (!push && b->hist_n && !strcmp(b->hist[b->hist_pos].url, url)) {
        b->load_top = b->hist[b->hist_pos].top;
        b->load_focus = b->hist[b->hist_pos].focus;
    }
    if (!strncasecmp(url, "https:", 6) || !strncasecmp(url, "http:", 5) || !strncasecmp(url, "file:", 5) ||
        !strncasecmp(url, "gopher:", 7)) {
        if (!b->net.open(b->net.user, url, body, body ? "application/x-www-form-urlencoded" : NULL, NULL)) {
            error_page(b, url, b->net.error ? b->net.error(b->net.user) : NULL);
            return;
        }
        b->load = LOAD_PAGE;
        b->buf_len = 0;
        b->dirty = true;
        return;
    }
    error_page(b, url, "Only http and https addresses can be opened.");
}

/* ---- internal pages ---- */

static void wifi_page(cyb_t *b, sb_t *s)
{
    char st[96] = "";
    SB_LIT(s, "<title>Wi-Fi</title><h2>Wi-Fi</h2>");
    if (!b->net.wifi_status) {
        SB_LIT(s, "<p>This computer is always online.");
        return;
    }
    int on = b->net.wifi_status(b->net.user, st, sizeof st);
    sb_printf(s, "<p><b>%s</b> ", on > 0 ? "Connected:" : on == 0 ? "Connecting:" : "Not connected.");
    sb_text(s, st);
    if (b->net.wifi_saved) {
        char names[8][33];
        int n = b->net.wifi_saved(b->net.user, names, 8);
        if (n > 0) {
            SB_LIT(s, "<p><b>Saved networks</b><br>");
            for (int i = 0; i < n; i++) {
                sb_text(s, names[i]);
                SB_LIT(s, " <a href=\"about:wifi-forget?ssid=");
                sb_query(s, names[i]);
                SB_LIT(s, "\">forget</a><br>");
            }
        }
    }
    if (b->net.wifi_scan) {
        cyb_ap_t aps[16];
        int n = b->net.wifi_scan(b->net.user, aps, 16);
        if (n < 0) {
            SB_LIT(s, "<p>Looking for networks...");
            b->wifi_scanning = true;
        } else {
            b->wifi_scanning = false;
            SB_LIT(s, "<p><b>Networks here</b> <a href=\"about:wifi?scan=1\">look again</a><br>");
            if (!n) SB_LIT(s, "None found.");
            for (int i = 0; i < n; i++) {
                SB_LIT(s, "<a href=\"about:wifi-join?ssid=");
                sb_query(s, aps[i].ssid);
                sb_printf(s, "&amp;open=%d\">", aps[i].open);
                sb_text(s, aps[i].ssid);
                sb_printf(s, "</a> %d%s<br>", aps[i].rssi, aps[i].open ? " open" : "");
            }
        }
    }
    SB_LIT(s, "<p>The networks are shared with Palm OS mode.");
}

static void home_page(cyb_t *b, sb_t *s)
{
    static const char top[] =
        "<title>Browser</title>"
        "<form action=\"http://frogfind.com/\"><input name=\"q\" size=\"20\"> <input type=\"submit\" value=\"Search\"></form>"
        "<p><b>Sites for small screens</b><ul>"
        "<li><a href=\"http://frogfind.com/\">FrogFind</a> search and read"
        "<li><a href=\"http://68k.news/\">68k.news</a> headlines"
        "<li><a href=\"http://lite.cnn.com/\">CNN Lite</a>"
        "<li><a href=\"http://text.npr.org/\">NPR text</a>"
        "<li><a href=\"https://wiby.me/\">wiby.me</a> the old web"
        "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first web page</a>"
        "</ul>";
    sb_add(s, top, sizeof top - 1);
    if (b->n_marks) {
        SB_LIT(s, "<p><b>Bookmarks</b><br>");
        for (int i = 0; i < b->n_marks && i < 8; i++) {
            SB_LIT(s, "<a href=\"");
            sb_text(s, b->marks[i].url);
            SB_LIT(s, "\">");
            sb_text(s, b->marks[i].title);
            SB_LIT(s, "</a><br>");
        }
    }
    SB_LIT(s, "<p><a href=\"about:bookmarks\">Bookmarks</a> | <a href=\"about:wifi\">Wi-Fi</a> | "
              "<a href=\"about:settings\">Settings</a> | <a href=\"about:help\">Help</a>");
}

static void help_page(sb_t *s)
{
    static const char t[] =
        "<title>Help</title><h2>Keys</h2>"
        "<b>Up, Down</b> next link or field, or scroll<br>"
        "<b>Left, Right</b> page up, page down<br>"
        "<b>Enter</b> open the link, change the field<br>"
        "<b>Esc</b> back (on the first page: close)<br>"
        "<b>Menu</b> address, back, bookmarks, Wi-Fi...<br>"
        "<b>Del</b> back<br>"
        "Typing a letter opens the address line: a web address, "
        "or words to search for with FrogFind."
        "<h2>Typing</h2>"
        "<b>Shift</b> capitals (press it before the letter, or hold it)<br>"
        "<b>Fn</b> Q to P: 1 to 0; A to L: @ # $ % &amp; * - + =; "
        "Z to M: &lt; &gt; \" _ { } '<br>"
        "<b>Fn</b> . / ; : ! ? ( [ ) ]<br>"
        "<b>Shift</b> . \\ , ~ ; | ! ^ ( `<br>"
        "<b>Del</b> deletes, <b>Left</b>, <b>Right</b> move, <b>Enter</b> done, <b>Esc</b> cancel."
        "<h2>About</h2>"
        "Pages are shown as text with pictures in four shades. Scripts and "
        "style sheets are not run. Sites made for old or small screens work best.";
    sb_add(s, t, sizeof t - 1);
}

static void settings_page(cyb_t *b, sb_t *s)
{
    SB_LIT(s, "<title>Settings</title><h2>Settings</h2>");
    sb_printf(s, "<p>Pictures: <b>%s</b> <a href=\"about:set?images=%d\">%s</a>",
              b->images ? "shown" : "not loaded", !b->images, b->images ? "turn off" : "turn on");
    sb_printf(s, "<p>Text: <b>%s</b> <a href=\"about:set?big=%d\">%s</a>",
              b->big ? "big" : "small", !b->big, b->big ? "make small" : "make big");
    SB_LIT(s, "<form action=\"about:set\"><p>Home page:<br><input name=\"home\" size=\"30\" value=\"");
    sb_text(s, b->home);
    SB_LIT(s, "\"> <input type=\"submit\" value=\"Set\"></form>");
    if (!b->data_dir[0]) SB_LIT(s, "<p>(Settings are not kept: no folder for them.)");
}

static void bookmarks_page(cyb_t *b, sb_t *s)
{
    SB_LIT(s, "<title>Bookmarks</title><h2>Bookmarks</h2>");
    if (!b->n_marks) SB_LIT(s, "<p>None yet. Menu, Add bookmark keeps the page you are on.");
    for (int i = 0; i < b->n_marks; i++) {
        SB_LIT(s, "<p><a href=\"");
        sb_text(s, b->marks[i].url);
        SB_LIT(s, "\">");
        sb_text(s, b->marks[i].title);
        sb_printf(s, "</a> <a href=\"about:bookmark-del?n=%d\">[x]</a>", i);
    }
}

void cyb_about(cyb_t *b, const char *url, bool push);
void cyb_about(cyb_t *b, const char *url, bool push)
{
    char page[32], v[512];
    snprintf(page, sizeof page, "%s", url + 6);
    page[strcspn(page, "?#")] = 0;
    sb_t s = { 0 };
    const char *show = url;
    if (!strcasecmp(page, "home") || !strcasecmp(page, "blank") || !page[0]) home_page(b, &s);
    else if (!strcasecmp(page, "help")) help_page(&s);
    else if (!strcasecmp(page, "settings")) settings_page(b, &s);
    else if (!strcasecmp(page, "bookmarks")) bookmarks_page(b, &s);
    else if (!strcasecmp(page, "set")) {
        if (query_get(url, "images", v, sizeof v)) b->images = atoi(v) != 0;
        if (query_get(url, "big", v, sizeof v)) b->big = atoi(v) != 0;
        if (query_get(url, "home", v, sizeof v)) {
            char u[512];
            if (v[0]) cyb_url_from_input(v, u, sizeof u);
            else snprintf(u, sizeof u, "%s", HOME_URL);
            snprintf(b->home, sizeof b->home, "%s", u);
        }
        save_settings(b);
        say(b, "Saved");
        show = "about:settings";
        settings_page(b, &s);
        push = false;
    } else if (!strcasecmp(page, "bookmark-del")) {
        if (query_get(url, "n", v, sizeof v)) {
            int n = atoi(v);
            if (n >= 0 && n < b->n_marks) {
                memmove(b->marks + n, b->marks + n + 1, (size_t)(b->n_marks - n - 1) * sizeof *b->marks);
                b->n_marks--;
                save_marks(b);
            }
        }
        show = "about:bookmarks";
        bookmarks_page(b, &s);
        push = false;
    } else if (!strcasecmp(page, "wifi")) {
        if (query_get(url, "scan", v, sizeof v) && b->net.wifi_scan) {
            b->net.wifi_scan(b->net.user, NULL, 0);    /* NULL, 0: start a new scan */
        }
        show = "about:wifi";
        wifi_page(b, &s);
    } else if (!strcasecmp(page, "wifi-forget")) {
        if (query_get(url, "ssid", v, sizeof v) && b->net.wifi_forget) b->net.wifi_forget(b->net.user, v);
        show = "about:wifi";
        push = false;
        wifi_page(b, &s);
    } else if (!strcasecmp(page, "wifi-join")) {
        char open[4];
        query_get(url, "ssid", v, sizeof v);
        query_get(url, "open", open, sizeof open);
        if (atoi(open) && b->net.wifi_join) {
            b->net.wifi_join(b->net.user, v, "");
            show = "about:wifi";
            wifi_page(b, &s);
        } else {
            SB_LIT(&s, "<title>Join</title><h2>Join ");
            sb_text(&s, v);
            SB_LIT(&s, "</h2><form action=\"about:wifi-do\"><input type=\"hidden\" name=\"ssid\" value=\"");
            sb_text(&s, v);
            SB_LIT(&s, "\">Password:<br><input type=\"password\" name=\"pw\" size=\"30\"> "
                       "<input type=\"submit\" value=\"Join\"></form>");
        }
    } else if (!strcasecmp(page, "wifi-do")) {
        char pw[80];
        query_get(url, "ssid", v, sizeof v);
        query_get(url, "pw", pw, sizeof pw);
        if (b->net.wifi_join && v[0]) b->net.wifi_join(b->net.user, v, pw);
        show = "about:wifi";
        push = false;
        wifi_page(b, &s);
    } else {
        SB_LIT(&s, "<title>Not found</title><p>There is no such page here.");
    }
    stop_loading(b);
    cyb_doc_free(&b->doc);
    cyb_html_parse(&b->doc, s.p ? s.p : "", s.len, show, CS_LATIN1, false);
    free(s.p);
    int32_t top = 0;
    uint16_t focus = 0;
    if (!push && b->hist_n && !strcmp(b->hist[b->hist_pos].url, show)) {
        top = b->hist[b->hist_pos].top;
        focus = b->hist[b->hist_pos].focus;
    }
    set_doc(b, show, push, top, focus);
}

/* ---- forms ---- */

static void submit(cyb_t *b, int field)
{
    cyb_doc_t *d = &b->doc;
    int form = d->fields[field].form;
    if (form < 0 || (uint32_t)form >= d->n_forms) { say(b, "Not in a form"); return; }
    char *q = calloc(1, 4096);
    if (!q) return;
    for (uint32_t i = 0; i < d->n_fields; i++) {
        const cyb_field_t *f = &d->fields[i];
        if (f->form != form) continue;
        const char *name = d->pool + f->name;
        if (!*name) continue;
        const char *value = f->value;
        char opt[FIELD_VALUE_MAX];
        bool raw = false;
        switch (f->type) {
        case FT_SUBMIT:
            if ((int)i != field) continue;
            break;
        case FT_CHECKBOX:
        case FT_RADIO:
            if (!f->checked) continue;
            if (!*value) value = "on";
            break;
        case FT_SELECT: {
            const char *o = d->pool + f->options;
            opt[0] = 0;
            for (int k = 0; k < f->n_options; k++) {
                if (k == f->chosen) { snprintf(opt, sizeof opt, "%s", o); break; }
                o += strlen(o) + 1;
                o += strlen(o) + 1;
            }
            value = opt;
            break;
        }
        case FT_HIDDEN:
            raw = true;              /* the page's own bytes */
            break;
        default:
            break;
        }
        size_t l = strlen(q);
        if (l + 8 > 4096) break;
        if (l) q[l++] = '&';
        q[l] = 0;
        cyb_url_encode(q, 4096, name, d->utf8);
        strcat(q, "=");
        cyb_url_encode(q, 4096, value, d->utf8 && !raw);
    }
    char url[1024];
    snprintf(url, sizeof url, "%s", d->pool + d->forms[form].action);
    if (d->forms[form].post && strncasecmp(url, "about:", 6)) {
        go(b, url, true, q);
    } else {
        url[strcspn(url, "?#")] = 0;
        size_t l = strlen(url);
        snprintf(url + l, sizeof url - l, "?%s", q);
        go(b, url, true, NULL);
    }
    free(q);
}

static int count_text_fields(const cyb_doc_t *d, int form)
{
    int n = 0;
    for (uint32_t i = 0; i < d->n_fields; i++)
        if (d->fields[i].form == form && (d->fields[i].type == FT_TEXT || d->fields[i].type == FT_PASSWORD)) n++;
    return n;
}

/* ---- the edit line and lists ---- */

static void edit_start(cyb_t *b, int kind, const char *prompt, const char *text, int field)
{
    b->mode = MODE_EDIT;
    b->edit_kind = kind;
    b->edit_field = field;
    snprintf(b->edit_prompt, sizeof b->edit_prompt, "%s", prompt);
    snprintf(b->edit, sizeof b->edit, "%s", text);
    b->edit_len = (int)strlen(b->edit);
    b->edit_cur = b->edit_len;
    b->dirty = true;
}

static void edit_done(cyb_t *b)
{
    b->mode = MODE_PAGE;
    b->dirty = true;
    if (b->edit_kind == EDIT_URL) {
        if (!b->edit_len) return;
        char url[1024];
        cyb_url_from_input(b->edit, url, sizeof url);
        go(b, url, true, NULL);
        return;
    }
    cyb_field_t *f = &b->doc.fields[b->edit_field];
    snprintf(f->value, sizeof f->value, "%s", b->edit);
    relayout(b, true);
    if (f->type != FT_TEXTAREA && count_text_fields(&b->doc, f->form) == 1) submit(b, b->edit_field);
}

static void edit_char(cyb_t *b, int ch)
{
    if (b->edit_len + 1 >= (int)sizeof b->edit) return;
    memmove(b->edit + b->edit_cur + 1, b->edit + b->edit_cur, (size_t)(b->edit_len - b->edit_cur + 1));
    b->edit[b->edit_cur++] = (char)ch;
    b->edit_len++;
    b->dirty = true;
}

enum { M_GO, M_BACK, M_FORWARD, M_RELOAD, M_HOME, M_MARKS, M_ADD_MARK, M_WIFI, M_SETTINGS, M_CLOSE, M_COUNT };
static const char *const menu_items[M_COUNT] = {
    "Go to address", "Back", "Forward", "Reload", "Home page", "Bookmarks", "Add bookmark",
    "Wi-Fi", "Settings", "Close browser",
};

static void list_open(cyb_t *b, int kind, const char *title)
{
    b->mode = MODE_PICK;
    b->list_kind = kind;
    b->list.first = 0;
    snprintf(b->list.title, sizeof b->list.title, "%s", title);
    b->dirty = true;
}

static void menu_open(cyb_t *b)
{
    b->list.n = M_COUNT;
    for (int i = 0; i < M_COUNT; i++) b->list.items[i] = menu_items[i];
    b->list.sel = 0;
    list_open(b, 0, "Menu");
}

static void history_go(cyb_t *b, int dir)
{
    if (b->load == LOAD_PAGE) { stop_loading(b); if (dir < 0) return; }
    int to = b->hist_pos + dir;
    if (to < 0 || to >= b->hist_n) { say(b, dir < 0 ? "No page before" : "No page after"); return; }
    b->hist[b->hist_pos].top = b->top;
    b->hist[b->hist_pos].focus = b->focus;
    b->hist_pos = to;
    char url[1024];
    snprintf(url, sizeof url, "%s", b->hist[to].url);
    go(b, url, false, NULL);
}

static void menu_choose(cyb_t *b, int item)
{
    b->mode = MODE_PAGE;
    b->dirty = true;
    switch (item) {
    case M_GO: edit_start(b, EDIT_URL, "Go", strncmp(b->url, "about:", 6) ? b->url : "", 0); break;
    case M_BACK: history_go(b, -1); break;
    case M_FORWARD: history_go(b, 1); break;
    case M_RELOAD: {
        char url[1024];
        snprintf(url, sizeof url, "%s", b->url);
        if (b->hist_n) { b->hist[b->hist_pos].top = b->top; b->hist[b->hist_pos].focus = b->focus; }
        go(b, url, false, NULL);
        break;
    }
    case M_HOME: go(b, b->home, true, NULL); break;
    case M_MARKS: go(b, "about:bookmarks", true, NULL); break;
    case M_ADD_MARK:
        if (!strncmp(b->url, "about:", 6)) { say(b, "Not this page"); break; }
        if (b->n_marks >= BOOKMARKS) { say(b, "Too many bookmarks"); break; }
        for (int i = 0; i < b->n_marks; i++)
            if (!strcmp(b->marks[i].url, b->url)) { say(b, "Already kept"); return; }
        snprintf(b->marks[b->n_marks].url, sizeof b->marks[0].url, "%s", b->url);
        if (b->doc.title[0]) snprintf(b->marks[b->n_marks].title, sizeof b->marks[0].title, "%s", b->doc.title);
        else cyb_url_host(b->url, b->marks[b->n_marks].title, sizeof b->marks[0].title);
        b->n_marks++;
        save_marks(b);
        say(b, "Bookmark kept");
        break;
    case M_WIFI: go(b, "about:wifi", true, NULL); break;
    case M_SETTINGS: go(b, "about:settings", true, NULL); break;
    case M_CLOSE: b->closed = true; break;
    }
}

static void pick_open(cyb_t *b, int field)
{
    cyb_field_t *f = &b->doc.fields[field];
    const char *o = b->doc.pool + f->options;
    int n = 0;
    for (int i = 0; i < f->n_options && n < (int)(sizeof b->list.items / sizeof b->list.items[0]); i++) {
        o += strlen(o) + 1;
        b->list.items[n++] = o;
        o += strlen(o) + 1;
    }
    b->list.n = n;
    b->list.sel = f->chosen < n ? f->chosen : 0;
    b->pick_field = field;
    list_open(b, 1, b->doc.pool + f->name);
}

/* Enter on the focused link or field */
static void activate(cyb_t *b)
{
    if (!b->focus) return;
    const cyb_target_t *t = &b->doc.targets[b->focus - 1];
    if (t->kind == T_LINK) {
        char url[1024];
        snprintf(url, sizeof url, "%s", b->doc.pool + t->href);
        /* a link to a place on this page */
        size_t base = strcspn(b->url, "#");
        if (!strncmp(url, b->url, base) && url[base] == '#') { say(b, "Same page"); return; }
        go(b, url, true, NULL);
        return;
    }
    cyb_field_t *f = &b->doc.fields[t->field];
    switch (f->type) {
    case FT_SUBMIT: submit(b, t->field); break;
    case FT_CHECKBOX: f->checked = !f->checked; b->dirty = true; break;
    case FT_RADIO:
        for (uint32_t i = 0; i < b->doc.n_fields; i++) {
            cyb_field_t *o = &b->doc.fields[i];
            if (o->type == FT_RADIO && o->form == f->form && !strcmp(b->doc.pool + o->name, b->doc.pool + f->name))
                o->checked = false;
        }
        f->checked = true;
        b->dirty = true;
        break;
    case FT_SELECT: if (f->n_options) pick_open(b, t->field); break;
    default: {
        const char *name = b->doc.pool + f->name;
        char prompt[24];
        snprintf(prompt, sizeof prompt, "%s", f->type == FT_PASSWORD ? "Password" : *name ? name : "Text");
        prompt[0] = (char)toupper((unsigned char)prompt[0]);
        edit_start(b, EDIT_FIELD, prompt, f->value, t->field);
    }
    }
}

/* ---- keys ---- */

static int typed(cyb_t *b, int key)
{
    bool shift = b->shift || b->sticky_shift;
    int ch = cyb_key_char(key, shift, b->fn);
    if (ch) {
        b->sticky_shift = false;
        if (b->shift) b->shift_used = true;
        if (b->fn) b->fn_used = true;
    }
    return ch;
}

static void key_edit(cyb_t *b, int key)
{
    switch (key) {
    case CYB_K_ENTER: edit_done(b); return;
    case CYB_K_ESC: b->mode = MODE_PAGE; b->dirty = true; return;
    case CYB_K_LEFT: if (b->edit_cur > 0) b->edit_cur--; b->dirty = true; return;
    case CYB_K_RIGHT: if (b->edit_cur < b->edit_len) b->edit_cur++; b->dirty = true; return;
    case CYB_K_UP: b->edit_cur = 0; b->dirty = true; return;
    case CYB_K_DOWN: b->edit_cur = b->edit_len; b->dirty = true; return;
    case CYB_K_DEL:
    case CYB_K_BKSP:
        if (b->edit_cur > 0) {
            memmove(b->edit + b->edit_cur - 1, b->edit + b->edit_cur, (size_t)(b->edit_len - b->edit_cur + 1));
            b->edit_cur--;
            b->edit_len--;
            b->dirty = true;
        }
        return;
    }
    int ch = typed(b, key);
    if (ch) edit_char(b, ch);
}

static void key_list(cyb_t *b, int key)
{
    list_t *l = &b->list;
    switch (key) {
    case CYB_K_UP: l->sel = (l->sel + l->n - 1) % l->n; break;
    case CYB_K_DOWN: l->sel = (l->sel + 1) % l->n; break;
    case CYB_K_LEFT: l->sel = l->sel > 5 ? l->sel - 5 : 0; break;
    case CYB_K_RIGHT: l->sel = l->sel + 5 < l->n ? l->sel + 5 : l->n - 1; break;
    case CYB_K_ESC:
    case CYB_K_MENU:
        b->mode = MODE_PAGE;
        break;
    case CYB_K_ENTER:
    case CYB_K_SELECT:
        if (b->list_kind == 0) { menu_choose(b, l->sel); return; }
        b->doc.fields[b->pick_field].chosen = (uint16_t)l->sel;
        b->mode = MODE_PAGE;
        relayout(b, true);
        break;
    }
    b->dirty = true;
}

static void key_page(cyb_t *b, int key)
{
    switch (key) {
    case CYB_K_UP: move_focus(b, -1); return;
    case CYB_K_DOWN:
    case CYB_K_TAB: move_focus(b, 1); return;
    case CYB_K_LEFT: page_scroll(b, -1); return;
    case CYB_K_RIGHT: page_scroll(b, 1); return;
    case CYB_K_ENTER:
    case CYB_K_SELECT: activate(b); return;
    case CYB_K_MENU: menu_open(b); return;
    case CYB_K_HELP: go(b, "about:help", true, NULL); return;
    case CYB_K_DEL:
    case CYB_K_BKSP: history_go(b, -1); return;
    case CYB_K_ESC:
        if (b->load == LOAD_PAGE) { stop_loading(b); say(b, "Stopped"); return; }
        if (b->load == LOAD_IMAGE) {
            b->doc.imgs[b->img].state = IMG_WAIT;
            stop_loading(b);
            b->stop_images = true;
            say(b, "Pictures stopped");
            return;
        }
        if (b->hist_pos > 0) history_go(b, -1);
        else b->closed = true;
        return;
    }
    int ch = typed(b, key);
    if (ch && ch != ' ') {
        edit_start(b, EDIT_URL, "Go", "", 0);
        edit_char(b, ch);
    } else if (ch == ' ') page_scroll(b, 1);
}

static void key_press(cyb_t *b, int key)
{
    switch (b->mode) {
    case MODE_EDIT: key_edit(b, key); break;
    case MODE_PICK: key_list(b, key); break;
    default: key_page(b, key); break;
    }
}

void cyb_key(cyb_t *b, int key, bool down)
{
    if (key == CYB_K_SHIFT) {
        if (down) { b->shift = true; b->shift_used = false; }
        else {
            b->shift = false;
            if (!b->shift_used) b->sticky_shift = !b->sticky_shift;
        }
        b->dirty = true;
        return;
    }
    if (key == CYB_K_FN) {
        b->fn = down;
        b->fn_used = false;
        return;
    }
    if (!down) {
        if (b->held == key) b->held = 0;
        return;
    }
    if (key == CYB_K_UP || key == CYB_K_DOWN || key == CYB_K_LEFT || key == CYB_K_RIGHT ||
        key == CYB_K_DEL || key == CYB_K_BKSP) {
        b->held = key;
        b->held_since = b->held_last = b->now;
    }
    key_press(b, key);
}

void cyb_char(cyb_t *b, int ch)
{
    if (ch < 32 || ch > 255 || ch == 127) return;
    if (b->mode == MODE_EDIT) edit_char(b, ch);
    else if (b->mode == MODE_PAGE && ch != ' ') {
        edit_start(b, EDIT_URL, "Go", "", 0);
        edit_char(b, ch);
    } else if (b->mode == MODE_PAGE) page_scroll(b, 1);
}

/* ---- drawing ---- */

static void draw_bar(cyb_t *b)
{
    uint8_t *fb = b->fb;
    const cyb_font_t *f = &b->fonts[F_SMALL_BOLD];
    fb_fill(fb, 0, 0, CYB_W, BAR_H, BLACK);
    char right[24] = "";
    if (b->load == LOAD_PAGE) snprintf(right, sizeof right, "%uK", (unsigned)(b->buf_len / 1024));
    else if (b->load == LOAD_IMAGE) snprintf(right, sizeof right, "pic %u/%u", (unsigned)b->img + 1, (unsigned)b->doc.n_imgs);
    else if (b->shift || b->sticky_shift) snprintf(right, sizeof right, "ABC");
    else if (b->lay.height > VIEW_H)
        snprintf(right, sizeof right, "%d%%", (int)(100 * (int64_t)b->top / max_top(b)));
    int rw = cyb_text_w(f, right, (int)strlen(right));
    char left[128];
    if (b->status[0] && b->now < b->status_until) snprintf(left, sizeof left, "%s", b->status);
    else if (b->load == LOAD_PAGE) {
        char host[64];
        cyb_url_host(b->load_url, host, sizeof host);
        snprintf(left, sizeof left, "%s", host);
    } else if (b->doc.title[0]) snprintf(left, sizeof left, "%s", b->doc.title);
    else cyb_url_host(b->url, left, sizeof left);
    int n = (int)strlen(left), room = CYB_W - 4 - (rw ? rw + 4 : 0);
    while (n && cyb_text_w(f, left, n) > room) n--;
    cyb_text(fb, f, 2, 0, left, n, WHITE, 0, BAR_H);
    if (rw) cyb_text(fb, f, CYB_W - 2 - rw, 0, right, (int)strlen(right), WHITE, 0, BAR_H);
    if (b->load) {   /* a moving dot while loading */
        int x = (int)(b->now / 120 % 8);
        fb_fill(fb, CYB_W - 2 - rw - 6 + (x > 3 ? 7 - x : x) - 2, 4, 2, 2, LIGHT);
    }
}

static void draw_edit(cyb_t *b)
{
    uint8_t *fb = b->fb;
    const cyb_font_t *f = &b->fonts[b->big ? F_BIG : F_SMALL];
    const cyb_font_t *bf = &b->fonts[F_SMALL_BOLD];
    int h = f->height + 4, y = CYB_H - h - 1;
    fb_fill(fb, 0, y - 1, CYB_W, h + 2, WHITE);
    fb_frame(fb, 0, y, CYB_W, h + 1, BLACK);
    fb_fill(fb, 1, y + 1, CYB_W - 2, 1, LIGHT);
    char prompt[32];
    snprintf(prompt, sizeof prompt, "%s:", b->edit_prompt);
    int pw = cyb_text(fb, bf, 3, y + 2 + (f->height - bf->height), prompt, (int)strlen(prompt), BLACK, y, y + h);
    int x0 = 3 + pw + 3, room = CYB_W - x0 - 4;
    /* scroll the text so the cursor shows */
    char shown[FIELD_VALUE_MAX];
    if (b->doc.n_fields && b->edit_kind == EDIT_FIELD && b->doc.fields[b->edit_field].type == FT_PASSWORD) {
        memset(shown, '*', (size_t)b->edit_len);
        shown[b->edit_len] = 0;
    } else snprintf(shown, sizeof shown, "%s", b->edit);
    int start = 0;
    while (start < b->edit_cur && cyb_text_w(f, shown + start, b->edit_cur - start) > room - 4) start++;
    int end = start;
    while (end < b->edit_len && cyb_text_w(f, shown + start, end + 1 - start) <= room) end++;
    cyb_text(fb, f, x0, y + 2, shown + start, end - start, BLACK, y + 1, y + h);
    int cx = x0 + cyb_text_w(f, shown + start, b->edit_cur - start);
    fb_fill(fb, cx, y + 2, 1, f->height, (b->shift || b->sticky_shift) ? DARK : BLACK);
    if (b->shift || b->sticky_shift) fb_fill(fb, cx + 1, y + 2, 1, f->height, DARK);
}

static void draw_list(cyb_t *b)
{
    uint8_t *fb = b->fb;
    list_t *l = &b->list;
    const cyb_font_t *f = &b->fonts[F_SMALL];
    int row = f->height;
    int w = 40;
    for (int i = 0; i < l->n; i++) {
        int iw = cyb_text_w(f, l->items[i], (int)strlen(l->items[i]));
        if (iw > w) w = iw;
    }
    w += 8;
    if (w > CYB_W - 8) w = CYB_W - 8;
    int show = l->n, max = (VIEW_H - 4) / row;
    if (show > max) show = max;
    if (l->sel < l->first) l->first = l->sel;
    if (l->sel >= l->first + show) l->first = l->sel - show + 1;
    int h = show * row + 4;
    int x = b->list_kind == 0 ? CYB_W - w - 2 : (CYB_W - w) / 2;
    int y = VIEW_Y + (VIEW_H - h) / 2;
    if (b->list_kind == 0) y = VIEW_Y;
    fb_fill(fb, x - 1, y - 1, w + 3, h + 3, WHITE);
    fb_fill(fb, x + 2, y + h, w, 2, DARK);      /* a shadow */
    fb_fill(fb, x + w, y + 2, 2, h, DARK);
    fb_frame(fb, x, y, w, h, BLACK);
    for (int i = 0; i < show; i++) {
        int k = l->first + i;
        int ty = y + 2 + i * row;
        int len = (int)strlen(l->items[k]);
        while (len && cyb_text_w(f, l->items[k], len) > w - 6) len--;
        if (k == l->sel) {
            fb_fill(fb, x + 1, ty, w - 2, row, BLACK);
            cyb_text(fb, f, x + 3, ty, l->items[k], len, WHITE, ty, ty + row);
        } else cyb_text(fb, f, x + 3, ty, l->items[k], len, BLACK, ty, ty + row);
    }
    if (l->first > 0) fb_fill(fb, x + w - 4, y + 1, 2, 2, DARK);
    if (l->first + show < l->n) fb_fill(fb, x + w - 4, y + h - 3, 2, 2, DARK);
}

bool cyb_render(cyb_t *b, uint8_t *out)
{
    if (!b->dirty) return false;
    b->dirty = false;
    cyb_draw_page(b->fb, &b->lay, &b->doc, b->fonts, b->top, b->mode == MODE_PAGE || b->mode == MODE_PICK ? b->focus : 0);
    draw_bar(b);
    if (b->mode == MODE_EDIT) draw_edit(b);
    if (b->mode == MODE_PICK) draw_list(b);
    memcpy(out, b->fb, CYB_FB_SIZE);
    return true;
}

/* ---- the life of it ---- */

bool cyb_step(cyb_t *b, uint32_t now)
{
    uint32_t was = b->now;
    b->now = now;
    if (b->closed) return false;
    if (b->status[0] && was < b->status_until && now >= b->status_until) b->dirty = true;
    /* key repeat */
    if (b->held && now - b->held_since > REPEAT_DELAY && now - b->held_last > REPEAT_EVERY) {
        b->held_last = now;
        key_press(b, b->held);
    }
    if (b->load == LOAD_PAGE) {
        size_t before = b->buf_len;
        int r = pump(b, PAGE_MAX);
        if (b->buf_len / 1024 != before / 1024) b->dirty = true;
        if (r == CYB_EOF) page_done(b);
        else if (r == CYB_FAIL) {
            const char *why = b->net.error ? b->net.error(b->net.user) : NULL;
            char w[160];
            snprintf(w, sizeof w, "%s", why ? why : "");
            error_page(b, b->load_url, w);
        }
    } else if (b->load == LOAD_IMAGE) {
        int r = pump(b, CYB_IMG_MAX_BYTES);
        if (r == CYB_EOF) image_done(b, b->buf_len < CYB_IMG_MAX_BYTES);
        else if (r == CYB_FAIL) image_done(b, false);
        if (r) b->dirty = true;
    }
    if (b->load == LOAD_NONE) next_image(b);
    if (b->load) {
        static uint32_t last_anim;
        if (now - last_anim > 120) { last_anim = now; b->dirty = true; }
    }
    /* the Wi-Fi page fills in when the scan is done */
    if (b->wifi_scanning && !b->load && b->mode == MODE_PAGE && !strcmp(b->url, "about:wifi")) {
        cyb_ap_t ap;
        if (b->net.wifi_scan(b->net.user, &ap, 0) >= 0) {
            b->wifi_scanning = false;
            int32_t top = b->top;
            cyb_about(b, "about:wifi", false);
            scroll_to(b, top);
        }
    }
    return !b->closed;
}

bool cyb_busy(const cyb_t *b) { return b->load != LOAD_NONE; }

void cyb_go(cyb_t *b, const char *url)
{
    char u[1024];
    cyb_url_from_input(url, u, sizeof u);
    b->shown = true;
    b->closed = false;
    go(b, u, true, NULL);
}

void cyb_show(cyb_t *b)
{
    b->closed = false;
    b->mode = MODE_PAGE;
    b->held = 0;
    b->shift = b->fn = b->sticky_shift = false;
    b->dirty = true;
    if (!b->shown) {
        b->shown = true;
        go(b, b->home, true, NULL);
    }
}

cyb_t *cyb_create(const cyb_net_t *net, const uint8_t *flash, size_t flash_len, const char *data_dir)
{
    cyb_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->net = *net;
    if (data_dir) snprintf(b->data_dir, sizeof b->data_dir, "%s", data_dir);
    cyb_fonts_load(b->fonts, flash, flash_len);
    load_settings(b);
    b->dirty = true;
    return b;
}

void cyb_destroy(cyb_t *b)
{
    if (!b) return;
    stop_loading(b);
    cyb_doc_free(&b->doc);
    cyb_layout_free(&b->lay);
    cyb_fonts_free(b->fonts);
    free(b->buf);
    free(b);
}

/* For tests: the page's boxes. */
void cyb_dump(const cyb_t *b, FILE *f)
{
    fprintf(f, "%s top %d height %d focus %u\n", b->url, (int)b->top, (int)b->lay.height, b->focus);
    for (uint32_t i = 0; i < b->lay.n; i++) {
        const cyb_box_t *x = &b->lay.boxes[i];
        fprintf(f, "  y%-4d x%-3d w%-3d h%-3d kind %d target %u '%.*s'\n", (int)x->y, x->x, x->w, x->h, x->kind,
                x->target, x->kind == B_TEXT ? x->len : 0, b->doc.pool + x->text);
    }
}
