/* PocketPager - BlackBerry OS family: screen access for the host. */
#ifndef BBOS_BBGFX_H
#define BBOS_BBGFX_H
#include <stdint.h>

#define BB_SCREEN_W 480
#define BB_SCREEN_H 320

/* RGB565 frame buffer of the LCD; frame counts updateDisplay calls. */
const uint16_t *bbgfx_framebuffer(int *w, int *h, int *frame);

#endif
