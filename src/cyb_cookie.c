/* Cookies (RFC 6265, the parts small sites need): a jar per Cybiko, kept
 * in memory. There is no clock to trust on the board, so a cookie is kept
 * until it is replaced, deleted (Max-Age <= 0, or Expires before 2000,
 * which is how sites delete them) or pushed out by newer ones. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cyb_internal.h"

#define JAR_MAX 64                /* cookies in a jar */
#define JAR_BYTES 16384           /* names and values in a jar */
#define COOKIE_MAX 4096           /* one cookie's name and value */

typedef struct {
    char *name, *value;
    char domain[128], path[128];
    bool host_only, secure;
    uint32_t used;                /* when it was last set, for pushing out */
} cookie_t;

struct cyb_jar {
    cookie_t c[JAR_MAX];
    unsigned n;
    size_t bytes;
    uint32_t clock;
};

cyb_jar_t *cyb_jar_create(void) { return calloc(1, sizeof(cyb_jar_t)); }

static void drop(cyb_jar_t *j, unsigned i)
{
    j->bytes -= strlen(j->c[i].name) + strlen(j->c[i].value);
    free(j->c[i].name);
    free(j->c[i].value);
    j->c[i] = j->c[--j->n];
}

void cyb_jar_clear(cyb_jar_t *j)
{
    if (!j) return;
    while (j->n) drop(j, 0);
}

void cyb_jar_free(cyb_jar_t *j)
{
    cyb_jar_clear(j);
    free(j);
}

unsigned cyb_jar_count(const cyb_jar_t *j) { return j ? j->n : 0; }

/* The URL's scheme, host (lower case) and path. */
static bool parts(const char *url, bool *https, char *host, size_t hcap, char *path, size_t pcap)
{
    const char *a = strstr(url, "://");
    if (!a) return false;
    *https = a - url == 5 && !strncasecmp(url, "https", 5);
    a += 3;
    const char *at = a + strcspn(a, "/?#");
    const char *user = memchr(a, '@', (size_t)(at - a));
    if (user) a = user + 1;
    size_t n = strcspn(a, ":/?#");
    if (!n || n >= hcap) return false;
    for (size_t i = 0; i < n; i++) host[i] = (char)tolower((unsigned char)a[i]);
    host[n] = 0;
    a += n;
    a += strcspn(a, "/?#");
    n = *a == '/' ? strcspn(a, "?#") : 0;
    if (!n) snprintf(path, pcap, "/");
    else snprintf(path, pcap, "%.*s", (int)n, a);
    return true;
}

static bool domain_match(const char *host, const char *domain)
{
    size_t h = strlen(host), d = strlen(domain);
    if (h == d) return !strcmp(host, domain);
    return h > d && host[h - d - 1] == '.' && !strcmp(host + h - d, domain);
}

static bool path_match(const char *path, const char *cpath)
{
    size_t n = strlen(cpath);
    if (strncmp(path, cpath, n)) return false;
    return cpath[n - 1] == '/' || path[n] == 0 || path[n] == '/';
}

/* A trimmed copy of s[0..n). */
static void trim(char *out, size_t cap, const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

/* The year in an Expires date ("Wed, 21 Oct 2015 07:28:00 GMT", or with
 * dashes and a two-digit year), 0 if none is found. */
static int expires_year(const char *v)
{
    for (const char *p = v; *p; p++) {
        if (!isdigit((unsigned char)*p) || (p > v && isdigit((unsigned char)p[-1]))) continue;
        int n = 0, k = 0;
        while (isdigit((unsigned char)p[k])) n = n * 10 + p[k++] - '0';
        if (k == 4) return n;
        if (k == 2 && p > v && p[-1] == '-' && p[2] != ':') return n < 70 ? 2000 + n : 1900 + n;
    }
    return 0;
}

void cyb_jar_set(cyb_jar_t *j, const char *url, const char *header)
{
    bool https;
    char host[128], path[256];
    if (!j || !header || !parts(url, &https, host, sizeof host, path, sizeof path)) return;
    size_t first = strcspn(header, ";");
    const char *eq = memchr(header, '=', first);
    if (!eq) return;
    char *name = malloc(COOKIE_MAX), *value = malloc(COOKIE_MAX);
    if (!name || !value) { free(name); free(value); return; }
    trim(name, COOKIE_MAX, header, (size_t)(eq - header));
    trim(value, COOKIE_MAX, eq + 1, first - (size_t)(eq - header) - 1);
    cookie_t c = { 0 };
    bool remove = false;
    c.host_only = true;
    snprintf(c.domain, sizeof c.domain, "%s", host);
    /* the default path: the request's up to its last slash */
    char *slash = strrchr(path, '/');
    if (slash && slash != path) snprintf(c.path, sizeof c.path, "%.*s", (int)(slash - path), path);
    else snprintf(c.path, sizeof c.path, "/");
    for (const char *a = header + first; *a == ';';) {
        a++;
        size_t n = strcspn(a, ";");
        char attr[256], key[32], val[224] = "";
        trim(attr, sizeof attr, a, n);
        a += n;
        char *e = strchr(attr, '=');
        if (e) {
            *e = 0;
            trim(val, sizeof val, e + 1, strlen(e + 1));
        }
        trim(key, sizeof key, attr, strlen(attr));
        if (!strcasecmp(key, "Domain") && val[0]) {
            const char *d = val[0] == '.' ? val + 1 : val;
            char lower[128];
            size_t i = 0;
            for (; d[i] && i < sizeof lower - 1; i++) lower[i] = (char)tolower((unsigned char)d[i]);
            lower[i] = 0;
            /* only the host or a domain above it, and not a bare top level */
            if (!domain_match(host, lower) || !strchr(lower, '.')) goto out;
            snprintf(c.domain, sizeof c.domain, "%s", lower);
            c.host_only = false;
        } else if (!strcasecmp(key, "Path") && val[0] == '/') {
            snprintf(c.path, sizeof c.path, "%s", val);
        } else if (!strcasecmp(key, "Secure")) {
            c.secure = true;
        } else if (!strcasecmp(key, "Max-Age") && val[0]) {
            remove |= atol(val) <= 0;
        } else if (!strcasecmp(key, "Expires")) {
            int y = expires_year(val);
            remove |= y && y < 2000;
        }
    }
    if (c.secure && !https) goto out;
    /* the same cookie set again replaces the old one */
    for (unsigned i = 0; i < j->n; i++)
        if (!strcmp(j->c[i].name, name) && !strcmp(j->c[i].domain, c.domain) && !strcmp(j->c[i].path, c.path)) {
            drop(j, i);
            break;
        }
    if (remove) goto out;
    const size_t size = strlen(name) + strlen(value);
    if (size > JAR_BYTES / 2) goto out;
    while (j->n && (j->n == JAR_MAX || j->bytes + size > JAR_BYTES)) {
        unsigned old = 0;
        for (unsigned i = 1; i < j->n; i++)
            if (j->c[i].used < j->c[old].used) old = i;
        drop(j, old);
    }
    c.name = strdup(name);
    c.value = strdup(value);
    if (!c.name || !c.value) { free(c.name); free(c.value); goto out; }
    c.used = ++j->clock;
    j->c[j->n++] = c;
    j->bytes += size;
out:
    free(name);
    free(value);
}

size_t cyb_jar_header(const cyb_jar_t *j, const char *url, char *out, size_t cap)
{
    bool https;
    char host[128], path[256];
    size_t len = 0;
    if (cap) out[0] = 0;
    if (!j || !cap || !parts(url, &https, host, sizeof host, path, sizeof path)) return 0;
    /* longer paths first, as RFC 6265 asks */
    bool sent[JAR_MAX] = { false };
    for (;;) {
        int best = -1;
        for (unsigned i = 0; i < j->n; i++) {
            const cookie_t *c = &j->c[i];
            if (sent[i] || (c->secure && !https) || !path_match(path, c->path)) continue;
            if (c->host_only ? strcmp(host, c->domain) : !domain_match(host, c->domain)) continue;
            if (best < 0 || strlen(c->path) > strlen(j->c[best].path)) best = (int)i;
        }
        if (best < 0) break;
        sent[best] = true;
        const cookie_t *c = &j->c[best];
        size_t need = (len ? 2 : 0) + strlen(c->name) + 1 + strlen(c->value);
        if (len + need >= cap) continue;
        len += (size_t)snprintf(out + len, cap - len, "%s%s=%s", len ? "; " : "", c->name, c->value);
    }
    return len;
}
