/* cyusb_libusb: the Xtreme's USB port through libusb. See cyusb_libusb.h. */
#include "cyusb_libusb.h"
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    cyusb_dev_t dev;
    libusb_context *ctx;
    libusb_device_handle *h;
} lu_t;

static int result(int r)
{
    if (r >= 0) return r;
    if (r == LIBUSB_ERROR_TIMEOUT) return CYUSB_ETIMEOUT;
    if (r == LIBUSB_ERROR_PIPE) return CYUSB_ESTALL;
    return CYUSB_EIO;
}

static int lu_control(cyusb_dev_t *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                      uint8_t *data, uint16_t len, unsigned timeout_ms)
{
    lu_t *u = (lu_t *)d;
    return result(libusb_control_transfer(u->h, type, req, value, index, data, len, timeout_ms));
}

static int lu_bulk(cyusb_dev_t *d, uint8_t ep, uint8_t *data, int len, unsigned timeout_ms, int *timed_out)
{
    lu_t *u = (lu_t *)d;
    int moved = 0;
    int r = libusb_bulk_transfer(u->h, ep, data, len, &moved, timeout_ms ? timeout_ms : 1);
    if (timed_out) *timed_out = r == LIBUSB_ERROR_TIMEOUT;
    if (r == LIBUSB_ERROR_TIMEOUT) return moved;
    if (r < 0) return result(r);
    return moved;
}

static unsigned lu_now(cyusb_dev_t *d)
{
    (void)d;
#ifdef _WIN32
    return (unsigned)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static void lu_log(cyusb_dev_t *d, const char *msg)
{
    (void)d;
    fprintf(stderr, "usb: %s\n", msg);
}

cyusb_dev_t *cyusb_libusb_open(char *err, int err_len)
{
    lu_t *u = calloc(1, sizeof *u);
    if (!u) { snprintf(err, (size_t)err_len, "out of memory"); return NULL; }
    int r = libusb_init(&u->ctx);
    if (r < 0) { snprintf(err, (size_t)err_len, "libusb: %s", libusb_strerror(r)); free(u); return NULL; }
    u->h = libusb_open_device_with_vid_pid(u->ctx, CYUSB_VID, CYUSB_PID_GATE);
    if (!u->h) u->h = libusb_open_device_with_vid_pid(u->ctx, CYUSB_VID, CYUSB_PID);
    if (!u->h) {
        snprintf(err, (size_t)err_len, "no Cybiko Xtreme (USB %04x:%04x or %04x) found, or no permission to open it",
                 CYUSB_VID, CYUSB_PID_GATE, CYUSB_PID);
        libusb_exit(u->ctx);
        free(u);
        return NULL;
    }
    libusb_set_auto_detach_kernel_driver(u->h, 1);
    r = libusb_claim_interface(u->h, 0);
    if (r < 0) {
        snprintf(err, (size_t)err_len, "can't claim the Xtreme's interface: %s", libusb_strerror(r));
        libusb_close(u->h);
        libusb_exit(u->ctx);
        free(u);
        return NULL;
    }
    struct libusb_device_descriptor dd;
    if (!libusb_get_device_descriptor(libusb_get_device(u->h), &dd))
        fprintf(stderr, "usb: Xtreme at %04x:%04x%s\n", dd.idVendor, dd.idProduct,
                dd.idProduct == CYUSB_PID_GATE ? " (USB Gate running)" : "");
    u->dev.control = lu_control;
    u->dev.bulk = lu_bulk;
    u->dev.log = lu_log;
    u->dev.now_ms = lu_now;
    u->dev.ctx = u;
    return &u->dev;
}

void cyusb_libusb_close(cyusb_dev_t *d)
{
    if (!d) return;
    lu_t *u = (lu_t *)d;
    libusb_release_interface(u->h, 0);
    libusb_close(u->h);
    libusb_exit(u->ctx);
    free(u);
}
