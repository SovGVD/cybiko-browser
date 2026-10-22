/* The browser's network on an ESP32 (see cyb_net_esp.h).
 *
 * Requests run one at a time in a worker task, which joins Wi-Fi when it
 * has to, follows redirects and streams the body into a stream buffer that
 * the browser reads without blocking. Each request has its own buffer and
 * state, so one the browser gave up on can finish (or time out) in the
 * worker without getting mixed into the next. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cyb_net_esp.h"
#include "cyb_internal.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cyb_net";

#define STREAM_SIZE (16 * 1024)
#define WORKER_STACK 8192
#define REDIRECTS 6
#define TIMEOUT_MS 15000
#define JOIN_MS 12000
#define SAVED_MAX 5
#define SCAN_MAX 16

/* ---- the saved networks, shared with the Palm OS gateway ---- */

typedef struct { char ssid[33], password[65]; } saved_network_t;
typedef struct { uint32_t version, count; saved_network_t nets[SAVED_MAX]; } saved_store_t;

static saved_store_t saved;
static bool saved_dirty;          /* written to NVS by cyb_net_esp_stop */
static SemaphoreHandle_t lock;

static void load_saved(void)
{
    nvs_handle_t h;
    memset(&saved, 0, sizeof saved);
    if (nvs_open("palm_wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof saved;
        if (nvs_get_blob(h, "networks", &saved, &len) != ESP_OK || len != sizeof saved ||
            saved.version != 1 || saved.count > SAVED_MAX) memset(&saved, 0, sizeof saved);
        nvs_close(h);
    }
    for (unsigned i = 0; i < saved.count; i++) {
        saved.nets[i].ssid[32] = 0;
        saved.nets[i].password[64] = 0;
    }
    saved.version = 1;
}

/* Only from a task with its stack in internal RAM: writing flash turns the
 * cache (and so PSRAM) off. */
static bool store_saved(void)
{
    nvs_handle_t h;
    if (nvs_open("palm_wifi", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "networks", &saved, sizeof saved) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ---- Wi-Fi ---- */

enum { GOT_IP = 1, JOIN_FAILED = 2, SCAN_DONE = 4 };

static EventGroupHandle_t events;
static atomic_int scan_state;          /* 0 none, 1 running, 2 done */
static wifi_ap_record_t *scanned;
static uint16_t scanned_n;
static char joined[33];
static char wifi_error[64];
static atomic_bool want_join;          /* a network was just added: join it now */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(events, GOT_IP);
        xEventGroupSetBits(events, JOIN_FAILED);
        snprintf(wifi_error, sizeof wifi_error, "left the network (reason %d)", d ? d->reason : 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(events, SCAN_DONE);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(events, JOIN_FAILED);
        xEventGroupSetBits(events, GOT_IP);
    }
}

/* Wi-Fi up in station mode with a network interface. ESP-NOW (Cybiko
 * radio) may have started Wi-Fi already, without one. */
static bool wifi_ready(void)
{
    static bool ready;
    if (ready) return true;
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (err != ESP_OK) goto fail;
    wifi_mode_t mode;
    bool started = esp_wifi_get_mode(&mode) == ESP_OK;
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        if (!esp_netif_create_default_wifi_sta()) { err = ESP_ERR_NO_MEM; goto fail; }
        if (started) {
            /* the new interface has to see the station start */
            esp_wifi_stop();
            err = esp_wifi_start();
            if (err != ESP_OK) goto fail;
        }
    }
    if (!started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) err = esp_wifi_start();
        if (err != ESP_OK) goto fail;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ready = true;
    return true;
fail:
    ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    snprintf(wifi_error, sizeof wifi_error, "Wi-Fi did not start (%s)", esp_err_to_name(err));
    return false;
}

static bool join(const char *ssid, const char *password)
{
    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof cfg.sta.ssid, "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof cfg.sta.password, "%s", password);
    cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    esp_wifi_disconnect();
    xEventGroupClearBits(events, GOT_IP | JOIN_FAILED);
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
    EventBits_t bits = xEventGroupWaitBits(events, GOT_IP | JOIN_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(JOIN_MS));
    if (bits & GOT_IP) {
        snprintf(joined, sizeof joined, "%s", ssid);
        ESP_LOGI(TAG, "joined %s", ssid);
        return true;
    }
    esp_wifi_disconnect();
    return false;
}

/* On a network, or false with wifi_error set. Runs on the worker. */
static bool online(void)
{
    if (!wifi_ready()) return false;
    bool fresh = atomic_exchange(&want_join, false);
    if (!fresh && (xEventGroupGetBits(events) & GOT_IP)) return true;
    xSemaphoreTake(lock, portMAX_DELAY);
    saved_store_t list = saved;
    xSemaphoreGive(lock);
    if (!list.count) {
        snprintf(wifi_error, sizeof wifi_error, "No Wi-Fi network set up yet: Menu, Wi-Fi.");
        return false;
    }
    /* the newest first: a network just added is tried at once */
    for (int i = (int)list.count - 1; i >= 0; i--)
        if (join(list.nets[i].ssid, list.nets[i].password)) return true;
    snprintf(wifi_error, sizeof wifi_error, "Could not join %s%s.", list.nets[list.count - 1].ssid,
             list.count > 1 ? " or the other saved networks" : "");
    joined[0] = 0;
    return false;
}

/* ---- requests ---- */

typedef struct req {
    int refs;
    bool cancel, done, failed;
    StreamBufferHandle_t data;
    char url[1024];
    char *body;
    char content_type[96];        /* of the body sent */
    char final_url[1024];
    char type[128];               /* of the answer */
    char location[1024];
    char error[160];
} req_t;

static QueueHandle_t work;
static req_t *current;            /* the browser's request */

static void req_drop(req_t *r)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool last = --r->refs == 0;
    xSemaphoreGive(lock);
    if (!last) return;
    if (r->data) vStreamBufferDeleteWithCaps(r->data);
    free(r->body);
    free(r);
}

static esp_err_t on_http(esp_http_client_event_t *e)
{
    req_t *r = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
        if (!strcasecmp(e->header_key, "Content-Type")) snprintf(r->type, sizeof r->type, "%s", e->header_value);
        else if (!strcasecmp(e->header_key, "Location")) snprintf(r->location, sizeof r->location, "%s", e->header_value);
    }
    return ESP_OK;
}

static void fail(req_t *r, const char *fmt, const char *what)
{
    snprintf(r->error, sizeof r->error, fmt, what);
    r->failed = true;
}

static void fetch(req_t *r)
{
    if (!online()) { fail(r, "%s", wifi_error); return; }
    char url[1024];
    snprintf(url, sizeof url, "%s", r->url);
    bool post = r->body != NULL;
    uint8_t *buf = malloc(2048);
    if (!buf) { fail(r, "%s", "Out of memory"); return; }
    for (int hop = 0; hop <= REDIRECTS && !r->cancel; hop++) {
        r->type[0] = r->location[0] = 0;
        esp_http_client_config_t cfg = {
            .url = url, .timeout_ms = TIMEOUT_MS, .disable_auto_redirect = true,
            .event_handler = on_http, .user_data = r, .buffer_size = 2048, .buffer_size_tx = 2048,
            .user_agent = "Mozilla/1.0 (compatible; Cybiko Xtreme; CyOS 1.5) cybiko-browser",
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) { fail(r, "%s", "Bad address"); break; }
        esp_http_client_set_method(c, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
        esp_http_client_set_header(c, "Accept", "text/html, text/plain, image/gif, image/png, image/jpeg, */*;q=0.5");
        esp_http_client_set_header(c, "Accept-Encoding", "identity");
        if (post) esp_http_client_set_header(c, "Content-Type", r->content_type[0] ? r->content_type : "application/x-www-form-urlencoded");
        int len = post ? (int)strlen(r->body) : 0;
        esp_err_t err = esp_http_client_open(c, len);
        if (err != ESP_OK) {
            fail(r, "Could not connect (%s).", esp_err_to_name(err));
            esp_http_client_cleanup(c);
            break;
        }
        for (int sent = 0; sent < len;) {
            int n = esp_http_client_write(c, r->body + sent, len - sent);
            if (n <= 0) break;
            sent += n;
        }
        if (esp_http_client_fetch_headers(c) < 0) {
            fail(r, "%s", "No answer from the server.");
            esp_http_client_cleanup(c);
            break;
        }
        int status = esp_http_client_get_status_code(c);
        if (status >= 300 && status < 400 && r->location[0]) {
            char next[1024];
            esp_http_client_cleanup(c);
            if (!cyb_url_resolve(url, r->location, next, sizeof next)) { fail(r, "%s", "Bad redirect"); break; }
            snprintf(url, sizeof url, "%s", next);
            if (status != 307 && status != 308) post = false;
            continue;
        }
        snprintf(r->final_url, sizeof r->final_url, "%s", url);
        bool any = false;
        for (;;) {
            int n = esp_http_client_read(c, (char *)buf, 2048);
            if (n < 0) { if (!any) fail(r, "%s", "The connection broke."); break; }
            if (n == 0) {
                if (esp_http_client_is_complete_data_received(c) || !esp_http_client_is_chunked_response(c)) break;
                continue;
            }
            any = true;
            size_t off = 0;
            while (off < (size_t)n && !r->cancel)
                off += xStreamBufferSend(r->data, buf + off, (size_t)n - off, pdMS_TO_TICKS(200));
            if (r->cancel) break;
        }
        if (!any && status >= 400) {
            snprintf(r->error, sizeof r->error, "The server answered %d%s.", status, status == 404 ? " (not found)" : "");
            r->failed = true;
        }
        esp_http_client_cleanup(c);
        free(buf);
        r->done = true;
        return;
    }
    free(buf);
    if (!r->failed) fail(r, "%s", r->cancel ? "Stopped" : "Too many redirects");
    r->done = true;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        req_t *r;
        if (xQueueReceive(work, &r, portMAX_DELAY) != pdTRUE) continue;
        if (!r->cancel) fetch(r);
        r->done = true;
        req_drop(r);
    }
}

static bool start(void)
{
    if (work) return true;
    lock = xSemaphoreCreateMutex();
    events = xEventGroupCreate();
    work = xQueueCreate(4, sizeof(req_t *));
    if (!lock || !events || !work) return false;
    load_saved();
    if (xTaskCreatePinnedToCore(worker, "cyb_net", WORKER_STACK, NULL, 4, NULL, 0) != pdPASS &&
        xTaskCreatePinnedToCoreWithCaps(worker, "cyb_net", WORKER_STACK, NULL, 4, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the network task");
        return false;
    }
    return true;
}

static void net_close(void *user)
{
    (void)user;
    if (!current) return;
    current->cancel = true;
    req_drop(current);
    current = NULL;
}

static bool net_open(void *user, const char *url, const char *body, const char *content_type)
{
    net_close(user);
    if (!start()) return false;
    req_t *r = calloc(1, sizeof *r);
    if (!r) return false;
    r->data = xStreamBufferCreateWithCaps(STREAM_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->data || (body && !(r->body = strdup(body)))) {
        if (r->data) vStreamBufferDeleteWithCaps(r->data);
        free(r);
        return false;
    }
    snprintf(r->url, sizeof r->url, "%s", url);
    if (content_type) snprintf(r->content_type, sizeof r->content_type, "%s", content_type);
    r->refs = 2;               /* the browser's and the worker's */
    if (xQueueSend(work, &r, 0) != pdTRUE) {
        r->refs = 1;
        req_drop(r);
        return false;
    }
    current = r;
    return true;
}

static int net_read(void *user, uint8_t *buf, int cap)
{
    (void)user;
    req_t *r = current;
    if (!r) return CYB_FAIL;
    bool done = r->done;          /* read before draining: the last bytes are in by then */
    size_t n = xStreamBufferReceive(r->data, buf, (size_t)cap, 0);
    if (n) return (int)n;
    if (!done) return 0;
    if (xStreamBufferBytesAvailable(r->data)) return 0;
    return r->failed ? CYB_FAIL : CYB_EOF;
}

static const char *net_final(void *user) { (void)user; return current ? current->final_url : ""; }
static const char *net_type(void *user) { (void)user; return current ? current->type : ""; }
static const char *net_error(void *user)
{
    (void)user;
    return current ? current->error : "The network did not start.";
}

/* ---- Wi-Fi for the browser's Wi-Fi page ---- */

static int wifi_status(void *user, char *text, size_t cap)
{
    (void)user;
    if (!start() || !events) { snprintf(text, cap, "Wi-Fi is off."); return -1; }
    if (xEventGroupGetBits(events) & GOT_IP) {
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) esp_netif_get_ip_info(sta, &ip);
        snprintf(text, cap, "%s, " IPSTR, joined, IP2STR(&ip.ip));
        return 1;
    }
    snprintf(text, cap, "%s", wifi_error[0] ? wifi_error : "It joins a saved network when a page is opened.");
    return -1;
}

static int wifi_scan(void *user, cyb_ap_t *aps, int max)
{
    (void)user;
    if (!start() || !wifi_ready()) return 0;
    if (!aps || atomic_load(&scan_state) == 0) {
        /* start one */
        if (atomic_load(&scan_state) == 1) return -1;
        xEventGroupClearBits(events, SCAN_DONE);
        wifi_scan_config_t sc = { .show_hidden = false };
        if (esp_wifi_scan_start(&sc, false) != ESP_OK) { atomic_store(&scan_state, 2); scanned_n = 0; return 0; }
        atomic_store(&scan_state, 1);
        return -1;
    }
    if (atomic_load(&scan_state) == 1) {
        if (!(xEventGroupGetBits(events) & SCAN_DONE)) return -1;
        if (!scanned) scanned = calloc(SCAN_MAX, sizeof *scanned);
        scanned_n = scanned ? SCAN_MAX : 0;
        if (!scanned || esp_wifi_scan_get_ap_records(&scanned_n, scanned) != ESP_OK) scanned_n = 0;
        atomic_store(&scan_state, 2);
    }
    int n = 0;
    for (int i = 0; i < scanned_n && n < max; i++) {
        if (!scanned[i].ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (!strcmp(aps[k].ssid, (const char *)scanned[i].ssid)) dup = true;
        if (dup) continue;
        snprintf(aps[n].ssid, sizeof aps[n].ssid, "%s", (const char *)scanned[i].ssid);
        aps[n].rssi = scanned[i].rssi;
        aps[n].open = scanned[i].authmode == WIFI_AUTH_OPEN;
        n++;
    }
    return n;
}

static bool wifi_join(void *user, const char *ssid, const char *password)
{
    (void)user;
    if (!start()) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    int at = -1;
    for (unsigned i = 0; i < saved.count; i++) if (!strcmp(saved.nets[i].ssid, ssid)) at = (int)i;
    if (at >= 0) {                     /* moved to the end: the newest */
        memmove(&saved.nets[at], &saved.nets[at + 1], (saved.count - (unsigned)at - 1) * sizeof saved.nets[0]);
        saved.count--;
    }
    if (saved.count == SAVED_MAX) {    /* the oldest goes */
        memmove(&saved.nets[0], &saved.nets[1], (SAVED_MAX - 1) * sizeof saved.nets[0]);
        saved.count--;
    }
    saved_network_t *n = &saved.nets[saved.count++];
    memset(n, 0, sizeof *n);
    snprintf(n->ssid, sizeof n->ssid, "%s", ssid);
    snprintf(n->password, sizeof n->password, "%s", password);
    saved_dirty = true;
    xSemaphoreGive(lock);
    atomic_store(&want_join, true);
    snprintf(wifi_error, sizeof wifi_error, "Joins %s when a page is opened.", ssid);
    return true;
}

static int wifi_saved(void *user, char (*ssids)[33], int max)
{
    (void)user;
    if (!start()) return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = 0;
    for (int i = (int)saved.count - 1; i >= 0 && n < max; i--) snprintf(ssids[n++], 33, "%s", saved.nets[i].ssid);
    xSemaphoreGive(lock);
    return n;
}

static bool wifi_forget(void *user, const char *ssid)
{
    (void)user;
    if (!start()) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (unsigned i = 0; i < saved.count; i++)
        if (!strcmp(saved.nets[i].ssid, ssid)) {
            memmove(&saved.nets[i], &saved.nets[i + 1], (saved.count - i - 1) * sizeof saved.nets[0]);
            saved.count--;
            break;
        }
    saved_dirty = true;
    xSemaphoreGive(lock);
    if (!strcmp(joined, ssid)) { esp_wifi_disconnect(); joined[0] = 0; }
    return true;
}

void cyb_net_esp(cyb_net_t *net)
{
    memset(net, 0, sizeof *net);
    net->open = net_open;
    net->read = net_read;
    net->close = net_close;
    net->final_url = net_final;
    net->content_type = net_type;
    net->error = net_error;
    net->wifi_status = wifi_status;
    net->wifi_scan = wifi_scan;
    net->wifi_join = wifi_join;
    net->wifi_saved = wifi_saved;
    net->wifi_forget = wifi_forget;
}

void cyb_net_esp_stop(void)
{
    net_close(NULL);
    if (!events) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (saved_dirty && store_saved()) saved_dirty = false;
    xSemaphoreGive(lock);
    if (xEventGroupGetBits(events) & GOT_IP) esp_wifi_disconnect();
    xEventGroupClearBits(events, GOT_IP);
    joined[0] = 0;
    atomic_store(&scan_state, 0);
}
