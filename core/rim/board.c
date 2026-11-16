/*
 * PocketPager - RIM 85x/95x board emulation (Intel 386EX based).
 *
 * Memory map (26-bit physical address space, 64 MB):
 *   0x00600000  SRAM (OS data, GDT at 0x609A34)
 *   0x03FB0000  OS image (256 KB, PagerXXX.bin, runs in place from flash)
 *   0x03FF0000  boot ROM (not dumped - we start the OS directly)
 *
 * The OS entry point is the first byte of the image, entered in "flat real
 * mode" (as left by the boot ROM): CS points at the image, GS is flat 4 GB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <setjmp.h>
#include "board.h"
#include "scramble.h"
#include "../cpu/i386.h"
#include "../cpu/i8259.h"
#include "../cpu/i8254.h"

#define MEM_SIZE   0x4000000u
#define ADDR_MASK  0x3FFFFFFu
#define OS_BASE    0x03FB0000u
#define OS_SIZE    0x40000u
#define FLASH_BASE 0x03C00000u
#define FLASH_SIZE 0x00400000u
/* The flash chip is also decoded at 0x03000000 (chip-select alias). */
#define FLASH_ALIAS 0x03000000u
#define FLASH_ID_MFR 0x0089     /* Intel */
#define FLASH_ID_DEV 0x8896     /* 28F320B3 top boot, 4 MB (OS accepts 8890-8896) */

/* Emulated speed: instructions per microsecond. */
#define IPS_PER_US 4

unsigned char pp_iomem_r[1024], pp_iomem_w[1024];

typedef struct PPBoard PPBoard;
static PPBoard *g_board; /* for get_uticks() and aborts */
static jmp_buf g_abort_jmp;
static int g_abort_armed;

enum { SEG_ES = 0, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS };

struct PPBoard {
	uint8_t *mem;
	CPUI386 *cpu;
	CPU_CB *cb;
	PicState2 *pic;
	PITState *pit;
	int model;
	int lcd_w, lcd_h;

	uint8_t os[OS_SIZE];

	/* Generic register file for expanded I/O space, so reads return
	 * what was last written for registers we don't model yet. */
	uint8_t ioreg[0x10000];
	uint16_t seen[0x10000];      /* logging budget per port */

	/* 386EX serial ports: debug output goes to the log. */
	char sio_line[2][256];
	int sio_len[2];

	/* GPIO ports P1..P3 input pins */
	uint8_t pin[3];

	/* Intel boot-block flash state machine */
	int fl_mode;              /* 0 array, 1 id, 2 status, 3 cfi */
	int fl_cmd;               /* pending two-cycle command */
	uint8_t fl_status;
	int fl_dirty;
	uint32_t fl_blk_start, fl_blk_len; /* block the current mode applies to */

	/* LCD controller at ports 0x380 (command) / 0x382 (data) */
	uint8_t lcd_ram[160 * 20];
	uint8_t lcd_cmd;
	int lcd_row, lcd_col, lcd_argc;
	uint8_t lcd_args[4];
	uint8_t lcd_contrast;
	int lcd_on, lcd_dirty;

	/* MC146818-compatible RTC at ports 0x70/0x71 */
	uint8_t rtc_idx;
	uint8_t rtc[64];
	int64_t rtc_secs;         /* seconds since 1970, advanced by emulated time */
	uint64_t rtc_last_us;

	uint64_t instr;

	/* debug: instruction trace ring */
	int trace;
	uint32_t ring[1 << 20];
	unsigned ringpos;
	int stop;
	uint32_t stop_at;
};

static void dump_ring(PPBoard *b, const char *why);

void pp_cpu_abort(int code)
{
	if (g_board) {
		pp_log("CPU stopped (code %d)", code);
		dump_ring(g_board, "cpu abort");
		g_board->stop = 2;
	}
	if (g_abort_armed)
		longjmp(g_abort_jmp, 1);
}

void pp_board_set_trace(PPBoard *b, int on)
{
	b->trace = on;
	if (getenv("PP_STOPAT")) b->stop_at = strtoul(getenv("PP_STOPAT"), NULL, 16);
}
int pp_board_stopped(PPBoard *b) { return b->stop; }

static void dump_ring(PPBoard *b, const char *why)
{
	b->stop = 1;
	if (!b->trace) return;
	pp_log("--- trace before %s ---", why);
	uint32_t last = 0xFFFFFFFF; int rep = 0;
	for (unsigned i = 0; i < (1u << 20); i++) {
		uint32_t e = b->ring[(b->ringpos + i) & ((1 << 20) - 1)];
		if (e == last) { rep++; continue; }
		if (rep) pp_log("   (x%d)", rep);
		rep = 0; last = e;
		pp_log("  %08x", e);
	}
	b->stop = 1;
}



/* ------------------------------------------------------------ logging */

static pp_log_fn g_log;
void pp_set_log(pp_log_fn fn) { g_log = fn; }

void pp_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (g_log)
		g_log(buf);
	else
		fprintf(stderr, "%s\n", buf);
}

static uint32_t cur_eip(PPBoard *b)
{
	CPUI386_State st;
	cpui386_get_state(b->cpu, &st);
	return st.ip;
}

static void log_port(PPBoard *b, const char *what, int port, uint32_t val, int size)
{
	static int all = -1;
	if (all < 0) all = getenv("PP_PORTLOG") ? (int)strtol(getenv("PP_PORTLOG"), NULL, 16) : 0;
	if (all && (port & ~3) == all) {
		pp_log("io %-3s %04x = %02x @eip %08x", what, port, val, cur_eip(b));
		return;
	}
	if (b->seen[port] >= 4)
		return;
	b->seen[port]++;
	pp_log("io %-3s %04x%s = %0*x  @eip %08x", what, port,
	       size == 1 ? "b" : size == 2 ? "w" : "d", size * 2, val, cur_eip(b));
}

/* emulated time for the PIT */
uint32_t get_uticks(void)
{
	return g_board ? (uint32_t)(g_board->instr / IPS_PER_US) : 0;
}

/* ------------------------------------------------------------ serial */

static void sio_tx(PPBoard *b, int n, uint8_t c)
{
	if (c == '\n' || c == '\r' || b->sio_len[n] >= 255) {
		if (b->sio_len[n]) {
			b->sio_line[n][b->sio_len[n]] = 0;
			pp_log("SIO%d> %s", n, b->sio_line[n]);
			b->sio_len[n] = 0;
		}
		if (c == '\n' || c == '\r')
			return;
	}
	b->sio_line[n][b->sio_len[n]++] = (c >= 32 && c < 127) ? c : '.';
}

/* returns 1 if handled */
static int sio_read(PPBoard *b, int port, uint8_t *v)
{
	int n, r;
	if (port >= 0xF4F8 && port <= 0xF4FF) { n = 0; r = port - 0xF4F8; }
	else if (port >= 0xF8F8 && port <= 0xF8FF) { n = 1; r = port - 0xF8F8; }
	else if (port >= 0x3F8 && port <= 0x3FF) { n = 0; r = port - 0x3F8; }
	else if (port >= 0x2F8 && port <= 0x2FF) { n = 1; r = port - 0x2F8; }
	else return 0;
	(void)n;
	switch (r) {
	case 0: *v = 0; break;                  /* RBR */
	case 2: *v = 0x01; break;               /* IIR: none pending */
	case 5: *v = 0x60; break;               /* LSR: THR empty */
	case 6: *v = 0xB0; break;               /* MSR: CTS, DSR, DCD */
	default: *v = b->ioreg[port]; break;
	}
	return 1;
}

static int sio_write(PPBoard *b, int port, uint8_t v)
{
	int n, r;
	if (port >= 0xF4F8 && port <= 0xF4FF) { n = 0; r = port - 0xF4F8; }
	else if (port >= 0xF8F8 && port <= 0xF8FF) { n = 1; r = port - 0xF8F8; }
	else if (port >= 0x3F8 && port <= 0x3FF) { n = 0; r = port - 0x3F8; }
	else if (port >= 0x2F8 && port <= 0x2FF) { n = 1; r = port - 0x2F8; }
	else return 0;
	b->ioreg[port] = v;
	/* THR, unless DLAB set in LCR */
	int lcr = (n == 0) ? (port >= 0xF000 ? b->ioreg[0xF4FB] : b->ioreg[0x3FB])
			   : (port >= 0xF000 ? b->ioreg[0xF8FB] : b->ioreg[0x2FB]);
	if (r == 0 && !(lcr & 0x80))
		sio_tx(b, n, v);
	return 1;
}

/* ------------------------------------------------------------ RTC */

static void rtc_tick(PPBoard *b)
{
	uint64_t now = b->instr / IPS_PER_US;
	if (now - b->rtc_last_us >= 1000000) {
		uint64_t secs = (now - b->rtc_last_us) / 1000000;
		b->rtc_last_us += secs * 1000000;
		if (!(b->rtc[0x0B] & 0x80))     /* SET bit stops updates */
			b->rtc_secs += secs;
		b->rtc[0x0C] |= 0x10;           /* UF */
	}
}

static uint8_t rtc_enc(PPBoard *b, int v)
{
	if (b->rtc[0x0B] & 0x04)                /* DM: binary */
		return v;
	return (v / 10) << 4 | (v % 10);
}

static int rtc_dec(PPBoard *b, uint8_t v)
{
	if (b->rtc[0x0B] & 0x04)
		return v;
	return (v >> 4) * 10 + (v & 15);
}

/* civil date from days since 1970 (Howard Hinnant's algorithm) */
static void days_to_ymd(int64_t z, int *y, int *m, int *d)
{
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t yy = (int64_t)yoe + era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = (int)(yy + (*m <= 2));
}

static int64_t ymd_to_days(int y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

static uint8_t rtc_read(PPBoard *b)
{
	int i = b->rtc_idx & 0x3F;
	rtc_tick(b);
	int64_t s = b->rtc_secs;
	int64_t days = s / 86400;
	int sod = (int)(s % 86400);
	int y, m, d;
	days_to_ymd(days, &y, &m, &d);
	switch (i) {
	case 0: return rtc_enc(b, sod % 60);
	case 2: return rtc_enc(b, (sod / 60) % 60);
	case 4: {
		int h = sod / 3600;
		if (b->rtc[0x0B] & 0x02)
			return rtc_enc(b, h);
		int pm = h >= 12;
		h %= 12;
		if (h == 0) h = 12;
		return rtc_enc(b, h) | (pm ? 0x80 : 0);
	}
	case 6: return (uint8_t)(((days + 4) % 7) + 1); /* 1970-01-01 = Thursday */
	case 7: return rtc_enc(b, d);
	case 8: return rtc_enc(b, m);
	case 9: return rtc_enc(b, y % 100);
	case 0x0A: return b->rtc[0x0A] & 0x7F;  /* UIP never set */
	case 0x0C: {
		uint8_t v = b->rtc[0x0C];
		b->rtc[0x0C] = 0;
		return v;
	}
	case 0x0D: return 0x80;                 /* VRT: battery OK */
	}
	return b->rtc[i];
}

static void rtc_write(PPBoard *b, uint8_t v)
{
	int i = b->rtc_idx & 0x3F;
	rtc_tick(b);
	int64_t s = b->rtc_secs;
	int64_t days = s / 86400;
	int sod = (int)(s % 86400);
	int y, m, d;
	days_to_ymd(days, &y, &m, &d);
	int hh = sod / 3600, mm = (sod / 60) % 60, ss = sod % 60;
	switch (i) {
	case 0: ss = rtc_dec(b, v); break;
	case 2: mm = rtc_dec(b, v); break;
	case 4:
		if (b->rtc[0x0B] & 0x02) hh = rtc_dec(b, v);
		else {
			hh = rtc_dec(b, v & 0x7F) % 12;
			if (v & 0x80) hh += 12;
		}
		break;
	case 7: d = rtc_dec(b, v); break;
	case 8: m = rtc_dec(b, v); break;
	case 9: y = 2000 + rtc_dec(b, v); if (y >= 2090) y -= 100; break;
	default:
		b->rtc[i] = v;
		return;
	}
	if (m < 1 || m > 12) m = 1;
	if (d < 1 || d > 31) d = 1;
	b->rtc_secs = ymd_to_days(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
}

/* ------------------------------------------------------------ LCD */

static void lcd_command(PPBoard *b, uint8_t c)
{
	b->lcd_cmd = c;
	b->lcd_argc = 0;
	switch (c) {
	case 0x56: b->lcd_row = 0; b->lcd_col = 0; break;   /* home */
	case 0x5C: b->lcd_col = 0; break;                   /* write row */
	case 0xBE: b->lcd_row++; b->lcd_col = 0; break;     /* next row */
	case 0xAF: b->lcd_on = 1; b->lcd_dirty = 1; break;  /* display on */
	case 0xAE: b->lcd_on = 0; b->lcd_dirty = 1; break;  /* display off */
	}
}

static void lcd_data(PPBoard *b, uint8_t v)
{
	switch (b->lcd_cmd) {
	case 0x5C:
		if (b->lcd_row >= 0 && b->lcd_row < 160 && b->lcd_col < 20) {
			uint8_t *p = &b->lcd_ram[b->lcd_row * 20 + b->lcd_col];
			if (*p != v) { *p = v; b->lcd_dirty = 1; }
		}
		b->lcd_col++;
		break;
	case 0x15:                                          /* row address */
		if (b->lcd_argc < 4) b->lcd_args[b->lcd_argc++] = v;
		if (b->lcd_argc == 1) { b->lcd_row = v; b->lcd_col = 0; }
		break;
	case 0xC6:                                          /* contrast */
		b->lcd_contrast = v;
		break;
	}
}

/* ------------------------------------------------------------ I/O ports */

static uint8_t io_read8(void *o, int port)
{
	PPBoard *b = o;
	uint8_t v;
	port &= 0xFFFF;
	switch (port) {
	case 0x20: case 0x21: case 0xA0: case 0xA1:
		return i8259_ioport_read(b->pic, port);
	case 0xF020: case 0xF021: case 0xF0A0: case 0xF0A1:
		return i8259_ioport_read(b->pic, port & 0xFF);
	case 0x40: case 0x41: case 0x42: case 0x43:
		return i8254_ioport_read(b->pit, port);
	case 0xF040: case 0xF041: case 0xF042: case 0xF043:
		return i8254_ioport_read(b->pit, port & 0xFF);
	case 0x70: return b->rtc_idx;
	case 0x71: return rtc_read(b);
	case 0xF860: return b->pin[0];          /* P1PIN */
	case 0xF868: return b->pin[1];          /* P2PIN */
	case 0xF870: return b->pin[2];          /* P3PIN */
	}
	if (sio_read(b, port, &v))
		return v;
	v = b->ioreg[port];
	log_port(b, "in", port, v, 1);
	return v;
}

static void io_write8(void *o, int port, uint8_t v)
{
	PPBoard *b = o;
	port &= 0xFFFF;
	switch (port) {
	case 0x20: case 0x21: case 0xA0: case 0xA1:
		i8259_ioport_write(b->pic, port, v); return;
	case 0xF020: case 0xF021: case 0xF0A0: case 0xF0A1:
		i8259_ioport_write(b->pic, port & 0xFF, v); return;
	case 0x40: case 0x41: case 0x42: case 0x43:
		i8254_ioport_write(b->pit, port, v); return;
	case 0xF040: case 0xF041: case 0xF042: case 0xF043:
		i8254_ioport_write(b->pit, port & 0xFF, v); return;
	case 0x380: lcd_command(b, v); return;
	case 0x382: lcd_data(b, v); return;
	case 0x70: b->rtc_idx = v; return;
	case 0x71: rtc_write(b, v); return;
	case 0x92:
		if (v & 1) { pp_log("port 92 reset requested"); dump_ring(b, "reset"); }
		b->ioreg[port] = v; return;
	case 0xF4C8: case 0xF4C9:               /* WDTCLR: watchdog kick */
		b->ioreg[port] = v; return;
	}
	if (sio_write(b, port, v))
		return;
	b->ioreg[port] = v;
	log_port(b, "out", port, v, 1);
}

static uint16_t io_read16(void *o, int port)
{
	return io_read8(o, port) | io_read8(o, port + 1) << 8;
}
static void io_write16(void *o, int port, uint16_t v)
{
	io_write8(o, port, v & 0xFF);
	io_write8(o, port + 1, v >> 8);
}
static uint32_t io_read32(void *o, int port)
{
	return io_read16(o, port) | (uint32_t)io_read16(o, port + 2) << 16;
}
static void io_write32(void *o, int port, uint32_t v)
{
	io_write16(o, port, v & 0xFFFF);
	io_write16(o, port + 2, v >> 16);
}

/* ------------------------------------------------------------ flash */

enum { FL_ARRAY, FL_ID, FL_STATUS, FL_CFI };

/* block containing offset (top boot: 8 KB parameter blocks in the last 64 KB) */
static void fl_block(uint32_t off, uint32_t *start, uint32_t *len)
{
	if (off >= FLASH_SIZE - 0x10000) {
		*start = off & ~0x1FFFu;
		*len = 0x2000;
	} else {
		*start = off & ~0xFFFFu;
		*len = 0x10000;
	}
}

/* The non-array read modes only affect the block that was last addressed,
 * so code keeps running from other blocks (read-while-write behaviour). */
static void fl_set_mode_at(PPBoard *b, int mode, uint32_t off)
{
	memset(pp_iomem_r + (FLASH_BASE >> 16), 0, FLASH_SIZE >> 16);
	memset(pp_iomem_r + (FLASH_ALIAS >> 16), 1, FLASH_SIZE >> 16);
	b->fl_mode = mode;
	fl_block(off, &b->fl_blk_start, &b->fl_blk_len);
	if (mode != FL_ARRAY)
		pp_iomem_r[(FLASH_BASE + b->fl_blk_start) >> 16] = 1;
}
#define fl_set_mode(b, m) fl_set_mode_at(b, m, off)

static uint16_t fl_read16(PPBoard *b, uint32_t off)
{
	int mode = b->fl_mode;
	if (off - b->fl_blk_start >= b->fl_blk_len)
		mode = FL_ARRAY;
	switch (mode) {
	case FL_ID:
		switch ((off >> 1) & 0xFF) {
		case 0: return FLASH_ID_MFR;
		case 1: return FLASH_ID_DEV;
		default: return 0;      /* block lock status: unlocked */
		}
	case FL_STATUS:
		return b->fl_status;
	case FL_CFI: {
		/* CFI table for a 4 MB x16 Intel top-boot part */
		static const uint8_t cfi[] = {
			[0x10] = 'Q', 'R', 'Y', 0x03, 0x00, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00,
			[0x1B] = 0x27, 0x36, 0xB4, 0xC6, 0x05, 0x00, 0x0A, 0x00, 0x04, 0x00, 0x03, 0x00,
			[0x27] = 0x16, 0x01, 0x00, 0x00, 0x00, 0x02,
			[0x2D] = 0x3E, 0x00, 0x00, 0x01,     /* 63 x 64 KB */
			[0x31] = 0x07, 0x00, 0x20, 0x00,     /* 8 x 8 KB */
			[0x35] = 'P', 'R', 'I', '1', '0',
		};
		uint32_t w = (off >> 1) & 0xFF;
		return w < sizeof(cfi) ? cfi[w] : 0;
	}
	}
	return b->mem[FLASH_BASE + off] | b->mem[FLASH_BASE + off + 1] << 8;
}

static void fl_write16(PPBoard *b, uint32_t off, uint16_t v, int size)
{
	off &= FLASH_SIZE - 1;
	if (getenv("PP_FLASHLOG"))
		pp_log("flash w%d %08x = %04x (cmd %02x) @eip %08x", size, FLASH_BASE + off, v, b->fl_cmd, cur_eip(b));
	if (b->fl_cmd == 0x40 || b->fl_cmd == 0x10) {
		/* word program: bits can only go 1 -> 0 */
		uint32_t a = FLASH_BASE + (off & ~1u);
		if (size == 1) {
			b->mem[FLASH_BASE + off] &= (uint8_t)v;
		} else {
			b->mem[a] &= v & 0xFF;
			b->mem[a + 1] &= v >> 8;
		}
		b->fl_cmd = 0;
		b->fl_status = 0x80;
		b->fl_dirty = 1;
		fl_set_mode(b, FL_STATUS);
		return;
	}
	if (b->fl_cmd == 0x20) {
		b->fl_cmd = 0;
		if ((v & 0xFF) == 0xD0) {
			uint32_t st, len;
			fl_block(off, &st, &len);
			memset(b->mem + FLASH_BASE + st, 0xFF, len);
			b->fl_status = 0x80;
			b->fl_dirty = 1;
		} else {
			b->fl_status = 0x80 | 0x30;     /* command sequence error */
		}
		fl_set_mode(b, FL_STATUS);
		return;
	}
	if (b->fl_cmd == 0x60) {                /* lock commands: accept */
		b->fl_cmd = 0;
		fl_set_mode(b, FL_ARRAY);
		return;
	}
	switch (v & 0xFF) {
	case 0xFF: case 0xF0: fl_set_mode(b, FL_ARRAY); break;
	case 0x90: fl_set_mode(b, FL_ID); break;
	case 0x98: fl_set_mode(b, FL_CFI); break;
	case 0x70: fl_set_mode(b, FL_STATUS); break;
	case 0x50:
		/* Clear status. RIM's programming loop reads the array straight
		 * after this, so on these parts it also returns to read-array. */
		b->fl_status = 0x80;
		fl_set_mode(b, FL_ARRAY);
		break;
	case 0x40: case 0x10: case 0x20: case 0x60:
		b->fl_cmd = v & 0xFF;
		b->fl_status = 0x80;
		fl_set_mode(b, FL_STATUS);
		break;
	case 0xB0: b->fl_status = 0x80; fl_set_mode(b, FL_STATUS); break; /* suspend: already done */
	case 0xD0: b->fl_status = 0x80; fl_set_mode(b, FL_STATUS); break; /* resume */
	default:
		pp_log("flash: unknown command %02x at %06x @eip %08x", v & 0xFF, off, cur_eip(b));
		break;
	}
}

static int in_flash(uint32_t a)
{
	return (a >= FLASH_BASE && a < FLASH_BASE + FLASH_SIZE) ||
	       (a >= FLASH_ALIAS && a < FLASH_ALIAS + FLASH_SIZE);
}
static uint32_t flash_off(uint32_t a) { return (a - FLASH_ALIAS) & (FLASH_SIZE - 1); }

/* ------------------------------------------------------------ MMIO */

#define LCD_BASE 0x02A00000u
static void lcd_mm_log(PPBoard *b, const char *w, uint32_t a, uint32_t v)
{
	static int n;
	if (n++ < 200 || getenv("PP_LCDLOG"))
		pp_log("lcdmem %s %08x = %x @eip %08x", w, a, v, cur_eip(b));
}

static uint8_t mm_read8(void *o, uint32_t a)
{
	PPBoard *b = o;
	a &= ADDR_MASK;
	if ((a >> 20) == (LCD_BASE >> 20)) lcd_mm_log(b, "rd", a, b->mem[a]);
	if (in_flash(a)) {
		uint16_t w = fl_read16(b, flash_off(a) & ~1u);
		return (a & 1) ? w >> 8 : w & 0xFF;
	}
	return b->mem[a];
}
static void mm_write8(void *o, uint32_t a, uint8_t v)
{
	PPBoard *b = o;
	a &= ADDR_MASK;
	if ((a >> 20) == (LCD_BASE >> 20)) lcd_mm_log(b, "wr", a, v);
	if (in_flash(a)) {
		fl_write16(b, flash_off(a), v, 1);
		return;
	}
	b->mem[a] = v;
}
static uint16_t mm_read16(void *o, uint32_t a)
{
	PPBoard *b = o;
	a &= ADDR_MASK;
	if (in_flash(a) && !(a & 1))
		return fl_read16(b, flash_off(a));
	return mm_read8(o, a) | mm_read8(o, a + 1) << 8;
}
static void mm_write16(void *o, uint32_t a, uint16_t v)
{
	PPBoard *b = o;
	a &= ADDR_MASK;
	if (in_flash(a) && !(a & 1)) {
		fl_write16(b, flash_off(a), v, 2);
		return;
	}
	mm_write8(o, a, v);
	mm_write8(o, a + 1, v >> 8);
}
static uint32_t mm_read32(void *o, uint32_t a) { return mm_read16(o, a) | (uint32_t)mm_read16(o, a + 2) << 16; }
static void mm_write32(void *o, uint32_t a, uint32_t v) { mm_write16(o, a, v); mm_write16(o, a + 2, v >> 16); }

/* ------------------------------------------------------------ PIC glue */

static void raise_irq(void *o, PicState2 *s)
{
	(void)s;
	cpui386_raise_irq(o);
}
static int read_irq(void *o) { return i8259_read_irq(o); }
static void set_irq(void *pic, int irq, int level) { i8259_set_irq(pic, irq, level); }

/* ------------------------------------------------------------ board */

static int detect_model(const uint8_t *os)
{
	const char *s957 = "Handheld for Mobitex";
	const char *s857 = "Handheld for DataTAC";
	int has957 = 0, has857 = 0, hasdt = 0;
	for (size_t i = 0; i + 24 < OS_SIZE; i++) {
		if (!memcmp(os + i, s957, strlen(s957))) has957 = 1;
		if (!memcmp(os + i, s857, strlen(s857))) has857 = 1;
		if (!memcmp(os + i, "DataTAC", 7)) hasdt = 1;
	}
	if (has957) return PP_MODEL_957;
	if (has857) return PP_MODEL_857;
	return hasdt ? PP_MODEL_850 : PP_MODEL_950;
}

PPBoard *pp_board_new(const uint8_t *img, size_t len, char *err, size_t errlen)
{
	PPBoard *b = calloc(1, sizeof(*b));
	if (!b) { snprintf(err, errlen, "out of memory"); return NULL; }

	if (pp_is_scrambled(img, len)) {
		uint8_t *tmp = malloc(len);
		long n = pp_descramble(img, len, tmp);
		if (n != OS_SIZE) {
			free(tmp); free(b);
			snprintf(err, errlen, "could not descramble OS image");
			return NULL;
		}
		memcpy(b->os, tmp, OS_SIZE);
		free(tmp);
	} else if (len == OS_SIZE && img[0] == 0xFA) {
		memcpy(b->os, img, OS_SIZE);
	} else {
		free(b);
		snprintf(err, errlen, "not a RIM 85x/95x OS image (PagerXXX.bin)");
		return NULL;
	}

	b->mem = malloc(MEM_SIZE);
	if (!b->mem) { free(b); snprintf(err, errlen, "out of memory"); return NULL; }

	b->model = detect_model(b->os);
	if (b->model == PP_MODEL_957 || b->model == PP_MODEL_857) {
		b->lcd_w = 160; b->lcd_h = 160;
	} else {
		b->lcd_w = 132; b->lcd_h = 65;
	}

	b->cpu = cpui386_new(3, (char *)b->mem, MEM_SIZE, &b->cb);
	b->cb->io = b;
	b->cb->io_read8 = (void *)io_read8;
	b->cb->io_write8 = (void *)io_write8;
	b->cb->io_read16 = (void *)io_read16;
	b->cb->io_write16 = (void *)io_write16;
	b->cb->io_read32 = (void *)io_read32;
	b->cb->io_write32 = (void *)io_write32;
	b->cb->iomem = b;
	b->cb->iomem_read8 = (void *)mm_read8;
	b->cb->iomem_write8 = (void *)mm_write8;
	b->cb->iomem_read16 = (void *)mm_read16;
	b->cb->iomem_write16 = (void *)mm_write16;
	b->cb->iomem_read32 = (void *)mm_read32;
	b->cb->iomem_write32 = (void *)mm_write32;

	b->pic = i8259_init(raise_irq, b->cpu);
	b->cb->pic = b->pic;
	b->cb->pic_read_irq = read_irq;
	g_board = b;
	b->pit = i8254_init(0, b->pic, set_irq);

	pp_board_reset(b);
	return b;
}

void pp_board_reset(PPBoard *b)
{
	memset(b->mem, 0, MEM_SIZE);
	/* flash is erased (0xFF) apart from the OS */
	memset(b->mem + 0x03C00000, 0xFF, 0x400000);
	memcpy(b->mem + OS_BASE, b->os, OS_SIZE);
	memset(b->ioreg, 0, sizeof(b->ioreg));
	memset(b->seen, 0, sizeof(b->seen));
	b->pin[0] = b->pin[1] = b->pin[2] = 0xFF;
	memset(pp_iomem_r, 0, sizeof(pp_iomem_r));
	memset(pp_iomem_w, 0, sizeof(pp_iomem_w));
	memset(pp_iomem_w + (FLASH_BASE >> 16), 1, FLASH_SIZE >> 16);
	memset(pp_iomem_w + (FLASH_ALIAS >> 16), 1, FLASH_SIZE >> 16);
	memset(pp_iomem_w + (LCD_BASE >> 16), 1, 16);
	memset(pp_iomem_r + (LCD_BASE >> 16), 1, 16);
	b->fl_cmd = 0;
	b->fl_status = 0x80;
	fl_set_mode_at(b, FL_ARRAY, 0);
	b->instr = 0;
	memset(b->lcd_ram, 0, sizeof(b->lcd_ram));
	b->lcd_on = 0; b->lcd_row = 0; b->lcd_col = 0; b->lcd_dirty = 1;

	/* RTC starts at the host's local time */
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	b->rtc_secs = (int64_t)now + lt.tm_gmtoff;
	b->rtc_last_us = 0;
	memset(b->rtc, 0, sizeof(b->rtc));
	b->rtc[0x0A] = 0x26;
	b->rtc[0x0B] = 0x02;

	cpui386_reset(b->cpu);
	/* State left by the boot ROM: flat real mode, CS at the OS image. */
	const uint32_t rm16 = 0x93 << 8;     /* present, data/code r/w, 16-bit */
	cpui386_set_seg(b->cpu, SEG_CS, OS_BASE >> 4, OS_BASE, 0xFFFF, 0x9B << 8);
	cpui386_set_seg(b->cpu, SEG_DS, 0, 0, 0xFFFFFFFF, rm16);
	cpui386_set_seg(b->cpu, SEG_ES, 0, 0, 0xFFFFFFFF, rm16);
	cpui386_set_seg(b->cpu, SEG_FS, 0, 0, 0xFFFFFFFF, rm16);
	cpui386_set_seg(b->cpu, SEG_GS, 0, 0, 0xFFFFFFFF, rm16);
	cpui386_set_seg(b->cpu, SEG_SS, 0, 0, 0xFFFFFFFF, rm16);
	cpui386_set_ip(b->cpu, 0);
}

void pp_board_free(PPBoard *b)
{
	if (!b) return;
	if (g_board == b) g_board = NULL;
	cpui386_delete(b->cpu);
	free(b->mem);
	free(b);
}

void pp_board_run(PPBoard *b, uint32_t us)
{
	uint64_t target = b->instr + (uint64_t)us * IPS_PER_US;
	if (b->stop)
		return;
	g_board = b;
	g_abort_armed = 1;
	if (setjmp(g_abort_jmp)) {
		g_abort_armed = 0;
		return;
	}
	while (b->instr < target && !b->stop) {
		if (b->trace) {
			for (int i = 0; i < 256 && !b->stop; i++) {
				CPUI386_State st;
				cpui386_get_state(b->cpu, &st);
				b->ring[b->ringpos++ & ((1 << 20) - 1)] = st.ip;
				if (b->stop_at && st.ip == b->stop_at) { pp_log("hit stop address %08x", st.ip); b->stop = 3; break; }
				cpui386_step(b->cpu, 1);
			}
			b->instr += 256;
			i8254_update_irq(b->pit);
			continue;
		}
		int chunk = 256;
		cpui386_step(b->cpu, chunk);
		b->instr += chunk;
		i8254_update_irq(b->pit);
	}
	g_abort_armed = 0;
}

int pp_board_model(PPBoard *b) { return b->model; }
int pp_lcd_width(PPBoard *b) { return b->lcd_w; }
int pp_lcd_height(PPBoard *b) { return b->lcd_h; }

int pp_lcd_render(PPBoard *b, uint32_t *argb)
{
	const uint32_t on = 0xFF1E2418, off = 0xFFB8C4A8;
	int changed = b->lcd_dirty;
	b->lcd_dirty = 0;
	for (int y = 0; y < b->lcd_h; y++)
		for (int x = 0; x < b->lcd_w; x++) {
			int bit = 0;
			if (b->lcd_on && y < 160 && x < 160)
				bit = (b->lcd_ram[y * 20 + (x >> 3)] >> (7 - (x & 7))) & 1;
			argb[y * b->lcd_w + x] = bit ? on : off;
		}
	return changed;
}

void pp_key(PPBoard *b, int key, int down)
{
	(void)b; (void)key; (void)down;
}

void pp_board_status(PPBoard *b, char *buf, size_t len)
{
	CPUI386_State st;
	cpui386_get_state(b->cpu, &st);
	snprintf(buf, len,
		 "instr=%llu cs=%04x eip=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esp=%08x cr0=%x halt=%d",
		 (unsigned long long)b->instr, st.seg[1], st.ip, st.gpr[0], st.gpr[3],
		 st.gpr[1], st.gpr[2], st.gpr[4], st.cr0, cpui386_is_halted(b->cpu));
}

uint64_t pp_board_instructions(PPBoard *b) { return b->instr; }
uint8_t *pp_board_mem(PPBoard *b) { return b->mem; }
