/* cyusb_tcp: the USB port of an emulated Xtreme over TCP. See cyusb_tcp.h. */
#define _POSIX_C_SOURCE 200809L
#include "cyusb_tcp.h"
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    cyusb_dev_t dev;
    int fd;
} tcp_t;

static int put_all(int fd, const void *p, int n)
{
    const char *c = p;
    while (n > 0) {
        ssize_t r = send(fd, c, (size_t)n, 0);
        if (r <= 0) return -1;
        c += r;
        n -= (int)r;
    }
    return 0;
}

static int get_all(int fd, void *p, int n)
{
    char *c = p;
    while (n > 0) {
        ssize_t r = recv(fd, c, (size_t)n, 0);
        if (r <= 0) return -1;
        c += r;
        n -= (int)r;
    }
    return 0;
}

static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v & 0xffff); }
static int32_t rd32(const uint8_t *p) { return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]); }

static int tcp_control(cyusb_dev_t *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                       uint8_t *data, uint16_t len, unsigned timeout_ms)
{
    tcp_t *t = (tcp_t *)d;
    uint8_t h[13];
    h[0] = 'C'; h[1] = type; h[2] = req;
    be16(h + 3, value); be16(h + 5, index); be16(h + 7, len); be32(h + 9, timeout_ms);
    if (put_all(t->fd, h, sizeof h) < 0) return CYUSB_EIO;
    if (!(type & 0x80) && len && put_all(t->fd, data, len) < 0) return CYUSB_EIO;
    uint8_t r[4];
    if (get_all(t->fd, r, 4) < 0) return CYUSB_EIO;
    int32_t n = rd32(r);
    if ((type & 0x80) && n > 0 && (n > len || get_all(t->fd, data, n) < 0)) return CYUSB_EIO;
    return n;
}

static int tcp_bulk(cyusb_dev_t *d, uint8_t ep, uint8_t *data, int len, unsigned timeout_ms, int *timed_out)
{
    tcp_t *t = (tcp_t *)d;
    uint8_t h[10];
    h[0] = 'B'; h[1] = ep;
    be32(h + 2, (uint32_t)len); be32(h + 6, timeout_ms);
    if (put_all(t->fd, h, sizeof h) < 0) return CYUSB_EIO;
    if (!(ep & 0x80) && len && put_all(t->fd, data, len) < 0) return CYUSB_EIO;
    uint8_t r[5];
    if (get_all(t->fd, r, 5) < 0) return CYUSB_EIO;
    int32_t n = rd32(r);
    if (timed_out) *timed_out = r[4];
    if ((ep & 0x80) && n > 0 && (n > len || get_all(t->fd, data, n) < 0)) return CYUSB_EIO;
    return n;
}

static unsigned tcp_now(cyusb_dev_t *d)
{
    (void)d;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void tcp_log(cyusb_dev_t *d, const char *msg)
{
    (void)d;
    fprintf(stderr, "usb: %s\n", msg);
}

cyusb_dev_t *cyusb_tcp_open(const char *where, char *err, int err_len)
{
    char host[256];
    const char *colon = strrchr(where, ':');
    if (!colon) { snprintf(err, (size_t)err_len, "'%s' is not host:port", where); return NULL; }
    snprintf(host, sizeof host, "%.*s", (int)(colon - where), where);
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host[0] ? host : "127.0.0.1", colon + 1, &hints, &ai);
    if (e) { snprintf(err, (size_t)err_len, "%s: %s", where, gai_strerror(e)); return NULL; }
    int fd = -1;
    for (struct addrinfo *a = ai; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) < 0) { close(fd); fd = -1; }
    }
    freeaddrinfo(ai);
    if (fd < 0) { snprintf(err, (size_t)err_len, "can't connect to %s", where); return NULL; }
    tcp_t *t = calloc(1, sizeof *t);
    if (!t) { close(fd); snprintf(err, (size_t)err_len, "out of memory"); return NULL; }
    t->fd = fd;
    t->dev.control = tcp_control;
    t->dev.bulk = tcp_bulk;
    t->dev.log = tcp_log;
    t->dev.now_ms = tcp_now;
    t->dev.ctx = t;
    return &t->dev;
}

void cyusb_tcp_close(cyusb_dev_t *d)
{
    if (!d) return;
    tcp_t *t = (tcp_t *)d;
    close(t->fd);
    free(t);
}
