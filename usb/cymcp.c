/* cymcp: CyOS Multi Channel Protocol framing. See cymcp.h. */
#include "cymcp.h"
#include <string.h>

uint32_t cymcp_crc(uint32_t crc, const uint8_t *p, int n)
{
    crc = ~crc;
    while (n-- > 0) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    return ~crc;
}

int cymcp_build(uint8_t *out, int type, int pipe, int seq, const uint8_t *body, int len)
{
    if (len < 0 || len > CYMCP_MAX || (type == CYMCP_CONTROL && len > 4)) return -1;
    out[0] = 'M'; out[1] = 'M';
    out[2] = (uint8_t)type; out[3] = (uint8_t)pipe; out[4] = (uint8_t)seq;
    out[5] = (uint8_t)(len >> 8); out[6] = (uint8_t)len;
    if (len) memcpy(out + CYMCP_HDR, body, (size_t)len);
    uint32_t c = cymcp_crc(0, out, CYMCP_HDR + len);
    uint8_t *t = out + CYMCP_HDR + len;
    t[0] = (uint8_t)(c >> 24); t[1] = (uint8_t)(c >> 16); t[2] = (uint8_t)(c >> 8); t[3] = (uint8_t)c;
    return CYMCP_HDR + len + 4;
}

void cymcp_rx_init(cymcp_rx_t *r)
{
    memset(r, 0, sizeof *r);
}

/* states: 0 hunting 'M', 1 second 'M', 2 header, 3 body, 4 crc */
int cymcp_feed(cymcp_rx_t *r, uint8_t b, cymcp_frame_t *f)
{
    switch (r->state) {
    case 0:
        if (b == 'M') { r->hdr[0] = b; r->state = 1; } else r->junk++;
        return 0;
    case 1:
        if (b == 'M') { r->hdr[1] = b; r->have = 2; r->state = 2; }
        else { r->junk += 2; r->state = b == 'M' ? 1 : 0; }
        return 0;
    case 2:
        r->hdr[r->have++] = b;
        if (r->have < CYMCP_HDR) return 0;
        f->type = r->hdr[2]; f->pipe = r->hdr[3]; f->seq = r->hdr[4];
        f->len = (uint16_t)(r->hdr[5] << 8 | r->hdr[6]);
        if (f->len > CYMCP_MAX) {
            r->bad_len++; r->state = 0; r->err = "length over 512";
            return -1;
        }
        r->have = 0;
        r->state = f->len ? 3 : 4;
        return 0;
    case 3:
        f->body[r->have++] = b;
        if (r->have == f->len) { r->have = 0; r->state = 4; }
        return 0;
    default: {
        r->crc[r->have++] = b;
        if (r->have < 4) return 0;
        r->state = 0;
        uint32_t c = cymcp_crc(cymcp_crc(0, r->hdr, CYMCP_HDR), f->body, f->len);
        uint32_t got = (uint32_t)r->crc[0] << 24 | (uint32_t)r->crc[1] << 16 | (uint32_t)r->crc[2] << 8 | r->crc[3];
        if (c != got) { r->bad_crc++; r->err = "bad CRC"; return -1; }
        return 1;
    }
    }
}
