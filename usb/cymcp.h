/* cymcp: CyOS 1.5's Multi Channel Protocol (MCP), the framing that carries
 * numbered pipes over the Cybiko Xtreme's USB bulk endpoints.
 *
 * A frame is
 *     'M' 'M' type pipe seq len_hi len_lo body[len] crc32 (4 bytes, big endian)
 * with the standard CRC-32 (reflected, init ffffffff, final complement) over
 * everything before it. Decoded from CyOS 1.5.08's kernel (receive parser
 * 4aa984-4ab300, sender 4a9f80); see cybiko-docs usb.md. */
#pragma once
#include <stdint.h>

#define CYMCP_DATA    0x11   /* pipe bytes, body up to CYMCP_MAX */
#define CYMCP_CONTROL 0x12   /* pipe control, body up to 4 bytes */
#define CYMCP_MAX     512
#define CYMCP_HDR     7
#define CYMCP_FRAME   (CYMCP_HDR + CYMCP_MAX + 4)

uint32_t cymcp_crc(uint32_t crc, const uint8_t *p, int n);   /* start with 0 */

/* Builds a frame into out (at least CYMCP_HDR + len + 4 bytes); returns its
 * size, or -1 when len is too big for the type. */
int cymcp_build(uint8_t *out, int type, int pipe, int seq, const uint8_t *body, int len);

typedef struct {
    uint8_t type, pipe, seq;
    uint16_t len;
    uint8_t body[CYMCP_MAX];
} cymcp_frame_t;

/* Byte-at-a-time receiver. cymcp_feed returns 1 when f holds a whole frame
 * with a good CRC, 0 while it waits for more, and -1 for a frame it threw
 * away (bad CRC or length); r->err says why. Bytes outside a frame are
 * counted in r->junk. */
typedef struct {
    int state, have;
    uint8_t hdr[CYMCP_HDR];
    uint8_t crc[4];
    uint32_t junk, bad_crc, bad_len;
    const char *err;
} cymcp_rx_t;

void cymcp_rx_init(cymcp_rx_t *r);
int cymcp_feed(cymcp_rx_t *r, uint8_t b, cymcp_frame_t *f);

/* ------------------------------------------------------------------ */
/* The PC end of the link                                             */
/* ------------------------------------------------------------------ */
/* Flow control, the same in both directions (CyOS 1.5.08 kernel 4ab398
 * for receiving, 4ab8e4 for sending): the receiving end asks for frame N
 * of a pipe with a control frame whose body is 02 N; the sender sends data
 * frame N only when asked for it, and takes 02 N+1 (or 03 N) as the
 * acknowledgement. Sequence numbers are 8 bits. Every pipe goes one way:
 * the console takes command lines on pipe 1 and answers on pipe 2. CyOS
 * keeps asking (about every 3 s) and gives up on a frame nobody asked for
 * within the stream's timeout (3 s for the console), so a PC must keep
 * asking for the pipes it reads. All frames go over bulk OUT 2 / IN 1. */
#include "cyusb.h"

#define CYMCP_CONSOLE_IN  1   /* PC -> Xtreme: command lines */
#define CYMCP_CONSOLE_OUT 2   /* Xtreme -> PC: console output */

typedef struct cymcp_link cymcp_link_t;

cymcp_link_t *cymcp_open(cyusb_dev_t *dev);
void cymcp_close(cymcp_link_t *l);

/* Start or stop asking the Xtreme for data on a pipe it sends on. */
void cymcp_listen(cymcp_link_t *l, int pipe, int on);

/* Moves frames: reads what the Xtreme sent for up to ms, answers it, and
 * asks again on the pipes being listened to. Gives < 0 when the device is
 * gone. */
int cymcp_poll(cymcp_link_t *l, unsigned ms);

/* Sends bytes on a pipe, in frames of up to 512, each one when the Xtreme
 * asks for it or straight away; waits for every acknowledgement. Gives the
 * bytes sent, or < 0 (CYUSB_ETIMEOUT when the Xtreme stops answering). */
int cymcp_write(cymcp_link_t *l, int pipe, const void *data, int len, unsigned timeout_ms);

/* Takes up to max bytes that arrived on a pipe, polling for up to
 * timeout_ms while there are none. Gives the bytes taken (0 when none
 * came), or < 0. */
int cymcp_read(cymcp_link_t *l, int pipe, void *buf, int max, unsigned timeout_ms);

/* Bytes waiting on a pipe. */
int cymcp_available(cymcp_link_t *l, int pipe);

/* Counters, for logs. */
typedef struct {
    uint32_t frames_in, frames_out, data_in, data_out, resent, dup, bad, unexpected;
} cymcp_stats_t;
const cymcp_stats_t *cymcp_stats(cymcp_link_t *l);
