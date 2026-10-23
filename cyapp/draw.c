/* Drawing on CyOS's screen buffer: 160 x 100, 2 bits a pixel, rows of 40
 * bytes, the leftmost pixel in the top bits, 3 black. Text is CyOS's. */
#include "app.h"

u8 *fb;
extern struct DisplayGraphics *gfx;

static struct Font **const fonts[4] = {
    &cy_mini_normal_font, &cy_mini_bold_font, &cy_cool_normal_font, &cy_cool_bold_font,
};
static const int colors[4] = { CLR_WHITE, CLR_LTGRAY, CLR_DKGRAY, CLR_BLACK };
static const u8 fill[4] = { 0x00, 0x55, 0xaa, 0xff };

static int clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > SCR_W) *w = SCR_W - *x;
    if (*y + *h > SCR_H) *h = SCR_H - *y;
    return *w > 0 && *h > 0;
}

void fb_fill(int x, int y, int w, int h, int c)
{
    int j, i;
    if (!clip(&x, &y, &w, &h)) return;
    for (j = y; j < y + h; j++) {
        u8 *row = fb + j * (SCR_W / 4);
        i = x;
        while (i < x + w && (i & 3)) {
            int sh = 6 - 2 * (i & 3);
            row[i >> 2] = (u8)((row[i >> 2] & ~(3 << sh)) | (c << sh));
            i++;
        }
        while (i + 4 <= x + w) { row[i >> 2] = fill[c]; i += 4; }
        while (i < x + w) {
            int sh = 6 - 2 * (i & 3);
            row[i >> 2] = (u8)((row[i >> 2] & ~(3 << sh)) | (c << sh));
            i++;
        }
    }
}

void fb_invert(int x, int y, int w, int h)
{
    int j, i;
    if (!clip(&x, &y, &w, &h)) return;
    for (j = y; j < y + h; j++) {
        u8 *row = fb + j * (SCR_W / 4);
        for (i = x; i < x + w; i++) row[i >> 2] ^= (u8)(3 << (6 - 2 * (i & 3)));
    }
}

void fb_frame(int x, int y, int w, int h, int c)
{
    fb_fill(x, y, w, 1, c);
    fb_fill(x, y + h - 1, w, 1, c);
    fb_fill(x, y, 1, h, c);
    fb_fill(x + w - 1, y, 1, h, c);
}

void text(int font, int x, int y, const char *s, int len, int c)
{
    if (len <= 0 || y >= SCR_H || y <= -16) return;
    cy_Graphics_set_font(gfx, *fonts[font & 3]);
    cy_TGraph_set_color(gfx, colors[c]);
    cy_Graphics_draw_text_Ex(gfx, s, x, y, len);
}

int text_w(int font, const char *s, int len)
{
    if (len <= 0) return 0;
    cy_Graphics_set_font(gfx, *fonts[font & 3]);
    return cy_Graphics_string_width_Ex(gfx, s, len);
}

int font_h(int font)
{
    return cy_Font_get_char_height(*fonts[font & 3]);
}

int slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

void scpy(char *d, const char *s, int cap)
{
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
