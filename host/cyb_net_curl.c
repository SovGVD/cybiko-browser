/* The browser's network on a PC, through the curl program. A request runs
 * to its end in open() (so it blocks: fine for tests and the PC emulators),
 * and read() then hands out the saved body. Redirects are followed here,
 * one curl run each, so that each step gets and sets its cookies. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cyb_net_curl.h"

typedef struct {
    const char *agent;
    FILE *body;
    char final_url[1024];
    char content_type[128];
    char error[160];
} curl_t;

static curl_t the_curl;

static void curl_close(void *user)
{
    curl_t *c = user;
    if (c->body) fclose(c->body);
    c->body = NULL;
}

#define REDIRECTS 6

/* The Set-Cookie headers of the last answer in a curl -D dump. */
static void take_cookies(FILE *headers, const char *url, cyb_jar_t *jar)
{
    char line[8192];
    rewind(headers);
    while (fgets(line, sizeof line, headers)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncasecmp(line, "Set-Cookie:", 11)) cyb_jar_set(jar, url, line + 11);
    }
}

/* One request, without following a redirect: true with c->body set, or a
 * redirect's target in next. */
static bool one(curl_t *c, const char *url, const char *body, const char *content_type, cyb_jar_t *jar,
                char *next, size_t next_cap, int *code)
{
    next[0] = 0;
    *code = 0;
    FILE *out = tmpfile(), *info = tmpfile(), *post = NULL, *headers = tmpfile();
    if (!out || !info || !headers) { snprintf(c->error, sizeof c->error, "No temporary files"); goto fail; }
    char cookie[4200] = "", headers_arg[32];
    snprintf(headers_arg, sizeof headers_arg, "/dev/fd/%d", fileno(headers));
    if (jar) {
        memcpy(cookie, "Cookie: ", 8);
        if (!cyb_jar_header(jar, url, cookie + 8, sizeof cookie - 8)) cookie[0] = 0;
    }
    char ctype[160] = "", post_arg[64] = "";
    if (body) {
        post = tmpfile();
        if (!post) goto fail;
        fputs(body, post);
        fflush(post);
        rewind(post);
        snprintf(ctype, sizeof ctype, "Content-Type: %s", content_type ? content_type : "application/x-www-form-urlencoded");
        snprintf(post_arg, sizeof post_arg, "@/dev/fd/%d", fileno(post));
    }
    pid_t pid = fork();
    if (pid < 0) { snprintf(c->error, sizeof c->error, "Can't run curl"); goto fail; }
    if (pid == 0) {
        dup2(fileno(out), 1);
        dup2(fileno(info), 2);
        const char *argv[24];
        int n = 0;
        argv[n++] = "curl";
        argv[n++] = "-sS";
        argv[n++] = "-D";
        argv[n++] = headers_arg;
        if (cookie[0]) {
            argv[n++] = "-H";
            argv[n++] = cookie;
        }
        argv[n++] = "--compressed";
        argv[n++] = "--max-time";
        argv[n++] = "40";
        argv[n++] = "--max-filesize";
        argv[n++] = "4000000";
        argv[n++] = "-A";
        argv[n++] = c->agent;
        argv[n++] = "-w";
        argv[n++] = "\n@@cyb@@%{url_effective}\n%{content_type}\n%{http_code}\n%{redirect_url}\n";
        if (body) {
            argv[n++] = "-H";
            argv[n++] = ctype;
            argv[n++] = "--data-binary";
            argv[n++] = post_arg;
        }
        argv[n++] = "--";
        argv[n++] = url;
        argv[n] = NULL;
        execvp("curl", (char *const *)argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (post) fclose(post);
    fflush(headers);
    if (jar) take_cookies(headers, url, jar);
    fclose(headers);
    headers = NULL;
    /* -w goes to stdout after the body: split it off */
    fflush(out);
    long len = ftell(out);
    char tail[1400] = "";
    long from = len > (long)sizeof tail - 1 ? len - (long)sizeof tail + 1 : 0;
    fseek(out, from, SEEK_SET);
    size_t got = fread(tail, 1, sizeof tail - 1, out);
    tail[got] = 0;
    /* the last marker (the body may hold anything, NULs too) */
    char *mark = NULL;
    for (size_t i = got >= 8 ? got - 8 + 1 : 0; i-- > 0;)
        if (!memcmp(tail + i, "\n@@cyb@@", 8)) { mark = tail + i; break; }
    rewind(info);
    char err[160] = "";
    if (fgets(err, sizeof err, info)) err[strcspn(err, "\r\n")] = 0;
    fclose(info);
    info = NULL;
    if (!mark || !WIFEXITED(status) || WEXITSTATUS(status)) {
        const char *e = err[0] ? err : WIFEXITED(status) && WEXITSTATUS(status) == 127 ? "curl is not installed" : "Failed";
        /* curl: (6) Could not resolve host: x -> Could not resolve host: x */
        if (!strncmp(e, "curl: (", 7) && strchr(e, ')')) e = strchr(e, ')') + 2;
        snprintf(c->error, sizeof c->error, "%s", e);
        goto fail;
    }
    long body_len = from + (long)(mark - tail);
    char *u = mark + 8, *t = strchr(u, '\n');
    if (t) {
        *t++ = 0;
        char *code_s = strchr(t, '\n');
        if (code_s) *code_s++ = 0;
        snprintf(c->content_type, sizeof c->content_type, "%s", t);
        int http = code_s ? atoi(code_s) : 0;
        *code = http;
        char *to = code_s ? strchr(code_s, '\n') : NULL;
        if (to) {
            to++;
            to[strcspn(to, "\n")] = 0;
            if (http >= 300 && http < 400 && *to) {
                snprintf(next, next_cap, "%s", to);
                fclose(out);
                return false;
            }
        }
        if (http >= 400 && http != 404 && body_len == 0) {
            snprintf(c->error, sizeof c->error, "The server said %d", http);
            goto fail;
        }
    }
    snprintf(c->final_url, sizeof c->final_url, "%s", u);
    if (ftruncate(fileno(out), body_len) != 0) goto fail;
    rewind(out);
    c->body = out;
    return true;
fail:
    if (out) fclose(out);
    if (info) fclose(info);
    if (headers) fclose(headers);
    if (!c->error[0]) snprintf(c->error, sizeof c->error, "Failed");
    return false;
}

static bool curl_open(void *user, const char *url, const char *body, const char *content_type, cyb_jar_t *jar)
{
    curl_t *c = user;
    curl_close(c);
    c->final_url[0] = c->content_type[0] = c->error[0] = 0;
    char at[1024], next[1024];
    snprintf(at, sizeof at, "%s", url);
    for (int hop = 0; hop <= REDIRECTS; hop++) {
        int code;
        if (one(c, at, body, content_type, jar, next, sizeof next, &code)) return true;
        if (!next[0]) return false;
        snprintf(at, sizeof at, "%s", next);
        if (code != 307 && code != 308) body = NULL;
    }
    snprintf(c->error, sizeof c->error, "Too many redirects");
    return false;
}

static int curl_read(void *user, uint8_t *buf, int cap)
{
    curl_t *c = user;
    if (!c->body) return CYB_FAIL;
    size_t n = fread(buf, 1, (size_t)cap, c->body);
    return n ? (int)n : CYB_EOF;
}

static const char *curl_final(void *user) { return ((curl_t *)user)->final_url; }
static const char *curl_type(void *user) { return ((curl_t *)user)->content_type; }
static const char *curl_error(void *user) { return ((curl_t *)user)->error; }

void cyb_net_curl(cyb_net_t *net, const char *user_agent)
{
    memset(net, 0, sizeof *net);
    the_curl.agent = user_agent ? user_agent : "Mozilla/1.0 (compatible; Cybiko Xtreme; CyOS 1.5) cybiko-browser";
    net->user = &the_curl;
    net->open = curl_open;
    net->read = curl_read;
    net->close = curl_close;
    net->final_url = curl_final;
    net->content_type = curl_type;
    net->error = curl_error;
}
