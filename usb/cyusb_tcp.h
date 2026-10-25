#pragma once
/* A Cybiko Xtreme's USB port over a TCP connection: Dragonfruit's PC
 * emulator (cybiko_host --usb-server PORT) offers its emulated Xtreme this
 * way, so PC tools can be tried against it before a real cable.
 *
 * Requests and answers, numbers big-endian:
 *   'C' type req value(2) index(2) len(2) timeout_ms(4) [data, if OUT]
 *       -> result(4, signed) [result bytes of data, if IN]
 *   'B' ep len(4) timeout_ms(4) [data, if OUT]
 *       -> result(4, signed) timed_out(1) [result bytes, if IN]
 * The server enumerates the device itself when a client connects. */
#include "cyusb.h"

/* host:port; NULL on failure, with the reason in err. */
cyusb_dev_t *cyusb_tcp_open(const char *where, char *err, int err_len);
void cyusb_tcp_close(cyusb_dev_t *d);
