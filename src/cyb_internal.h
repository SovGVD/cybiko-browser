#pragma once
/* Inside the Cybiko browser: the document model, layout and drawing. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cy_browser.h"

/* ---- screen ---- */

#define BAR_H 10                       /* title bar at the top */
#define VIEW_Y BAR_H
#define VIEW_H (CYB_H - BAR_H)
#define SCROLL_W 3                     /* scroll bar at the right */
#define PAGE_W (CYB_W - SCROLL_W - 1)  /* text area width */

enum { WHITE = 0, LIGHT = 1, DARK = 2, BLACK = 3 };

void fb_clear(uint8_t *fb, int c);
void fb_pixel(uint8_t *fb, int x, int y, int c);
void fb_fill(uint8_t *fb, int x, int y, int w, int h, int c);
void fb_invert(uint8_t *fb, int x, int y, int w, int h);
void fb_frame(uint8_t *fb, int x, int y, int w, int h, int c);
/* A 2-bit picture (rows of (w + 3) / 4 bytes), clipped to rows y0..y1-1. */
void fb_image(uint8_t *fb, int x, int y, int w, int h, const uint8_t *pix, int y0, int y1);

/* ---- fonts (cyb_font.c) ---- */

enum { F_SMALL, F_SMALL_BOLD, F_BIG, F_BIG_BOLD, F_COUNT };

typedef struct {
    int8_t x, y;
    uint8_t w, h;              /* advance is x + w */
    uint32_t bits;             /* offset of the rows in the font's data */
} cyb_glyph_t;

typedef struct {
    uint8_t height;
    uint8_t count;             /* glyphs from character 32 */
    cyb_glyph_t g[256 - 32];
    uint8_t *data;             /* 1 bit a pixel, rows of (w + 7) / 8 bytes */
    bool owned;
} cyb_font_t;

/* Loads CyOS's fonts from its flash image, or the built-in 5x7 one. */
void cyb_fonts_load(cyb_font_t *fonts, const uint8_t *flash, size_t len);
void cyb_fonts_free(cyb_font_t *fonts);
int cyb_char_w(const cyb_font_t *f, uint8_t ch);
int cyb_text_w(const cyb_font_t *f, const char *s, int len);
/* Draws in colour c, clipped to rows y0..y1-1; returns the width. */
int cyb_text(uint8_t *fb, const cyb_font_t *f, int x, int y, const char *s, int len,
             int c, int y0, int y1);

/* ---- text: Unicode to the font's characters (cyb_text.c) ---- */

/* Appends what code point cp looks like in Latin-1 (one or a few chars). */
int cyb_latin1(uint32_t cp, char *out);
/* Character sets of pages. */
enum { CS_UTF8, CS_LATIN1, CS_CP1251, CS_KOI8R };
int cyb_charset(const char *name);    /* -1 if unknown */
uint32_t cyb_cs_byte(int cs, uint8_t b);   /* single-byte sets */
/* An HTML entity name (without & and ;) to a code point, 0 if unknown. */
uint32_t cyb_entity(const char *name, int len);
/* A key with Shift/Fn to the character it types, 0 if none. */
int cyb_key_char(int key, bool shift, bool fn);

/* ---- URLs (cyb_url.c) ---- */

/* Resolves href against base into out. False if it is not usable. */
bool cyb_url_resolve(const char *base, const char *href, char *out, size_t cap);
/* What a user typed in the address line to a URL (adds http://, or makes a
 * search for words). */
void cyb_url_from_input(const char *in, char *out, size_t cap);
/* Appends s form-encoded (application/x-www-form-urlencoded). */
void cyb_url_encode(char *out, size_t cap, const char *s, bool utf8);
/* Decodes a form value in place (+ and %xx). */
void cyb_url_decode(char *s);
/* The host part of a URL, for the title bar. */
void cyb_url_host(const char *url, char *out, size_t cap);

/* ---- pictures (cyb_image.c) ---- */

#define CYB_IMG_MAX_H 180         /* tallest picture shown, in dots */
#define CYB_IMG_MAX_BYTES (512 * 1024)   /* biggest picture file fetched */

typedef struct {
    uint16_t w, h;
    uint8_t *pix;              /* 2 bits a pixel, rows of (w + 3) / 4 bytes */
} cyb_pic_t;

/* Decodes a GIF, PNG or JPEG file and scales it to at most max_w x max_h,
 * dithered to four shades. False if the format is not supported. */
bool cyb_image_decode(const uint8_t *data, size_t len, int max_w, int max_h, cyb_pic_t *out);

/* ---- the document (cyb_html.c) ---- */

enum {
    IT_TEXT,        /* text in the pool, spaces already collapsed */
    IT_BREAK,       /* new line */
    IT_PARA,        /* new line and a gap */
    IT_HR,          /* a rule */
    IT_IMG,         /* picture `ref` */
    IT_FIELD,       /* form field `ref` */
    IT_INDENT,      /* left margin in or out by one step (ref = +1 / -1) */
};

enum {
    S_BOLD = 1, S_BIG = 2, S_PRE = 4, S_LINK = 8, S_CENTER = 16,
};

typedef struct {
    uint8_t kind, style;
    uint16_t target;           /* focus target + 1, 0 none */
    uint32_t text, len;        /* in the pool */
    int16_t ref;
} cyb_item_t;

enum { T_LINK, T_FIELD };
typedef struct {
    uint8_t kind;
    uint16_t field;
    uint32_t href;             /* pool offset of the resolved URL */
} cyb_target_t;

enum {
    FT_TEXT, FT_PASSWORD, FT_HIDDEN, FT_SUBMIT, FT_CHECKBOX, FT_RADIO, FT_SELECT,
    FT_TEXTAREA, FT_RESET,
};
#define FIELD_VALUE_MAX 256
typedef struct {
    uint8_t type;
    bool checked;
    int16_t form;
    uint16_t size;             /* text fields: the size attribute */
    uint32_t name;             /* pool offsets */
    uint32_t label;            /* submit buttons: the caption */
    char value[FIELD_VALUE_MAX];
    /* select: options are pool strings "value\0label\0", count and choice */
    uint32_t options;
    uint16_t n_options, chosen;
} cyb_field_t;

typedef struct {
    uint32_t action;           /* pool offset of the resolved URL */
    bool post;
} cyb_form_t;

enum { IMG_WAIT, IMG_LOADING, IMG_DONE, IMG_FAILED, IMG_SKIP };
typedef struct {
    uint32_t src, alt;         /* pool offsets (alt may be the empty string) */
    uint16_t want_w, want_h;   /* from the width/height attributes, 0 if none */
    uint8_t state;
    cyb_pic_t pic;
} cyb_img_t;

#define POOL_NONE 0            /* offset 0 holds an empty string */

typedef struct {
    char *pool;
    uint32_t pool_len, pool_cap;
    cyb_item_t *items;
    uint32_t n_items, cap_items;
    cyb_target_t *targets;
    uint32_t n_targets, cap_targets;
    cyb_field_t *fields;
    uint32_t n_fields, cap_fields;
    cyb_form_t *forms;
    uint32_t n_forms, cap_forms;
    cyb_img_t *imgs;
    uint32_t n_imgs, cap_imgs;
    char title[96];
    bool utf8;                 /* the page's encoding, for form values */
    bool oom;                  /* ran out of memory; the rest is cut */
} cyb_doc_t;

void cyb_doc_free(cyb_doc_t *d);
uint32_t cyb_doc_str(cyb_doc_t *d, const char *s, size_t len);
/* Parses an HTML (or, with plain, text) page. cs < 0: find out. */
void cyb_html_parse(cyb_doc_t *d, const char *src, size_t len, const char *base_url,
                    int cs, bool plain);

/* ---- layout (cyb_layout.c) ---- */

enum { B_TEXT, B_IMG, B_FIELD, B_HR, B_ALT };
typedef struct {
    int32_t y;
    int16_t x;
    uint16_t w;
    uint8_t h, kind, style, font;
    uint16_t target;
    uint32_t text;             /* B_TEXT/B_ALT: pool offset and length */
    uint16_t len;
    int16_t ref;
    uint32_t item;             /* the item it came from */
} cyb_box_t;

typedef struct {
    cyb_box_t *boxes;
    uint32_t n, cap;
    int32_t height;
    bool big;                  /* body text in the big font */
} cyb_layout_t;

void cyb_layout(cyb_layout_t *l, const cyb_doc_t *d, const cyb_font_t *fonts, bool big);
void cyb_layout_free(cyb_layout_t *l);
/* First box whose bottom is below y. */
uint32_t cyb_layout_find(const cyb_layout_t *l, int32_t y);
/* Draws the page rows top..top+VIEW_H at VIEW_Y; focus is the focused
 * target + 1. */
void cyb_draw_page(uint8_t *fb, const cyb_layout_t *l, const cyb_doc_t *d,
                   const cyb_font_t *fonts, int32_t top, uint16_t focus);
/* What a field box shows. */
void cyb_field_text(const cyb_doc_t *d, const cyb_field_t *f, char *out, size_t cap);

/* For tests: prints the page's boxes. */
#include <stdio.h>
void cyb_dump(const cyb_t *b, FILE *f);
