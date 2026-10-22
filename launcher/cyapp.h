/* Calls from a Cybiko program into this emulator (see CY_APP_OP in
 * src/components/cybiko/include/cybiko.h). Only the emulator answers them. */
#pragma once

#define CY_APP_VERSION 0        /* returns the API version, 0 if none */
#define CY_APP_BROWSER 1        /* opens the web browser, returns when it closes */

long cy_app_call(long fn, long arg);
