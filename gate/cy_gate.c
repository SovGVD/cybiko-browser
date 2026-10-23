/* The virtual CyWIG (see cy_gate.h). */
#include "cy_gate.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cy_page.h"
#include "cy_wml.h"

#define SHORT 50
#define LONG 200
#define SHORT_DATA 30
#define LONG_DATA 120
#define HEAD 16

#define T_BEACON 0x00
#define T_MSG 0x20
#define T_ACK 0x40
#define T_QUERY 0x60
#define F_ACK_ME 0x10
#define F_CYWIG 0x04

#define ACK_PART 0
#define ACK_GOT 2
#define ACK_DELIVERED 4
#define ACK_NOBODY 5

/* message flags (first data byte) */
#define M_P0 0x08
#define M_P1 0x04
#define M_DATA 0x10
#define M_NAME 0x20                 /* "to" is a process name */

#define MSG_WAP 0x1022
#define WAP_PORT 0x93
#define WAP_PROCESS "WAP"
#define WAP_GET 1
#define WAP_POST 7                  /* (guessed: the browser's other request) */
#define WAP_PART 6
#define WAP_ERROR 5
/* The Browser application (cyapp/) asks the same way with p0 = APP_GET
 * (APP_BIG added for the bigger font) and gets the page laid out
 * (cy_page.h) in messages to its process, numbered as the WAP browser's. */
#define APP_GET 0x21
#define APP_BIG 0x100
#define APP_PICS 0x200              /* with the part's pictures */
#define PIC_MAX_W 120               /* the biggest picture sent */
#define PIC_MAX_H 80
#define PICS_MAX 6                  /* pictures fetched for a part */
#define PIC_MS 15000                /* the longest a picture may take */
#define APP_PROCESS "Browser"

#define BEACON_MS 3000
#define RETRY_MS 400
#define TRIES 10
#define MAX_PAGE (256 * 1024)       /* biggest web page fetched */
#define FETCH_MS 45000              /* the WAP browser gives up after 60 s */
#define ALIASES 512                 /* short URLs handed out */
#define ALIAS_HOST "http://c/"
#define LONG_URL 48                 /* longer links get a short URL */

/* The page shown for the Cybiko's own home page, long gone. */
#define HOME_URL "http://www.cybiko.com/wap/index.asp"
static const char home_html[] =
    "<title>CyWIG</title><b>Cybiko Internet Gate</b><br>"
    "<form action=\"http://frogfind.com/\">Search the web:<br><input name=\"q\" size=\"14\">"
    "<input type=\"submit\" value=\"Search\"></form>"
    "<a href=\"http://68k.news/\">68k.news</a><br>"
    "<a href=\"http://lite.cnn.com/\">CNN Lite</a><br>"
    "<a href=\"http://text.npr.org/\">NPR Text</a><br>"
    "<a href=\"http://wiby.me/\">Wiby search</a><br>"
    "<a href=\"http://wttr.in/?T0\">Weather</a><br>"
    "<a href=\"http://c/wifi\">Wi-Fi setup</a><br>";

enum { IDLE, FETCH, SEND, WIFI, PICS };

#define WIFI_SCAN_MS 8000           /* longest wait for a scan */

struct cy_gate {
    uint32_t id;
    cy_gate_host_t host;
    cyb_net_t net;
    uint32_t now;
    uint32_t next_beacon;
    uint16_t beacons;
    uint8_t channel;

    /* the request being answered */
    int state;
    uint32_t to;                    /* the Cybiko asking */
    bool app;                       /* the Browser application, not WAP */
    bool big;                       /* it wants the bigger font */
    bool pics;                      /* and the pictures */
    uint16_t pic[PICS_MAX];         /* the pictures being fetched */
    int n_pics, at_pic;
    uint16_t number;                /* its request number: the next part's */
    char url[1024];
    uint32_t fetch_until;
    uint8_t *buf;                   /* the web page, then the WML */
    size_t buf_len, buf_cap;
    char *wml;
    size_t wml_len, wml_sent;
    bool error;                     /* the answer is an error code */
    uint16_t status;
    cy_wml_pos_t start;             /* the part of the page asked for */
    int32_t top;                    /* the same for the application */

    /* the message part in the air */
    uint8_t tx[LONG];
    bool tx_busy, tx_query;
    uint8_t tx_number;              /* header byte 10 */
    uint8_t tries;
    uint32_t tx_at;

    /* message parts heard, to answer repeats without acting twice */
    struct { uint32_t from; uint16_t crc; uint8_t number; } seen[8];
    int seen_next;

    /* the last page, kept for its further parts */
    cyb_doc_t doc;
    char doc_url[1024];
    bool have_doc;
    cyb_layout_t lay;               /* the page laid out for the application */
    bool lay_ok, lay_big;
    cyb_font_t fonts[F_COUNT];

    /* the Wi-Fi page waits for a scan or for joining a network */
    uint32_t wifi_until;
    char wifi_note[96];

    char home[256];

    /* a message coming in parts */
    uint8_t lp[1024];
    size_t lp_len;
    uint32_t lp_from;
    unsigned lp_next;
    char *alias[ALIASES];
    uint32_t alias_next;            /* the next short URL's number */
};

static void logf_(cy_gate_t *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(cy_gate_t *g, const char *fmt, ...)
{
    if (!g->host.log) return;
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g->host.log(g->host.user, line);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

/* ---- Reed-Solomon over GF(256), x^8+x^4+x^3+x^2+1 (the boot ROM's tables) ----
 * A packet is data then parity: 30 + 20 bytes (short) or 120 + 80 (long).
 * As a codeword the parity bytes are the coefficients of x^0..x^(n-1) and
 * data byte i that of x^(n+i); its roots are a^1..a^n. */
static uint8_t gf_exp[512], gf_log[256];
static uint8_t gen20[21], gen80[81];

static uint8_t gf_mul(uint8_t a, uint8_t b) { return a && b ? gf_exp[gf_log[a] + gf_log[b]] : 0; }

static void rs_gen(uint8_t *g, int n)
{
    memset(g, 0, (size_t)n + 1);
    g[0] = 1;
    for (int j = 1; j <= n; j++) {
        const uint8_t r = gf_exp[j];
        for (int i = j; i > 0; i--) g[i] = (uint8_t)(g[i - 1] ^ gf_mul(g[i], r));
        g[0] = gf_mul(g[0], r);
    }
}

static void rs_init(void)
{
    if (gf_exp[0]) return;
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = gf_exp[i + 255] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    rs_gen(gen20, 20);
    rs_gen(gen80, 80);
}

static void rs_parity(uint8_t *pkt, size_t len)
{
    const int k = len == SHORT ? SHORT_DATA : LONG_DATA, n = (int)len - k;
    const uint8_t *g = len == SHORT ? gen20 : gen80;
    uint8_t r[80] = { 0 };
    for (int d = k - 1; d >= 0; d--) {
        const uint8_t fb = pkt[d] ^ r[n - 1];
        memmove(r + 1, r, (size_t)n - 1);
        r[0] = 0;
        if (fb)
            for (int i = 0; i < n; i++) r[i] ^= gf_mul(fb, g[i]);
    }
    memcpy(pkt + k, r, (size_t)n);
}

/* CRC-16/MODBUS */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xffff;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c & 1 ? (uint16_t)((c >> 1) ^ 0xa001) : (uint16_t)(c >> 1);
    }
    return c;
}

void cy_gate_seal(uint8_t *pkt, size_t len)
{
    rs_init();
    put16(pkt + 12, crc16(pkt + HEAD, (len == SHORT ? SHORT_DATA : LONG_DATA) - HEAD));
    put16(pkt + 14, crc16(pkt, 14));
    rs_parity(pkt, len);
}

/* ---- sending ---- */

static void send(cy_gate_t *g, uint8_t *pkt, size_t len)
{
    cy_gate_seal(pkt, len);
    g->host.rf_send(g->host.user, pkt, len, g->channel);
}

static void header(cy_gate_t *g, uint8_t *pkt, size_t len, uint32_t to, uint8_t type, uint8_t part, uint8_t number)
{
    memset(pkt, 0, len);
    put32(pkt, to);
    put32(pkt + 4, g->id);
    pkt[8] = (uint8_t)(0xc0 | g->channel);
    pkt[9] = type | F_CYWIG;         /* on every packet: a Cybiko that hears one
                                        without it stops taking us for a gate */
    pkt[10] = part;
    pkt[11] = number;
}

static void ack(cy_gate_t *g, const uint8_t *in, int code)
{
    uint8_t pkt[SHORT];
    header(g, pkt, SHORT, get32(in + 4), T_ACK, in[10], in[11]);
    pkt[HEAD] = (uint8_t)(code << 4);
    send(g, pkt, SHORT);
}

static void beacon(cy_gate_t *g)
{
    uint8_t pkt[SHORT];
    header(g, pkt, SHORT, 0xffffffffu, T_BEACON, (uint8_t)(g->beacons >> 8), (uint8_t)g->beacons);
    g->beacons++;
    send(g, pkt, SHORT);
}

/* The next part of the answer as a message to the WAP browser. */
static const char *process(const cy_gate_t *g) { return g->app ? APP_PROCESS : WAP_PROCESS; }

/* The most of the page one message part takes. */
static size_t part_room(const cy_gate_t *g)
{
    return LONG_DATA - HEAD - (2 + 2 + 4 + 4 + strlen(process(g)) + 1 + 1);
}

static void send_part(cy_gate_t *g)
{
    uint8_t *m = g->tx + HEAD;
    if (++g->tx_number == 0) g->tx_number = 1;
    /* Message number 0 is delivered right away and acknowledged with code 0;
     * others get "got it" first and need a query for "delivered", which takes
     * twice as long. Byte 10 then tells the acknowledgements apart. The WAP
     * browser drops a part that comes twice (its number is past). */
    header(g, g->tx, LONG, g->to, T_MSG | F_ACK_ME, g->tx_number, 0);
    size_t n = 0;
    m[n++] = M_P0 | M_P1 | M_NAME | (g->error ? 0 : M_DATA);
    m[n++] = 0;
    put16(m + n, MSG_WAP); n += 2;
    size_t part = 0;
    if (g->error) {
        put32(m + n, (uint32_t)g->number << 16 | WAP_ERROR); n += 4;
        put32(m + n, g->status); n += 4;
    } else {
        part = g->wml_len - g->wml_sent;
        if (part > part_room(g)) part = part_room(g);
        put32(m + n, (uint32_t)g->number << 16 | WAP_PART); n += 4;
        put32(m + n, (uint32_t)g->wml_len << 16 | (uint32_t)part); n += 4;
    }
    memcpy(m + n, process(g), strlen(process(g)) + 1); n += strlen(process(g)) + 1;
    if (!g->error) {
        m[n++] = (uint8_t)part;
        memcpy(m + n, g->wml + g->wml_sent, part);
    }
    g->tx_busy = true;
    g->tx_query = false;
    g->tries = 0;
    g->tx_at = g->now;
}

static void tx_again(cy_gate_t *g)
{
    if (g->tx_query) {
        uint8_t q[SHORT];
        header(g, q, SHORT, g->to, T_QUERY, g->tx_number, 0);
        send(g, q, SHORT);
    } else {
        uint8_t copy[LONG];
        memcpy(copy, g->tx, LONG);
        send(g, copy, LONG);
    }
    g->tries++;
    g->tx_at = g->now + RETRY_MS;
}

static void finish(cy_gate_t *g)
{
    if (g->state == FETCH || g->state == PICS) g->net.close(g->net.user);
    g->state = IDLE;
    g->tx_busy = false;
    free(g->wml);
    g->wml = NULL;
    g->buf_len = 0;
}

/* A part went through: on to the next, or done. */
static void part_delivered(cy_gate_t *g)
{
    g->tx_busy = false;
    if (g->error) { finish(g); return; }
    size_t sent = g->wml_len - g->wml_sent;
    g->wml_sent += sent > part_room(g) ? part_room(g) : sent;
    if (++g->number == 0) g->number = 1;
    if (g->wml_sent >= g->wml_len) {
        logf_(g, "page sent to %08x", (unsigned)g->to);
        finish(g);
    } else {
        send_part(g);
    }
}

static void answer(cy_gate_t *g, char *wml, size_t len)
{
    free(g->wml);
    g->wml = wml;
    g->wml_len = len > CY_WML_MAX && !g->app ? CY_WML_MAX : len;
    g->wml_sent = 0;
    g->error = false;
    g->state = SEND;
    logf_(g, "sending %u bytes of %s to %08x", (unsigned)g->wml_len, g->app ? "page" : "WML", (unsigned)g->to);
    send_part(g);
}

static void app_text(cy_gate_t *g, const char *title, const char *text);

static void answer_text(cy_gate_t *g, const char *title, const char *text)
{
    if (g->app) { app_text(g, title, text); return; }
    char *w = malloc(CY_WML_MAX + 1);
    if (!w) { finish(g); return; }
    answer(g, w, cy_wml_text(w, CY_WML_MAX, title, text));
}

/* ---- short URLs ---- */

static uint32_t alias_add(cy_gate_t *g, const char *kind, const char *text)
{
    uint32_t n = ++g->alias_next;
    char **slot = &g->alias[n % ALIASES];
    free(*slot);
    size_t kl = strlen(kind), tl = strlen(text);
    *slot = malloc(kl + tl + 1);
    if (!*slot) return 0;
    memcpy(*slot, kind, kl);
    memcpy(*slot + kl, text, tl + 1);
    return n;
}

/* The text behind short URL n (its first byte says what it is), or NULL. */
static const char *alias_get(cy_gate_t *g, uint32_t n)
{
    if (!n || n > g->alias_next || g->alias_next - n >= ALIASES) return NULL;
    return g->alias[n % ALIASES];
}

static void link_url(void *user, const char *url, char *out, size_t cap)
{
    cy_gate_t *g = user;
    uint32_t n;
    if (strlen(url) <= LONG_URL || strlen(url) >= cap || !(n = alias_add(g, "U", url)))
        snprintf(out, cap, "%s", strlen(url) < cap ? url : "about:blank");
    else
        snprintf(out, cap, ALIAS_HOST "%u", (unsigned)n);
}

/* A form: "F" or "P" (post), the action, then a line for each field the
 * WAP browser sends ("v12 name"), a fixed one ("=name value") for hidden
 * fields and the submit button pressed. */
static void form_url(void *user, const cyb_doc_t *d, int form, int submit, char *out, size_t cap)
{
    cy_gate_t *g = user;
    size_t size = 1024, len = 0;
    char *s = malloc(size);
    if (!s) { snprintf(out, cap, "about:blank"); return; }
#define ADD(...) do { int k_ = snprintf(s + len, size - len, __VA_ARGS__); \
        if (k_ > 0) len = len + (size_t)k_ < size ? len + (size_t)k_ : size - 1; } while (0)
    ADD("%s\n", d->pool + d->forms[form].action);
    for (uint32_t i = 0; i < d->n_fields; i++) {
        const cyb_field_t *f = &d->fields[i];
        const char *name = d->pool + f->name;
        if (f->form != form || !*name || f->type == FT_RESET) continue;
        if (f->type == FT_RADIO) {
            /* the group is one select, named after its first button */
            bool first = true;
            for (uint32_t k = 0; k < i && first; k++)
                if (d->fields[k].type == FT_RADIO && d->fields[k].form == form && !strcmp(d->pool + d->fields[k].name, name))
                    first = false;
            if (!first) continue;
        }
        if (f->type == FT_HIDDEN) ADD("=%s\n%s\n", name, f->value);
        else if (f->type == FT_SUBMIT) { if ((int)i == submit) ADD("=%s\n%s\n", name, f->value); }
        else ADD("v%u\n%s\n", (unsigned)i, name);
    }
#undef ADD
    uint32_t n = alias_add(g, d->forms[form].post ? "P" : "F", s);
    free(s);
    snprintf(out, cap, ALIAS_HOST "f%u", (unsigned)n);
}

static void more_url(void *user, cy_wml_pos_t pos, char *out, size_t cap)
{
    cy_gate_t *g = user;
    char s[1100];
    snprintf(s, sizeof s, "%u %u %s", (unsigned)pos.item, (unsigned)pos.off, g->doc_url);
    snprintf(out, cap, ALIAS_HOST "p%u", (unsigned)alias_add(g, "M", s));
}

static void part_url(void *user, int32_t y, char *out, size_t cap)
{
    cy_gate_t *g = user;
    char s[1100];
    snprintf(s, sizeof s, "%ld %s", (long)y, g->doc_url);
    snprintf(out, cap, ALIAS_HOST "y%u", (unsigned)alias_add(g, "Y", s));
}

/* The value of v<n> in a query the WAP browser made ("v3=a&v5=b"). */
static bool query_value(const char *q, const char *var, char *out, size_t cap)
{
    size_t vl = strlen(var);
    while (q && *q) {
        if (!strncmp(q, var, vl) && q[vl] == '=') {
            q += vl + 1;
            size_t n = strcspn(q, "&");
            if (n >= cap) n = cap - 1;
            memcpy(out, q, n);
            out[n] = 0;
            cyb_url_decode(out);
            return true;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
    return false;
}

/* ---- fetching ---- */

static void wifi_page(cy_gate_t *g);

/* No way online: no network saved yet, or Wi-Fi not connected after a
 * failed fetch. Then the Wi-Fi page is shown instead, with why. */
static bool offline_wifi(cy_gate_t *g, const char *why)
{
    if (!g->net.wifi_status) return false;
    char st[8];
    if (g->net.wifi_status(g->net.user, st, sizeof st) > 0) return false;
    if (g->net.wifi_scan) g->net.wifi_scan(g->net.user, NULL, 0);     /* fresh list */
    snprintf(g->wifi_note, sizeof g->wifi_note, "%s", why);
    g->state = WIFI;
    g->wifi_until = g->now + WIFI_SCAN_MS;
    return true;
}

static bool no_networks(cy_gate_t *g)
{
    char names[1][33];
    return g->net.wifi_saved && g->net.wifi_saved(g->net.user, names, 1) == 0;
}

static bool own_page(cy_gate_t *g, const char *url);

/* Fetches url from the web, or answers with one of the gate's own pages. */
static void start_fetch(cy_gate_t *g, const char *url, const char *body)
{
    if (own_page(g, url)) return;
    if (no_networks(g) && offline_wifi(g, "No Wi-Fi network yet: pick one.")) return;
    snprintf(g->url, sizeof g->url, "%s", url);
    g->buf_len = 0;
    g->start = (cy_wml_pos_t){ 0, 0 };
    g->top = 0;
    logf_(g, "fetch %s%s", url, body ? " (post)" : "");
    if (!g->net.open || !g->net.open(g->net.user, url, body, body ? "application/x-www-form-urlencoded" : NULL)) {
        const char *why = g->net.error ? g->net.error(g->net.user) : NULL;
        if (offline_wifi(g, why && *why ? why : "Not online.")) return;
        answer_text(g, "Error", why && *why ? why : "No connection to the Internet.");
        return;
    }
    g->state = FETCH;
    g->fetch_until = g->now + FETCH_MS;
}

static void form_submit(cy_gate_t *g, const char *spec, bool post, const char *query)
{
    char *q = calloc(1, 4096);
    if (!q) return;
    const char *line = strchr(spec, '\n');
    char action[1024];
    size_t al = line ? (size_t)(line - spec) : strlen(spec);
    snprintf(action, sizeof action, "%.*s", (int)al, spec);
    while (line && line[1]) {
        const char *a = line + 1, *b = strchr(a, '\n');
        if (!b) break;
        const char *c = strchr(b + 1, '\n');
        if (!c) break;
        char name[256], value[FIELD_VALUE_MAX];
        snprintf(name, sizeof name, "%.*s", (int)(c - b - 1), b + 1);
        if (*a == '=') {
            snprintf(value, sizeof value, "%.*s", (int)(b - a - 1), a + 1);
            /* "=name\nvalue": name first, then the value */
            char t[256];
            snprintf(t, sizeof t, "%s", value);
            snprintf(value, sizeof value, "%s", name);
            snprintf(name, sizeof name, "%s", t);
        } else {
            char var[16];
            snprintf(var, sizeof var, "%.*s", (int)(b - a), a);
            if (!query_value(query, var, value, sizeof value)) value[0] = 0;
        }
        size_t l = strlen(q);
        if (l + 8 > 4096) break;
        if (l) { q[l++] = '&'; q[l] = 0; }
        cyb_url_encode(q, 4096, name, false);
        strcat(q, "=");
        cyb_url_encode(q, 4096, value, false);
        line = c;
    }
    if (post) {
        start_fetch(g, action, q);
    } else {
        action[strcspn(action, "?#")] = 0;
        size_t l = strlen(action);
        snprintf(action + l, sizeof action - l, "?%s", q);
        start_fetch(g, action, NULL);
    }
    free(q);
}

static bool type_is(const char *ct, const char *what)
{
    return ct && !strncasecmp(ct, what, strlen(what));
}

/* A page laid out for the application, from y = top on. */
static void send_layout(cy_gate_t *g, const cyb_doc_t *d, const cyb_layout_t *l, int32_t top)
{
    const size_t cap = g->pics ? CY_PAGE_PICS_MAX : CY_PAGE_MAX;
    uint8_t *w = malloc(cap);
    if (!w) { finish(g); return; }
    cy_page_links_t k = { g, link_url, form_url, part_url };
    answer(g, (char *)w, cy_page_part(d, l, &top, w, cap, &k));
}

static void lay_out(cy_gate_t *g)
{
    if (g->lay_ok && g->lay_big == g->big) return;
    cyb_layout(&g->lay, &g->doc, g->fonts, g->big);
    g->lay_ok = true;
    g->lay_big = g->big;
}

/* The pictures of the part from g->top on that are still to be fetched:
 * true if there are any (then they are fetched first, in PICS). */
static bool want_pics(cy_gate_t *g)
{
    uint8_t *tmp = malloc(CY_PAGE_MAX);
    if (!tmp) return false;
    /* where the part would end without them */
    int32_t end = g->top;
    cy_page_links_t k = { g, link_url, form_url, part_url };
    cy_page_part(&g->doc, &g->lay, &end, tmp, CY_PAGE_MAX, &k);
    free(tmp);
    g->n_pics = 0;
    for (uint32_t i = cyb_layout_find(&g->lay, g->top); i < g->lay.n && g->n_pics < PICS_MAX; i++) {
        const cyb_box_t *b = &g->lay.boxes[i];
        if (b->y >= end && end < g->lay.height) break;
        if ((b->kind != B_IMG && b->kind != B_ALT) || b->ref < 0 || (uint32_t)b->ref >= g->doc.n_imgs) continue;
        if (g->doc.imgs[b->ref].state != IMG_WAIT) continue;
        bool dup = false;
        for (int j = 0; j < g->n_pics; j++) dup |= g->pic[j] == (uint16_t)b->ref;
        if (!dup) g->pic[g->n_pics++] = (uint16_t)b->ref;
    }
    g->at_pic = -1;
    return g->n_pics > 0;
}

/* The page from g->start (g->top) on, as WML (or laid out). */
static void send_doc(cy_gate_t *g)
{
    if (g->app) {
        lay_out(g);
        if (g->pics && want_pics(g)) {
            g->state = PICS;
            logf_(g, "fetching %d pictures", g->n_pics);
            return;
        }
        send_layout(g, &g->doc, &g->lay, g->top);
        return;
    }
    char *w = malloc(CY_WML_MAX + 1);
    if (!w) { finish(g); return; }
    cy_wml_links_t l = { g, link_url, form_url, more_url };
    cy_wml_pos_t pos = g->start;
    answer(g, w, cy_wml_page(&g->doc, &pos, w, CY_WML_MAX, &l));
}

/* A message for the application: a page of its own, so the page kept for
 * its further parts stays. */
static void app_text(cy_gate_t *g, const char *title, const char *text)
{
    cyb_doc_t d = { 0 };
    cyb_layout_t l = { 0 };
    cyb_html_parse(&d, text, strlen(text), ALIAS_HOST, CS_LATIN1, true);
    snprintf(d.title, sizeof d.title, "%s", title);
    cyb_layout(&l, &d, g->fonts, g->big);
    send_layout(g, &d, &l, 0);
    cyb_layout_free(&l);
    cyb_doc_free(&d);
}

/* One of the gate's own pages (Latin-1 HTML), shown as a web page is. */
static void show_html(cy_gate_t *g, const char *html, const char *url)
{
    cyb_doc_free(&g->doc);
    cyb_html_parse(&g->doc, html, strlen(html), url, CS_LATIN1, false);
    snprintf(g->doc_url, sizeof g->doc_url, "%s", url);
    g->have_doc = true;
    g->lay_ok = false;
    g->start = (cy_wml_pos_t){ 0, 0 };
    g->top = 0;
    send_doc(g);
}

static void fetched(cy_gate_t *g)
{
    const char *final = g->net.final_url ? g->net.final_url(g->net.user) : NULL;
    const char *ct = g->net.content_type ? g->net.content_type(g->net.user) : NULL;
    char type[96] = "";
    if (final && *final) snprintf(g->url, sizeof g->url, "%s", final);
    if (ct) snprintf(type, sizeof type, "%s", ct);
    g->net.close(g->net.user);
    g->state = IDLE;
    logf_(g, "got %u bytes, %s", (unsigned)g->buf_len, type[0] ? type : "no type");
    if (type_is(type, "text/vnd.wap.wml") && !g->app) {
        /* a WAP page already */
        char *w = malloc(g->buf_len + 1);
        if (!w) { finish(g); return; }
        memcpy(w, g->buf, g->buf_len);
        w[g->buf_len] = 0;
        answer(g, w, g->buf_len);
        return;
    }
    if (type[0] && !type_is(type, "text/") && !type_is(type, "application/xhtml") && !type_is(type, "application/xml")) {
        char why[160];
        snprintf(why, sizeof why, "This is a %s file, which can't be shown.", type);
        answer_text(g, "Can't show", why);
        return;
    }
    int cs = -1;
    const char *c = strcasestr(type, "charset=");
    if (c) {
        char name[24];
        snprintf(name, sizeof name, "%s", c + 8);
        name[strcspn(name, " ;\"'")] = 0;
        cs = cyb_charset(name);
    }
    cyb_doc_free(&g->doc);
    cyb_html_parse(&g->doc, (const char *)g->buf, g->buf_len, g->url, cs, type_is(type, "text/plain"));
    snprintf(g->doc_url, sizeof g->doc_url, "%s", g->url);
    g->have_doc = true;
    g->lay_ok = false;
    if (!g->doc.title[0]) cyb_url_host(g->url, g->doc.title, sizeof g->doc.title);
    send_doc(g);
}

/* The pictures for the application: one after another, scaled to at most
 * PIC_MAX_W x PIC_MAX_H, then the part with them. */
static void pic_done(cy_gate_t *g, bool ok)
{
    g->net.close(g->net.user);
    cyb_img_t *im = &g->doc.imgs[g->pic[g->at_pic]];
    im->state = IMG_FAILED;
    if (ok) {
        int max_w = PIC_MAX_W, max_h = PIC_MAX_H;
        if (im->want_w && im->want_w < max_w) max_w = im->want_w;
        if (im->want_w && im->want_h) {
            int h = im->want_h * max_w / im->want_w;
            if (h > 0 && h < max_h) max_h = h;
        }
        if (cyb_image_decode(g->buf, g->buf_len, max_w, max_h, &im->pic)) im->state = IMG_DONE;
    }
    logf_(g, "picture %s: %s", g->doc.pool + im->src, im->state == IMG_DONE ? "ok" : "failed");
    g->buf_len = 0;
}

static void pics_step(cy_gate_t *g)
{
    if (g->at_pic >= 0) {
        for (int k = 0; k < 16; k++) {
            if (g->buf_len + 4096 + 1 > g->buf_cap) {
                size_t cap = g->buf_cap ? g->buf_cap * 2 : 32768;
                uint8_t *p = cap <= CYB_IMG_MAX_BYTES ? realloc(g->buf, cap + 1) : NULL;
                if (!p) { pic_done(g, false); goto next; }
                g->buf = p;
                g->buf_cap = cap;
            }
            int n = g->net.read(g->net.user, g->buf + g->buf_len, 4096);
            if (n == CYB_EOF) { pic_done(g, true); goto next; }
            if (n == CYB_FAIL) { pic_done(g, false); goto next; }
            if (n <= 0) break;
            g->buf_len += (size_t)n;
        }
        if ((int32_t)(g->now - g->fetch_until) >= 0) pic_done(g, false);
        else return;
    }
next:
    while (++g->at_pic < g->n_pics) {
        cyb_img_t *im = &g->doc.imgs[g->pic[g->at_pic]];
        g->buf_len = 0;
        if (g->net.open && g->net.open(g->net.user, g->doc.pool + im->src, NULL, NULL)) {
            im->state = IMG_LOADING;
            g->fetch_until = g->now + PIC_MS;
            return;
        }
        im->state = IMG_FAILED;
    }
    g->state = IDLE;
    g->lay_ok = false;
    lay_out(g);
    send_layout(g, &g->doc, &g->lay, g->top);
}

static void fetch_step(cy_gate_t *g)
{
    for (int k = 0; k < 16; k++) {
        if (g->buf_len + 4096 + 1 > g->buf_cap) {
            if (g->buf_cap >= MAX_PAGE) { fetched(g); return; }
            size_t cap = g->buf_cap ? g->buf_cap * 2 : 32768;
            uint8_t *p = realloc(g->buf, cap + 1);
            if (!p) { fetched(g); return; }
            g->buf = p;
            g->buf_cap = cap;
        }
        int n = g->net.read(g->net.user, g->buf + g->buf_len, 4096);
        if (n == CYB_EOF) { fetched(g); return; }
        if (n == CYB_FAIL) {
            const char *why = g->net.error ? g->net.error(g->net.user) : NULL;
            g->net.close(g->net.user);
            g->state = IDLE;
            logf_(g, "fetch failed: %s: %s", g->url, why ? why : "?");
            if (offline_wifi(g, why && *why ? why : "Not online.")) return;
            answer_text(g, "Error", why && *why ? why : "The page could not be fetched.");
            return;
        }
        if (n <= 0) break;
        g->buf_len += (size_t)n;
    }
    if ((int32_t)(g->now - g->fetch_until) >= 0) {
        g->net.close(g->net.user);
        g->state = IDLE;
        answer_text(g, "Error", "The web site took too long to answer.");
    }
}

/* ---- the Wi-Fi page ---- */

/* Text for HTML: markup characters escaped. */
static size_t html_esc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        const char *e = NULL;
        switch (*s) {
        case '<': e = "&lt;"; break;
        case '>': e = "&gt;"; break;
        case '&': e = "&amp;"; break;
        case '"': e = "&quot;"; break;
        }
        if (e) { size_t k = strlen(e); memcpy(out + n, e, k); n += k; }
        else if ((uint8_t)*s >= 0x20) out[n++] = *s;
    }
    out[n] = 0;
    return n;
}

#define WIFI_PAGE_MAX 3000

/* The page: what the gate is connected to, the saved networks (each can be
 * forgotten) and the ones around (each opens a page asking for its
 * password). Networks go into short URLs: "W" and the network's name. */
static void wifi_page(cy_gate_t *g)
{
    char *w = malloc(WIFI_PAGE_MAX + 1);
    if (!w) { finish(g); return; }
    size_t n = 0;
    char e[200];
#define PUT(...) do { int k_ = snprintf(w + n, WIFI_PAGE_MAX + 1 - n, __VA_ARGS__); \
        if (k_ > 0) n = n + (size_t)k_ <= WIFI_PAGE_MAX ? n + (size_t)k_ : WIFI_PAGE_MAX; } while (0)
    PUT("<title>Wi-Fi</title>");
    if (g->wifi_note[0]) { html_esc(e, sizeof e, g->wifi_note); PUT("<b>%s</b><br>", e); }
    char st[96] = "";
    int on = g->net.wifi_status(g->net.user, st, sizeof st);
    html_esc(e, sizeof e, st);
    PUT("%s%s<br>", on > 0 ? "Online: " : on == 0 ? "Connecting: " : "Offline. ", e);
    if (g->net.wifi_saved) {
        char names[5][33];
        int k = g->net.wifi_saved(g->net.user, names, 5);
        if (k > 0) PUT("<b>Saved</b><br>");
        for (int i = 0; i < k; i++) {
            uint32_t a = alias_add(g, "W", names[i]);
            html_esc(e, sizeof e, names[i]);
            PUT("%s <a href=\"" ALIAS_HOST "x%u\">[forget]</a><br>", e, (unsigned)a);
        }
    }
    if (g->net.wifi_scan) {
        cyb_ap_t aps[12];
        int k = g->net.wifi_scan(g->net.user, aps, 12);
        PUT("<b>Networks here</b> <a href=\"" ALIAS_HOST "wifi?scan\">[look again]</a><br>");
        if (k < 0) PUT("Still looking...<br>");
        else if (!k) PUT("None found.<br>");
        for (int i = 0; i < k && n + 300 < WIFI_PAGE_MAX; i++) {
            uint32_t a = alias_add(g, "W", aps[i].ssid);
            html_esc(e, sizeof e, aps[i].ssid);
            PUT("<a href=\"" ALIAS_HOST "j%u\">%s</a> %d%s<br>", (unsigned)a, e, aps[i].rssi, aps[i].open ? " open" : "");
        }
    }
    PUT("<a href=\"" ALIAS_HOST "\">Home</a>");
#undef PUT
    g->wifi_note[0] = 0;
    show_html(g, w, ALIAS_HOST "wifi");
    free(w);
}

/* Asks for the password of network a (a short URL's number), in a plain
 * text field: on the Cybiko's keys it is easier to see what was typed. */
static void wifi_ask(cy_gate_t *g, uint32_t a, const char *ssid)
{
    char w[800], e[200], url[32];
    html_esc(e, sizeof e, ssid);
    snprintf(url, sizeof url, ALIAS_HOST "j%u", (unsigned)a);
    snprintf(w, sizeof w,
             "<title>Wi-Fi</title><b>%s</b><br><form action=\"%s\">Password (empty if open):<br>"
             "<input name=\"p\" size=\"14\"><br><input type=\"submit\" value=\"Join\"></form>"
             "<a href=\"" ALIAS_HOST "wifi\">Back</a>", e, url);
    show_html(g, w, url);
}

/* Waits (in cy_gate_poll) for the scan to finish, then shows the page. */
static bool scanning(cy_gate_t *g)
{
    cyb_ap_t ap;
    return g->net.wifi_scan(g->net.user, &ap, 1) < 0;
}

static void wifi_step(cy_gate_t *g)
{
    if (scanning(g) && (int32_t)(g->now - g->wifi_until) < 0) return;
    g->state = IDLE;
    wifi_page(g);
}

/* http://c/wifi[?scan], http://c/jN[?p=password] and http://c/xN */
static bool wifi_request(cy_gate_t *g, const char *path)
{
    if (!g->net.wifi_status) {
        if (!strncmp(path, "wifi", 4)) { answer_text(g, "Wi-Fi", "This gate is always online."); return true; }
        return false;
    }
    if (!strcmp(path, "wifi") || !strcmp(path, "wifi?scan")) {
        if (g->net.wifi_scan) {
            if (path[4]) g->net.wifi_scan(g->net.user, NULL, 0);    /* a new scan */
            if (scanning(g)) {
                g->state = WIFI;
                g->wifi_until = g->now + WIFI_SCAN_MS;
                return true;
            }
        }
        wifi_page(g);
        return true;
    }
    if (path[0] != 'j' && path[0] != 'x') return false;
    const char *ssid = alias_get(g, (uint32_t)strtoul(path + 1, NULL, 10));
    if (!ssid || ssid[0] != 'W') { answer_text(g, "Expired", "This link is too old. Open the Wi-Fi page again."); return true; }
    ssid++;
    if (path[0] == 'x') {
        if (g->net.wifi_forget) g->net.wifi_forget(g->net.user, ssid);
        snprintf(g->wifi_note, sizeof g->wifi_note, "Forgot %s.", ssid);
        wifi_page(g);
        return true;
    }
    const char *q = strchr(path, '?');
    char pw[72];
    if (!q || !query_value(q + 1, "p", pw, sizeof pw)) {
        wifi_ask(g, (uint32_t)strtoul(path + 1, NULL, 10), ssid);
        return true;
    }
    logf_(g, "joining %s", ssid);
    if (!g->net.wifi_join || !g->net.wifi_join(g->net.user, ssid, pw)) {
        snprintf(g->wifi_note, sizeof g->wifi_note, "Could not join %s.", ssid);
        wifi_page(g);
        return true;
    }
    snprintf(g->wifi_note, sizeof g->wifi_note, "Saved %s: it is joined for the next page.", ssid);
    wifi_page(g);
    return true;
}

/* ---- requests ---- */

static void request(cy_gate_t *g, uint32_t from, uint16_t number, const char *url, const char *body,
                    bool app, bool big, bool pics)
{
    if (g->state != IDLE) {
        if (from != g->to) {
            /* one at a time: the other Cybiko tries again */
            logf_(g, "busy: %08x asked for %s", (unsigned)from, url);
            return;
        }
        finish(g);
    }
    g->to = from;
    g->number = number;
    g->app = app;
    g->big = big;
    g->pics = pics;
    logf_(g, "%08x asks for %s%s", (unsigned)from, url, app ? " (Browser)" : "");
    start_fetch(g, url, body);
}

/* The Cybiko's home page and the short URLs. */
static bool own_page(cy_gate_t *g, const char *url)
{
    if (!strcasecmp(url, HOME_URL) || !strcasecmp(url, "http://www.cybiko.com/") || !strcasecmp(url, ALIAS_HOST)) {
        if (g->home[0] && strcasecmp(g->home, url)) start_fetch(g, g->home, NULL);
        else if (!(no_networks(g) && offline_wifi(g, "No Wi-Fi network yet: pick one."))) show_html(g, home_html, ALIAS_HOST);
        return true;
    }
    if (strncasecmp(url, ALIAS_HOST, strlen(ALIAS_HOST))) return false;
    const char *p = url + strlen(ALIAS_HOST);
    if (wifi_request(g, p)) return true;
    char kind = (*p == 'f' || *p == 'p' || *p == 'y') ? *p++ : 'u';
    const char *query = strchr(p, '?');
    const char *a = alias_get(g, (uint32_t)strtoul(p, NULL, 10));
    if (!a) { answer_text(g, "Expired", "This link is too old. Open the page again."); return true; }
    if (kind == 'u' && a[0] == 'U') { start_fetch(g, a + 1, NULL); return true; }
    if (kind == 'f' && (a[0] == 'F' || a[0] == 'P')) { form_submit(g, a + 1, a[0] == 'P', query ? query + 1 : ""); return true; }
    if (kind == 'p' && a[0] == 'M') {
        unsigned item = 0, off = 0;
        int used = 0;
        if (sscanf(a + 1, "%u %u %n", &item, &off, &used) >= 2) {
            const char *page = a + 1 + used;
            if (g->have_doc && !strcmp(page, g->doc_url) && item < g->doc.n_items) {
                g->start = (cy_wml_pos_t){ item, off };
                send_doc(g);
                return true;
            }
            start_fetch(g, page, NULL);
            g->start = (cy_wml_pos_t){ item, off };
            return true;
        }
    }
    if (kind == 'y' && a[0] == 'Y') {
        long y = 0;
        int used = 0;
        if (sscanf(a + 1, "%ld %n", &y, &used) >= 1 && y >= 0) {
            const char *page = a + 1 + used;
            if (g->have_doc && !strcmp(page, g->doc_url)) {
                g->top = (int32_t)y;
                send_doc(g);
                return true;
            }
            start_fetch(g, page, NULL);
            g->top = (int32_t)y;
            return true;
        }
    }
    answer_text(g, "Expired", "This link is too old. Open the page again.");
    return true;
}

/* A message for the gate. */
static void message(cy_gate_t *g, uint32_t from, const uint8_t *m, size_t len)
{
    size_t n = 4;
    const uint8_t flags = m[0];
    const uint16_t id = get16(m + 2);
    uint32_t p0 = 0, p1 = 0;
    if (flags & M_P0) { p0 = get32(m + n); n += 4; }
    if (flags & M_P1) { p1 = get32(m + n); n += 4; }
    if (flags & M_NAME) n += strnlen((const char *)m + n, len - n) + 1;
    else n += 2;
    size_t dl = 0;
    if ((flags & M_DATA) && n < len) {
        dl = (flags & 0x02) ? len - n : m[n++];
        if (flags & 0x02) while (dl && n + dl <= len && m[n + dl - 1] == 0) dl--;
        if (n + dl > len) dl = len - n;
    }
    if (id != MSG_WAP) {
        logf_(g, "msg %04x from %08x (flags %02x) not handled", id, (unsigned)from, flags);
        return;
    }
    const bool app = (p0 & 0xff) == APP_GET;
    if ((p0 & 0xffff) != WAP_GET && (p0 & 0xffff) != WAP_POST && !app) {
        logf_(g, "WAP request %08x from %08x not handled", (unsigned)p0, (unsigned)from);
        return;
    }
    char url[1024];
    size_t ul = strnlen((const char *)m + n, dl);
    snprintf(url, sizeof url, "%.*s", (int)ul, (const char *)m + n);
    /* a post's form data would follow the URL */
    char body[128];
    const bool post = (p0 & 0xffff) == WAP_POST && ul + 1 < dl;
    if (post) snprintf(body, sizeof body, "%.*s", (int)(dl - ul - 1), (const char *)m + n + ul + 1);
    request(g, from, (uint16_t)p1, url, post ? body : NULL, app, app && (p0 & APP_BIG), app && (p0 & APP_PICS));
}

/* A message too big for one packet comes in parts: byte 10 is the part
 * (from 1), byte 11 how many. The first is a message's start whose data
 * runs to the end of the packet (flag 02); the others have two bytes
 * (03 00 on the last) and the length of their data, or run to the end. */
static void long_part(cy_gate_t *g, const uint8_t *pkt, size_t data)
{
    const uint32_t from = get32(pkt + 4);
    const uint8_t *m = pkt + HEAD;
    const unsigned part = pkt[10], parts = pkt[11];
    if (part == 1) {
        g->lp_from = from;
        g->lp_next = 2;
        g->lp_len = 0;
        memcpy(g->lp, m, data);
        g->lp_len = data;
    } else {
        if (from != g->lp_from || part != g->lp_next || data < 3) return;
        size_t n = m[0] & 1 ? m[2] : data - 2;
        const uint8_t *d = m + (m[0] & 1 ? 3 : 2);
        if (d + n > m + data) n = (size_t)(m + data - d);
        if (g->lp_len + n > sizeof g->lp) n = sizeof g->lp - g->lp_len;
        memcpy(g->lp + g->lp_len, d, n);
        g->lp_len += n;
        g->lp_next++;
    }
    if (part == parts && part > 1) {
        g->lp_next = 0;
        message(g, from, g->lp, g->lp_len);
    }
}

static bool seen_before(cy_gate_t *g, const uint8_t *pkt)
{
    const uint32_t from = get32(pkt + 4);
    const uint16_t crc = get16(pkt + 12);
    for (int i = 0; i < 8; i++)
        if (g->seen[i].from == from && g->seen[i].number == pkt[11] && g->seen[i].crc == crc) return true;
    g->seen[g->seen_next].from = from;
    g->seen[g->seen_next].number = pkt[11];
    g->seen[g->seen_next].crc = crc;
    g->seen_next = (g->seen_next + 1) % 8;
    return false;
}

void cy_gate_rf_in(cy_gate_t *g, const uint8_t *pkt, size_t len, uint8_t ch)
{
    if ((len != SHORT && len != LONG) || get32(pkt) != g->id) return;
    if (get16(pkt + 14) != crc16(pkt, 14)) return;
    const size_t data = (len == SHORT ? SHORT_DATA : LONG_DATA) - HEAD;
    if (get16(pkt + 12) != crc16(pkt + HEAD, data)) return;
    g->channel = (uint8_t)(pkt[8] & 0x3f);
    (void)ch;
    const uint8_t type = pkt[9] & 0xe0;
    const uint32_t from = get32(pkt + 4);
    switch (type) {
    case T_MSG: {
        const bool repeat = seen_before(g, pkt);
        if (pkt[9] & F_ACK_ME) ack(g, pkt, ACK_DELIVERED);
        if (!repeat) {
            if (pkt[10]) long_part(g, pkt, data);
            else message(g, from, pkt + HEAD, data);
        }
        break;
    }
    case T_QUERY:
        ack(g, pkt, ACK_DELIVERED);
        break;
    case T_ACK:
        if (g->tx_busy && from == g->to && pkt[10] == g->tx_number) {
            const int code = pkt[HEAD] >> 4;
            if (code == ACK_DELIVERED || code == ACK_PART) {
                part_delivered(g);
            } else if (code == ACK_GOT) {
                /* ask whether it reached the WAP browser */
                if (!g->tx_query) { g->tx_query = true; g->tries = 0; g->tx_at = g->now + 50; }
            } else if (code == ACK_NOBODY) {
                logf_(g, "%08x: no %s running", (unsigned)from, g->app ? "Browser" : "WAP browser");
                finish(g);
            }
        }
        break;
    default:
        break;
    }
}

uint32_t cy_gate_poll(cy_gate_t *g, uint32_t now_ms)
{
    g->now = now_ms;
    if ((int32_t)(now_ms - g->next_beacon) >= 0) {
        g->next_beacon = now_ms + BEACON_MS;
        beacon(g);
    }
    if (g->state == FETCH) fetch_step(g);
    if (g->state == PICS) pics_step(g);
    if (g->state == WIFI) wifi_step(g);
    if (g->tx_busy && (int32_t)(now_ms - g->tx_at) >= 0) {
        if (g->tries >= TRIES) {
            logf_(g, "%08x stopped answering", (unsigned)g->to);
            finish(g);
        } else {
            tx_again(g);
        }
    }
    if (g->state == FETCH || g->state == PICS) return 5;
    if (g->state == WIFI) return 100;
    uint32_t wait = g->next_beacon - now_ms;
    if (g->tx_busy && g->tx_at - now_ms < wait) wait = g->tx_at - now_ms;
    return wait;
}

void cy_gate_set_fonts(cy_gate_t *g, const uint8_t *flash, size_t len)
{
    cyb_fonts_free(g->fonts);
    cyb_fonts_load(g->fonts, flash, len);
    g->lay_ok = false;
}

void cy_gate_set_home(cy_gate_t *g, const char *url)
{
    snprintf(g->home, sizeof g->home, "%s", url ? url : "");
}

void cy_gate_cancel(cy_gate_t *g)
{
    if (g->state == PICS) {         /* the part without the rest of its pictures */
        if (g->at_pic >= 0 && g->at_pic < g->n_pics) g->net.close(g->net.user);
        g->state = IDLE;
        g->lay_ok = false;
        lay_out(g);
        send_layout(g, &g->doc, &g->lay, g->top);
        return;
    }
    if (g->state != FETCH) return;
    g->net.close(g->net.user);
    g->state = IDLE;
    answer_text(g, "Error", "The gate was busy. Try again.");
}

cy_gate_t *cy_gate_create(uint32_t id, const cy_gate_host_t *host, const cyb_net_t *net)
{
    cy_gate_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->id = id;
    g->host = *host;
    g->net = *net;
    g->channel = 2;                 /* CyOS's */
    cyb_fonts_load(g->fonts, NULL, 0);
    rs_init();
    return g;
}

void cy_gate_destroy(cy_gate_t *g)
{
    if (!g) return;
    finish(g);
    cyb_doc_free(&g->doc);
    cyb_layout_free(&g->lay);
    cyb_fonts_free(g->fonts);
    for (int i = 0; i < ALIASES; i++) free(g->alias[i]);
    free(g->buf);
    free(g);
}
