/* The gate against a pretend Cybiko: asks for URL as the WAP browser does,
 * acknowledges every part, puts the WML together and prints it.
 *
 *   test_gate [-q] [-w] [-a] URL...   -q: check the parts only, print nothing;
 *                                    -w: a pretend Wi-Fi (for http://c/wifi);
 *                                    -a: ask as the Browser application and
 *                                        print its pages' boxes (cy_page.h)
 *
 * The URLs are asked for one after another from the same gate, so later
 * ones can be the gate's short URLs from earlier pages (http://c/...). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy_gate.h"
#include "cy_page.h"
#include "cyb_net_curl.h"

#define GATE 0x4c80c0deu
#define CYBIKO 0x4c000001u

static uint8_t q[64][200];
static size_t q_len[64];
static int q_n;

static void rf_send(void *user, const uint8_t *pkt, size_t len, uint8_t ch)
{
    (void)user;
    (void)ch;
    if (q_n < 64) { memcpy(q[q_n], pkt, len); q_len[q_n++] = len; }
}

static bool quiet;

/* -w: a pretend Wi-Fi, for the gate's Wi-Fi page */
static char fake_saved[5][33];
static int fake_n, fake_scans;
static int fw_status(void *u, char *t, size_t cap) { (void)u; snprintf(t, cap, fake_n ? "%s, 10.0.0.2" : "not joined", fake_saved[0]); return fake_n ? 1 : -1; }
static int fw_scan(void *u, cyb_ap_t *aps, int max)
{
    (void)u;
    if (!aps) { fake_scans = 3; return -1; }
    if (fake_scans > 0) { fake_scans--; return -1; }
    const char *names[] = { "Home <5G>", "Cafe & Bar", "Neighbour$" };
    int n = 0;
    for (; n < 3 && n < max; n++) { snprintf(aps[n].ssid, 33, "%s", names[n]); aps[n].rssi = (int8_t)(-40 - 10 * n); aps[n].open = n == 1; }
    return n;
}
static bool fw_join(void *u, const char *ssid, const char *pw)
{
    (void)u;
    if (!quiet) fprintf(stderr, "join [%s] [%s]\n", ssid, pw);
    if (fake_n < 5) snprintf(fake_saved[fake_n++], 33, "%s", ssid);
    return true;
}
static int fw_saved(void *u, char (*ssids)[33], int max) { (void)u; int i = 0; for (; i < fake_n && i < max; i++) memcpy(ssids[i], fake_saved[i], 33); return i; }
static bool fw_forget(void *u, const char *ssid) { (void)u; if (!quiet) fprintf(stderr, "forget [%s]\n", ssid); fake_n = 0; return true; }
static void log_line(void *user, const char *line)
{
    (void)user;
    if (!quiet) fprintf(stderr, "gate: %s\n", line);
}

static unsigned g16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* A part for the application, checked and printed. */
static bool dump_page(const uint8_t *p, size_t len)
{
    if (len < 9 || memcmp(p, "CB\1", 3)) { fprintf(stderr, "not a page\n"); return false; }
    size_t table = g16(p + 6), at = 9 + p[8];
    if (table > len || at > table) { fprintf(stderr, "bad table offset\n"); return false; }
    if (!quiet) printf("page \"%.*s\" height %u%s\n", p[8], p + 9, g16(p + 4), p[3] & 1 ? ", more" : "");
    while (at < table) {
        if (at + 8 > table) { fprintf(stderr, "cut box\n"); return false; }
        const uint8_t *b = p + at;
        int kind = b[0] >> 4;
        at += 8;
        if (!quiet) printf("  %d f%d x%3u y%4u w%3u h%2u s%02x t%u", kind, b[0] & 15, b[1], g16(b + 2), b[4], b[5], b[6], b[7]);
        if (kind == PB_TEXT || kind == PB_ALT) {
            if (at + 1 + p[at] > table) { fprintf(stderr, "cut text\n"); return false; }
            if (!quiet) printf(" \"%.*s\"", p[at], p + at + 1);
            at += 1 + p[at];
        }
        if (!quiet) putchar('\n');
    }
    unsigned n = p[at++];
    for (unsigned t = 1; t <= n; t++) {
        if (at >= len) { fprintf(stderr, "cut target table\n"); return false; }
        if (p[at] == PT_LINK) {
            if (!quiet) printf("  t%u link %.*s\n", t, p[at + 1], p + at + 2);
            at += 2 + p[at + 1];
            continue;
        }
        const uint8_t *f = p + at;
        if (!quiet) printf("  t%u field type %u form %u flags %u v%u \"%.*s\"", t, f[1], f[2], f[3], g16(f + 4), f[6], f + 7);
        at += 7 + f[6];
        if (f[1] == 6) {        /* FT_SELECT */
            unsigned k = p[at];
            if (!quiet) printf(" chosen %u:", p[at + 1]);
            at += 2;
            for (unsigned j = 0; j < k; j++) {
                if (!quiet) printf(" %.*s=%.*s", p[at], p + at + 1, p[at + 1 + p[at]], p + at + 2 + p[at]);
                at += 1 + p[at];
                at += 1 + p[at];
            }
        } else if (f[1] == 3) { /* FT_SUBMIT */
            if (!quiet) printf(" -> %.*s", p[at], p + at + 1);
            at += 1 + p[at];
        }
        if (!quiet) putchar('\n');
    }
    if (at != len) { fprintf(stderr, "%u bytes left over\n", (unsigned)(len - at)); return false; }
    return true;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void header(uint8_t *p, size_t len, uint32_t to, uint8_t type, uint8_t number)
{
    memset(p, 0, len);
    put32(p, to);
    put32(p + 4, CYBIKO);
    p[8] = 0xc2;
    p[9] = type;
    p[11] = number;
}

int main(int argc, char **argv)
{
    int a = 1;
    bool wifi = false, app = false;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-q")) quiet = true;
        else if (!strcmp(argv[a], "-a")) app = true;
        else if (!strcmp(argv[a], "-w")) wifi = true;
    }
    if (a >= argc) { fprintf(stderr, "usage: test_gate [-q] URL\n"); return 2; }
    cyb_net_t net;
    cyb_net_curl(&net, NULL);
    if (wifi) {
        net.wifi_status = fw_status;
        net.wifi_scan = fw_scan;
        net.wifi_join = fw_join;
        net.wifi_saved = fw_saved;
        net.wifi_forget = fw_forget;
    }
    cy_gate_host_t host = { .rf_send = rf_send, .log = log_line };
    cy_gate_t *g = cy_gate_create(GATE, &host, &net);
    uint32_t now = 0;
    for (; a < argc; a++) {
    const char *url = argv[a];

    /* the request: msg 1022 to port 93, p0 1, p1 the request number */
    uint8_t pkt[200];
    header(pkt, 200, GATE, 0x30, 1);
    uint8_t *m = pkt + 16;
    size_t ul = strlen(url) + 1, n = 0;
    if (ul > 120 - 16 - 15) { fprintf(stderr, "URL too long for one part\n"); return 2; }
    m[n++] = 0x1c; m[n++] = 0; m[n++] = 0x10; m[n++] = 0x22;
    put32(m + n, app ? 0x21 : 1); n += 4;
    put32(m + n, (uint32_t)(11 + 100 * a)); n += 4;
    m[n++] = 0; m[n++] = 0x93;
    m[n++] = (uint8_t)ul;
    memcpy(m + n, url, ul);
    cy_gate_seal(pkt, 200);
    cy_gate_rf_in(g, pkt, 200, 2);

    char *page = calloc(1, 65536);
    size_t got = 0, total = 0;
    uint16_t number = (uint16_t)(11 + 100 * a);
    int status = -1, acks = 0;
    bool done = false;
    for (uint32_t until = now + 120000; now < until && !done; now += 5) {
        cy_gate_poll(g, now);
        for (int i = 0; i < q_n; i++) {
            const uint8_t *p = q[i];
            if (get32(p) != CYBIKO) continue;
            if ((p[9] & 0xe0) == 0x40) { acks++; continue; }
            if ((p[9] & 0xe0) != 0x20 || q_len[i] != 200) continue;
            const uint8_t *d = p + 16;
            uint32_t p0 = get32(d + 4), p1 = get32(d + 8);
            const char *proc = app ? "Browser" : "WAP";
            if ((d[0] & 0x20) && strcmp((const char *)d + 12, proc)) { fprintf(stderr, "not for %s\n", proc); return 1; }
            const uint8_t *data = d + 12 + strlen(proc) + 1;
            if ((p0 & 0xffff) == 5) { status = (int)p1; done = true; }
            else if ((p0 & 0xffff) == 6) {
                if (p0 >> 16 != number) { fprintf(stderr, "part %u, expected %u\n", p0 >> 16, number); return 1; }
                /* (a part sent again would be dropped, as the WAP browser does) */
                number++;
                size_t len = data[0];
                total = p1 >> 16;
                if ((p1 & 0xffff) != len || got + len > 65535) { fprintf(stderr, "bad part size\n"); return 1; }
                memcpy(page + got, data + 1, len);
                got += len;
                if (got >= total) done = true;
            }
            uint8_t ack[50];
            header(ack, 50, GATE, 0x40, p[11]);
            ack[10] = p[10];
            ack[16] = 0x40;                       /* delivered */
            cy_gate_seal(ack, 50);
            cy_gate_rf_in(g, ack, 50, 2);
        }
        q_n = 0;
    }
    if (status >= 0) { printf("error %d\n", status); continue; }
    if (!done) { fprintf(stderr, "no page (%u of %u bytes)\n", (unsigned)got, (unsigned)total); return 1; }
    if (got > (app ? CY_PAGE_MAX : 5000)) { fprintf(stderr, "page too big: %u\n", (unsigned)got); return 1; }
    if (app) { if (!dump_page((const uint8_t *)page, got)) return 1; }
    else if (!quiet) fwrite(page, 1, got, stdout), putchar('\n');
    free(page);
    }
    cy_gate_destroy(g);
    return 0;
}
