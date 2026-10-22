/* Runs the browser on a PC and saves what the Cybiko screen would show.
 *
 *   cyb_shot [--flash CYOS.BIN] [--data DIR] [--keys KEYS] [--big] OUT.png [URL]
 *
 * KEYS is a comma list of key names (up down left right enter esc tab del
 * menu select help space shift fn, or a letter), "type:TEXT" to type TEXT,
 * "shot:FILE.png" to save the screen there and then, and "dump" to print
 * the laid out page. Without URL the
 * browser opens its home page. The picture is 3x, in the LCD's four shades. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy_browser.h"
#include "cyb_net_curl.h"
#include "cyb_internal.h"

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), d, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

/* 8-bit grey PNG, stored (not compressed) */
static void write_png(const char *path, const uint8_t *fb, int zoom)
{
    static const uint8_t shade[4] = { 0xc8, 0x8c, 0x50, 0x14 };   /* an LCD's look */
    int w = CYB_W * zoom, h = CYB_H * zoom;
    size_t raw_len = (size_t)(w + 1) * h;
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (w + 1);
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            int sx = x / zoom, sy = y / zoom;
            int v = fb[sy * (CYB_W / 4) + sx / 4] >> (6 - 2 * (sx & 3)) & 3;
            r[1 + x] = shade[v];
        }
    }
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len), *p = z;
    *p++ = 0x78;
    *p++ = 0x01;
    uint32_t a = 1, bsum = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; bsum = (bsum + a) % 65521; }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        *p++ = off + n == raw_len;
        *p++ = n & 0xff; *p++ = n >> 8; *p++ = ~n & 0xff; *p++ = (~n >> 8) & 0xff;
        memcpy(p, raw + off, n);
        p += n;
    }
    be32(p, bsum << 16 | a);
    p += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 0; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

static uint8_t fb[CYB_FB_SIZE];

/* steps until nothing is loading */
static uint32_t clock_ms;

static void settle(cyb_t *b)
{
    for (int i = 0; i < 2000 && (i < 3 || cyb_busy(b)); i++) cyb_step(b, clock_ms += 20);
    cyb_render(b, fb);
}

static int key_code(const char *k)
{
    static const struct { const char *name; int key; } names[] = {
        { "up", CYB_K_UP }, { "down", CYB_K_DOWN }, { "left", CYB_K_LEFT }, { "right", CYB_K_RIGHT },
        { "enter", CYB_K_ENTER }, { "esc", CYB_K_ESC }, { "tab", CYB_K_TAB }, { "del", CYB_K_DEL },
        { "menu", CYB_K_MENU }, { "select", CYB_K_SELECT }, { "help", CYB_K_HELP }, { "space", ' ' },
        { "shift", CYB_K_SHIFT }, { "fn", CYB_K_FN },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(k, names[i].name)) return names[i].key;
    if (strlen(k) == 1) return k[0];
    fprintf(stderr, "unknown key %s\n", k);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *flash_path = NULL, *data = NULL, *keys = NULL, *out = NULL, *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--flash") && i + 1 < argc) flash_path = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!out) out = argv[i];
        else url = argv[i];
    }
    if (!out) {
        fprintf(stderr, "usage: cyb_shot [--flash CYOS.BIN] [--data DIR] [--keys KEYS] OUT.png [URL]\n");
        return 2;
    }
    uint8_t *flash = NULL;
    size_t flash_len = 0;
    if (flash_path) {
        FILE *f = fopen(flash_path, "rb");
        if (!f) { perror(flash_path); return 1; }
        fseek(f, 0, SEEK_END);
        flash_len = (size_t)ftell(f);
        rewind(f);
        flash = malloc(flash_len);
        if (fread(flash, 1, flash_len, f) != flash_len) return 1;
        fclose(f);
    }
    cyb_net_t net;
    cyb_net_curl(&net, NULL);
    cyb_t *b = cyb_create(&net, flash, flash_len, data);
    if (url) cyb_go(b, url);
    else cyb_show(b);
    settle(b);
    if (keys) {
        char *list = strdup(keys), *save = NULL;
        for (char *k = strtok_r(list, ",", &save); k; k = strtok_r(NULL, ",", &save)) {
            if (!strncmp(k, "type:", 5)) {
                for (const char *c = k + 5; *c; c++) cyb_char(b, (unsigned char)*c);
            } else if (!strcmp(k, "dump")) {
                settle(b);
                cyb_dump(b, stdout);
                continue;
            } else if (!strncmp(k, "shot:", 5)) {
                settle(b);
                write_png(k + 5, fb, 3);
                continue;
            } else {
                int code = key_code(k);
                cyb_key(b, code, true);
                if (code != CYB_K_SHIFT && code != CYB_K_FN) cyb_key(b, code, false);
            }
            settle(b);
        }
        free(list);
    }
    settle(b);
    write_png(out, fb, 3);
    if (!cyb_step(b, clock_ms += 20)) printf("closed\n");
    cyb_destroy(b);
    free(flash);
    return 0;
}
