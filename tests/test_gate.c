/* The gate against a pretend Cybiko: asks for URL as the WAP browser does,
 * acknowledges every part, puts the WML together and prints it.
 *
 *   test_gate [-q] URL...   -q: check the parts only, print nothing
 *
 * The URLs are asked for one after another from the same gate, so later
 * ones can be the gate's short URLs from earlier pages (http://c/...). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy_gate.h"
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
static void log_line(void *user, const char *line)
{
    (void)user;
    if (!quiet) fprintf(stderr, "gate: %s\n", line);
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
    if (a < argc && !strcmp(argv[a], "-q")) { quiet = true; a++; }
    if (a >= argc) { fprintf(stderr, "usage: test_gate [-q] URL\n"); return 2; }
    cyb_net_t net;
    cyb_net_curl(&net, NULL);
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
    put32(m + n, 1); n += 4;
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
            if ((d[0] & 0x20) && strcmp((const char *)d + 12, "WAP")) { fprintf(stderr, "not for WAP\n"); return 1; }
            if ((p0 & 0xffff) == 5) { status = (int)p1; done = true; }
            else if ((p0 & 0xffff) == 6) {
                if (p0 >> 16 != number) { fprintf(stderr, "part %u, expected %u\n", p0 >> 16, number); return 1; }
                /* (a part sent again would be dropped, as the WAP browser does) */
                number++;
                size_t len = d[16];
                total = p1 >> 16;
                if ((p1 & 0xffff) != len || got + len > 65535) { fprintf(stderr, "bad part size\n"); return 1; }
                memcpy(page + got, d + 17, len);
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
    if (got > 5000) { fprintf(stderr, "page too big: %u\n", (unsigned)got); return 1; }
    if (!quiet) fwrite(page, 1, got, stdout), putchar('\n');
    free(page);
    }
    cy_gate_destroy(g);
    return 0;
}
