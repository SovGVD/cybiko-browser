#pragma once
/* A Cybiko Wireless Internet Gate (CyWIG) for emulated Cybikos.
 *
 * On the Cybiko Classic the CyWIG was a Cybiko on a PC's serial port that
 * ran a gateway firmware, with the VCyWIG program on the PC fetching the
 * pages. Cybikos nearby saw it among the people around them, flagged as a
 * gate, and the Xtreme's WAP browser asked it for pages. This is the same
 * gate in plain C for the board (or a PC): it is one more Cybiko on the
 * emulated radio (cy_rf.c) that answers the WAP browser, fetching pages
 * through the browser's network (cyb_net_t: Wi-Fi on the board, curl on a
 * PC) and turning HTML into the WML the WAP browser reads.
 *
 * What goes over the air (worked out from CyOS 1.5 and the WAP browser):
 *
 *   packets  50 bytes (30 data, 20 parity) or 200 (120 data, 80 parity),
 *            Reed-Solomon over GF(256), polynomial 11d
 *   header   0 dst, 4 src, 8 c0|channel, 9 type and flags, 10 part of the
 *            message, 11 message number, 12 CRC-16 of the data after the
 *            header, 14 CRC-16 of bytes 0-13 (both MODBUS, big-endian)
 *   type     00 beacon (flag 04: "I am a CyWIG"), 20 message part (flag
 *            10: acknowledge it), 40 acknowledgement (data byte 0: code in
 *            the top 4 bits: 2 got it, 4 delivered, 5 nobody to take it),
 *            60 "was it delivered?"
 *   message  flags, 0, msg id, [p0], [p1], to (a port number or a process
 *            name), [length, data]
 *
 * The WAP browser sends msg 1022 to port 93 with p0 = 1, p1 = a request
 * number and the URL as data; the gate answers with msg 1022 to process
 * "WAP": p0 = number << 16 | 6, p1 = page size << 16 | part size and part
 * of the WML page as data, the number going up by one for each part (or
 * p0 = 5, p1 = an HTTP status for an error).
 *
 * The Browser application (cyapp/) asks with p0 = 0x21 (0x121 for the
 * bigger font) and gets the page laid out for its screen (cy_page.h) in
 * messages to process "Browser", numbered the same way. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cy_browser.h"

typedef struct cy_gate cy_gate_t;

typedef struct {
    void *user;
    /* A packet for the Cybikos: 50 or 200 bytes on radio channel ch. */
    void (*rf_send)(void *user, const uint8_t *pkt, size_t len, uint8_t ch);
    /* A line about what the gate does (may be NULL). */
    void (*log)(void *user, const char *line);
    /* Message mode (may be NULL): the gate talks whole CyOS messages
     * instead of radio packets, for a real Cybiko that is the gate and
     * passes them on (cybiko-cywig's USB Gate). Deliver msg msgid with p0,
     * p1 and data to process `process` on Cybiko `to`, and report how it
     * went with cy_gate_msg_done. False: it could not be sent at all.
     * rf_send and cy_gate_rf_in are not used then, nor beacons. */
    bool (*msg_send)(void *user, uint32_t to, uint16_t msgid, uint32_t p0, uint32_t p1,
                     const char *process, const uint8_t *data, size_t len);
} cy_gate_host_t;

/* id is the gate's Cybiko ID. net is used one request at a time. */
cy_gate_t *cy_gate_create(uint32_t id, const cy_gate_host_t *host, const cyb_net_t *net);
void cy_gate_destroy(cy_gate_t *g);

/* The page shown for the WAP browser's home page (the Cybiko site, long
 * gone): NULL or "" for the gate's own, with a search box and a few sites. */
void cy_gate_set_home(cy_gate_t *g, const char *url);

/* CyOS's fonts, from the Cybiko's flash image, for laying pages out for the
 * Browser application (cy_page.h). Until then a plain font of the same
 * heights is used. The image is only read during the call. */
void cy_gate_set_fonts(cy_gate_t *g, const uint8_t *flash, size_t len);

/* A packet heard on the radio (50 or 200 bytes). */
void cy_gate_rf_in(cy_gate_t *g, const uint8_t *pkt, size_t len, uint8_t ch);
/* Do a bit of work: beacons, fetching, sending. now_ms is any millisecond
 * clock. Returns how many milliseconds it can wait (at most). */
uint32_t cy_gate_poll(cy_gate_t *g, uint32_t now_ms);
/* Stop fetching (someone else needs the network) and answer with an error. */
void cy_gate_cancel(cy_gate_t *g);

/* Message mode: a message that came to the gate's port 93. */
void cy_gate_msg_in(cy_gate_t *g, uint32_t from, uint16_t msgid, uint32_t p0, uint32_t p1,
                    const uint8_t *data, size_t len);
/* Message mode: how the last msg_send went. */
enum { CY_GATE_DELIVERED, CY_GATE_NOBODY, CY_GATE_FAILED };
void cy_gate_msg_done(cy_gate_t *g, int result);

/* Fill in the CRCs and the parity of a 50- or 200-byte packet. */
void cy_gate_seal(uint8_t *pkt, size_t len);
