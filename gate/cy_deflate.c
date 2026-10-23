/* Deflate (RFC 1951) for the pages sent to the Browser application: one
 * block with its own Huffman codes, LZ77 matches found with hash chains
 * and one step of lazy matching. Pages are a few kilobytes, so it keeps
 * things simple rather than fast. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cy_deflate.h"

#define HASH_BITS 12
#define CHAIN 128               /* candidates tried per position */
#define WINDOW 32768

static const uint16_t lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577};
static const uint8_t dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

typedef struct {
    uint8_t *p;
    size_t len, cap;
    uint32_t bb;
    int bc;
    bool over;
} bits_t;

static void put(bits_t *b, uint32_t v, int n)
{
    b->bb |= v << b->bc;
    b->bc += n;
    while (b->bc >= 8) {
        if (b->len < b->cap) b->p[b->len++] = (uint8_t)b->bb;
        else b->over = true;
        b->bb >>= 8;
        b->bc -= 8;
    }
}

static int len_code(unsigned len)
{
    int c = 28;
    while (lbase[c] > len) c--;
    return c;
}

static int dist_code(unsigned dist)
{
    int c = 29;
    while (dbase[c] > dist) c--;
    return c;
}

/* Huffman code lengths of at most maxbits for freq[0..n): the frequencies
 * are halved until the tree is shallow enough. At least two symbols get a
 * code, so the code is complete. */
typedef struct {                /* lengths()'s work space, kept off the stack */
    uint32_t freq[288], w[2 * 288];
    int16_t parent[2 * 288];
    int live[2 * 288];
} tree_t;

static void lengths(tree_t *t, const uint32_t *freq_in, int n, int maxbits, uint8_t *len)
{
    uint32_t *freq = t->freq, *w = t->w;
    int16_t *parent = t->parent;
    int *live = t->live;
    memcpy(freq, freq_in, n * sizeof *freq);
    int used = 0;
    for (int i = 0; i < n; i++) used += freq[i] != 0;
    for (int i = 0; used < 2 && i < n; i++)
        if (!freq[i]) { freq[i] = 1; used++; }
    for (;;) {
        int nodes = 0, nlive = 0;
        for (int i = 0; i < n; i++)
            if (freq[i]) { w[i] = freq[i]; parent[i] = -1; live[nlive++] = i; }
        nodes = n;
        while (nlive > 1) {
            int a = 0, b = 1;
            if (w[live[b]] < w[live[a]]) { a = 1; b = 0; }
            for (int i = 2; i < nlive; i++) {
                if (w[live[i]] < w[live[a]]) { b = a; a = i; }
                else if (w[live[i]] < w[live[b]]) b = i;
            }
            int x = live[a], y = live[b];
            w[nodes] = w[x] + w[y];
            parent[nodes] = -1;
            parent[x] = parent[y] = (int16_t)nodes;
            if (a > b) { int t = a; a = b; b = t; }
            live[a] = nodes++;
            live[b] = live[--nlive];
        }
        int deepest = 0;
        for (int i = 0; i < n; i++) {
            len[i] = 0;
            if (!freq[i]) continue;
            int d = 0;
            for (int j = i; parent[j] >= 0; j = parent[j]) d++;
            len[i] = (uint8_t)d;
            if (d > deepest) deepest = d;
        }
        if (deepest <= maxbits) return;
        for (int i = 0; i < n; i++)
            if (freq[i]) freq[i] = (freq[i] >> 1) | 1;
    }
}

/* The canonical codes for len[0..n), bit-reversed for writing. */
static void codes(const uint8_t *len, int n, uint16_t *code)
{
    uint16_t count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    uint16_t c = 0;
    for (int b = 1; b < 16; b++) {
        c = (uint16_t)((c + count[b - 1]) << 1);
        next[b] = c;
    }
    for (int i = 0; i < n; i++) {
        if (!len[i]) { code[i] = 0; continue; }
        uint16_t v = next[len[i]]++, r = 0;
        for (int b = 0; b < len[i]; b++)
            if (v & (1 << b)) r |= (uint16_t)(1 << (len[i] - 1 - b));
        code[i] = r;
    }
}

typedef struct { uint16_t sym, dist; } sym_t;   /* dist 0: a literal */

static unsigned match(const uint8_t *in, size_t n, size_t i, const int32_t *head, const int32_t *prev,
                      unsigned *dist)
{
    if (i + 3 > n) return 0;
    unsigned best = 0;
    const size_t most = n - i < 258 ? n - i : 258;
    int32_t j = head[((in[i] << 8 ^ in[i + 1] << 4 ^ in[i + 2]) & ((1 << HASH_BITS) - 1))];
    for (int tries = 0; j >= 0 && i - (size_t)j <= WINDOW && tries < CHAIN; tries++, j = prev[j]) {
        if (in[j + best] != in[i + best]) continue;
        unsigned l = 0;
        while (l < most && in[j + l] == in[i + l]) l++;
        if (l > best) {
            best = l;
            *dist = (unsigned)(i - (size_t)j);
            if (l == most) break;
        }
    }
    return best >= 3 ? best : 0;
}

size_t cy_deflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    int32_t *head = malloc(sizeof *head << HASH_BITS);
    int32_t *prev = malloc(sizeof *prev * (n ? n : 1));
    sym_t *syms = malloc(sizeof *syms * (n + 1));
    tree_t *tree = malloc(sizeof *tree);
    if (!head || !prev || !syms || !tree) { free(head); free(prev); free(syms); free(tree); return 0; }
    for (int i = 0; i < 1 << HASH_BITS; i++) head[i] = -1;

    /* LZ77 */
    uint32_t lfreq[286] = { 0 }, dfreq[30] = { 0 };
    size_t ns = 0, i = 0, hashed = 0;
#define HASH_UP_TO(e) \
    for (; hashed < (e) && hashed + 3 <= n; hashed++) { \
        unsigned h = (in[hashed] << 8 ^ in[hashed + 1] << 4 ^ in[hashed + 2]) & ((1 << HASH_BITS) - 1); \
        prev[hashed] = head[h]; \
        head[h] = (int32_t)hashed; \
    }
    while (i < n) {
        unsigned d = 0, d2 = 0, l = match(in, n, i, head, prev, &d);
        if (l && l < 258) {
            HASH_UP_TO(i + 1);
            unsigned l2 = match(in, n, i + 1, head, prev, &d2);
            if (l2 > l) l = 0;      /* a literal, and the longer match next */
        }
        if (l) {
            syms[ns++] = (sym_t){ (uint16_t)l, (uint16_t)d };
            lfreq[257 + len_code(l)]++;
            dfreq[dist_code(d)]++;
            i += l;
        } else {
            syms[ns++] = (sym_t){ in[i], 0 };
            lfreq[in[i]]++;
            i++;
        }
        HASH_UP_TO(i);
    }
#undef HASH_UP_TO
    lfreq[256]++;

    /* the codes */
    uint8_t ll[286 + 30];
    uint16_t lcode[286], dcode[30];
    lengths(tree, lfreq, 286, 15, ll);
    lengths(tree, dfreq, 30, 15, ll + 286);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1]) hlit--;
    while (hdist > 1 && !ll[286 + hdist - 1]) hdist--;
    codes(ll, 286, lcode);
    codes(ll + 286, 30, dcode);

    /* the code lengths, run-length coded */
    uint8_t all[286 + 30], rle[286 + 30], rle_extra[286 + 30];
    int na = 0, nr = 0;
    for (int k = 0; k < hlit; k++) all[na++] = ll[k];
    for (int k = 0; k < hdist; k++) all[na++] = ll[286 + k];
    uint32_t cfreq[19] = { 0 };
    for (int k = 0; k < na;) {
        int run = 1;
        while (k + run < na && all[k + run] == all[k]) run++;
        if (!all[k] && run >= 3) {
            int r = run > 138 ? 138 : run;
            if (r >= 11) { rle[nr] = 18; rle_extra[nr++] = (uint8_t)(r - 11); }
            else { rle[nr] = 17; rle_extra[nr++] = (uint8_t)(r - 3); }
            k += r;
        } else if (all[k] && run >= 4) {
            rle[nr] = all[k]; rle_extra[nr++] = 0;
            int r = run - 1 > 6 ? 6 : run - 1;
            rle[nr] = 16; rle_extra[nr++] = (uint8_t)(r - 3);
            k += 1 + r;
        } else {
            rle[nr] = all[k]; rle_extra[nr++] = 0;
            k++;
        }
    }
    for (int k = 0; k < nr; k++) cfreq[rle[k]]++;
    uint8_t cl[19];
    uint16_t ccode[19];
    lengths(tree, cfreq, 19, 7, cl);
    codes(cl, 19, ccode);
    int hclen = 19;
    while (hclen > 4 && !cl[order[hclen - 1]]) hclen--;

    /* the block */
    bits_t b = { .p = out, .cap = cap };
    put(&b, 1, 1);
    put(&b, 2, 2);
    put(&b, (uint32_t)(hlit - 257), 5);
    put(&b, (uint32_t)(hdist - 1), 5);
    put(&b, (uint32_t)(hclen - 4), 4);
    for (int k = 0; k < hclen; k++) put(&b, cl[order[k]], 3);
    for (int k = 0; k < nr; k++) {
        put(&b, ccode[rle[k]], cl[rle[k]]);
        if (rle[k] == 16) put(&b, rle_extra[k], 2);
        else if (rle[k] == 17) put(&b, rle_extra[k], 3);
        else if (rle[k] == 18) put(&b, rle_extra[k], 7);
    }
    for (size_t k = 0; k < ns && !b.over; k++) {
        if (!syms[k].dist) {
            put(&b, lcode[syms[k].sym], ll[syms[k].sym]);
            continue;
        }
        int lc = len_code(syms[k].sym), dc = dist_code(syms[k].dist);
        put(&b, lcode[257 + lc], ll[257 + lc]);
        if (lext[lc]) put(&b, syms[k].sym - lbase[lc], lext[lc]);
        put(&b, dcode[dc], ll[286 + dc]);
        if (dext[dc]) put(&b, syms[k].dist - dbase[dc], dext[dc]);
    }
    put(&b, lcode[256], ll[256]);
    put(&b, 0, 7);              /* the last bits */
    free(head);
    free(prev);
    free(syms);
    free(tree);
    return b.over ? 0 : b.len;
}
