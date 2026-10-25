#pragma once
/* The browser's network on a PC through libcurl, for systems where
 * cyb_net_curl.c (which runs the curl program) can't work, such as Windows.
 * Same behaviour: redirects followed one by one with the cookie jar. */
#include "cy_browser.h"

/* Fills net; user_agent may be NULL. */
void cyb_net_libcurl(cyb_net_t *net, const char *user_agent);
