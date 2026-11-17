/*
 * PocketPager headless runner, used for reverse engineering and CI.
 *   ppcli Pager957.bin [milliseconds] [screen.pgm]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <errno.h>
#include "../core/rim/board.h"

#include <signal.h>
static volatile sig_atomic_t g_quit;
static void on_quit(int sig) { (void)sig; g_quit = 1; }
static int g_pty = -1;
static int g_serlog;
static void pty_tx(void *ctx, unsigned char c)
{
	(void)ctx;
	if (g_serlog) fprintf(stderr, "dev> %02x\n", c);
	if (write(g_pty, &c, 1) < 0) { /* host not connected yet */ }
}

static long long now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* Real-time loop with COM2 bridged to a pseudo-terminal. */
static void run_serial(PPBoard *b, int ms)
{
	g_pty = posix_openpt(O_RDWR | O_NOCTTY);
	grantpt(g_pty);
	unlockpt(g_pty);
	struct termios t;
	int slave = open(ptsname(g_pty), O_RDWR | O_NOCTTY);
	tcgetattr(slave, &t);
	cfmakeraw(&t);
	tcsetattr(slave, TCSANOW, &t);
	fcntl(g_pty, F_SETFL, O_NONBLOCK);
	printf("COM2 bridged to %s\n", ptsname(g_pty));
	fflush(stdout);
	if (getenv("PP_PTYLINK")) {
		unlink(getenv("PP_PTYLINK"));
		if (symlink(ptsname(g_pty), getenv("PP_PTYLINK")) != 0) perror("symlink");
	}
	g_serlog = getenv("PP_SERLOG") != NULL;
	pp_serial_set_host(b, 1, (void (*)(void *, unsigned char))pty_tx, NULL);
	long long start = now_us();
	long long emu = 0;
	signal(SIGTERM, on_quit);
	signal(SIGINT, on_quit);
	while ((ms <= 0 || emu < ms * 1000LL) && !g_quit) {
		unsigned char buf[512];
		int space = pp_serial_rx_space(b, 1);
		if (space > (int)sizeof buf) space = sizeof buf;
		if (space > 0) {
			int n = read(g_pty, buf, space);
			if (n > 0) {
				if (g_serlog) for (int i = 0; i < n; i++) fprintf(stderr, "host> %02x\n", buf[i]);
				pp_serial_rx(b, 1, buf, n);
			}
		}
		pp_board_run(b, 2000);
		emu += 2000;
		if (pp_board_stopped(b)) break;
		long long ahead = emu - (now_us() - start);
		if (ahead > 0) usleep(ahead);
	}
	close(slave);
}

/* PP_INPUT="ms:key[:up],..." keys: a letter, WU, WD, CL (click), ESC, ENT, SP, BS, ALT, SH
 * PP_SHOT="ms:file.pgm,..." writes the LCD at those times. */
static int parse_key(const char *k)
{
	if (!strcmp(k, "WU")) return PP_KEY_WHEEL_UP;
	if (!strcmp(k, "WD")) return PP_KEY_WHEEL_DOWN;
	if (!strcmp(k, "CL")) return PP_KEY_WHEEL_CLICK;
	if (!strcmp(k, "ESC")) return PP_KEY_ESC;
	if (!strcmp(k, "ENT")) return 13;
	if (!strcmp(k, "SP")) return ' ';
	if (!strcmp(k, "BS")) return 8;
	if (!strcmp(k, "ALT")) return PP_KEY_ALT;
	if (!strcmp(k, "SH")) return PP_KEY_SHIFT;
	return (unsigned char)k[0];
}

static void write_pgm(PPBoard *b, const char *path)
{
	int w = pp_lcd_width(b), h = pp_lcd_height(b);
	unsigned int *px = malloc(w * h * 4);
	pp_lcd_render(b, px);
	FILE *o = fopen(path, "wb");
	fprintf(o, "P5 %d %d 255\n", w, h);
	for (int i = 0; i < w * h; i++) fputc(px[i] & 0xFF, o);
	fclose(o);
	free(px);
}

static void scripted_events(PPBoard *b, int t)
{
	char buf[4096], *save, *tok;
	if (getenv("PP_INPUT")) {
		snprintf(buf, sizeof buf, "%s", getenv("PP_INPUT"));
		for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
			int ms; char key[16] = "", ud[8] = "";
			if (sscanf(tok, "%d:%15[^:]:%7s", &ms, key, ud) >= 2 && ms == t) {
				int k = parse_key(key);
				if (k == PP_KEY_WHEEL_UP || k == PP_KEY_WHEEL_DOWN) pp_key(b, k, 1);
				else pp_key(b, k, strcmp(ud, "up") != 0);
			}
		}
	}
	if (getenv("PP_SHOT")) {
		snprintf(buf, sizeof buf, "%s", getenv("PP_SHOT"));
		for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
			int ms; char file[256];
			if (sscanf(tok, "%d:%255s", &ms, file) == 2 && ms == t) write_pgm(b, file);
		}
	}
}

static const char *models[] = { "950", "850", "957", "857" };

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s PagerXXX.bin [ms] [screen.pgm]\n", argv[0]);
		return 1;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *img = malloc(len);
	if (fread(img, 1, len, f) != (size_t)len) { perror("read"); return 1; }
	fclose(f);

	char err[128];
	PPBoard *b = pp_board_new(img, len, err, sizeof err);
	if (!b) { fprintf(stderr, "error: %s\n", err); return 1; }
	printf("model RIM %s, LCD %dx%d\n", models[pp_board_model(b)],
	       pp_lcd_width(b), pp_lcd_height(b));

	if (getenv("PP_LOADFLASH")) {
		FILE *fi = fopen(getenv("PP_LOADFLASH"), "rb");
		unsigned char *fl = malloc(PP_FLASH_IMAGE_SIZE);
		size_t n = fi ? fread(fl, 1, PP_FLASH_IMAGE_SIZE, fi) : 0;
		if (fi) fclose(fi);
		if (!pp_board_set_flash(b, fl, n)) { fprintf(stderr, "bad flash image\n"); return 1; }
		free(fl);
		pp_board_reset(b);
		printf("flash loaded from %s\n", getenv("PP_LOADFLASH"));
	}
	if (getenv("PP_TRACE")) pp_board_set_trace(b, 1);
	int ms = argc > 2 ? atoi(argv[2]) : 1000;
	if (getenv("PP_SERIAL")) {
		run_serial(b, ms);
		goto done;
	}
	char st[256];
	int inject = getenv("PP_INJECT_HS") ? atoi(getenv("PP_INJECT_HS")) : -1;
	for (int t = 0; t < ms; t += 100) {
		scripted_events(b, t);
		if (t == inject) {
			static const unsigned char hs[] = { 0x0E, 0xAA, 0x0F, 0xF0 };
			pp_serial_rx(b, 1, hs, 4);
			printf("injected loader handshake\n");
		}
		pp_board_run(b, 100000);
		pp_board_status(b, st, sizeof st);
		printf("[%5d ms] %s\n", t + 100, st);
		if (pp_board_stopped(b)) break;
	}

done:
	if (getenv("PP_SAVEFLASH")) {
		FILE *o = fopen(getenv("PP_SAVEFLASH"), "wb");
		unsigned char *fl = malloc(PP_FLASH_IMAGE_SIZE);
		pp_board_get_flash(b, fl);
		fwrite(fl, 1, PP_FLASH_IMAGE_SIZE, o);
		fclose(o);
		free(fl);
		printf("flash saved to %s\n", getenv("PP_SAVEFLASH"));
	}
	if (argc > 3) {
		int w = pp_lcd_width(b), h = pp_lcd_height(b);
		unsigned int *px = malloc(w * h * 4);
		pp_lcd_render(b, px);
		FILE *o = fopen(argv[3], "wb");
		fprintf(o, "P5 %d %d 255\n", w, h);
		for (int i = 0; i < w * h; i++)
			fputc(px[i] & 0xFF, o);
		fclose(o);
	}
	if (getenv("PP_DUMP")) {
		FILE *o = fopen(getenv("PP_DUMP"), "wb");
		fwrite(pp_board_mem(b), 1, 0x4000000, o);
		fclose(o);
	}
	pp_board_free(b);
	return 0;
}
