#ifndef PP_BOARD_H
#define PP_BOARD_H
#include <stdint.h>
#include <stddef.h>

/* Models of the 85x/95x family. */
enum pp_model { PP_MODEL_950 = 0, PP_MODEL_850, PP_MODEL_957, PP_MODEL_857 };

typedef struct PPBoard PPBoard;

/* Log sink (defaults to stderr). */
typedef void (*pp_log_fn)(const char *line);
void pp_set_log(pp_log_fn fn);
void pp_log(const char *fmt, ...);

/* Create a board from an OS image (scrambled PagerXXX.bin or already plain). */
PPBoard *pp_board_new(const uint8_t *os_image, size_t len, char *err, size_t errlen);
void pp_board_free(PPBoard *b);
void pp_board_reset(PPBoard *b);

/* Run roughly `us` microseconds of emulated time. */
void pp_board_run(PPBoard *b, uint32_t us);

int pp_board_model(PPBoard *b);
int pp_lcd_width(PPBoard *b);
int pp_lcd_height(PPBoard *b);
/* Render the LCD as 32-bit ARGB pixels (w*h). Returns 1 if changed. */
int pp_lcd_render(PPBoard *b, uint32_t *argb);

/* Input. key codes: ASCII for letters/digits/space/enter(13)/backspace(8),
 * plus PP_KEY_* below. */
enum {
	PP_KEY_ALT = 0x100, PP_KEY_SHIFT, PP_KEY_CAP, PP_KEY_ESC,
	PP_KEY_WHEEL_CLICK, PP_KEY_WHEEL_UP, PP_KEY_WHEEL_DOWN, PP_KEY_POWER,
};
void pp_key(PPBoard *b, int key, int down);

/* Debug */
void pp_board_status(PPBoard *b, char *buf, size_t len);
uint64_t pp_board_instructions(PPBoard *b);
uint8_t *pp_board_mem(PPBoard *b);
void pp_board_set_trace(PPBoard *b, int on);
int pp_board_stopped(PPBoard *b);

#endif
