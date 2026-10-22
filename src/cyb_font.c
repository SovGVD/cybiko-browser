/* The browser's text: CyOS's own fonts, taken from the flash image, and the
 * 2-bit frame buffer it draws on.
 *
 * CyOS 1.5 keeps its fonts as files in a Cy archive in flash (with fdoctor.e,
 * the keyboard maps and so on): coolnormal.pic and coolbold.pic (12 dots
 * high, the desktop's font) and mininormal.pic and minibold.pic (9 high).
 * A font file is a 1-bit .pic with one picture a character from 32 up:
 *
 *   u8 bits per pixel (1), u8 count, u8 widest, u8 height
 *   count x { u8 x, u8 y, u8 w, u8 h, h rows of (w + 7) / 8 bytes }
 *
 * x and y place the picture in the character cell and x + w is the advance.
 * The archive format and its compression are in
 * docs/cybiko-mp3-cartridge.md (4.2). */
#include <stdlib.h>
#include <string.h>
#include "cyb_internal.h"

/* ---- frame buffer: rows of 40 bytes, leftmost pixel in the top bits ---- */

void fb_clear(uint8_t *fb, int c)
{
    memset(fb, (c & 3) * 0x55, CYB_FB_SIZE);
}

void fb_pixel(uint8_t *fb, int x, int y, int c)
{
    if ((unsigned)x >= CYB_W || (unsigned)y >= CYB_H) return;
    uint8_t *p = fb + y * (CYB_W / 4) + x / 4;
    int sh = 6 - 2 * (x & 3);
    *p = (uint8_t)((*p & ~(3 << sh)) | (c & 3) << sh);
}

void fb_fill(uint8_t *fb, int x, int y, int w, int h, int c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) fb_pixel(fb, i, j, c);
}

void fb_invert(uint8_t *fb, int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            if ((unsigned)i >= CYB_W || (unsigned)j >= CYB_H) continue;
            uint8_t *p = fb + j * (CYB_W / 4) + i / 4;
            *p ^= (uint8_t)(3 << (6 - 2 * (i & 3)));
        }
}

void fb_frame(uint8_t *fb, int x, int y, int w, int h, int c)
{
    fb_fill(fb, x, y, w, 1, c);
    fb_fill(fb, x, y + h - 1, w, 1, c);
    fb_fill(fb, x, y, 1, h, c);
    fb_fill(fb, x + w - 1, y, 1, h, c);
}

void fb_image(uint8_t *fb, int x, int y, int w, int h, const uint8_t *pix, int y0, int y1)
{
    int stride = (w + 3) / 4;
    for (int j = 0; j < h; j++) {
        int sy = y + j;
        if (sy < y0 || sy >= y1) continue;
        for (int i = 0; i < w; i++) {
            int v = pix[j * stride + i / 4] >> (6 - 2 * (i & 3)) & 3;
            fb_pixel(fb, x + i, sy, v);
        }
    }
}

/* ---- CyOS archives ---- */

typedef struct {
    const uint8_t *p, *end;
    uint32_t bit;
} bits_t;

static int bit1(bits_t *b)
{
    if (b->p >= b->end) return 0;
    int v = *b->p >> b->bit & 1;
    if (++b->bit == 8) { b->bit = 0; b->p++; }
    return v;
}

static int bitn(bits_t *b, int n)
{
    int v = 0;
    while (n--) v = v << 1 | bit1(b);
    return v;
}

static int lz_length(bits_t *b)
{
    if (!bit1(b)) return 2 + bit1(b);
    if (!bit1(b)) return 4 + bit1(b);
    if (!bit1(b)) return !bit1(b) ? 6 : 8 + bit1(b);
    if (!bit1(b)) return !bit1(b) ? 7 : 10 + bit1(b);
    switch (bitn(b, 3)) {
    case 0: return 12;
    case 1: return 13;
    case 2: return 0x14 + bitn(b, 2);
    case 3: return 16 + bit1(b);
    case 4: return 0x18 + bitn(b, 3);
    case 5: return 14;
    case 6: return !bit1(b) ? 0x20 + bitn(b, 5) : 18 + bit1(b);
    }
    if (!bit1(b)) return 15;
    if (!bit1(b)) return 0x40 + bitn(b, 6);
    if (!bit1(b)) return 0x80 + bitn(b, 7);
    return 0x100;
}

static int lz_offset(bits_t *b)
{
    switch (bitn(b, 3)) {
    case 0: return 0x400 + bitn(b, 10);
    case 1: return 0x200 + bitn(b, 9);
    case 2: return 0x100 + bitn(b, 8);
    case 3: return 0x40 + bitn(b, 6);
    case 4: return 0x80 + bitn(b, 7);
    case 5: return !bit1(b) ? 0x800 + bitn(b, 11) : 0x1000 + bitn(b, 12);
    case 6: return !bit1(b) ? 0x20 + bitn(b, 5) : 0x10 + bitn(b, 4);
    }
    switch (bitn(b, 2)) {
    case 0: return bit1(b);
    case 1: return 6 + bitn(b, 2);
    case 2: return bit1(b) ? 2 : 4 + bit1(b);
    }
    if (!bit1(b)) return 0xc + bitn(b, 2);
    if (!bit1(b)) return 3;
    return 10 + bit1(b);
}

static void lz_unpack(const uint8_t *src, size_t len, uint8_t *out, uint32_t size)
{
    bits_t b = { src, src + len, 0 };
    uint32_t n = 0;
    while (n < size) {
        if (!bit1(&b)) {
            out[n++] = (uint8_t)bitn(&b, 8);
            continue;
        }
        uint32_t length = (uint32_t)lz_length(&b);
        uint32_t dist = (uint32_t)lz_offset(&b) + length;
        for (uint32_t i = 0; i < length && n < size; i++, n++)
            out[n] = n >= dist ? out[n - dist] : 0;
    }
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* A part of a Cy archive at `a` (of at most len bytes), unpacked into a new
 * buffer. */
static uint8_t *archive_part(const uint8_t *a, size_t len, const char *name, uint32_t *size)
{
    if (len < 6 || a[0] != 'C' || a[1] != 'y') return NULL;
    unsigned n = be16(a + 2);
    if (n == 0 || n > 64 || 6 + 10 * n > len) return NULL;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *e = a + 6 + 10 * i;
        uint32_t name_off = be16(e), off = be32(e + 2), stored = be32(e + 6);
        if (name_off >= len || off >= len || stored > len - off || stored < 1) return NULL;
        if (strncmp((const char *)a + name_off, name, len - name_off)) continue;
        const uint8_t *raw = a + off;
        if (raw[0] == 0) {
            uint8_t *out = malloc(stored - 1);
            if (out) memcpy(out, raw + 1, stored - 1);
            *size = stored - 1;
            return out;
        }
        if (raw[0] != 2 || stored < 5) return NULL;
        uint32_t want = be32(raw + 1);
        if (want > 65536) return NULL;
        uint8_t *out = malloc(want);
        if (out) lz_unpack(raw + 5, stored - 5, out, want);
        *size = want;
        return out;
    }
    return NULL;
}

static bool parse_font(cyb_font_t *f, uint8_t *data, uint32_t size)
{
    if (size < 4 || data[0] != 1 || data[1] < 95) return false;
    f->height = data[3];
    f->count = data[1] > 256 - 32 ? 256 - 32 : data[1];
    uint32_t p = 4;
    for (unsigned i = 0; i < f->count; i++) {
        if (p + 4 > size) return false;
        cyb_glyph_t *g = &f->g[i];
        g->x = (int8_t)data[p];
        g->y = (int8_t)data[p + 1];
        g->w = data[p + 2];
        g->h = data[p + 3];
        p += 4;
        g->bits = p;
        p += (uint32_t)((g->w + 7) / 8) * g->h;
        if (p > size) return false;
    }
    f->data = data;
    f->owned = true;
    return true;
}

/* Built-in stand-in for a flash without fonts: plain blocks, 6 dots high,
 * so pages still lay out (used by the PC tests). */
static uint8_t block_rows[6] = { 0xe0, 0xa0, 0xa0, 0xa0, 0xe0, 0 };
static void block_font(cyb_font_t *f, int height)
{
    memset(f, 0, sizeof *f);
    f->height = (uint8_t)height;
    f->count = 256 - 32;
    f->data = block_rows;
    for (int i = 0; i < f->count; i++) {
        f->g[i] = (cyb_glyph_t){ 0, (int8_t)(height - 7), 4, i ? 6 : 0, 0 };
        if (!i) f->g[i].w = 3;
    }
}

void cyb_fonts_load(cyb_font_t *fonts, const uint8_t *flash, size_t len)
{
    static const char *const names[F_COUNT] = {
        "mininormal.pic", "minibold.pic", "coolnormal.pic", "coolbold.pic",
    };
    static const char key[] = "coolnormal.pic";
    const uint8_t *arch = NULL;
    /* The archive's name table holds the font names; its header is at most
     * 6 + 10 * 64 bytes before them. */
    for (size_t i = 0; flash && i + sizeof key < len && !arch; i++) {
        if (flash[i] != 'c' || memcmp(flash + i, key, sizeof key)) continue;
        for (size_t back = 6; back <= 6 + 10 * 64 && back <= i; back += 2) {
            const uint8_t *a = flash + i - back;
            uint32_t sz;
            uint8_t *t = archive_part(a, len - (size_t)(a - flash), key, &sz);
            if (t) { free(t); arch = a; break; }
        }
    }
    for (int k = 0; k < F_COUNT; k++) {
        uint32_t size = 0;
        uint8_t *data = arch ? archive_part(arch, len - (size_t)(arch - flash), names[k], &size) : NULL;
        if (!data || !parse_font(&fonts[k], data, size)) {
            free(data);
            block_font(&fonts[k], k >= F_BIG ? 12 : 9);
        }
    }
}

void cyb_fonts_free(cyb_font_t *fonts)
{
    for (int k = 0; k < F_COUNT; k++)
        if (fonts[k].owned) free(fonts[k].data);
}

static const cyb_glyph_t *glyph(const cyb_font_t *f, uint8_t ch)
{
    unsigned i = ch < 32 ? 0 : ch - 32u;
    if (i >= f->count) i = '?' - 32;
    return &f->g[i];
}

int cyb_char_w(const cyb_font_t *f, uint8_t ch)
{
    const cyb_glyph_t *g = glyph(f, ch);
    return g->x + g->w;
}

int cyb_text_w(const cyb_font_t *f, const char *s, int len)
{
    int w = 0;
    for (int i = 0; i < len; i++) w += cyb_char_w(f, (uint8_t)s[i]);
    return w;
}

int cyb_text(uint8_t *fb, const cyb_font_t *f, int x, int y, const char *s, int len,
             int c, int y0, int y1)
{
    int x0 = x;
    for (int i = 0; i < len; i++) {
        const cyb_glyph_t *g = glyph(f, (uint8_t)s[i]);
        const uint8_t *rows = f->data + g->bits;
        int stride = (g->w + 7) / 8;
        for (int r = 0; r < g->h; r++) {
            int py = y + g->y + r;
            if (py < y0 || py >= y1) continue;
            for (int col = 0; col < g->w; col++)
                if (rows[r * stride + col / 8] >> (7 - col % 8) & 1)
                    fb_pixel(fb, x + g->x + col, py, c);
        }
        x += g->x + g->w;
        if (x >= CYB_W) break;
    }
    return x - x0;
}
