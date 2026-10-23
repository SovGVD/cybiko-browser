/* The Browser application on the Cybiko: pages come laid out from the gate
 * (gate/cy_page.h), this only draws them and handles the keys. */
#pragma once
#include "cyos.h"

#define SCR_W 160
#define SCR_H 100
#define BAR_H 10                    /* title bar */
#define VIEW_Y BAR_H
#define VIEW_H (SCR_H - BAR_H)
#define SCROLL_W 3
#define LINE_STEP 10

/* the page format (gate/cy_page.h) */
enum { PB_TEXT, PB_IMG, PB_FIELD, PB_HR, PB_ALT, PB_MORE };
enum { PT_LINK, PT_FIELD };
enum { FT_TEXT, FT_PASSWORD, FT_HIDDEN, FT_SUBMIT, FT_CHECKBOX, FT_RADIO, FT_SELECT, FT_TEXTAREA, FT_RESET };
#define S_LINK 8
#define VALUE_MAX 80

enum { WHITE, LIGHT, DARK, BLACK };

struct box {
    u8 kind, font, x, w, h, style, target, len;
    long y;
    const char *text;
};

struct target {
    u8 kind, type, form, checked;
    u16 var;
    u8 n_options, chosen;
    const u8 *options;              /* n_options x (value, label) */
    const u8 *url;                  /* links, submit buttons: length byte, URL */
    char *value;                    /* fields: what is in them now */
};

#define MAX_TARGETS 256
#define MAX_BOXES 700

struct page {
    u8 *data;
    u16 len;
    long height;                    /* with the More line */
    u8 more;
    char title[41];
    u16 n_boxes;
    u16 box[MAX_BOXES];             /* offsets in data */
    u16 n_targets;
    struct target t[MAX_TARGETS];   /* t[k - 1] is target k */
};

extern struct page pg;
extern long top;                    /* the page row at the top of the view */
extern u8 focus;                    /* target, 0 none */
extern u8 big;                      /* the bigger font */
extern char status[24];             /* in the title bar, right */

/* page.c */
u8 page_set(u8 *data, u16 len);     /* takes data (malloc'd); 0 if bad */
void page_free(void);
void page_box(u16 i, struct box *b);
void page_draw(void);
void page_scroll_to(long y);
void page_move_focus(int dir);
void page_scroll(int dir);
void field_text(const struct target *t, char *out, int cap);

/* draw.c */
extern u8 *fb;
void fb_fill(int x, int y, int w, int h, int c);
void fb_invert(int x, int y, int w, int h);
void fb_frame(int x, int y, int w, int h, int c);
void text(int font, int x, int y, const char *s, int len, int c);
int text_w(int font, const char *s, int len);
int font_h(int font);

/* strings */
int slen(const char *s);
void scpy(char *d, const char *s, int cap);
