/* URLs: resolving links against the page, what the user types, forms. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "cyb_internal.h"

#define SEARCH_URL "http://frogfind.com/?q="

static size_t scheme_len(const char *s)
{
    size_t i = 0;
    if (!isalpha((unsigned char)s[0])) return 0;
    while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
    return s[i] == ':' ? i : 0;
}

/* Copy s[0..n) without spaces, tabs and new lines (they creep into hrefs). */
static void copy_clean(char *out, size_t cap, const char *s, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && s[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c == ' ') {
            if (o + 3 >= cap) break;
            memcpy(out + o, "%20", 3);
            o += 3;
            continue;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
}

/* Remove "." and ".." segments from the path that starts at p. */
static void dot_segments(char *p)
{
    char *q = p + strcspn(p, "?#");
    char saved = *q;
    *q = 0;
    char out[1024];
    size_t o = 0;
    const char *s = p;
    while (*s) {
        if (!strncmp(s, "/./", 3)) { s += 2; continue; }
        if (!strcmp(s, "/.")) { s += 1; out[o++] = '/'; break; }
        if (!strncmp(s, "/../", 4) || !strcmp(s, "/..")) {
            s += 3;
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            if (!*s) { out[o++] = '/'; break; }
            continue;
        }
        do {
            if (o + 1 < sizeof out) out[o++] = *s;
            s++;
        } while (*s && *s != '/');
    }
    out[o] = 0;
    *q = saved;
    size_t rest = strlen(q);
    if (o + rest + 1 > 1024) return;
    memmove(p + o, q, rest + 1);
    memcpy(p, out, o);
}

bool cyb_url_resolve(const char *base, const char *href, char *out, size_t cap)
{
    while (*href == ' ' || *href == '\n' || *href == '\t' || *href == '\r') href++;
    size_t n = strlen(href);
    while (n && (href[n - 1] == ' ' || href[n - 1] == '\n' || href[n - 1] == '\r')) n--;
    char ref[1024];
    copy_clean(ref, sizeof ref, href, n);
    size_t sl = scheme_len(ref);
    if (sl) {
        if (!strncasecmp(ref, "javascript:", 11) || !strncasecmp(ref, "mailto:", 7) ||
            !strncasecmp(ref, "tel:", 4) || !strncasecmp(ref, "data:", 5)) return false;
        snprintf(out, cap, "%s", ref);
        return true;
    }
    if (!base || !*base) return false;
    size_t bs = scheme_len(base);
    if (!ref[0]) { snprintf(out, cap, "%.*s", (int)strcspn(base, "#"), base); return true; }
    if (ref[0] == '#') {
        snprintf(out, cap, "%.*s%s", (int)strcspn(base, "#"), base, ref);
        return true;
    }
    if (ref[0] == '/' && ref[1] == '/') {
        snprintf(out, cap, "%.*s:%s", (int)bs, base, ref);
        return true;
    }
    /* base = scheme://authority/path?query */
    const char *auth = base + bs + 1;
    if (auth[0] == '/' && auth[1] == '/') auth += 2;
    const char *path = auth + strcspn(auth, "/?#");
    char buf[2048];
    if (ref[0] == '/') {
        snprintf(buf, sizeof buf, "%.*s%s", (int)(path - base), base, ref);
    } else if (ref[0] == '?') {
        snprintf(buf, sizeof buf, "%.*s%s", (int)(path + strcspn(path, "?#") - base), base, ref);
    } else {
        const char *end = path + strcspn(path, "?#");
        const char *slash = end;
        while (slash > path && slash[-1] != '/') slash--;
        if (slash == path) snprintf(buf, sizeof buf, "%.*s/%s", (int)(path - base), base, ref);
        else snprintf(buf, sizeof buf, "%.*s%s", (int)(slash - base), base, ref);
    }
    /* dot segments in the path part */
    const char *a = buf + bs + 1;
    if (a[0] == '/' && a[1] == '/') a += 2;
    char *p = buf + (a - buf) + strcspn(a, "/?#");
    if (*p == '/') dot_segments(p);
    snprintf(out, cap, "%s", buf);
    return true;
}

void cyb_url_from_input(const char *in, char *out, size_t cap)
{
    while (*in == ' ') in++;
    size_t n = strlen(in);
    while (n && in[n - 1] == ' ') n--;
    char s[512];
    snprintf(s, sizeof s, "%.*s", (int)n, in);
    if (scheme_len(s) && (strstr(s, "://") || !strncasecmp(s, "about:", 6))) {
        snprintf(out, cap, "%s", s);
        return;
    }
    /* Words, or a single word without a dot: search for them. */
    if (strchr(s, ' ') || (!strchr(s, '.') && strncasecmp(s, "localhost", 9))) {
        size_t o = (size_t)snprintf(out, cap, SEARCH_URL);
        if (o < cap) cyb_url_encode(out + o, cap - o, s, true);
        return;
    }
    snprintf(out, cap, "http://%s", s);
}

void cyb_url_encode(char *out, size_t cap, const char *s, bool utf8)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = strlen(out);
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 7 < cap; p++) {
        unsigned c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else if (c >= 0x80 && utf8) {
            out[o++] = '%'; out[o++] = hex[(0xc0 | c >> 6) >> 4]; out[o++] = hex[(0xc0 | c >> 6) & 15];
            out[o++] = '%'; out[o++] = hex[(0x80 | (c & 0x3f)) >> 4]; out[o++] = hex[(0x80 | (c & 0x3f)) & 15];
        } else {
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void cyb_url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)(hexval(s[1]) << 4 | hexval(s[2]));
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

void cyb_url_host(const char *url, char *out, size_t cap)
{
    const char *a = strstr(url, "://");
    a = a ? a + 3 : url;
    size_t n = strcspn(a, "/?#:");
    if (!strncmp(a, "www.", 4) && n > 4) { a += 4; n -= 4; }
    snprintf(out, cap, "%.*s", (int)n, a);
}
