#pragma once
/* Web pages for the Browser application on the Cybiko (cy_page.c): the gate
 * lays the page out for the Cybiko's 160-dot screen with CyOS's own fonts
 * and sends what to draw, a part of the page at a time. The application
 * (cyapp/) only draws, scrolls, follows links and fills in forms.
 *
 * A part (all numbers big-endian):
 *
 *   0   'C' 'B' 1        magic and version
 *   3   u8   flags        1: a later part follows (its URL is target 1)
 *   4   u16  height       of the part, in dots
 *   6   u16  targets      offset of the target table
 *   8   u8 n, n bytes     the title
 *       boxes up to the target table, each:
 *         u8 kind << 4 | font, u8 x, u16 y, u8 w, u8 h, u8 style, u8 target
 *         (kind PB_TEXT and PB_ALT: then u8 n and n bytes of text;
 *         PB_PIC, a picture: then h rows of (w + 3) / 4 bytes, 2 bits a
 *         pixel, the leftmost in the top bits, 0 white to 3 black)
 *       target table: u8 count, then each target (numbered from 1):
 *         PT_LINK   u8 n, URL
 *         PT_FIELD  u8 type (FT_*), u8 form, u8 flags (1 checked),
 *                   u16 var, u8 n, value,
 *                   select: u8 options, u8 chosen, options (u8 n, value,
 *                           u8 n, label);
 *                   submit: u8 n, URL (the form's, before "?v<var>=...")
 *
 * Text is Latin-1, as CyOS's fonts have it. Fonts are F_* (mini normal,
 * mini bold, cool normal, cool bold), styles S_*. A form is sent by going
 * to its submit button's URL with "?v<var>=value&..." for its fields: a
 * check box's value only when it is checked, a radio button's when it is
 * the one picked (its var is its group's). */
#include "cyb_internal.h"

#define CY_PAGE_MAX 2500       /* a part's size: about 9 s on the radio */
#define CY_PAGE_PICS_MAX 8000  /* with pictures: about 30 s */

enum { PB_TEXT, PB_IMG, PB_FIELD, PB_HR, PB_ALT, PB_PIC };
enum { PT_LINK, PT_FIELD };

typedef struct {
    void *user;
    /* The URL to put in the page for url (it may hand out a shorter one). */
    void (*link)(void *user, const char *url, char *out, size_t cap);
    /* The URL a form's submit button goes to (see cy_wml.h). */
    void (*form)(void *user, const cyb_doc_t *d, int form, int submit, char *out, size_t cap);
    /* The URL of the part that starts at y. */
    void (*more)(void *user, int32_t y, char *out, size_t cap);
} cy_page_links_t;

/* Writes the part of the laid-out page that starts at *top, at most cap
 * bytes, and moves *top to where the next part starts (l->height when it
 * was the last). Returns the length. */
size_t cy_page_part(const cyb_doc_t *d, const cyb_layout_t *l, int32_t *top, uint8_t *out, size_t cap,
                    const cy_page_links_t *k);
