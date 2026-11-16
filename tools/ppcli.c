/*
 * PocketPager headless runner, used for reverse engineering and CI.
 *   ppcli Pager957.bin [milliseconds] [screen.pgm]
 */
#include <stdio.h>
#include <stdlib.h>
#include "../core/rim/board.h"

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

	if (getenv("PP_TRACE")) pp_board_set_trace(b, 1);
	int ms = argc > 2 ? atoi(argv[2]) : 1000;
	char st[256];
	for (int t = 0; t < ms; t += 100) {
		pp_board_run(b, 100000);
		pp_board_status(b, st, sizeof st);
		printf("[%5d ms] %s\n", t + 100, st);
		if (pp_board_stopped(b)) break;
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
