#pragma once
/* A small web browser for the emulated Cybiko.
 *
 * The Browser icon on the Cybiko desktop (launcher/browser) asks the
 * emulator to open it (CY_APP_BROWSER). The emulator then stops running
 * CyOS, hands the keys to this browser and shows its 160x100 four-shade
 * picture on the Cybiko screen until the user closes it.
 *
 * It reads plain HTML: text, headings, lists, links, simple tables and
 * forms, and GIF, PNG and JPEG pictures, which it scales to the screen and
 * dithers to the LCD's four shades. Text is drawn with CyOS's own fonts,
 * read from the flash image. No scripts or style sheets.
 *
 * Portable C: the network comes from the host through cyb_net_t (Wi-Fi and
 * HTTPS on the board, curl on a PC), files through stdio. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CYB_W 160
#define CYB_H 100
#define CYB_FB_SIZE (CYB_W * CYB_H / 4)

/* cyb_net_t.read results besides a byte count */
#define CYB_EOF  (-1)
#define CYB_FAIL (-2)

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;          /* no password */
} cyb_ap_t;

/* The host's network. One request at a time. Every function may be called
 * often, so none should block for long. */
typedef struct {
    void *user;
    /* Start a GET, or a POST when body is not NULL. False if it cannot start
     * (error() says why). */
    bool (*open)(void *user, const char *url, const char *body, const char *content_type);
    /* Up to cap bytes of the response body: a count, 0 when nothing has come
     * yet, CYB_EOF at the end or CYB_FAIL. */
    int (*read)(void *user, uint8_t *buf, int cap);
    void (*close)(void *user);
    /* Once read has returned something: the URL after redirects and the
     * Content-Type header (either may be NULL or ""). */
    const char *(*final_url)(void *user);
    const char *(*content_type)(void *user);
    /* Why open failed or read returned CYB_FAIL. */
    const char *(*error)(void *user);

    /* Wi-Fi, all optional (NULL on a PC, which is always online). */
    /* 1 connected, 0 connecting, -1 not connected; text gets a short status
     * line ("Home: 192.168.1.20"). */
    int (*wifi_status)(void *user, char *text, size_t cap);
    /* Start a scan, or return its result: the number of networks put in
     * aps, or -1 while the scan runs. */
    int (*wifi_scan)(void *user, cyb_ap_t *aps, int max);
    /* Remember a network (an empty password for an open one) and join it. */
    bool (*wifi_join)(void *user, const char *ssid, const char *password);
    /* Saved networks, for the Wi-Fi page: their number, names in ssids. */
    int (*wifi_saved)(void *user, char (*ssids)[33], int max);
    bool (*wifi_forget)(void *user, const char *ssid);
} cyb_net_t;

typedef struct cyb cyb_t;

/* flash is the CyOS flash image, for the fonts (NULL: a plain built-in font).
 * data_dir is a folder for bookmarks and settings (NULL: none kept). */
cyb_t *cyb_create(const cyb_net_t *net, const uint8_t *flash, size_t flash_len,
                  const char *data_dir);
void cyb_destroy(cyb_t *b);

/* Show the browser again (after it was closed): redraws everything, keeps
 * the page and history. The first call opens the home page. */
void cyb_show(cyb_t *b);
/* Open a URL ("about:home", "http://...", or what a user would type). */
void cyb_go(cyb_t *b, const char *url);
/* Cybiko keys: the letter, punctuation and space keys are their own
 * lowercase character ('a', '.', ',', ';', '!', '(', ')', ' '), the others
 * these. Shift and Fn are keys like the rest. */
enum {
    CYB_K_UP = 0x100, CYB_K_DOWN, CYB_K_LEFT, CYB_K_RIGHT,
    CYB_K_ENTER, CYB_K_ESC, CYB_K_TAB, CYB_K_DEL, CYB_K_BKSP,
    CYB_K_SELECT, CYB_K_MENU, CYB_K_HELP, CYB_K_SHIFT, CYB_K_FN,
    CYB_K_F1, CYB_K_F2, CYB_K_F3, CYB_K_F4, CYB_K_F5, CYB_K_F6, CYB_K_F7,
};
/* A key went down or up. */
void cyb_key(cyb_t *b, int key, bool down);
/* A character typed on a PC keyboard (Latin-1), for hosts that have one. */
void cyb_char(cyb_t *b, int ch);
/* Do a bit of work (network, pictures). Returns false once the user has
 * closed the browser. now_ms is any millisecond clock. */
bool cyb_step(cyb_t *b, uint32_t now_ms);
/* True while a page or picture is being fetched (the host may poll faster). */
bool cyb_busy(const cyb_t *b);
/* The picture: CYB_H rows of CYB_W / 4 bytes from the top, four pixels a
 * byte with the leftmost in the top bits, 0 white to 3 black. Returns false
 * (and leaves fb alone) when nothing changed since the last call. */
bool cyb_render(cyb_t *b, uint8_t *fb);

/* The CyOS launcher application, to put on the RAM disk as "Browser.app". */
extern const uint8_t *const cyb_launcher_app;
extern const size_t cyb_launcher_app_size;
