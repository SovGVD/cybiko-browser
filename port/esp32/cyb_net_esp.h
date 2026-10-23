#pragma once
/* The browser's network on an ESP32: the board's Wi-Fi (the networks the
 * Palm OS Wi-Fi gateway saved, in NVS "palm_wifi"/"networks", so both modes
 * share them) and HTTP/HTTPS through esp_http_client. Wi-Fi joins on the
 * first request. */
#include "cy_browser.h"

void cyb_net_esp(cyb_net_t *net);
/* Leaves the Wi-Fi network (when the browser closes), so the radio is free
 * for whatever had it before (ESP-NOW on its own channel), and saves the
 * networks added or forgotten meanwhile. Call it from a task whose stack is
 * in internal RAM (it writes NVS); the browser itself may run on a PSRAM
 * stack. */
void cyb_net_esp_stop(void);
/* Saves the networks added or forgotten (on the Wi-Fi page) if there are
 * any; cheap when there are none. The same rule: a task with its stack in
 * internal RAM. */
void cyb_net_esp_save(void);
