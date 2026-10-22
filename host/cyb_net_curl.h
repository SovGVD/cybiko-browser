#pragma once
/* The browser's network on a PC: each request runs the curl program (which
 * also opens file:// addresses). For tests and the PC emulators. */
#include "cy_browser.h"

/* Fills net; user_agent may be NULL. */
void cyb_net_curl(cyb_net_t *net, const char *user_agent);
