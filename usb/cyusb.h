#pragma once
/* A USB device as the PC side of the Cybiko Xtreme's USB link sees it:
 * blocking control and bulk transfers, like libusb's synchronous calls.
 * cyusb_libusb.c opens a real Xtreme (VID 0B66, PID 0041) through
 * libusb; Dragonfruit's cybiko_host gives an emulated one. */
#include <stdint.h>

#define CYUSB_VID 0x0b66
#define CYUSB_PID 0x0041          /* CyOS running; 0x0040 is the boot ROM */

/* Results below 0 */
#define CYUSB_ETIMEOUT (-1)
#define CYUSB_ESTALL   (-2)
#define CYUSB_EIO      (-3)

typedef struct cyusb_dev {
    /* A control transfer on endpoint 0; data goes out or comes in by bit 7
     * of type. Gives the data stage's length, or < 0. */
    int (*control)(struct cyusb_dev *d, uint8_t type, uint8_t req, uint16_t value,
                   uint16_t index, uint8_t *data, uint16_t len, unsigned timeout_ms);
    /* A bulk transfer: ep with bit 7 set reads. A read ends at a short
     * packet or after len bytes. Gives the bytes moved (also when it ran
     * out of time part way, then *timed_out is set), or < 0. */
    int (*bulk)(struct cyusb_dev *d, uint8_t ep, uint8_t *data, int len, unsigned timeout_ms,
                int *timed_out);
    void (*log)(struct cyusb_dev *d, const char *msg);
    /* Milliseconds on the link's clock (the emulator's, for an emulated
     * Xtreme), for timeouts and retries. */
    unsigned (*now_ms)(struct cyusb_dev *d);
    void *ctx;
} cyusb_dev_t;
