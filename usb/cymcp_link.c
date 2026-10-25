/* cymcp_link: the PC end of CyOS's Multi Channel Protocol over USB. See
 * cymcp.h for the flow control. Nothing that arrives is dropped quietly:
 * frames the link cannot use are logged through the device's log.
 *
 * A new link can meet an Xtreme whose pipes are part way through: CyOS
 * keeps its sequence numbers when only the PC's side starts again (a
 * program reopening the device, or USB Gate putting the Xtreme back on
 * the bus under another USB ID). So until a pipe has moved a frame, the
 * link takes the Xtreme's numbers: the first data frame it is sent, or
 * the frame the Xtreme asks for. */
#include "cymcp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EP_OUT 0x02
#define EP_IN  0x81
#define RX_BUF 8192
#define ASK_MS 250     /* ask again this often on a quiet pipe we listen to */
#define RESEND_MS 1000 /* send an unacknowledged frame again after this */

typedef struct {
    /* receiving (Xtreme -> PC) */
    uint8_t want;          /* next sequence number we ask for */
    uint8_t listening;
    uint8_t got_any;       /* a frame was taken since the link opened */
    unsigned asked_at;
    unsigned heard_at;     /* last frame of any kind on the pipe, 0 never */
    uint8_t *buf;
    int head, count;
    /* sending (PC -> Xtreme) */
    uint8_t seq;           /* sequence number of the frame being sent */
    int asked;             /* last 02 N heard from the Xtreme, -1 none */
    int done3;             /* last 03 N heard, -1 none */
    uint8_t sent_any;      /* a frame was acknowledged since the link opened */
} pipe_t;

struct cymcp_link {
    cyusb_dev_t *dev;
    cymcp_rx_t rx;
    cymcp_frame_t f;
    pipe_t p[256];
    cymcp_stats_t st;
    int dead;
};

static void lg(cymcp_link_t *l, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (l->dev->log) l->dev->log(l->dev, b);
    else fprintf(stderr, "mcp: %s\n", b);
}

static unsigned now(cymcp_link_t *l) { return l->dev->now_ms(l->dev); }

cymcp_link_t *cymcp_open(cyusb_dev_t *dev)
{
    cymcp_link_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->dev = dev;
    cymcp_rx_init(&l->rx);
    for (int i = 0; i < 256; i++) l->p[i].asked = l->p[i].done3 = -1;
    return l;
}

void cymcp_close(cymcp_link_t *l)
{
    if (!l) return;
    for (int i = 0; i < 256; i++) free(l->p[i].buf);
    free(l);
}

const cymcp_stats_t *cymcp_stats(cymcp_link_t *l) { return &l->st; }

static int send_frame(cymcp_link_t *l, int type, int pipe, int seq, const uint8_t *body, int len)
{
    uint8_t fr[CYMCP_FRAME];
    int n = cymcp_build(fr, type, pipe, seq, body, len);
    if (n < 0) return CYUSB_EIO;
    int to = 0;
    int r = l->dev->bulk(l->dev, EP_OUT, fr, n, 2000, &to);
    if (r != n) {
        lg(l, "OUT of a %d byte frame (pipe %d) moved %d%s", n, pipe, r, to ? ", timed out" : "");
        if (r < 0 && r != CYUSB_ETIMEOUT) l->dead = 1;
        return r < 0 ? r : CYUSB_ETIMEOUT;
    }
    l->st.frames_out++;
    return 0;
}

static int ask(cymcp_link_t *l, int pipe)
{
    pipe_t *p = &l->p[pipe];
    uint8_t b[2] = { 2, p->want };
    p->asked_at = now(l);
    return send_frame(l, CYMCP_CONTROL, pipe, 0, b, 2);
}

void cymcp_listen(cymcp_link_t *l, int pipe, int on)
{
    pipe_t *p = &l->p[pipe & 0xff];
    if (on && !p->buf) p->buf = malloc(RX_BUF);
    p->listening = on && p->buf;
    if (p->listening) ask(l, pipe & 0xff);
}

static void got_data(cymcp_link_t *l)
{
    cymcp_frame_t *f = &l->f;
    pipe_t *p = &l->p[f->pipe];
    if (!p->listening) {
        l->st.unexpected++;
        lg(l, "data frame on pipe %d, which nobody reads (%d bytes, seq %d)", f->pipe, f->len, f->seq);
        return;
    }
    if (f->seq != p->want && !p->got_any) {
        lg(l, "pipe %d: the Xtreme is at frame %d, taking it from there", f->pipe, f->seq);
        p->want = f->seq;
    }
    if (f->seq != p->want) {
        /* Sent again for an older ask of ours that was still queued on the
         * Xtreme: counted in stats.dup, it already arrived once. */
        l->st.dup++;
        if (f->seq != (uint8_t)(p->want - 1))
            lg(l, "pipe %d: frame %d when waiting for %d", f->pipe, f->seq, p->want);
        return;
    }
    int room = RX_BUF - p->count;
    if (f->len > room) {
        /* keep it on the Xtreme: don't acknowledge until there is room */
        lg(l, "pipe %d: %d bytes unread, holding frame %d", f->pipe, p->count, f->seq);
        return;
    }
    for (int i = 0; i < f->len; i++) p->buf[(p->head + p->count + i) % RX_BUF] = f->body[i];
    p->count += f->len;
    l->st.data_in += f->len;
    p->want++;
    p->got_any = 1;
    ask(l, f->pipe);
}

static void got_control(cymcp_link_t *l)
{
    cymcp_frame_t *f = &l->f;
    pipe_t *p = &l->p[f->pipe];
    int kind = f->len ? f->body[0] : -1, v = f->len > 1 ? f->body[1] : -1;
    if (kind == 2 && v >= 0) p->asked = v;
    else if (kind == 3 && v >= 0) p->done3 = v;
    else if (kind == 5) {
        /* The Xtreme closed its end (a program started with $pipe=N ended);
         * the next one on this pipe starts again from frame 0. */
        lg(l, "pipe %d closed by the Xtreme", f->pipe);
        p->seq = 0; p->want = 0; p->asked = p->done3 = -1;
        p->got_any = p->sent_any = 1;
    } else if (kind == 4) {
        lg(l, "pipe %d reset by the Xtreme", f->pipe);
        p->seq = 0; p->want = 0; p->asked = p->done3 = -1;
        p->got_any = p->sent_any = 1;
    } else {
        l->st.unexpected++;
        char hex[16] = "";
        for (int i = 0; i < f->len && i < 4; i++) snprintf(hex + 3 * i, 4, " %02x", f->body[i]);
        lg(l, "control frame on pipe %d not understood:%s", f->pipe, hex);
    }
}

int cymcp_poll(cymcp_link_t *l, unsigned ms)
{
    if (l->dead) return CYUSB_EIO;
    unsigned until = now(l) + ms;
    do {
        uint8_t b[512];
        int to = 0;
        unsigned left = until - now(l);
        int r = l->dev->bulk(l->dev, EP_IN, b, sizeof b, left > 50 ? 50 : left ? left : 1, &to);
        if (r < 0 && r != CYUSB_ETIMEOUT) { l->dead = 1; lg(l, "IN failed (%d)", r); return r; }
        for (int i = 0; i < r; i++) {
            int k = cymcp_feed(&l->rx, b[i], &l->f);
            if (k < 0) { l->st.bad++; lg(l, "frame thrown away: %s", l->rx.err); }
            if (k <= 0) continue;
            l->st.frames_in++;
            l->p[l->f.pipe].heard_at = now(l) ? now(l) : 1;
            if (l->f.type == CYMCP_DATA) got_data(l);
            else if (l->f.type == CYMCP_CONTROL) got_control(l);
            else { l->st.unexpected++; lg(l, "frame of type %02x on pipe %d", l->f.type, l->f.pipe); }
        }
        for (int i = 0; i < 256; i++)
            if (l->p[i].listening && now(l) - l->p[i].asked_at >= ASK_MS) ask(l, i);
        if (r > 0) break;   /* hand data up as soon as it comes */
    } while ((int)(until - now(l)) > 0);
    return l->dead ? CYUSB_EIO : 0;
}

int cymcp_write(cymcp_link_t *l, int pipe, const void *data, int len, unsigned timeout_ms)
{
    pipe_t *p = &l->p[pipe & 0xff];
    const uint8_t *d = data;
    int done = 0;
    while (done < len) {
        int n = len - done > CYMCP_MAX ? CYMCP_MAX : len - done;
        uint8_t seq = p->seq;
        unsigned start = now(l), sent_at = 0;
        int sent = 0;
        for (;;) {
            if (!p->sent_any && p->asked >= 0 && p->asked != seq && p->asked != (uint8_t)(seq + 1)) {
                lg(l, "pipe %d: the Xtreme asks for frame %d, going on from there", pipe, p->asked);
                seq = (uint8_t)p->asked;
                sent = 0;
            }
            if (p->asked == (uint8_t)(seq + 1) || p->done3 == seq) break;
            if (!sent || (p->asked == seq && now(l) - sent_at >= 100) || now(l) - sent_at >= RESEND_MS) {
                if (sent) { l->st.resent++; }
                p->asked = -1;
                int r = send_frame(l, CYMCP_DATA, pipe, seq, d + done, n);
                if (r < 0 && r != CYUSB_ETIMEOUT) return r;
                sent = 1;
                sent_at = now(l);
            }
            if (now(l) - start >= timeout_ms) {
                lg(l, "pipe %d: frame %d not acknowledged", pipe, seq);
                return done ? done : CYUSB_ETIMEOUT;
            }
            int r = cymcp_poll(l, 20);
            if (r < 0) return r;
        }
        p->seq = seq + 1;
        p->done3 = -1;
        p->sent_any = 1;
        l->st.data_out += n;
        done += n;
    }
    return done;
}

int cymcp_available(cymcp_link_t *l, int pipe) { return l->p[pipe & 0xff].count; }

unsigned cymcp_heard(cymcp_link_t *l, int pipe) { return l->p[pipe & 0xff].heard_at; }

int cymcp_read(cymcp_link_t *l, int pipe, void *buf, int max, unsigned timeout_ms)
{
    pipe_t *p = &l->p[pipe & 0xff];
    if (!p->listening) cymcp_listen(l, pipe, 1);
    unsigned start = now(l);
    while (!p->count && now(l) - start < timeout_ms) {
        int r = cymcp_poll(l, timeout_ms - (now(l) - start));
        if (r < 0) return r;
    }
    int n = p->count < max ? p->count : max;
    for (int i = 0; i < n; i++) ((uint8_t *)buf)[i] = p->buf[(p->head + i) % RX_BUF];
    p->head = (p->head + n) % RX_BUF;
    p->count -= n;
    return n;
}
