/* The browser's network through libcurl (see cyb_net_libcurl.h): what
 * cyb_net_curl.c does with the curl program, for systems without fork()
 * and /dev/fd, such as Windows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>
#include "cyb_net_libcurl.h"

#define REDIRECTS 6
#define MAX_BODY 4000000

typedef struct {
    const char *agent;
    CURL *h;
    uint8_t *body;
    size_t len, cap, at;
    bool open;
    char final_url[1024];
    char content_type[128];
    char error[CURL_ERROR_SIZE + 32];
    cyb_jar_t *jar;
    const char *url;
} lc_t;

static lc_t the_lc;

static size_t on_body(char *p, size_t size, size_t n, void *user)
{
    lc_t *c = user;
    size_t k = size * n;
    if (c->len + k > MAX_BODY) return 0;      /* too big: curl stops with an error */
    if (c->len + k > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 65536;
        while (cap < c->len + k) cap *= 2;
        uint8_t *b = realloc(c->body, cap);
        if (!b) return 0;
        c->body = b;
        c->cap = cap;
    }
    memcpy(c->body + c->len, p, k);
    c->len += k;
    return k;
}

static size_t on_header(char *p, size_t size, size_t n, void *user)
{
    lc_t *c = user;
    size_t k = size * n;
    if (c->jar && k > 11 && !strncasecmp(p, "Set-Cookie:", 11)) {
        char line[8192];
        size_t m = k - 11 < sizeof line - 1 ? k - 11 : sizeof line - 1;
        memcpy(line, p + 11, m);
        line[m] = 0;
        line[strcspn(line, "\r\n")] = 0;
        cyb_jar_set(c->jar, c->url, line);
    }
    return k;
}

static void lc_close(void *user)
{
    lc_t *c = user;
    c->open = false;
    c->len = c->at = 0;
}

/* One request, without following a redirect: true with the body in, or a
 * redirect's target in next. */
static bool one(lc_t *c, const char *url, const char *body, const char *content_type, char *next, size_t next_cap,
                long *code)
{
    next[0] = 0;
    *code = 0;
    c->len = c->at = 0;
    c->url = url;
    CURL *h = c->h;
    curl_easy_reset(h);
    char cookie[4200] = "", ctype[160] = "";
    struct curl_slist *hdr = NULL;
    if (c->jar) {
        memcpy(cookie, "Cookie: ", 8);
        if (cyb_jar_header(c->jar, url, cookie + 8, sizeof cookie - 8)) hdr = curl_slist_append(hdr, cookie);
    }
    if (body) {
        snprintf(ctype, sizeof ctype, "Content-Type: %s", content_type ? content_type : "application/x-www-form-urlencoded");
        hdr = curl_slist_append(hdr, ctype);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    }
    char err[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_USERAGENT, c->agent);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(h, CURLOPT_MAXFILESIZE, (long)MAX_BODY);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, c);
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);   /* Windows: the system's certificates */
#endif
    CURLcode r = curl_easy_perform(h);
    curl_slist_free_all(hdr);
    if (r != CURLE_OK) {
        snprintf(c->error, sizeof c->error, "%s", err[0] ? err : curl_easy_strerror(r));
        return false;
    }
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, code);
    char *s = NULL;
    curl_easy_getinfo(h, CURLINFO_CONTENT_TYPE, &s);
    snprintf(c->content_type, sizeof c->content_type, "%s", s ? s : "");
    s = NULL;
    curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &s);
    snprintf(c->final_url, sizeof c->final_url, "%s", s ? s : url);
    if (*code >= 300 && *code < 400) {
        s = NULL;
        curl_easy_getinfo(h, CURLINFO_REDIRECT_URL, &s);
        if (s && *s) {
            snprintf(next, next_cap, "%s", s);
            return false;
        }
    }
    if (*code >= 400 && *code != 404 && c->len == 0) {
        snprintf(c->error, sizeof c->error, "The server said %ld", *code);
        return false;
    }
    return true;
}

static bool lc_open(void *user, const char *url, const char *body, const char *content_type, cyb_jar_t *jar)
{
    lc_t *c = user;
    lc_close(c);
    c->final_url[0] = c->content_type[0] = c->error[0] = 0;
    if (!c->h) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        c->h = curl_easy_init();
        if (!c->h) { snprintf(c->error, sizeof c->error, "Can't start libcurl"); return false; }
    }
    c->jar = jar;
    char at[1024], next[1024];
    snprintf(at, sizeof at, "%s", url);
    for (int hop = 0; hop <= REDIRECTS; hop++) {
        long code;
        if (one(c, at, body, content_type, next, sizeof next, &code)) { c->open = true; return true; }
        if (!next[0]) {
            if (!c->error[0]) snprintf(c->error, sizeof c->error, "Failed");
            return false;
        }
        snprintf(at, sizeof at, "%s", next);
        if (code != 307 && code != 308) body = NULL;
    }
    snprintf(c->error, sizeof c->error, "Too many redirects");
    return false;
}

static int lc_read(void *user, uint8_t *buf, int cap)
{
    lc_t *c = user;
    if (!c->open) return CYB_FAIL;
    size_t n = c->len - c->at < (size_t)cap ? c->len - c->at : (size_t)cap;
    if (!n) return CYB_EOF;
    memcpy(buf, c->body + c->at, n);
    c->at += n;
    return (int)n;
}

static const char *lc_final(void *user) { return ((lc_t *)user)->final_url; }
static const char *lc_type(void *user) { return ((lc_t *)user)->content_type; }
static const char *lc_error(void *user) { return ((lc_t *)user)->error; }

void cyb_net_libcurl(cyb_net_t *net, const char *user_agent)
{
    memset(net, 0, sizeof *net);
    the_lc.agent = user_agent ? user_agent : "Mozilla/1.0 (compatible; Cybiko Xtreme; CyOS 1.5) cybiko-browser";
    net->user = &the_lc;
    net->open = lc_open;
    net->read = lc_read;
    net->close = lc_close;
    net->final_url = lc_final;
    net->content_type = lc_type;
    net->error = lc_error;
}
