/* The CyOS kernel functions the browser calls (cyos.ld gives their export
 * numbers; the names and layouts are the Cybiko SDK's). gcc -ms passes the
 * first three arguments in er0-er2 and the rest on the stack, 4 bytes
 * each, as CyOS does; int is 16 bits on both. */
#pragma once

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef long cyid_t;

#define CLR_WHITE 0
#define CLR_LTGRAY 85
#define CLR_DKGRAY 171
#define CLR_BLACK 255

#define MSG_KEYDOWN 1
#define MSG_QUIT 0x31
#define MSG_GOTFOCUS 0x41
#define MSG_LOSTFOCUS 0x42
#define MSG_SHUTUP 0x101a
#define MSG_USER 0x2000

#define KEY_ESC 0x107
#define KEY_LEFT 0x108
#define KEY_UP 0x109
#define KEY_RIGHT 0x10a
#define KEY_DOWN 0x10b
#define KEY_DEL 0x10d
#define KEY_TAB 0x10e
#define KEY_SELECT 0x10f
#define KEY_ENTER 0x110
#define KEY_BACKSPACE 0x111
#define KEY_HELP 0x112

struct Buffer { char members[40]; };       /* the SDK's size */
struct Font;
struct Process;
struct Queue;
struct DisplayGraphics;

struct Message {
    struct Buffer *buffer;
    char procname[12];
    struct Message *next;
    char *dst_name;             /* a process name, or a port number < 256 */
    cyid_t cyid_from;
    cyid_t cyid_to;
    u8 deleted;
    u8 pad;
    short msgid;
    long param[2];
    void *session;
};

struct KeyParam {               /* a MSG_KEYDOWN's param */
    int scancode;
    int mask;
    char ch;
};

/* The process's message queue, inside its Process object */
#define PROCESS_QUEUE(p) ((struct Queue *)((char *)(p) + 108))
/* The screen buffer of a graphics context: 160 x 100, 2 bits a pixel */
#define GFX_BUF(g) (*(u8 **)((char *)(g) + 12))

extern struct Process *cy_current_thread;
extern struct Font *cy_cool_bold_font, *cy_cool_normal_font;
extern struct Font *cy_mini_bold_font, *cy_mini_normal_font;

void *cy_malloc(u32 n);
void cy_free(void *p);
struct Message *cy_Message_new(u32 size);
void cy_Message_delete(struct Message *m);
u8 cy_Message_post(struct Message *m, const char *to, cyid_t cyid);
void *cy_Message_deliver(struct Message *m, const char *to, cyid_t cyid, long timeout);
int cy_Message_check_delivery(struct Message *m);
struct Buffer *cy_Message_get_buffer(struct Message *m);
u8 cy_Message_has_buffer(struct Message *m);
void cy_Message_attach_buffer(struct Message *m, struct Buffer *b);
struct Message *cy_Queue_get_message(struct Queue *q, long timeout, int min, int max);
struct Buffer *cy_Buffer_ctor(struct Buffer *b, u32 size, u32 increment);
void cy_Buffer_store(struct Buffer *b, const void *data, u32 offset, u32 len);
void *cy_Buffer_load(struct Buffer *b, void *data, u32 offset, u32 len);
u32 cy_Buffer_get_size(struct Buffer *b);
void cy_Buffer_dtor(struct Buffer *b, int memory_flag);
struct DisplayGraphics *cy_Process_get_display(void);
struct Process *cy_Process_request_focus(struct Process *p);
u8 cy_Process_has_focus(struct Process *p);
void cy_TGraph_set_color(void *g, int color);
void cy_TGraph_fill_screen(void *g, int color);
void cy_Graphics_set_font(void *g, struct Font *f);
void cy_Graphics_draw_text(void *g, const char *s, int x, int y);
void cy_Graphics_draw_text_Ex(void *g, const char *s, int x, int y, int len);
int cy_Graphics_string_width_Ex(void *g, const char *s, int len);
int cy_Font_get_char_width(struct Font *f, int c);
int cy_Font_get_char_height(struct Font *f);
void cy_DisplayGraphics_show(struct DisplayGraphics *g);
cyid_t cy_get_gate_cyid(void);
cyid_t cy_get_own_id(void);
int cy_sprintf(char *out, const char *fmt, ...);
u32 cy_strlen(const char *s);

/* A process of our own (CyOS starts applications this way: main() makes
 * the process and returns). The kernel's Process is 0xac bytes; its
 * thread runs the function in slot 4 of the table at +0x10. */
#define PROCESS_SIZE 0x100
#define PROCESS_VTABLE(p) (*(void ***)((char *)(p) + 0x10))
struct Process *cy_Process_ctor(void *p, const char *name, long stack);
void cy_Process_dtor(struct Process *p, int memory_flag);
void cy_Thread_start(struct Process *p, int flag);

/* CyWin's application object (a Process with the main window at +0xac),
 * as the SDK's runtime makes it: then CyWin's window manager knows the
 * application is on screen. The thread runs slot 7 of the table at +0x10. */
#define CWINAPP_SIZE 0xf4
#define CWINAPP_WINDOW(a) ((void *)((char *)(a) + 0xac))
void *cy_cWinApp_ctor(void *app, const char *name, int stack);

#define FREE_MEMORY 1
#define LEAVE_MEMORY 2
