#pragma once
/* A real Cybiko Xtreme on the USB cable, through libusb 1.0: the first
 * device with VID 0B66 and PID 0041 (CyOS running). On Windows the Xtreme
 * needs the WinUSB driver first (Zadig does it); on Linux, permission to
 * open it (a udev rule). */
#include "cyusb.h"

/* NULL on failure, with the reason in err. */
cyusb_dev_t *cyusb_libusb_open(char *err, int err_len);
void cyusb_libusb_close(cyusb_dev_t *d);
