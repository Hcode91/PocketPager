#ifndef BBHOST_H
#define BBHOST_H
#include <stddef.h>
#include <stdint.h>

/* Boot the OS from a folder of .cod modules. */
int bbh_start(const char *dir, char *err, size_t errn);
/* Run for up to maxMs of real time: 1 when everything is waiting (the
 * caller may sleep a little), 0 when busy, -1 when the VM stopped. */
int bbh_run(int maxMs);
int bbh_booted(void);
long long bbh_instructions(void);
const char *bbh_status(void);
int bbh_uptime(void);
const uint16_t *bbh_screen(int *w, int *h, int *frame);

int bbh_launch(const char *module);
int bbh_pid_of(const char *module);
int bbh_foreground(const char *module);

enum { BBH_PRESS = 0, BBH_DOWN = 1, BBH_UP = 2, BBH_REPEAT = 3 };
/* keypad: ch is the character or a key code such as BBH_KEY_MENU */
enum { BBH_KEY_ESCAPE = 27, BBH_KEY_ENTER = 10, BBH_KEY_BACKSPACE = 8, BBH_KEY_DELETE = 127,
       BBH_KEY_SEND = 0x1011, BBH_KEY_END = 0x1012, BBH_KEY_MENU = 0x1002,
       BBH_STATUS_SHIFT = 1, BBH_STATUS_ALT = 2 };
void bbh_key(int ch, int action, int status);
void bbh_trackball(int dx, int dy);
void bbh_click(int down);

/* Save states: the whole OS (memory, threads, screen). Loading works in a
 * fresh process or a running one; the file must come from the same build. */
int bbh_save(const char *path, char *err, size_t errn);
int bbh_load(const char *path, char *err, size_t errn);
#endif
