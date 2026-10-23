/* The Browser: a CyOS application that shows web pages it gets from a
 * Cybiko Internet Gate (the virtual CyWIG on the Dragonfruit board). It asks
 * for a URL the way the WAP browser does (msg 1022 to port 93 of the gate)
 * and gets the page laid out for its screen, a part at a time
 * (gate/cy_page.h). Then it draws it, scrolls, follows links and fills in
 * and sends forms; the gate does the web. */
#include "app.h"

#define MSG_WAP 0x1022
#define GATE_PORT 0x93
#define APP_GET 0x21
#define APP_BIG 0x100
#define APP_PICS 0x200
#define APP_ZIP 0x400                /* parts may come packed */
#define ANS_PART 6
#define ANS_ERROR 5
#define MSG_AUTODELETE 0x80000000UL
#define LOAD_MS 70000L              /* no answer: give up */
#define PICS_MS 120000L             /* with pictures (fetched first) */
#define URL_MAX 200
#define HISTORY 12
#define HOME "http://c/"
#define WIFI "http://c/wifi"

struct DisplayGraphics *gfx;
u8 big;
char status[24];

static u32 proc_mem[CWINAPP_SIZE / 4 + 4];
static void *vtable[20];
static struct Process *me;

/* history: the URLs and where each was scrolled to */
static struct { char url[URL_MAX]; long top; u8 focus; } hist[HISTORY];
static int hist_n, hist_pos = -1;

/* loading */
static u8 loading;
static u8 push_when_done;
static u8 with_pics;                /* this load asks for the pictures */
static u8 pics_next;                /* the next one will */
static char want[URL_MAX];          /* the URL asked for */
static u16 next_no = 1, expect;     /* request numbers */
static u8 *rx;
static u16 rx_total, rx_got;
static long waited;
static struct Buffer send_buf;

/* modes */
enum { M_PAGE, M_EDIT, M_LIST };
static u8 mode = M_PAGE;
static char edit[VALUE_MAX + 1];
static int edit_len;
static u8 edit_target;              /* 0: the address line */
static const char *list_items[48];
static u8 list_n, list_sel, list_kind, list_target;   /* kind 0 menu, 1 select */
static char message[40];            /* shown at the bottom for a while */

/* ---- screen ---- */

static void bar(void)
{
    int sw = 0, n;
    const char *t = mode == M_EDIT ? (edit_target ? "Edit field" : "Go to") : pg.title[0] ? pg.title : "Browser";
    fb_fill(0, 0, SCR_W, BAR_H, BLACK);
    if (status[0]) {
        sw = text_w(1, status, slen(status));
        text(1, SCR_W - 2 - sw, 0, status, slen(status), WHITE);
        sw += 6;
    }
    n = slen(t);
    while (n && text_w(1, t, n) > SCR_W - 4 - sw) n--;
    text(1, 2, 0, t, n, WHITE);
}

static void draw_edit(void)
{
    int y = SCR_H - 15, f = 0, len = edit_len, w;
    const char *p = edit;
    fb_fill(0, y - 1, SCR_W, 16, WHITE);
    fb_fill(0, y - 1, SCR_W, 1, BLACK);
    fb_frame(2, y + 1, SCR_W - 4, 13, BLACK);
    while (len && text_w(f, p, len) > SCR_W - 12) { p++; len--; }
    if (edit_target && pg.t[edit_target - 1].type == FT_PASSWORD) {
        static char stars[VALUE_MAX + 1];
        int i;
        for (i = 0; i < len; i++) stars[i] = '*';
        p = stars;
    }
    text(f, 4, y + 3, p, len, BLACK);
    w = text_w(f, p, len);
    fb_fill(4 + w + 1, y + 3, 1, 9, BLACK);          /* the cursor */
}

static void draw_list(void)
{
    int rows = list_n < 8 ? list_n : 8, h = rows * 10 + 4, y = VIEW_Y + (VIEW_H - h) / 2;
    int first = list_sel >= rows ? list_sel - rows + 1 : 0, i, w = 0;
    for (i = 0; i < list_n; i++) {
        int tw = text_w(0, list_items[i], slen(list_items[i]));
        if (tw > w) w = tw;
    }
    w += 10;
    if (w > SCR_W - 8) w = SCR_W - 8;
    if (w < 60) w = 60;
    fb_fill((SCR_W - w) / 2, y, w, h, WHITE);
    fb_frame((SCR_W - w) / 2, y, w, h, BLACK);
    for (i = 0; i < rows; i++) {
        int k = first + i, x = (SCR_W - w) / 2 + 4;
        text(0, x, y + 2 + i * 10, list_items[k], slen(list_items[k]), BLACK);
        if (k == list_sel) fb_invert((SCR_W - w) / 2 + 2, y + 2 + i * 10, w - 4, 10);
    }
}

static void paint(void)
{
    fb = GFX_BUF(gfx);
    if (pg.data) page_draw();
    else fb_fill(0, VIEW_Y, SCR_W, VIEW_H, WHITE);
    if (message[0]) {
        int w = text_w(0, message, slen(message)) + 6;
        if (w > SCR_W) w = SCR_W;
        fb_fill((SCR_W - w) / 2, SCR_H - 13, w, 13, WHITE);
        fb_frame((SCR_W - w) / 2, SCR_H - 13, w, 13, BLACK);
        text(0, (SCR_W - w) / 2 + 3, SCR_H - 11, message, slen(message), BLACK);
    }
    if (mode == M_EDIT) draw_edit();
    if (mode == M_LIST) draw_list();
    bar();
    cy_DisplayGraphics_show(gfx);
}

static void say(const char *s) { scpy(message, s, sizeof message); }

/* ---- the gate ---- */

static void stop_loading(void)
{
    loading = 0;
    status[0] = 0;
    if (rx) cy_free(rx);
    rx = 0;
}

static void load(const char *url, u8 push)
{
    cyid_t gate = cy_get_gate_cyid();
    struct Message *m;
    u32 len = (u32)slen(url) + 1;
    stop_loading();
    message[0] = 0;
    with_pics = pics_next;
    pics_next = 0;
    if (!gate) { say("No gate nearby"); return; }
    if (url != want) scpy(want, url, sizeof want);
    m = cy_Message_new(sizeof(struct Message));
    if (!m) return;
    m->msgid = MSG_WAP;
    m->param[0] = APP_GET | APP_ZIP | (big ? APP_BIG : 0) | (with_pics ? APP_PICS : 0);
    m->param[1] = next_no;
    cy_Buffer_ctor(&send_buf, len, 16);
    cy_Buffer_store(&send_buf, want, 0, len);
    cy_Message_attach_buffer(m, &send_buf);
    cy_Message_deliver(m, (char *)GATE_PORT, gate, (long)(30000UL | MSG_AUTODELETE));
    expect = next_no;
    loading = 1;
    push_when_done = push;
    waited = 0;
    scpy(status, with_pics ? "Pictures..." : "Asking...", sizeof status);
}

static void remember_place(void)
{
    if (hist_pos >= 0) { hist[hist_pos].top = top; hist[hist_pos].focus = focus; }
}

static void loaded(void)
{
    u8 *data = rx;
    u16 len = rx_got;
    rx = 0;
    loading = 0;
    status[0] = 0;
    if (len >= 4 && data[0] == 'C' && data[1] == 'Z') {
        /* packed: "CZ", u16 the length, raw Deflate */
        u16 raw = (u16)(data[2] << 8 | data[3]);
        u8 *u = cy_malloc(raw ? raw : 1);
        if (!u) { cy_free(data); say("Out of memory"); return; }
        if (!inflate(u, raw, data + 4, len - 4)) { cy_free(u); cy_free(data); say("Bad page from the gate"); return; }
        cy_free(data);
        data = u;
        len = raw;
    }
    if (!page_set(data, len)) { page_free(); say("Bad page from the gate"); return; }
    top = 0;
    focus = 0;
    if (push_when_done == 1) {
        int i;
        if (hist_pos == HISTORY - 1) {
            for (i = 1; i < HISTORY; i++) hist[i - 1] = hist[i];
            hist_pos--;
        }
        hist_pos++;
        hist_n = hist_pos + 1;
        scpy(hist[hist_pos].url, want, URL_MAX);
    } else if (push_when_done == 2 && hist_pos >= 0) {
        /* back, forward or reload: where it was */
        page_scroll_to(hist[hist_pos].top);
        focus = hist[hist_pos].focus;
    }
}

static void answer(struct Message *m)
{
    u32 p0 = (u32)m->param[0], p1 = (u32)m->param[1];
    u16 no = (u16)(p0 >> 16);
    if (!loading || no != expect) return;
    if ((p0 & 0xffff) == ANS_ERROR) {
        static char e[24];
        stop_loading();
        cy_sprintf(e, "Error %ld", (long)p1);
        say(e);
        return;
    }
    if ((p0 & 0xffff) != ANS_PART) return;
    {
        u16 total = (u16)(p1 >> 16), len = (u16)p1;
        struct Buffer *b = cy_Message_has_buffer(m) ? cy_Message_get_buffer(m) : 0;
        if (!rx) {
            rx = cy_malloc(total ? total : 1);
            rx_total = total;
            rx_got = 0;
            if (!rx) { stop_loading(); say("Out of memory"); return; }
        }
        if (b && cy_Buffer_get_size(b) < len) len = (u16)cy_Buffer_get_size(b);
        if (rx_got + len > rx_total) len = rx_total - rx_got;
        if (b && len) cy_Buffer_load(b, rx + rx_got, 0, len);
        rx_got += len;
        expect++;
        next_no = expect + 1;
        waited = 0;
        cy_sprintf(status, "%d%%", (int)((u32)rx_got * 100 / (rx_total ? rx_total : 1)));
        if (rx_got >= rx_total) loaded();
    }
}

/* ---- going places ---- */

static void go(const char *url) { remember_place(); load(url, 1); }

static void history_go(int d)
{
    int p = hist_pos + d;
    if (p < 0 || p >= hist_n) return;
    remember_place();
    hist_pos = p;
    load(hist[p].url, 2);
}

static void put_enc(char *out, int *n, int cap, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s && *n < cap - 4; s++) {
        u8 c = (u8)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            out[(*n)++] = (char)c;
        else if (c == ' ')
            out[(*n)++] = '+';
        else {
            out[(*n)++] = '%';
            out[(*n)++] = hex[c >> 4];
            out[(*n)++] = hex[c & 15];
        }
    }
    out[*n] = 0;
}

static const char *option_value(const struct target *t, char *out)
{
    const u8 *p = t->options;
    u8 k;
    out[0] = 0;
    for (k = 0; k < t->n_options; k++) {
        if (k == t->chosen) {
            int i;
            for (i = 0; i < *p; i++) out[i] = (char)p[1 + i];
            out[*p] = 0;
            break;
        }
        p += 1 + *p;
        p += 1 + *p;
    }
    return out;
}

/* A form sent with its submit button t: its URL, "?v<var>=value&..." */
static void submit(const struct target *s)
{
    static char url[URL_MAX];
    char val[VALUE_MAX + 1], var[8];
    int n = 0, i;
    u16 k;
    u8 first = 1;
    if (!s->url || !s->url[0]) return;
    for (i = 0; i < s->url[0] && n < URL_MAX - 1; i++) url[n++] = (char)s->url[1 + i];
    url[n] = 0;
    for (k = 0; k < pg.n_targets; k++) {
        const struct target *t = &pg.t[k];
        const char *v;
        if (t->kind != PT_FIELD || t->form != s->form) continue;
        switch (t->type) {
        case FT_SUBMIT: case FT_RESET: case FT_HIDDEN: continue;
        case FT_CHECKBOX: v = t->checked && t->value ? t->value : ""; break;
        case FT_RADIO: if (!t->checked) continue; v = t->value ? t->value : ""; break;
        case FT_SELECT: v = option_value(t, val); break;
        default: v = t->value ? t->value : ""; break;
        }
        if (n > URL_MAX - 12) break;
        url[n++] = first ? '?' : '&';
        first = 0;
        cy_sprintf(var, "v%u=", t->var);
        for (i = 0; var[i]; i++) url[n++] = var[i];
        put_enc(url, &n, URL_MAX, v);
    }
    url[n] = 0;
    go(url);
}

static void edit_start(u8 target, const char *init)
{
    mode = M_EDIT;
    edit_target = target;
    scpy(edit, init, sizeof edit);
    edit_len = slen(edit);
}

static void open_list(u8 kind, u8 target, u8 sel)
{
    mode = M_LIST;
    list_kind = kind;
    list_target = target;
    list_sel = sel < list_n ? sel : 0;
}

static void menu(void);

static void activate(void)
{
    struct target *t;
    if (!focus || focus > pg.n_targets) { menu(); return; }
    t = &pg.t[focus - 1];
    if (t->kind == PT_LINK) {
        static char url[URL_MAX];
        int i;
        if (!t->url || !t->url[0]) return;
        for (i = 0; i < t->url[0] && i < URL_MAX - 1; i++) url[i] = (char)t->url[1 + i];
        url[i] = 0;
        go(url);
        return;
    }
    switch (t->type) {
    case FT_CHECKBOX:
        t->checked = !t->checked;
        break;
    case FT_RADIO: {
        u16 k;
        for (k = 0; k < pg.n_targets; k++)
            if (pg.t[k].kind == PT_FIELD && pg.t[k].type == FT_RADIO && pg.t[k].var == t->var && pg.t[k].form == t->form)
                pg.t[k].checked = 0;
        t->checked = 1;
        break;
    }
    case FT_SELECT: {
        const u8 *p = t->options;
        static char labels[48][24];
        u8 k;
        list_n = t->n_options > 48 ? 48 : t->n_options;
        for (k = 0; k < list_n; k++) {
            const u8 *label = p + 1 + *p;
            int i, l = *label > 23 ? 23 : *label;
            for (i = 0; i < l; i++) labels[k][i] = (char)label[1 + i];
            labels[k][l] = 0;
            list_items[k] = labels[k];
            p = label + 1 + *label;
        }
        if (list_n) open_list(1, focus, t->chosen);
        break;
    }
    case FT_SUBMIT:
        submit(t);
        break;
    case FT_RESET:
        break;
    default:
        edit_start(focus, t->value ? t->value : "");
        break;
    }
}

static void menu(void)
{
    list_n = 0;
    list_items[list_n++] = "Go to address";
    list_items[list_n++] = "Home";
    list_items[list_n++] = "Wi-Fi setup";
    list_items[list_n++] = "Reload";
    list_items[list_n++] = "Load pictures";
    list_items[list_n++] = "Back";
    list_items[list_n++] = big ? "Smaller text" : "Bigger text";
    list_items[list_n++] = "Exit";
    open_list(0, 0, 0);
}

static u8 menu_choose(u8 i)
{
    mode = M_PAGE;
    switch (i) {
    case 0: edit_start(0, hist_pos >= 0 ? hist[hist_pos].url : ""); break;
    case 1: go(HOME); break;
    case 2: go(WIFI); break;
    case 3: if (hist_pos >= 0) { remember_place(); load(hist[hist_pos].url, 2); } break;
    case 4: if (hist_pos >= 0) { remember_place(); pics_next = 1; load(hist[hist_pos].url, 2); } break;
    case 5: history_go(-1); break;
    case 6: big = !big; if (hist_pos >= 0) load(hist[hist_pos].url, 2); break;
    case 7: return 1;
    }
    return 0;
}

/* What the address line typed means: a URL, or words to search for. */
static void go_typed(const char *s)
{
    static char url[URL_MAX];
    int n = 0, i, dot = 0, space = 0;
    for (i = 0; s[i]; i++) { if (s[i] == '.') dot = 1; if (s[i] == ' ') space = 1; }
    if (!s[0]) return;
    if (!dot || space) {
        scpy(url, "http://frogfind.com/?q=", sizeof url);
        n = slen(url);
        put_enc(url, &n, URL_MAX, s);
    } else if (s[0] == 'h' && s[1] == 't' && s[2] == 't' && s[3] == 'p') {
        scpy(url, s, sizeof url);
    } else {
        scpy(url, "http://", sizeof url);
        scpy(url + 7, s, sizeof url - 7);
    }
    go(url);
}

/* ---- keys ---- */

/* Returns 1 to quit. */
static u8 key(int code, char ch)
{
    message[0] = 0;
    if (mode == M_EDIT) {
        switch (code) {
        case KEY_ENTER:
            mode = M_PAGE;
            if (edit_target) {
                struct target *t = &pg.t[edit_target - 1];
                if (t->value) scpy(t->value, edit, VALUE_MAX + 1);
            } else {
                go_typed(edit);
            }
            return 0;
        case KEY_ESC: mode = M_PAGE; return 0;
        case KEY_BACKSPACE: case KEY_DEL: case KEY_LEFT:
            if (edit_len) edit[--edit_len] = 0;
            return 0;
        }
        if ((u8)ch >= 32 && edit_len < VALUE_MAX && (edit_target || edit_len < URL_MAX - 1)) {
            edit[edit_len++] = ch;
            edit[edit_len] = 0;
        }
        return 0;
    }
    if (mode == M_LIST) {
        switch (code) {
        case KEY_UP: list_sel = list_sel ? list_sel - 1 : list_n - 1; break;
        case KEY_DOWN: list_sel = list_sel + 1 < list_n ? list_sel + 1 : 0; break;
        case KEY_ESC: mode = M_PAGE; break;
        case KEY_ENTER: case KEY_SELECT:
            if (list_kind == 0) return menu_choose(list_sel);
            pg.t[list_target - 1].chosen = list_sel;
            mode = M_PAGE;
            break;
        }
        return 0;
    }
    switch (code) {
    case KEY_UP: page_move_focus(-1); return 0;
    case KEY_DOWN: case KEY_TAB: page_move_focus(1); return 0;
    case KEY_LEFT: page_scroll(-1); return 0;
    case KEY_RIGHT: page_scroll(1); return 0;
    case KEY_ENTER: case KEY_SELECT: activate(); return 0;
    case KEY_BACKSPACE: case KEY_DEL: history_go(-1); return 0;
    case KEY_ESC:
        if (loading) { stop_loading(); say("Stopped"); return 0; }
        if (hist_pos > 0) { history_go(-1); return 0; }
        return 1;
    case KEY_HELP: menu(); return 0;
    }
    if (ch == ' ') page_scroll(1);
    else if ((u8)ch > 32) {
        char s[2];
        s[0] = ch;
        s[1] = 0;
        edit_start(0, s);
    }
    return 0;
}

/* ---- the process ---- */

static void run(struct Process *p)
{
    gfx = cy_Process_get_display();
    cy_Process_request_focus(p);
    paint();
    go(HOME);
    paint();
    for (;;) {
        struct Message *m = cy_Queue_get_message(PROCESS_QUEUE(p), 500, 0, 0x7fff);
        u8 quit = 0;
        if (!m) {
            if (loading && (waited += 500) > (with_pics && !rx ? PICS_MS : LOAD_MS)) { stop_loading(); say("The gate did not answer"); paint(); }
            else if (loading && !rx && waited == 3000 && !cy_get_gate_cyid()) { stop_loading(); say("No gate nearby"); paint(); }
            continue;
        }
        switch (m->msgid) {
        case MSG_GOTFOCUS: {
            void *w = CWINAPP_WINDOW(p);
            u8 (*proc)(void *, struct Message *) = (*(void ***)((char *)w + 0x10))[6];
            proc(w, m);
            break;
        }
        case MSG_KEYDOWN: {
            struct KeyParam *k = (struct KeyParam *)m->param;
            quit = key(k->scancode, k->ch);
            break;
        }
        case MSG_WAP:
            answer(m);
            break;
        case MSG_QUIT:
        case MSG_SHUTUP:
            quit = 1;
            break;
        }
        cy_Message_delete(m);
        if (quit) break;
        paint();
    }
    stop_loading();
    page_free();
}

long main(int argc, char **argv, char start)
{
    void **kvt;
    int i;
    (void)argc;
    (void)argv;
    if (!start) return 0;
    me = cy_cWinApp_ctor(proc_mem, "Browser", 6000);
    kvt = PROCESS_VTABLE(me);
    for (i = 0; i < 20; i++) vtable[i] = kvt[i];
    vtable[7] = (void *)run;            /* the thread runs slot 7 */
    PROCESS_VTABLE(me) = vtable;
    cy_Thread_start(me, 0);
    return 0;
}
