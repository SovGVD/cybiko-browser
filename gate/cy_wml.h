#pragma once
/* Web pages as WML for the Cybiko's own WAP browser (cy_wml.c): the gate
 * parses a page with the browser's HTML reader and writes what it found as
 * a WML deck small enough for the WAP browser (5000 bytes). */
#include "cyb_internal.h"

#define CY_WML_MAX 5000            /* the biggest page the WAP browser takes */

/* Where a page part starts: an item and a byte offset in its text. */
typedef struct {
    uint32_t item, off;
} cy_wml_pos_t;

typedef struct {
    void *user;
    /* The URL to put in the page for url (it may hand out a shorter one). */
    void (*link)(void *user, const char *url, char *out, size_t cap);
    /* The URL a form's submit button goes to, before the WAP browser adds
     * "?v<field>=value&..." for the fields named v<field index>. */
    void (*form)(void *user, const cyb_doc_t *d, int form, int submit, char *out, size_t cap);
    /* The URL of the page's next part, which starts at pos. */
    void (*more)(void *user, cy_wml_pos_t pos, char *out, size_t cap);
} cy_wml_links_t;

/* Writes the page from *pos on as one deck of at most cap bytes (NUL not
 * counted) and moves *pos past what it wrote. Returns the length. */
size_t cy_wml_page(const cyb_doc_t *d, cy_wml_pos_t *pos, char *out, size_t cap, const cy_wml_links_t *l);

/* A deck with a title and a line of text (Latin-1), for errors. */
size_t cy_wml_text(char *out, size_t cap, const char *title, const char *text);
