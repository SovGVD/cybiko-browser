/* Checks of the browser engine's parts: URLs, text, the HTML parser,
 * pictures and the layout. Run from the repository root (make test). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cyb_internal.h"

static int failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

static void url(const char *base, const char *href, const char *want)
{
    char out[1024];
    bool ok = cyb_url_resolve(base, href, out, sizeof out);
    if (!want) { CHECK(!ok); return; }
    if (!ok || strcmp(out, want)) { fprintf(stderr, "resolve %s + %s = %s, want %s\n", base, href, ok ? out : "(fail)", want); failed++; }
}

static void test_urls(void)
{
    const char *b = "http://a.com/b/c/d.html?q=1#top";
    url(b, "e.html", "http://a.com/b/c/e.html");
    url(b, "../e.html", "http://a.com/b/e.html");
    url(b, "../../../e.html", "http://a.com/e.html");
    url(b, "./", "http://a.com/b/c/");
    url(b, "/x", "http://a.com/x");
    url(b, "//cdn.org/y.png", "http://cdn.org/y.png");
    url(b, "?z=2", "http://a.com/b/c/d.html?z=2");
    url(b, "#s", "http://a.com/b/c/d.html?q=1#s");
    url(b, "https://other.net", "https://other.net");
    url(b, " spaced name.html ", "http://a.com/b/c/spaced%20name.html");
    url(b, "javascript:void(0)", NULL);
    url(b, "mailto:x@y.z", NULL);
    url("http://a.com", "x", "http://a.com/x");
    char out[256];
    cyb_url_from_input("cybiko games", out, sizeof out);
    CHECK(!strcmp(out, "http://frogfind.com/?q=cybiko+games"));
    cyb_url_from_input("example.com", out, sizeof out);
    CHECK(!strcmp(out, "http://example.com"));
    cyb_url_from_input("https://x.org/a", out, sizeof out);
    CHECK(!strcmp(out, "https://x.org/a"));
    out[0] = 0;
    cyb_url_encode(out, sizeof out, "caf\xe9 & co", true);
    CHECK(!strcmp(out, "caf%C3%A9+%26+co"));
    out[0] = 0;
    cyb_url_encode(out, sizeof out, "caf\xe9", false);
    CHECK(!strcmp(out, "caf%E9"));
    cyb_url_host("http://www.example.com:8080/x", out, sizeof out);
    CHECK(!strcmp(out, "example.com"));
}

static void test_text(void)
{
    char o[8];
    CHECK(cyb_latin1(0xe9, o) == 1 && (uint8_t)o[0] == 0xe9);       /* the font has it */
    CHECK(cyb_latin1(0xe6, o) == 2 && !memcmp(o, "ae", 2));          /* it does not */
    CHECK(cyb_latin1(0x2014, o) >= 1 && o[0] == '-');
    CHECK(cyb_latin1(0x41f, o) == 1 && o[0] == 'P');                 /* Cyrillic Pe */
    CHECK(cyb_entity("amp", 3) == '&');
    CHECK(cyb_entity("eacute", 6) == 0xe9);
    CHECK(cyb_entity("nosuch", 6) == 0);
    CHECK(cyb_charset("UTF-8") == CS_UTF8);
    CHECK(cyb_charset("windows-1251") == CS_CP1251);
    CHECK(cyb_charset("iso-8859-1") == CS_LATIN1);
    CHECK(cyb_cs_byte(CS_CP1251, 0xc0) == 0x410);
    CHECK(cyb_key_char('q', false, true) == '1');
    CHECK(cyb_key_char('a', true, false) == 'A');
}

static const char *text_of(const cyb_doc_t *d, char *buf, size_t cap)
{
    size_t n = 0;
    for (uint32_t i = 0; i < d->n_items && n + 2 < cap; i++) {
        const cyb_item_t *it = &d->items[i];
        if (it->kind == IT_TEXT) {
            size_t l = it->len < cap - n - 1 ? it->len : cap - n - 1;
            memcpy(buf + n, d->pool + it->text, l);
            n += l;
        } else if (it->kind == IT_BREAK || it->kind == IT_PARA) buf[n++] = '|';
    }
    buf[n] = 0;
    return buf;
}

static void test_html(void)
{
    static const char page[] =
        "<html><head><title> A  &amp; B </title><style>p{}</style><script>var x='<p>';</script></head>"
        "<body><h1>Head</h1><p>One  two\n three <a href='x.html'>link</a>.<br>Next"
        "<!-- a comment <p> --><ul><li>i1<li>i2</ul>"
        "<form action='/s' method=post><input name=q value='v&eacute;'><input type=hidden name=h value='a&amp;b'>"
        "<select name=s><option>One<option value=2 selected>Two</select><input type=submit></form>"
        "<img src='p.png' alt='pic' width=10 height=5><img src='t.gif' width=1 height=1></body>";
    cyb_doc_t d = { 0 };
    char buf[512];
    cyb_html_parse(&d, page, sizeof page - 1, "http://h.org/dir/page.html", -1, false);
    CHECK(!strcmp(d.title, "A & B "));
    text_of(&d, buf, sizeof buf);
    CHECK(strstr(buf, "Head|One two three link.|Next"));
    CHECK(strstr(buf, "* i1|* i2"));
    CHECK(!strstr(buf, "var x"));
    CHECK(!strstr(buf, "comment"));
    CHECK(d.n_targets >= 1 && d.targets[0].kind == T_LINK && !strcmp(d.pool + d.targets[0].href, "http://h.org/dir/x.html"));
    CHECK(d.n_forms == 1 && d.forms[0].post && !strcmp(d.pool + d.forms[0].action, "http://h.org/s"));
    CHECK(d.n_fields == 4);
    CHECK(!strcmp(d.fields[0].value, "v\xe9"));
    CHECK(d.fields[1].type == FT_HIDDEN && !strcmp(d.fields[1].value, "a&b"));
    CHECK(d.fields[2].type == FT_SELECT && d.fields[2].n_options == 2 && d.fields[2].chosen == 1);
    CHECK(d.n_imgs == 1 && d.imgs[0].want_w == 10 && !strcmp(d.pool + d.imgs[0].src, "http://h.org/dir/p.png"));
    cyb_doc_free(&d);

    /* character sets */
    static const char cp[] = "<meta charset=windows-1251><p>\xcf\xf0\xe8";
    cyb_html_parse(&d, cp, sizeof cp - 1, "http://x/", -1, false);
    CHECK(!strcmp(text_of(&d, buf, sizeof buf), "Pri"));
    cyb_doc_free(&d);
    static const char u8[] = "<p>caf\xc3\xa9 \xe2\x80\x94 ok";
    cyb_html_parse(&d, u8, sizeof u8 - 1, "http://x/", -1, false);
    CHECK(!strcmp(text_of(&d, buf, sizeof buf), "caf\xe9 - ok"));
    cyb_doc_free(&d);
    cyb_html_parse(&d, "line 1\n  line 2 <b>", 19, "http://x/", -1, true);
    CHECK(!strcmp(text_of(&d, buf, sizeof buf), "line 1|  line 2 <b>"));
    cyb_doc_free(&d);
}

static void test_layout(void)
{
    static cyb_font_t fonts[F_COUNT];
    cyb_fonts_load(fonts, NULL, 0);           /* the built-in block font */
    cyb_doc_t d = { 0 };
    static const char page[] = "<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod "
                               "tempor incididunt ut labore et dolore magna aliqua.<p><a href=a>A link</a>";
    cyb_html_parse(&d, page, sizeof page - 1, "http://x/", -1, false);
    cyb_layout_t l = { 0 };
    cyb_layout(&l, &d, fonts, false);
    CHECK(l.n > 3 && l.height > 20);
    for (uint32_t i = 0; i < l.n; i++) CHECK(l.boxes[i].x + l.boxes[i].w <= PAGE_W);
    uint8_t fb[CYB_FB_SIZE];
    fb_clear(fb, WHITE);
    cyb_draw_page(fb, &l, &d, fonts, 0, 1);
    int dark = 0;
    for (int i = 0; i < CYB_FB_SIZE; i++) dark += fb[i] != 0;
    CHECK(dark > 50);
    cyb_layout_free(&l);
    cyb_doc_free(&d);
}

static void picture(const char *path, bool ok, int w, int h)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no %s\n", path); failed++; return; }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    cyb_pic_t p;
    bool got = cyb_image_decode(buf, n, 156, 180, &p);
    if (got != ok || (ok && (p.w != w || p.h != h))) {
        fprintf(stderr, "%s: %s %dx%d\n", path, got ? "decoded" : "failed", got ? p.w : 0, got ? p.h : 0);
        failed++;
    }
    if (got) {
        /* the white corner stays white, the dark middle is dark */
        CHECK((p.pix[0] >> 6) == WHITE);
        free(p.pix);
    }
}

static void test_pictures(void)
{
    picture("tests/pages/ball.png", true, 156, 93);
    picture("tests/pages/ball_adam7.png", true, 156, 93);
    picture("tests/pages/ball.gif", true, 156, 93);
    picture("tests/pages/ball_igif.gif", true, 156, 93);
    picture("tests/pages/ball.jpg", true, 156, 93);
    picture("tests/pages/ball_prog.jpg", false, 0, 0);
    picture("tests/pages/dot_alpha.png", true, 40, 40);
}

int main(void)
{
    test_urls();
    test_text();
    test_html();
    test_layout();
    test_pictures();
    if (failed) { fprintf(stderr, "%d failed\n", failed); return 1; }
    printf("all good\n");
    return 0;
}
