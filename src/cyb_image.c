/* Pictures: GIF, PNG and JPEG files to four-shade pictures for the page.
 *
 * Each decoder hands its pixels, as grey levels, to an accumulator the size
 * of the final picture, so a big picture is shrunk by averaging without ever
 * being held whole. The result is dithered (Floyd-Steinberg) to the LCD's
 * four shades. GIF and PNG (with its inflate) are decoded here; JPEG by
 * ChaN's TJpgDec (third_party/tjpgd), which also shrinks by 2, 4 or 8 while
 * it decodes. Progressive JPEGs are not supported. */
#include <stdlib.h>
#include <string.h>
#include "cyb_internal.h"
#include "tjpgd.h"

#define MAX_SIDE 8192
#define MAX_PIXELS (16u << 20)

/* ---- the accumulator ---- */

typedef struct {
    int sw, sh;            /* source size */
    int tw, th;            /* picture size */
    uint16_t *sum;         /* grey levels 0..63 added up */
    uint16_t *col;         /* source pixel -> picture column */
    uint16_t *colc, *rowc; /* source pixels in each picture column / row */
} acc_t;

static bool acc_init(acc_t *a, int sw, int sh, int max_w, int max_h)
{
    memset(a, 0, sizeof *a);
    if (sw <= 0 || sh <= 0 || sw > MAX_SIDE || sh > MAX_SIDE || (uint32_t)sw * (uint32_t)sh > MAX_PIXELS)
        return false;
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;
    int tw = sw, th = sh;
    if (tw > max_w) { th = (int)((int64_t)th * max_w / tw); tw = max_w; }
    if (th > max_h) { tw = (int)((int64_t)tw * max_h / th); th = max_h; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    a->sw = sw; a->sh = sh; a->tw = tw; a->th = th;
    a->sum = calloc((size_t)tw * th, sizeof *a->sum);
    a->col = malloc((size_t)sw * sizeof *a->col);
    a->colc = calloc((size_t)tw, sizeof *a->colc);
    a->rowc = calloc((size_t)th, sizeof *a->rowc);
    if (!a->sum || !a->col || !a->colc || !a->rowc) return false;
    for (int x = 0; x < sw; x++) {
        a->col[x] = (uint16_t)((int64_t)x * tw / sw);
        a->colc[a->col[x]]++;
    }
    for (int y = 0; y < sh; y++) a->rowc[(int64_t)y * th / sh]++;
    return true;
}

static void acc_free(acc_t *a)
{
    free(a->sum);
    free(a->col);
    free(a->colc);
    free(a->rowc);
}

/* n grey pixels (0 black .. 255 white) of source row y from column x. */
static void acc_row(acc_t *a, int x, int y, const uint8_t *grey, int n, int step)
{
    if ((unsigned)y >= (unsigned)a->sh) return;
    uint16_t *row = a->sum + (size_t)((int64_t)y * a->th / a->sh) * a->tw;
    for (int i = 0; i < n; i++, x += step) {
        if ((unsigned)x >= (unsigned)a->sw) continue;
        row[a->col[x]] += grey[i] >> 2;
    }
}

static void acc_pixel(acc_t *a, int x, int y, uint8_t grey)
{
    if ((unsigned)x >= (unsigned)a->sw || (unsigned)y >= (unsigned)a->sh) return;
    a->sum[(size_t)((int64_t)y * a->th / a->sh) * a->tw + a->col[x]] += grey >> 2;
}

/* The averages, dithered to four shades. */
static bool acc_finish(acc_t *a, cyb_pic_t *out)
{
    int tw = a->tw, th = a->th, stride = (tw + 3) / 4;
    out->pix = calloc((size_t)stride * th, 1);
    int16_t *err = calloc((size_t)(tw + 2) * 2, sizeof *err);
    if (!out->pix || !err) { free(out->pix); out->pix = NULL; free(err); return false; }
    out->w = (uint16_t)tw;
    out->h = (uint16_t)th;
    int16_t *cur = err + 1, *next = err + tw + 3;
    for (int y = 0; y < th; y++) {
        memset(next - 1, 0, (size_t)(tw + 2) * sizeof *next);
        for (int x = 0; x < tw; x++) {
            uint32_t n = (uint32_t)a->colc[x] * a->rowc[y];
            int v = n ? (int)((a->sum[(size_t)y * tw + x] * 255u + n * 63u / 2) / (n * 63u)) : 255;
            v += cur[x] / 16;
            int level = (v + 42) / 85;          /* 0 black .. 3 white */
            if (level < 0) level = 0;
            if (level > 3) level = 3;
            int e = v - level * 85;
            cur[x + 1] += (int16_t)(e * 7);
            next[x - 1] += (int16_t)(e * 3);
            next[x] += (int16_t)(e * 5);
            next[x + 1] += (int16_t)e;
            out->pix[y * stride + x / 4] |= (uint8_t)((3 - level) << (6 - 2 * (x & 3)));
        }
        int16_t *t = cur; cur = next; next = t;
    }
    free(err);
    return true;
}

static uint8_t grey_of(int r, int g, int b) { return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); }
/* over a white page */
static uint8_t over_white(uint8_t grey, uint8_t alpha) { return (uint8_t)(255 - ((255 - grey) * alpha + 127) / 255); }

/* ---- inflate (RFC 1951), output in 16 KB pieces to a sink ---- */

typedef void (*sink_fn)(void *user, const uint8_t *p, size_t n);

typedef struct {
    const uint8_t *in, *end;
    uint32_t bitbuf;
    int bitcnt;
    bool bad;
    uint8_t *win;          /* 32 KB */
    uint32_t pos, flushed;
    sink_fn sink;
    void *user;
} inf_t;

#define WIN 32768u

static int bits(inf_t *s, int need)
{
    while (s->bitcnt < need) {
        if (s->in >= s->end) { s->bad = true; return 0; }
        s->bitbuf |= (uint32_t)*s->in++ << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

static void flush(inf_t *s)
{
    while (s->flushed != s->pos) {
        uint32_t a = s->flushed & (WIN - 1), n = s->pos - s->flushed;
        if (n > WIN - a) n = WIN - a;
        s->sink(s->user, s->win + a, n);
        s->flushed += n;
    }
}

static void put(inf_t *s, uint8_t c)
{
    s->win[s->pos++ & (WIN - 1)] = c;
    if (s->pos - s->flushed >= WIN / 2) flush(s);
}

typedef struct { uint16_t count[16], symbol[320]; } huff_t;

static bool build(huff_t *h, const uint8_t *len, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return false;
    }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (uint16_t)i;
    return true;
}

static int decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->bad) return -1;
    }
    s->bad = true;
    return -1;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool codes(inf_t *s, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0 || s->bad) return false;
        if (sym < 256) { put(s, (uint8_t)sym); continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = len_base[sym] + bits(s, len_extra[sym]);
        int ds = decode(s, dist);
        if (ds < 0 || ds >= 30) return false;
        uint32_t d = dist_base[ds] + (uint32_t)bits(s, dist_extra[ds]);
        if (d > s->pos || d > WIN) return false;
        while (len--) put(s, s->win[(s->pos - d) & (WIN - 1)]);
    }
}

static bool inflate_all(inf_t *s)
{
    huff_t *lit = malloc(2 * sizeof *lit);
    if (!lit) return false;
    huff_t *dist = lit + 1;
    bool ok = true, last = false;
    while (ok && !last) {
        last = bits(s, 1);
        int type = bits(s, 2);
        if (s->bad) { ok = false; break; }
        if (type == 0) {
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->end - s->in < 4) { ok = false; break; }
            unsigned n = s->in[0] | s->in[1] << 8;
            if ((n ^ (s->in[2] | s->in[3] << 8)) != 0xffff) { ok = false; break; }
            s->in += 4;
            if ((size_t)(s->end - s->in) < n) { ok = false; break; }
            while (n--) put(s, *s->in++);
        } else if (type == 1) {
            uint8_t len[320];
            int i = 0;
            for (; i < 144; i++) len[i] = 8;
            for (; i < 256; i++) len[i] = 9;
            for (; i < 280; i++) len[i] = 7;
            for (; i < 288; i++) len[i] = 8;
            build(lit, len, 288);
            for (i = 0; i < 30; i++) len[i] = 5;
            build(dist, len, 30);
            ok = codes(s, lit, dist);
        } else if (type == 2) {
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t len[320];
            int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
            if (nlen > 286 || ndist > 30) { ok = false; break; }
            memset(len, 0, sizeof len);
            for (int i = 0; i < ncode; i++) len[order[i]] = (uint8_t)bits(s, 3);
            if (!build(lit, len, 19)) { ok = false; break; }
            int i = 0;
            while (i < nlen + ndist && !s->bad) {
                int sym = decode(s, lit);
                if (sym < 0) { ok = false; break; }
                if (sym < 16) { len[i++] = (uint8_t)sym; continue; }
                int rep, v = 0;
                if (sym == 16) {
                    if (!i) { ok = false; break; }
                    v = len[i - 1];
                    rep = 3 + bits(s, 2);
                } else if (sym == 17) rep = 3 + bits(s, 3);
                else rep = 11 + bits(s, 7);
                if (i + rep > nlen + ndist) { ok = false; break; }
                while (rep--) len[i++] = (uint8_t)v;
            }
            if (!ok || s->bad || !len[256]) { ok = false; break; }
            uint8_t dl[30];
            memcpy(dl, len + nlen, (size_t)ndist);
            if (!build(lit, len, nlen)) { ok = false; break; }
            build(dist, dl, ndist);      /* an incomplete distance code is allowed */
            ok = codes(s, lit, dist);
        } else ok = false;
    }
    free(lit);
    flush(s);
    return ok;
}

/* ---- PNG ---- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

typedef struct {
    acc_t *acc;
    int depth, color, chans, bpp;     /* bpp: bytes a pixel for filtering, at least 1 */
    int pass, pw, ph, row;            /* Adam7 pass, its size, its row */
    bool interlaced;
    size_t stride, have;              /* bytes in a row (with the filter byte) */
    uint8_t *cur, *prev;
    uint8_t *grey;
    uint8_t pal[256], pal_a[256];
    int trns_grey;                    /* grey key, -1 none */
    int trns_rgb[3];
    bool bad;
} png_t;

static const uint8_t a7_x[7] = { 0, 4, 0, 2, 0, 1, 0 }, a7_y[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const uint8_t a7_dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, a7_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static void png_pass(png_t *p)
{
    for (;;) {
        if (!p->interlaced) {
            p->pw = p->acc->sw;
            p->ph = p->pass == 0 ? p->acc->sh : 0;
        } else if (p->pass < 7) {
            int x0 = a7_x[p->pass], y0 = a7_y[p->pass], dx = a7_dx[p->pass], dy = a7_dy[p->pass];
            p->pw = (p->acc->sw - x0 + dx - 1) / dx;
            p->ph = (p->acc->sh - y0 + dy - 1) / dy;
            if (p->pw <= 0) p->ph = 0;
        } else p->ph = 0;
        p->row = 0;
        p->have = 0;
        p->stride = 1 + ((size_t)p->pw * p->chans * p->depth + 7) / 8;
        memset(p->prev, 0, p->stride);
        if (p->ph > 0 || p->pass >= 7 || !p->interlaced) return;
        p->pass++;
    }
}

static int paeth(int a, int b, int c)
{
    int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static int sample(const uint8_t *row, int i, int depth)
{
    if (depth == 8) return row[i];
    if (depth == 16) return row[2 * i];          /* the high byte */
    int per = 8 / depth;
    return row[i / per] >> (8 - depth * (i % per + 1)) & ((1 << depth) - 1);
}

static void png_row(png_t *p)
{
    uint8_t *r = p->cur + 1, *q = p->prev + 1;
    size_t n = p->stride - 1;
    int bpp = p->bpp;
    switch (p->cur[0]) {
    case 0: break;
    case 1: for (size_t i = (size_t)bpp; i < n; i++) r[i] = (uint8_t)(r[i] + r[i - (size_t)bpp]); break;
    case 2: for (size_t i = 0; i < n; i++) r[i] = (uint8_t)(r[i] + q[i]); break;
    case 3:
        for (size_t i = 0; i < n; i++)
            r[i] = (uint8_t)(r[i] + ((i >= (size_t)bpp ? r[i - (size_t)bpp] : 0) + q[i]) / 2);
        break;
    case 4:
        for (size_t i = 0; i < n; i++)
            r[i] = (uint8_t)(r[i] + paeth(i >= (size_t)bpp ? r[i - (size_t)bpp] : 0, q[i],
                                          i >= (size_t)bpp ? q[i - (size_t)bpp] : 0));
        break;
    default: p->bad = true; return;
    }
    int scale = p->depth < 8 ? 255 / ((1 << p->depth) - 1) : 1;
    int step16 = p->depth == 16 ? 2 : 1;
    for (int x = 0; x < p->pw; x++) {
        int g, a = 255;
        switch (p->color) {
        case 0:
            g = sample(r, x, p->depth);
            if (p->trns_grey >= 0 && g == (p->depth == 16 ? p->trns_grey >> 8 : p->trns_grey)) a = 0;
            g *= scale;
            break;
        case 2: {
            int c0 = sample(r, x * 3, p->depth), c1 = sample(r, x * 3 + 1, p->depth), c2 = sample(r, x * 3 + 2, p->depth);
            if (p->trns_rgb[0] >= 0 && p->depth == 8 && c0 == p->trns_rgb[0] && c1 == p->trns_rgb[1] && c2 == p->trns_rgb[2]) a = 0;
            g = grey_of(c0, c1, c2);
            break;
        }
        case 3: {
            int i = sample(r, x, p->depth);
            g = p->pal[i];
            a = p->pal_a[i];
            break;
        }
        case 4:
            g = sample(r, x * 2, p->depth);
            a = sample(r, x * 2 + 1, p->depth);
            break;
        default:
            g = grey_of(sample(r, x * 4, p->depth), sample(r, x * 4 + 1, p->depth), sample(r, x * 4 + 2, p->depth));
            a = sample(r, x * 4 + 3, p->depth);
            break;
        }
        (void)step16;
        p->grey[x] = a == 255 ? (uint8_t)g : over_white((uint8_t)g, (uint8_t)a);
    }
    if (p->interlaced)
        acc_row(p->acc, a7_x[p->pass], a7_y[p->pass] + p->row * a7_dy[p->pass], p->grey, p->pw, a7_dx[p->pass]);
    else
        acc_row(p->acc, 0, p->row, p->grey, p->pw, 1);
}

static void png_sink(void *user, const uint8_t *data, size_t n)
{
    png_t *p = user;
    while (n && !p->bad && p->ph > 0) {
        size_t take = p->stride - p->have;
        if (take > n) take = n;
        memcpy(p->cur + p->have, data, take);
        p->have += take;
        data += take;
        n -= take;
        if (p->have < p->stride) break;
        png_row(p);
        uint8_t *t = p->prev; p->prev = p->cur; p->cur = t;
        p->have = 0;
        if (++p->row >= p->ph) {
            if (!p->interlaced) { p->ph = 0; break; }
            p->pass++;
            png_pass(p);
        }
    }
}

static bool png_decode(const uint8_t *d, size_t len, int max_w, int max_h, cyb_pic_t *out)
{
    if (len < 33 || memcmp(d + 12, "IHDR", 4)) return false;
    int w = (int)be32(d + 16), h = (int)be32(d + 20), depth = d[24], color = d[25];
    bool interlaced = d[28] == 1;
    int chans = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!chans || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) return false;
    if (color == 3 && depth == 16) return false;
    if (w <= 0 || h <= 0 || w > MAX_SIDE) return false;
    png_t *p = calloc(1, sizeof *p);
    if (!p) return false;
    acc_t acc;
    bool ok = false;
    uint8_t *idat = NULL;
    inf_t inf = { 0 };
    if (!acc_init(&acc, w, h, max_w, max_h)) goto done;
    p->acc = &acc;
    p->depth = depth;
    p->color = color;
    p->chans = chans;
    p->bpp = (chans * depth + 7) / 8;
    p->interlaced = interlaced;
    p->trns_grey = -1;
    p->trns_rgb[0] = -1;
    for (int i = 0; i < 256; i++) { p->pal[i] = (uint8_t)i; p->pal_a[i] = 255; }
    size_t row_max = 1 + ((size_t)w * chans * depth + 7) / 8;
    p->cur = malloc(row_max);
    p->prev = malloc(row_max);
    p->grey = malloc((size_t)w);
    if (!p->cur || !p->prev || !p->grey) goto done;
    /* gather the IDAT chunks */
    size_t pos = 8, idat_len = 0;
    idat = malloc(len);
    if (!idat) goto done;
    while (pos + 12 <= len) {
        uint32_t n = be32(d + pos);
        const uint8_t *type = d + pos + 4, *body = d + pos + 8;
        if (n > len - pos - 12) break;
        if (!memcmp(type, "PLTE", 4)) {
            for (uint32_t i = 0; i < n / 3 && i < 256; i++) p->pal[i] = grey_of(body[3 * i], body[3 * i + 1], body[3 * i + 2]);
        } else if (!memcmp(type, "tRNS", 4)) {
            if (color == 3) for (uint32_t i = 0; i < n && i < 256; i++) p->pal_a[i] = body[i];
            else if (color == 0 && n >= 2) p->trns_grey = body[0] << 8 | body[1];
            else if (color == 2 && n >= 6) for (int i = 0; i < 3; i++) p->trns_rgb[i] = body[2 * i] << 8 | body[2 * i + 1];
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(idat + idat_len, body, n);
            idat_len += n;
        } else if (!memcmp(type, "IEND", 4)) break;
        pos += 12 + n;
    }
    if (p->trns_grey >= 0 && depth < 16) p->trns_grey &= 0xff;
    if (p->trns_rgb[0] >= 0 && depth == 8) for (int i = 0; i < 3; i++) p->trns_rgb[i] &= 0xff;
    if (idat_len < 6 || (idat[0] & 0x0f) != 8) goto done;
    png_pass(p);
    inf.in = idat + 2;
    inf.end = idat + idat_len;
    inf.win = malloc(WIN);
    inf.sink = png_sink;
    inf.user = p;
    if (!inf.win) goto done;
    inflate_all(&inf);
    /* a truncated picture still shows what came */
    if (inf.pos > 0 && !p->bad) ok = acc_finish(&acc, out);
done:
    free(inf.win);
    free(idat);
    free(p->cur);
    free(p->prev);
    free(p->grey);
    free(p);
    acc_free(&acc);
    return ok;
}

/* ---- GIF (the first picture) ---- */

static bool gif_decode(const uint8_t *d, size_t len, int max_w, int max_h, cyb_pic_t *out)
{
    if (len < 13) return false;
    int sw = d[6] | d[7] << 8, sh = d[8] | d[9] << 8;
    uint8_t grey[256];
    for (int i = 0; i < 256; i++) grey[i] = 255;
    size_t pos = 13;
    if (d[10] & 0x80) {
        int n = 2 << (d[10] & 7);
        if (pos + (size_t)n * 3 > len) return false;
        for (int i = 0; i < n; i++) grey[i] = grey_of(d[pos + 3 * i], d[pos + 3 * i + 1], d[pos + 3 * i + 2]);
        pos += (size_t)n * 3;
    }
    int transparent = -1;
    while (pos < len && d[pos] == 0x21) {          /* extensions */
        if (pos + 2 > len) return false;
        if (d[pos + 1] == 0xf9 && pos + 6 < len && (d[pos + 3] & 1)) transparent = d[pos + 6];
        pos += 2;
        while (pos < len && d[pos]) pos += 1 + d[pos];
        pos++;
    }
    if (pos + 10 > len || d[pos] != 0x2c) return false;
    int fx = d[pos + 1] | d[pos + 2] << 8, fy = d[pos + 3] | d[pos + 4] << 8;
    int fw = d[pos + 5] | d[pos + 6] << 8, fh = d[pos + 7] | d[pos + 8] << 8;
    int flags = d[pos + 9];
    pos += 10;
    if (!sw || !sh) { sw = fx + fw; sh = fy + fh; }
    if (flags & 0x80) {
        int n = 2 << (flags & 7);
        if (pos + (size_t)n * 3 > len) return false;
        for (int i = 0; i < n; i++) grey[i] = grey_of(d[pos + 3 * i], d[pos + 3 * i + 1], d[pos + 3 * i + 2]);
        pos += (size_t)n * 3;
    }
    if (transparent >= 0) grey[transparent] = 255;
    bool interlaced = flags & 0x40;
    if (pos >= len) return false;
    int min_code = d[pos++];
    if (min_code < 2 || min_code > 11) return false;
    acc_t acc;
    if (!acc_init(&acc, sw, sh, max_w, max_h)) { acc_free(&acc); return false; }
    /* the frame starts out as page background */
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            if (x < fx || x >= fx + fw || y < fy || y >= fy + fh) acc_pixel(&acc, x, y, 255);
    uint16_t *prefix = malloc(4096 * sizeof *prefix);
    uint8_t *suffix = malloc(4096), *stack = malloc(4097);
    bool ok = false;
    if (!prefix || !suffix || !stack || fw <= 0 || fh <= 0) goto done;
    {
        int clear = 1 << min_code, eoi = clear + 1, next = clear + 2, size = min_code + 1;
        int old = -1;
        uint8_t first = 0;
        uint32_t buf = 0;
        int nbits = 0, block = 0;
        int x = 0, y = 0, pass = 0, row = 0;
        static const uint8_t istart[4] = { 0, 4, 2, 1 }, istep[4] = { 8, 8, 4, 2 };
        long pixels = 0, total = (long)fw * fh;
        for (int i = 0; i < clear; i++) { prefix[i] = 0xffff; suffix[i] = (uint8_t)i; }
        while (pixels < total) {
            while (nbits < size) {
                if (!block) {
                    if (pos >= len || !d[pos]) goto end;
                    block = d[pos++];
                }
                if (pos >= len) goto end;
                buf |= (uint32_t)d[pos++] << nbits;
                nbits += 8;
                block--;
            }
            int code = (int)(buf & ((1u << size) - 1));
            buf >>= size;
            nbits -= size;
            if (code == clear) { next = clear + 2; size = min_code + 1; old = -1; continue; }
            if (code == eoi) break;
            int sp = 0, c = code;
            if (old < 0) {
                if (code >= clear) goto end;
                stack[sp++] = (uint8_t)code;
                first = (uint8_t)code;
            } else {
                if (code > next || (code == next && next >= 4096)) goto end;
                if (code == next) { stack[sp++] = first; c = old; }
                while (c >= clear && sp < 4096) { stack[sp++] = suffix[c]; c = prefix[c]; }
                if (sp >= 4096) goto end;
                stack[sp++] = (uint8_t)c;
                first = (uint8_t)c;
                if (next < 4096) {
                    prefix[next] = (uint16_t)old;
                    suffix[next] = first;
                    next++;
                    if (next == 1 << size && size < 12) size++;
                }
            }
            old = code;
            while (sp && pixels < total) {
                acc_pixel(&acc, fx + x, fy + row, grey[stack[--sp]]);
                pixels++;
                if (++x == fw) {
                    x = 0;
                    if (!interlaced) row = ++y;
                    else {
                        row += istep[pass];
                        while (row >= fh && pass < 3) { pass++; row = istart[pass]; }
                    }
                }
            }
        }
    end:
        /* a short picture keeps what came (the rest stays white) */
        for (; pixels < total; pixels++) {
            acc_pixel(&acc, fx + x, fy + row, 255);
            if (++x == fw) {
                x = 0;
                if (!interlaced) row = ++y;
                else {
                    row += istep[pass];
                    while (row >= fh && pass < 3) { pass++; row = istart[pass]; }
                    if (row >= fh) break;
                }
            }
        }
        ok = acc_finish(&acc, out);
    }
done:
    free(prefix);
    free(suffix);
    free(stack);
    acc_free(&acc);
    return ok;
}

/* ---- JPEG (TJpgDec) ---- */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
    acc_t *acc;
} jpg_t;

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpg_t *j = jd->device;
    if (n > j->len - j->pos) n = j->len - j->pos;
    if (buf) memcpy(buf, j->data + j->pos, n);
    j->pos += n;
    return n;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    jpg_t *j = jd->device;
    const uint8_t *g = bitmap;
    int w = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; y++, g += w) acc_row(j->acc, r->left, y, g, w, 1);
    return 1;
}

static bool jpg_decode(const uint8_t *d, size_t len, int max_w, int max_h, cyb_pic_t *out)
{
    enum { POOL = 4096 };
    void *pool = malloc(POOL);
    JDEC *jd = malloc(sizeof *jd);
    jpg_t j = { d, len, 0, NULL };
    acc_t acc;
    bool ok = false;
    memset(&acc, 0, sizeof acc);
    if (!pool || !jd || jd_prepare(jd, jpg_in, pool, POOL, &j) != JDR_OK) goto done;
    {
        int w = jd->width, h = jd->height;
        /* the target size first, then the biggest shrink that stays above it */
        int tw = w, th = h;
        if (tw > max_w) { th = (int)((int64_t)th * max_w / tw); tw = max_w; }
        if (th > max_h) { tw = (int)((int64_t)tw * max_h / th); th = max_h; }
        int scale = 0;
        while (scale < 3 && (w >> (scale + 1)) >= tw && (h >> (scale + 1)) >= th) scale++;
        int sw = (w + (1 << scale) - 1) >> scale, sh = (h + (1 << scale) - 1) >> scale;
        if (!acc_init(&acc, sw, sh, tw, th)) goto done;
        j.acc = &acc;
        JRESULT rc = jd_decomp(jd, jpg_out, (uint8_t)scale);
        if (rc == JDR_OK || rc == JDR_INP) ok = acc_finish(&acc, out);
    }
done:
    acc_free(&acc);
    free(jd);
    free(pool);
    return ok;
}

bool cyb_image_decode(const uint8_t *data, size_t len, int max_w, int max_h, cyb_pic_t *out)
{
    memset(out, 0, sizeof *out);
    if (len >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return png_decode(data, len, max_w, max_h, out);
    if (len >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))) return gif_decode(data, len, max_w, max_h, out);
    if (len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return jpg_decode(data, len, max_w, max_h, out);
    return false;
}
