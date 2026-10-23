/* Deflate decoding (RFC 1951) for the packed parts the gate sends
 * (gate/cy_deflate.c), from cybiko-doom. Codes of up to 9 bits come from
 * a table; longer ones are decoded bit by bit from their canonical form,
 * as in Mark Adler's puff. It includes nothing, so the tests can build it
 * on a PC too. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef short s16;
typedef long s32;
typedef unsigned long u32;

int inflate(void *dst, u32 dstlen, const void *src, u32 srclen);

#define FAST 9                  /* bits looked up at once */

typedef struct {
    u16 count[16];              /* codes of each length */
    u16 symbol[288];            /* symbols by code */
    u16 fast[1 << FAST];        /* next FAST bits: length << 9 | symbol, 0: longer */
} huff_t;

static const u8 *in, *inend;
static u8 *out, *outbase, *outend;
static u32 bitbuf;
static u8 bitcnt, bad;
static huff_t lencode, distcode;

static const u16 mask[17] = {
    0, 1, 3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535};

static u16 bits(u8 need)
{
    u16 v;
    while (bitcnt < need) {
        if (in >= inend) {
            bad = 1;
            return 0;
        }
        bitbuf |= (u32)*in++ << bitcnt;
        bitcnt += 8;
    }
    v = (u16)bitbuf & mask[need];
    bitbuf >>= need;
    bitcnt -= need;
    return v;
}

static s16 decode(const huff_t *h)
{
    s32 code = 0, first = 0, index = 0, count;
    u8 len;
    u16 e;
    while (bitcnt < FAST && in < inend) {
        bitbuf |= (u32)*in++ << bitcnt;
        bitcnt += 8;
    }
    e = h->fast[(u16)bitbuf & mask[FAST]];
    if (e && (e >> 9) <= bitcnt) {
        len = (u8)(e >> 9);
        bitbuf >>= len;
        bitcnt -= len;
        return (s16)(e & 511);
    }
    for (len = 1; len < 16; len++) {
        code |= bits(1);
        count = h->count[len];
        if (code - count < first) return (s16)h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    bad = 1;
    return 0;
}

/* the canonical code for lengths length[0..n-1]; 0 if over-subscribed */
static int construct(huff_t *h, const u8 *length, u16 n)
{
    u16 offs[16], i;
    s32 left = 1;
    for (i = 0; i < 16; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[length[i]]++;
    for (i = 0; i < 1 << FAST; i++) h->fast[i] = 0;
    if (h->count[0] == n) return 1;
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return 0;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = i;
    /* the table: a code's first bit is the lowest of the next bits */
    {
        u16 code = 0, k = 0, j, len;
        for (len = 1; len < 16; len++) {
            for (j = 0; j < h->count[len]; j++, k++, code++) {
                u16 rev = 0, b, e = (u16)(len << 9 | h->symbol[k]);
                if (len > FAST) continue;
                for (b = 0; b < len; b++)
                    if (code & (1 << b)) rev |= 1 << (len - 1 - b);
                for (; rev < 1 << FAST; rev += 1 << len) h->fast[rev] = e;
            }
            code <<= 1;
        }
    }
    return 1;
}

static const u16 lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577};
static const u8 dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(void)
{
    for (;;) {
        s16 sym = decode(&lencode);
        if (bad) return 0;
        if (sym < 256) {
            if (out >= outend) return 0;
            *out++ = (u8)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            u16 len, dist;
            sym -= 257;
            if (sym >= 29) return 0;
            len = lbase[sym] + bits(lext[sym]);
            sym = decode(&distcode);
            if (bad || sym >= 30) return 0;
            dist = dbase[sym] + bits(dext[sym]);
            if (bad || dist > (u32)(out - outbase) || len > (u32)(outend - out)) return 0;
            while (len--) {
                *out = out[-(s32)dist];
                out++;
            }
        }
    }
}

static u8 length[288 + 32];     /* code lengths */

static int fixed(void)
{
    u16 i;
    for (i = 0; i < 144; i++) length[i] = 8;
    for (; i < 256; i++) length[i] = 9;
    for (; i < 280; i++) length[i] = 7;
    for (; i < 288; i++) length[i] = 8;
    construct(&lencode, length, 288);
    for (i = 0; i < 30; i++) length[i] = 5;
    construct(&distcode, length, 30);
    return codes();
}

static int dynamic(void)
{
    static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    u16 nlen, ndist, ncode, i;
    nlen = bits(5) + 257;
    ndist = bits(5) + 1;
    ncode = bits(4) + 4;
    if (nlen > 286 || ndist > 30) return 0;
    for (i = 0; i < 19; i++) length[order[i]] = i < ncode ? (u8)bits(3) : 0;
    if (bad || !construct(&lencode, length, 19)) return 0;
    for (i = 0; i < nlen + ndist;) {
        s16 sym = decode(&lencode);
        u8 v = 0;
        u16 rep;
        if (bad) return 0;
        if (sym < 16) {
            length[i++] = (u8)sym;
            continue;
        }
        if (sym == 16) {
            if (!i) return 0;
            v = length[i - 1];
            rep = 3 + bits(2);
        } else if (sym == 17) {
            rep = 3 + bits(3);
        } else {
            rep = 11 + bits(7);
        }
        if (i + rep > nlen + ndist) return 0;
        while (rep--) length[i++] = v;
    }
    if (!length[256]) return 0;
    if (!construct(&lencode, length, nlen) || !construct(&distcode, length + nlen, ndist)) return 0;
    return codes();
}

int inflate(void *dst, u32 dstlen, const void *src, u32 srclen)
{
    u8 last;
    in = src;
    inend = in + srclen;
    out = outbase = dst;
    outend = out + dstlen;
    bitbuf = 0;
    bitcnt = 0;
    bad = 0;
    do {
        u8 type;
        last = (u8)bits(1);
        type = (u8)bits(2);
        if (bad) return 0;
        if (type == 0) {
            u16 len;
            bitbuf = 0;
            bitcnt = 0;
            if (inend - in < 4) return 0;
            len = (u16)(in[0] | in[1] << 8);
            in += 4;
            if (len > (u32)(inend - in) || len > (u32)(outend - out)) return 0;
            while (len--) *out++ = *in++;
        } else if (type == 1) {
            if (!fixed()) return 0;
        } else if (type == 2) {
            if (!dynamic()) return 0;
        } else {
            return 0;
        }
    } while (!last);
    return out == outend;
}
