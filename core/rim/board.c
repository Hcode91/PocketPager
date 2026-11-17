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
/* Second chip: AMD-command-set flash holding the file system. */
#define FS_BASE 0x03000000u
#define FS_SIZE 0x00400000u
#define FLASH_ID_MFR 0x0089     /* Intel */
#define FLASH_ID_DEV 0x8896     /* 28F320B3 top boot, 4 MB (OS accepts 8890-8896) */

/* Emulated speed: instructions per microsecond. */
#define IPS_PER_US 4

unsigned char pp_iomem_r[1024], pp_iomem_w[1024];

typedef struct PPBoard PPBoard;

#define UART_FIFO 8192
typedef struct {
	uint8_t ier, lcr, mcr, scr, dll, dlm, rbr, msr, msr_delta;
	int rbr_full, thre_pending, tx_busy, irq_level;
	uint32_t hs_window;
	uint64_t next_rx, tx_done;
	uint8_t rx[UART_FIFO];
	int rh, rt;
} UART;

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
	UART uart[2];
	void (*tx_fn[2])(void *, uint8_t);
	void *tx_ctx[2];

	/* GPIO ports P1..P3 input pins */
	uint8_t pin[3];

	/* Intel boot-block flash state machine */
	int fl_mode;              /* 0 array, 1 id, 2 status, 3 cfi */
	int fl_cmd;               /* pending two-cycle command */
	uint8_t fl_status;
	int fl_dirty;
	int amd_mode, amd_step;
	uint32_t fl_blk_start, fl_blk_len; /* block the current mode applies to */

	/* Radio DSP (ADSP-218x) seen through its IDMA port:
	 * index register at 0x02A00000, data register at 0x02A10000. */
	uint16_t dsp_addr;
	int dsp_pm_half;
	uint32_t dsp_pm[0x4000];
	uint16_t dsp_dm[0x4000];
	int dsp_running, dsp_irq, dsp_pwdack;

	/* Keyboard matrix (RIM ASIC at 0xFC30-0xFC38, IRQ 1) and trackwheel
	 * (quadrature: IRQ 13 + P1 0xF863 bit 2 / IRQ 12 + 0xF867 bit 6). */
	uint16_t keymap[64];          /* OS key table: index = row << 3 | col */
	uint8_t kb_rows[8];           /* pressed rows per column */
	uint8_t kb_colsel;            /* 0xFC30, active-low column select */
	uint16_t kb_ctl;              /* 0xFC38 */
	uint64_t kb_next;             /* next scan-timer interrupt */
	int kb_detect_fired;
	int wheel_pending;            /* queued steps: >0 one way, <0 the other */
	uint64_t wheel_next;
	int wheel_phase;              /* 0 idle, 1 pin asserted */
	int wheel_dir;

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

	/* Emulated RIM serial loader (replaces the loader that lives in flash
	 * on a real device; see the loader section). */
	int ldr_active;
	int reset_pending;
	uint8_t ldr_buf[0x12000];
	int ldr_len, ldr_step, ldr_cmd, ldr_need;
	uint32_t ldr_addr;
	uint32_t ldr_wlen;

	/* debug: instruction trace ring */
	int trace;
	uint32_t ring[1 << 20];
	unsigned ringpos;
	int stop;
	uint32_t stop_at;
	uint32_t bp[16];
	int nbp;
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
	if (getenv("PP_BP")) {
		char *p = getenv("PP_BP");
		while (*p && b->nbp < 16) {
			b->bp[b->nbp++] = strtoul(p, &p, 16);
			while (*p == ',') p++;
		}
	}
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

/* 16450-style UART. The OS uses COM2 (0x2F8, IRQ 3) for the cradle/loader;
 * COM1 (0x3F8, IRQ 4) is modelled the same way. */

static void place_loader_stub(PPBoard *b);

static void uart_update_irq(PPBoard *b, int n)
{
	UART *u = &b->uart[n];
	int pending = ((u->ier & 1) && u->rbr_full) ||
		      ((u->ier & 2) && u->thre_pending) ||
		      ((u->ier & 8) && u->msr_delta);
	if (pending != u->irq_level) {
		u->irq_level = pending;
		i8259_set_irq(b->pic, n == 0 ? 4 : 3, pending);
	}
}

static uint64_t uart_byte_instr(UART *u)
{
	unsigned div = u->dll | u->dlm << 8;
	uint64_t baud = div ? 1828571u / (16u * div) : 115200;
	if (baud == 0) baud = 1;
	return (uint64_t)IPS_PER_US * 10000000u / baud;
}

static void uart_log_tx(PPBoard *b, int n, uint8_t c)
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

static void uart_tick(PPBoard *b)
{
	for (int n = 0; n < 2; n++) {
		UART *u = &b->uart[n];
		if (!u->rbr_full && u->rh != u->rt && b->instr >= u->next_rx) {
			u->rbr = u->rx[u->rt];
			u->hs_window = (u->hs_window << 8) | u->rbr;
			if (n == 1 && u->hs_window == 0x0EAA0FF0u)
				place_loader_stub(b);   /* the OS may have reused that flash */
			u->rt = (u->rt + 1) % UART_FIFO;
			u->rbr_full = 1;
			u->next_rx = b->instr + uart_byte_instr(u);
		}
		if (u->tx_busy && b->instr >= u->tx_done) {
			u->tx_busy = 0;
			u->thre_pending = 1;
		}
		uart_update_irq(b, n);
	}
}

/* returns 1 if handled */
static int uart_decode(int port, int *n, int *r)
{
	if (port >= 0xF4F8 && port <= 0xF4FF) { *n = 0; *r = port - 0xF4F8; }
	else if (port >= 0xF8F8 && port <= 0xF8FF) { *n = 1; *r = port - 0xF8F8; }
	else if (port >= 0x3F8 && port <= 0x3FF) { *n = 0; *r = port - 0x3F8; }
	else if (port >= 0x2F8 && port <= 0x2FF) { *n = 1; *r = port - 0x2F8; }
	else return 0;
	return 1;
}

static int sio_read(PPBoard *b, int port, uint8_t *v)
{
	int n, r;
	if (!uart_decode(port, &n, &r))
		return 0;
	UART *u = &b->uart[n];
	int dlab = u->lcr & 0x80;
	static int ulog = -1;
	if (ulog < 0) ulog = getenv("PP_UARTLOG") != NULL;
	if (ulog && !(r == 5 && !u->rbr_full)) pp_log("uart%d rd r%d (rbr_full %d ier %x) @eip %08x", n, r, u->rbr_full, u->ier, cur_eip(b));
	switch (r) {
	case 0:
		if (dlab) { *v = u->dll; break; }
		*v = u->rbr;
		u->rbr_full = 0;
		break;
	case 1: *v = dlab ? u->dlm : u->ier; break;
	case 2:
		if (u->rbr_full && (u->ier & 1)) *v = 0x04;
		else if (u->thre_pending && (u->ier & 2)) { *v = 0x02; u->thre_pending = 0; }
		else if (u->msr_delta && (u->ier & 8)) *v = 0x00;
		else *v = 0x01;
		break;
	case 3: *v = u->lcr; break;
	case 4: *v = u->mcr; break;
	case 5: *v = (u->rbr_full ? 1 : 0) | (u->tx_busy ? 0 : 0x60); break;
	case 6:
		if (u->mcr & 0x10)  /* loopback */
			*v = ((u->mcr & 2) << 3) | ((u->mcr & 1) << 5) | ((u->mcr & 4) << 4) | ((u->mcr & 8) << 4);
		else
			*v = u->msr | u->msr_delta;
		u->msr_delta = 0;
		break;
	case 7: *v = u->scr; break;
	}
	uart_update_irq(b, n);
	return 1;
}

static int sio_write(PPBoard *b, int port, uint8_t v)
{
	int n, r;
	if (!uart_decode(port, &n, &r))
		return 0;
	UART *u = &b->uart[n];
	int dlab = u->lcr & 0x80;
	if (getenv("PP_UARTLOG")) pp_log("uart%d wr r%d = %02x @eip %08x", n, r, v, cur_eip(b));
	switch (r) {
	case 0:
		if (dlab) { u->dll = v; break; }
		if (u->mcr & 0x10) {            /* loopback */
			u->rbr = v; u->rbr_full = 1;
		} else if (b->tx_fn[n]) {
			b->tx_fn[n](b->tx_ctx[n], v);
		} else {
			uart_log_tx(b, n, v);
		}
		u->thre_pending = 0;
		u->tx_busy = 1;
		u->tx_done = b->instr + uart_byte_instr(u);
		break;
	case 1:
		if (dlab) { u->dlm = v; break; }
		if ((v & 2) && !(u->ier & 2) && !u->tx_busy)
			u->thre_pending = 1;
		u->ier = v & 0x0F;
		break;
	case 2: break;                          /* FCR: no FIFO */
	case 3: u->lcr = v; break;
	case 4: u->mcr = v; break;
	case 7: u->scr = v; break;
	}
	uart_update_irq(b, n);
	return 1;
}

void pp_serial_set_host(PPBoard *b, int n, void (*tx)(void *, uint8_t), void *ctx)
{
	b->tx_fn[n & 1] = tx;
	b->tx_ctx[n & 1] = ctx;
}

int pp_serial_rx(PPBoard *b, int n, const uint8_t *data, int len)
{
	UART *u = &b->uart[n & 1];
	int i;
	for (i = 0; i < len; i++) {
		int nh = (u->rh + 1) % UART_FIFO;
		if (nh == u->rt)
			break;
		u->rx[u->rh] = data[i];
		u->rh = nh;
	}
	return i;
}

int pp_serial_rx_space(PPBoard *b, int n)
{
	UART *u = &b->uart[n & 1];
	return UART_FIFO - 1 - (u->rh - u->rt + UART_FIFO) % UART_FIFO;
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

/* ------------------------------------------------------------ keyboard */

static void irq_pulse(PPBoard *b, int irq)
{
	i8259_set_irq(b->pic, irq, 1);
	i8259_set_irq(b->pic, irq, 0);
}

static int kb_any_pressed(PPBoard *b)
{
	for (int c = 0; c < 8; c++) if (b->kb_rows[c]) return 1;
	return 0;
}

static uint8_t kb_read_rows(PPBoard *b)
{
	uint8_t sel = ~b->kb_colsel, rows = 0;
	for (int c = 0; c < 8; c++)
		if (sel & (1 << c)) rows |= b->kb_rows[c];
	return ~rows;
}

static void kb_tick(PPBoard *b)
{
	if (b->kb_ctl & 0x100) {
		/* idle: interrupt when a key goes down */
		if (kb_any_pressed(b)) {
			if (!b->kb_detect_fired) { irq_pulse(b, 1); b->kb_detect_fired = 1; }
		} else {
			b->kb_detect_fired = 0;
		}
	} else if (b->kb_ctl & 1) {
		/* scanning: periodic interrupt, one column per tick */
		if (b->instr >= b->kb_next) {
			b->kb_next = b->instr + 2000 * IPS_PER_US;
			irq_pulse(b, 1);
		}
	}
	/* trackwheel: assert the phase pin, then interrupt */
	if (b->wheel_phase && b->instr >= b->wheel_next) {
		b->wheel_phase = 0;                      /* release pin */
		b->wheel_next = b->instr + 4000 * IPS_PER_US;
	} else if (!b->wheel_phase && b->wheel_pending && b->instr >= b->wheel_next) {
		b->wheel_dir = b->wheel_pending > 0 ? 1 : -1;
		b->wheel_pending -= b->wheel_dir;
		b->wheel_phase = 1;
		b->wheel_next = b->instr + 1000 * IPS_PER_US;
		irq_pulse(b, b->wheel_dir > 0 ? 13 : 12);
	}
}

static void kb_find_keymap(PPBoard *b)
{
	static const uint8_t qwert[] = { 'Q', 0, 'W', 0, 'E', 0, 'R', 0, 'T', 0 };
	memset(b->keymap, 0, sizeof b->keymap);
	for (size_t i = 0; i + 128 < OS_SIZE; i += 2) {
		if (!memcmp(b->os + i, qwert, sizeof qwert)) {
			for (int k = 0; k < 64; k++)
				b->keymap[k] = b->os[i + 2 * k] | b->os[i + 2 * k + 1] << 8;
			return;
		}
	}
}

/* Press or release the matrix key that produces OS key code `code`. */
static int kb_set_code(PPBoard *b, uint16_t code, int down)
{
	for (int k = 0; k < 64; k++) {
		if (b->keymap[k] == code) {
			int row = k >> 3, col = k & 7;
			if (down) b->kb_rows[col] |= 1 << row;
			else b->kb_rows[col] &= ~(1 << row);
			return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------ LCD */

static void lcd_command(PPBoard *b, uint8_t c)
{
	static int lg = -1;
	if (lg < 0) lg = getenv("PP_LCDLOG") != NULL;
	if (lg && c != 0x5C && c != 0xBE) pp_log("lcd cmd %02x", c);
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
	static int lg = -1;
	if (lg < 0) lg = getenv("PP_LCDLOG") != NULL;
	if (lg && v) pp_log("lcd data %02x cmd %02x row %d col %d", v, b->lcd_cmd, b->lcd_row, b->lcd_col);
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
	case 0x380: return 0x62;                /* LCD controller status/ID; bit 7 = busy */
	case 0xFC30: return b->kb_colsel;
	case 0xFC34: return kb_read_rows(b);
	case 0xF863: return (b->ioreg[0xF863] & ~5) | b->dsp_pwdack |
			    ((b->wheel_phase && b->wheel_dir > 0) ? 4 : 0);  /* DSP PWDACK, wheel */
	case 0xF867: return (b->ioreg[0xF867] & ~0x40) |
			    ((b->wheel_phase && b->wheel_dir < 0) ? 0x40 : 0); /* wheel */
	case 0xF877: return b->ioreg[0xF877] | 0x08;    /* holster sensor: out of holster */
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
	case 0xFC30: b->kb_colsel = v; return;
	case 0xFC38: b->kb_ctl = (b->kb_ctl & 0xFF00) | v; b->kb_next = b->instr + 2000 * IPS_PER_US; return;
	case 0xFC39: b->kb_ctl = (b->kb_ctl & 0x00FF) | v << 8; b->kb_detect_fired = 0; return;
	case 0x382: lcd_data(b, v); return;
	case 0x70: b->rtc_idx = v; return;
	case 0x71: rtc_write(b, v); return;
	case 0x92:
		if (v & 1) {
			pp_log("port 92 reset requested");
			if (b->trace) dump_ring(b, "reset");
			else b->reset_pending = 1;
		}
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
	return a >= FLASH_BASE && a < FLASH_BASE + FLASH_SIZE;
}
static uint32_t flash_off(uint32_t a) { return a - FLASH_BASE; }

/* ---- AMD-style x16 flash (file system chip) ---- */

enum { AMD_ARRAY, AMD_ID, AMD_CFI };

static int in_fs(uint32_t a) { return a >= FS_BASE && a < FS_BASE + FS_SIZE; }

static void amd_set_mode(PPBoard *b, int mode)
{
	b->amd_mode = mode;
	memset(pp_iomem_r + (FS_BASE >> 16), mode != AMD_ARRAY, FS_SIZE >> 16);
}

static uint16_t amd_read16(PPBoard *b, uint32_t off)
{
	uint32_t w = (off >> 1) & 0xFF;
	if (b->amd_mode == AMD_ID) {
		if (w == 0) return 0x0001;      /* AMD */
		if (w == 1) return 0x22F6;      /* Am29LV320DT-style ID */
		return 0;
	}
	if (b->amd_mode == AMD_CFI) {
		static const uint8_t cfi[] = {
			[0x10] = 'Q', 'R', 'Y', 0x02, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
			[0x1B] = 0x27, 0x36, 0x00, 0x00, 0x04, 0x00, 0x0A, 0x00, 0x05, 0x00, 0x04, 0x00,
			[0x27] = 0x16, 0x02, 0x00, 0x00, 0x00, 0x01,
			[0x2D] = 0x3F, 0x00, 0x00, 0x01,     /* 64 x 64 KB */
			[0x40] = 'P', 'R', 'I', '1', '0',
		};
		return w < sizeof(cfi) ? cfi[w] : 0;
	}
	return b->mem[FS_BASE + off] | b->mem[FS_BASE + off + 1] << 8;
}

static void amd_write(PPBoard *b, uint32_t off, uint16_t v, int size)
{
	uint32_t w = (off >> 1) & 0x7FF;        /* word address bits for unlock */
	uint8_t c = v & 0xFF;
	if (getenv("PP_FLASHLOG"))
		pp_log("fsflash w%d %08x = %04x (step %d) @eip %08x", size, FS_BASE + off, v, b->amd_step, cur_eip(b));
	if (b->amd_step == 100) {               /* program data */
		uint32_t a = FS_BASE + (off & ~1u);
		if (size == 1) b->mem[FS_BASE + off] &= c;
		else { b->mem[a] &= v & 0xFF; b->mem[a + 1] &= v >> 8; }
		b->amd_step = 0;
		b->fl_dirty = 1;
		return;
	}
	if (c == 0xF0 || c == 0xFF) { amd_set_mode(b, AMD_ARRAY); b->amd_step = 0; return; }
	if (c == 0x98 && w == 0x55) { amd_set_mode(b, AMD_CFI); b->amd_step = 0; return; }
	switch (b->amd_step) {
	case 0: case 3:
		if (c == 0xAA && w == 0x555) b->amd_step++;
		else b->amd_step = 0;
		break;
	case 1: case 4:
		if (c == 0x55 && w == 0x2AA) b->amd_step++;
		else b->amd_step = 0;
		break;
	case 2:
		b->amd_step = 0;
		if (w != 0x555) break;
		if (c == 0x90) amd_set_mode(b, AMD_ID);
		else if (c == 0xA0) b->amd_step = 100;
		else if (c == 0x80) b->amd_step = 3;
		break;
	case 5:
		b->amd_step = 0;
		if (c == 0x30) {                        /* sector erase, 64 KB */
			memset(b->mem + FS_BASE + (off & ~0xFFFFu), 0xFF, 0x10000);
			b->fl_dirty = 1;
		} else if (c == 0x10) {                 /* chip erase */
			memset(b->mem + FS_BASE, 0xFF, FS_SIZE);
			b->fl_dirty = 1;
		}
		break;
	}
}

/* ------------------------------------------------------------ MMIO */

/* ------------------------------------------------------------ radio DSP */

#define DSP_BASE 0x02A00000u
#define DSP_DATA 0x02A10000u
#define DSP_Q_FIRST 0x2080
#define DSP_Q_END   0x20FD   /* one past the last slot; also the status word */
#define DSP_Q_WR    0x20FE   /* DSP -> host write index */
#define DSP_Q_RD    0x20FF   /* host read index */

static int dsp_log_on(void)
{
	static int on = -1;
	if (on < 0) on = getenv("PP_DSPLOG") != NULL;
	return on;
}

static uint16_t dsp_q_next(uint16_t i)
{
	i++;
	return i >= DSP_Q_END ? DSP_Q_FIRST : i;
}

/* Post a message from the (pretend) DSP to the host. Queue entries are
 * [length][length data words]; DM[0x20FE] is the DSP's write index and
 * DM[0x20FF] the host's read index. */
static void dsp_post(PPBoard *b, const uint16_t *w, int n)
{
	uint16_t wr = b->dsp_dm[DSP_Q_WR];
	if (wr < DSP_Q_FIRST || wr >= DSP_Q_END)
		wr = b->dsp_dm[DSP_Q_WR] = b->dsp_dm[DSP_Q_RD] = DSP_Q_FIRST;
	b->dsp_dm[wr] = (uint16_t)n;
	wr = dsp_q_next(wr);
	for (int i = 0; i < n; i++) {
		b->dsp_dm[wr] = w[i];
		wr = dsp_q_next(wr);
	}
	b->dsp_dm[DSP_Q_WR] = wr;
	b->dsp_dm[DSP_Q_END] = 0;
	if (dsp_log_on()) pp_log("dsp: post %d words, first %04x", n, n ? w[0] : 0);
}

/* Host -> DSP command queue: DM 0x2000-0x207C, [len][cmd][params...],
 * host write index DM 0x207E, DSP read index DM 0x207F. */
#define DSP_C_FIRST 0x2000
#define DSP_C_END   0x207D
#define DSP_C_WR    0x207E
#define DSP_C_RD    0x207F

static uint16_t dsp_c_next(uint16_t i)
{
	i++;
	return i >= DSP_C_END ? DSP_C_FIRST : i;
}

/* Fake DSP firmware: acknowledge every command by echoing its ID. */
static void dsp_process_commands(PPBoard *b)
{
	int guard = 64;
	while (b->dsp_dm[DSP_C_RD] != b->dsp_dm[DSP_C_WR] && guard--) {
		uint16_t rd = b->dsp_dm[DSP_C_RD];
		if (rd < DSP_C_FIRST || rd >= DSP_C_END) {
			b->dsp_dm[DSP_C_RD] = b->dsp_dm[DSP_C_WR];
			break;
		}
		uint16_t len = b->dsp_dm[rd];
		uint16_t p = dsp_c_next(rd);
		uint16_t cmd = b->dsp_dm[p];
		if (dsp_log_on()) {
			char buf[160]; int o = 0;
			uint16_t q = p;
			for (int i = 0; i < len && i < 12; i++) { o += snprintf(buf + o, sizeof buf - o, " %04x", b->dsp_dm[q]); q = dsp_c_next(q); }
			pp_log("dsp: command len %d:%s", len, buf);
		}
		for (int i = 0; i <= len; i++)
			rd = dsp_c_next(rd);
		b->dsp_dm[DSP_C_RD] = rd;
		if (cmd == 2) {
			/* power down: no reply, acknowledge on the PWDACK pin */
			b->dsp_pwdack = 1;
			continue;
		}
		b->dsp_pwdack = 0;
		uint16_t reply[1] = { cmd };
		dsp_post(b, reply, 1);
	}
}

static void dsp_tick(PPBoard *b)
{
	int level = b->dsp_running && b->dsp_dm[DSP_Q_WR] != b->dsp_dm[DSP_Q_RD];
	if (level != b->dsp_irq) {
		b->dsp_irq = level;
		i8259_set_irq(b->pic, 5, level);
	}
}

/* The host starts the DSP by writing the entry point to PM 0. */
static void dsp_started(PPBoard *b)
{
	b->dsp_running = 1;
	b->dsp_pwdack = 0;
	if (dsp_log_on()) pp_log("dsp: started, entry %06x @eip %08x", b->dsp_pm[0], cur_eip(b));
	static const uint16_t alive[] = { 1 };
	dsp_post(b, alive, 1);  /* "boot complete" */
}

static uint16_t dsp_read16(PPBoard *b, uint32_t a)
{
	if (a < DSP_DATA)
		return b->dsp_addr;
	uint16_t addr = b->dsp_addr & 0x3FFF;
	uint16_t v;
	if (b->dsp_addr & 0x4000) {
		v = b->dsp_dm[addr];
		if (dsp_log_on() && b->dsp_running && addr >= 0x2000) pp_log("dsp: host read DM %04x = %04x @eip %08x", addr, v, cur_eip(b));
		b->dsp_addr = (b->dsp_addr & 0x4000) | ((addr + 1) & 0x3FFF);
	} else {
		if (!b->dsp_pm_half) {
			v = b->dsp_pm[addr] >> 8;
			b->dsp_pm_half = 1;
		} else {
			v = b->dsp_pm[addr] & 0xFF;
			b->dsp_pm_half = 0;
			b->dsp_addr = (addr + 1) & 0x3FFF;
		}
	}
	return v;
}

static void dsp_write16(PPBoard *b, uint32_t a, uint16_t v)
{
	if (a < DSP_DATA) {
		b->dsp_addr = v & 0x7FFF;
		b->dsp_pm_half = 0;
		return;
	}
	uint16_t addr = b->dsp_addr & 0x3FFF;
	if (b->dsp_addr & 0x4000) {
		b->dsp_dm[addr] = v;
		b->dsp_addr = 0x4000 | ((addr + 1) & 0x3FFF);
		if (addr == DSP_C_WR && b->dsp_running)
			dsp_process_commands(b);
		if (dsp_log_on() && addr >= 0x2000) pp_log("dsp: host wrote DM %04x = %04x @eip %08x", addr, v, cur_eip(b));
	} else {
		if (!b->dsp_pm_half) {
			b->dsp_pm[addr] = (b->dsp_pm[addr] & 0xFF) | (uint32_t)v << 8;
			b->dsp_pm_half = 1;
		} else {
			b->dsp_pm[addr] = (b->dsp_pm[addr] & 0xFFFF00) | (v & 0xFF);
			b->dsp_pm_half = 0;
			b->dsp_addr = (addr + 1) & 0x3FFF;
			if (addr == 0)
				dsp_started(b);
		}
	}
}

static int in_dsp(uint32_t a) { return (a >> 17) == (DSP_BASE >> 17); }

static uint8_t mm_read8(void *o, uint32_t a)
{
	PPBoard *b = o;
	a &= ADDR_MASK;
	if (in_dsp(a)) {
		uint16_t w = dsp_read16(b, a & ~1u);
		return (a & 1) ? w >> 8 : w & 0xFF;
	}
	if (in_flash(a)) {
		uint16_t w = fl_read16(b, flash_off(a) & ~1u);
		return (a & 1) ? w >> 8 : w & 0xFF;
	}
	if (in_fs(a)) {
		uint16_t w = amd_read16(b, (a - FS_BASE) & ~1u);
		return (a & 1) ? w >> 8 : w & 0xFF;
	}
	return b->mem[a];
}
static void mm_write8(void *o, uint32_t a, uint8_t v)
{
	PPBoard *b = o;
	a &= ADDR_MASK;
	if (in_dsp(a)) {
		dsp_write16(b, a & ~1u, v);
		return;
	}
	if (in_flash(a)) {
		fl_write16(b, flash_off(a), v, 1);
		return;
	}
	if (in_fs(a)) {
		amd_write(b, a - FS_BASE, v, 1);
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
	if (in_fs(a) && !(a & 1))
		return amd_read16(b, a - FS_BASE);
	if (in_dsp(a))
		return dsp_read16(b, a & ~1u);
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
	if (in_fs(a) && !(a & 1)) {
		amd_write(b, a - FS_BASE, v, 2);
		return;
	}
	if (in_dsp(a)) {
		dsp_write16(b, a & ~1u, v);
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

/* ------------------------------------------------------------ serial loader */

/* RIM's Programmer talks to a small loader on the device. Commands arrive
 * framed as 01 <cmd> 04; the loader answers 06 (ACK) / 15 (NAK). Most
 * commands then exchange fixed-size binary arguments. Data blocks carry a
 * CRC-16/KERMIT. Programmer does all the clever work (layout, relocation,
 * import tables); the loader only reads, writes and erases flash. */

static int ldr_log_on(void)
{
	static int on = -1;
	if (on < 0) on = getenv("PP_LDRLOG") != NULL;
	return on;
}

static uint16_t crc16_kermit(const uint8_t *p, size_t n, uint16_t crc)
{
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}

static uint32_t crc32_std(const uint8_t *p, size_t n, uint32_t crc)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
	}
	return ~crc;
}

static void ldr_tx(PPBoard *b, const uint8_t *d, int n)
{
	if (ldr_log_on()) {
		char hex[100]; int o = 0;
		for (int i = 0; i < n && i < 32; i++) o += snprintf(hex + o, sizeof hex - o, "%02x", d[i]);
		pp_log("ldr> %s%s (%d)", hex, n > 32 ? ".." : "", n);
	}
	for (int i = 0; i < n; i++)
		if (b->tx_fn[1]) b->tx_fn[1](b->tx_ctx[1], d[i]);
}
static void ldr_tx1(PPBoard *b, uint8_t c) { ldr_tx(b, &c, 1); }

/* Map a loader address (the device's linear view) to emulator memory. */
static uint8_t *ldr_ptr(PPBoard *b, uint32_t a)
{
	if (a >= 0x03800000u && a < 0x03C00000u)
		return b->mem + FS_BASE + (a - 0x03800000u);
	return b->mem + (a & ADDR_MASK);
}

static int ldr_is_flash(uint32_t a)
{
	return a >= 0x03800000u && a < 0x04000000u;
}

static void ldr_erase(PPBoard *b, uint32_t a)
{
	uint32_t start, len;
	if (a >= 0x03800000u && a < 0x03C00000u) {          /* FS chip: 64 KB */
		start = a & ~0xFFFFu; len = 0x10000;
	} else {
		fl_block(a - FLASH_BASE, &start, &len);
		start += FLASH_BASE;
	}
	if (ldr_log_on()) pp_log("ldr: erase %08x (+%x)", start, len);
	memset(ldr_ptr(b, start), 0xFF, len);
	b->fl_dirty = 1;
}

static void ldr_hw_descriptor(PPBoard *b, uint8_t out[62])
{
	/* 'WHMR' record: 4 bytes of hardware IDs and the device name, taken from
	 * the OS image so Programmer accepts it as the right device. */
	memset(out, 0, 62);
	memcpy(out, "WHMR", 4);
	const uint8_t *os = b->os;
	for (int i = 0; i + 40 < OS_SIZE; i++) {
		if (!memcmp(os + i, "WHMR", 4) && os[i + 8] == 'R') {
			memcpy(out + 4, os + i + 4, 4);
			size_t n = strnlen((const char *)os + i + 8, 47);
			memcpy(out + 8, os + i + 8, n);
			break;
		}
	}
	memcpy(out + 56, "WHMR", 4);
}

enum { LS_IDLE, LS_ARGS };

static void ldr_command(PPBoard *b, int c)
{
	b->ldr_cmd = c;
	if (ldr_log_on()) pp_log("ldr: command '%c' (%02x)", c >= 32 ? c : '.', c);
	uint8_t buf[64];
	switch (c) {
	case 'B': ldr_tx1(b, 0x04); b->ldr_need = 4; b->ldr_step = LS_ARGS; return;
	case 'U': ldr_tx1(b, 0x15); return;                     /* no scrambling */
	case 'v': ldr_tx1(b, 0x06); b->ldr_need = 8; b->ldr_step = LS_ARGS; return;
	case 'p': ldr_tx1(b, 0x06); b->ldr_need = 20; b->ldr_step = LS_ARGS; return;
	case 'H':
		ldr_tx1(b, 0x06);
		ldr_hw_descriptor(b, buf);
		ldr_tx(b, buf, 62);
		return;
	case 'I': ldr_tx1(b, 0x06); buf[0] = FLASH_ID_DEV & 0xFF; buf[1] = FLASH_ID_DEV >> 8; ldr_tx(b, buf, 2); return;  /* flash device ID */
	case 'r': ldr_tx1(b, 0x06); memset(buf, 0, 4); ldr_tx(b, buf, 4); return;
	case 'o': ldr_tx1(b, 0x06); memset(buf, 0, 8); ldr_tx(b, buf, 8); return;
	case 'h': ldr_tx1(b, 0x06); buf[0] = 0; ldr_tx(b, buf, 1); return;
	case 'n': ldr_tx1(b, 0x06); ldr_tx1(b, 0x06); return;
	case 'z': ldr_tx1(b, 0x06); ldr_tx1(b, 0x06); return;
	case 'R': case 'V': case 'T': case 'P':
		ldr_tx1(b, 0x06); b->ldr_need = (c == 'R') ? 6 : 8; b->ldr_step = LS_ARGS; return;
	case 'C': ldr_tx1(b, 0x06); b->ldr_need = 4; b->ldr_step = LS_ARGS; return;
	case 'S': ldr_tx1(b, 0x06); b->ldr_need = 2; b->ldr_step = LS_ARGS; return;
	case 'W': ldr_tx1(b, 0x06); b->ldr_need = 6; b->ldr_step = LS_ARGS; b->ldr_wlen = 0; return;
	case 'X':
		ldr_tx1(b, 0x06);
		pp_log("loader: Programmer disconnected, rebooting");
		b->ldr_active = 2;                              /* reboot after this tick */
		return;
	}
	pp_log("loader: unknown command %02x", c);
	ldr_tx1(b, 0x15);
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void ldr_args(PPBoard *b, const uint8_t *a)
{
	uint8_t out[8];
	switch (b->ldr_cmd) {
	case 'B': ldr_tx1(b, rd32le(a) ? 0x06 : 0x15); break;
	case 'v': case 'p': case 'S': ldr_tx1(b, 0x06); break;
	case 'R': {
		uint32_t addr = rd32le(a);
		uint16_t len = a[4] | a[5] << 8;
		if (ldr_log_on()) pp_log("ldr: read %08x +%x", addr, len);
		uint8_t *p = ldr_ptr(b, addr);
		ldr_tx1(b, 0x06);
		ldr_tx(b, p, len);
		uint16_t c = crc16_kermit(p, len, 0);
		out[0] = c; out[1] = c >> 8;
		ldr_tx(b, out, 2);
		break;
	}
	case 'V': case 'P': {
		uint32_t addr = rd32le(a), len = rd32le(a + 4);
		uint32_t c = crc32_std(ldr_ptr(b, addr), len, 0);
		if (ldr_log_on()) pp_log("ldr: %c %08x +%x -> %08x", b->ldr_cmd, addr, len, c);
		ldr_tx1(b, 0x06);
		out[0] = c; out[1] = c >> 8; out[2] = c >> 16; out[3] = c >> 24;
		ldr_tx(b, out, 4);
		break;
	}
	case 'T': {
		uint32_t addr = rd32le(a), len = rd32le(a + 4);
		const uint8_t *p = ldr_ptr(b, addr);
		int blank = 1;
		for (uint32_t i = 0; i < len; i++) if (p[i] != 0xFF) { blank = 0; break; }
		if (ldr_log_on()) pp_log("ldr: test %08x +%x blank=%d", addr, len, blank);
		ldr_tx1(b, blank ? 0x06 : 0x15);
		break;
	}
	case 'C': ldr_erase(b, rd32le(a)); ldr_tx1(b, 0x06); break;
	}
}

static void ldr_write_block(PPBoard *b, const uint8_t *hdr, const uint8_t *data, uint32_t len, const uint8_t *crc)
{
	uint32_t addr = rd32le(hdr);
	uint16_t want = crc[0] | crc[1] << 8;
	uint16_t got = crc16_kermit(hdr, 6, 0);
	got = crc16_kermit(data, len, got);
	if (ldr_log_on()) pp_log("ldr: write %08x +%x crc %04x/%04x", addr, len, want, got);
	if (want != got) {
		/* try data-only CRC before rejecting */
		if (crc16_kermit(data, len, 0) != want) {
			ldr_tx1(b, 0x15);
			return;
		}
	}
	uint8_t *p = ldr_ptr(b, addr);
	if (ldr_is_flash(addr)) {
		for (uint32_t i = 0; i < len; i++) p[i] &= data[i];
		b->fl_dirty = 1;
	} else {
		memcpy(p, data, len);
	}
	ldr_tx1(b, 0x06);
}

/* Feed bytes the host sent while the loader is in control. */
static void ldr_process(PPBoard *b)
{
	for (;;) {
		uint8_t *q = b->ldr_buf;
		int n = b->ldr_len, used = 0;
		if (b->ldr_step == LS_IDLE) {
			if (n < 1) return;
			if (q[0] == 0x0E) {
				if (n < 4) return;
				if (q[1] == 0xAA && q[2] == 0x0F && q[3] == 0xF0) ldr_tx1(b, 0x06);
				used = 4;
			} else if (q[0] == 0x01) {
				if (n < 3) return;
				used = 3;
				ldr_command(b, q[1]);
			} else {
				used = 1;
			}
		} else if (b->ldr_cmd == 'W') {
			if (n < 6) return;
			uint32_t len = q[4] | q[5] << 8;
			if ((uint32_t)n < 6 + len + 2) return;
			ldr_write_block(b, q, q + 6, len, q + 6 + len);
			used = 6 + len + 2;
			b->ldr_step = LS_IDLE;
		} else {
			if (n < b->ldr_need) return;
			used = b->ldr_need;
			b->ldr_step = LS_IDLE;
			ldr_args(b, q);
		}
		memmove(b->ldr_buf, b->ldr_buf + used, n - used);
		b->ldr_len = n - used;
	}
}

static void ldr_tick(PPBoard *b)
{
	UART *u = &b->uart[1];
	while (u->rh != u->rt && b->ldr_len < (int)sizeof(b->ldr_buf)) {
		uint8_t c = u->rx[u->rt];
		u->rt = (u->rt + 1) % UART_FIFO;
		if (ldr_log_on() && b->ldr_len < 0) pp_log("host> %02x", c);
		b->ldr_buf[b->ldr_len++] = c;
	}
	ldr_process(b);
}

static void ldr_check_entry(PPBoard *b)
{
	if (b->ldr_active || !cpui386_is_halted(b->cpu))
		return;
	CPUI386_State st;
	cpui386_get_state(b->cpu, &st);
	if ((st.ip & 0xFFFF) >= 0xFFF4 && (st.ip & 0xFFFF) <= 0xFFF7) {
		pp_log("loader: OS handed over to the serial loader");
		b->ldr_active = 1;
		b->ldr_len = 0;
		b->ldr_step = LS_IDLE;
		/* The OS saw 0E AA 0F F0 before handing over; answer it now. */
		ldr_tx1(b, 0x06);
	}
}

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

	kb_find_keymap(b);
	pp_board_factory(b);
	pp_board_reset(b);
	return b;
}

#define LOADER_STUB_LIN  0x03B0FFF0u   /* where the OS looks for the serial loader */

/* The boot ROM keeps an info block at 0x03FFC020 that Programmer reads:
 * magic AB CD EF "Rim:V", a boot ROM version word at +0x74 and a size in
 * KB at +0xC8. We report version 0x1050, older than every optional
 * Programmer feature, so it sticks to plain read/write/erase commands. */
static void write_boot_info(PPBoard *b)
{
	uint8_t *p = b->mem + 0x03FFC020;
	memset(p, 0, 0xCC);
	static const uint8_t magic[8] = { 0xAB, 0xCD, 0xEF, 'R', 'i', 'm', ':', 'V' };
	memcpy(p, magic, 8);
	p[0x74] = 0x50; p[0x75] = 0x10;
	p[0xC8] = 0x00; p[0xC9] = 0x02;      /* 512 KB (RAM) */
}

/* Erase both flash chips and install the OS image, as after a fresh load. */
void pp_board_factory(PPBoard *b)
{
	memset(b->mem + FLASH_BASE, 0xFF, FLASH_SIZE);
	memset(b->mem + FS_BASE, 0xFF, FS_SIZE);
	memcpy(b->mem + OS_BASE, b->os, OS_SIZE);
	/* Boot block 3 holds the loader's status word: -1 = last load never
	 * finished, -5 = enter loader, 0 = defragment. A finished load leaves
	 * the release ID there; RIM's default release ID is -2. */
	static const uint8_t release_id[4] = { 0xFE, 0xFF, 0xFF, 0xFF };
	memcpy(b->mem + 0x03FF6000, release_id, 4);
	write_boot_info(b);
}

/* Our stand-in for RIM's serial loader: "cli; jmp" where the OS checks for
 * it, then "hlt; jmp $-1" at the entry. The board notices the CPU halting
 * there and takes over the serial port (see loader.c section). */
static void place_loader_stub(PPBoard *b)
{
	static const uint8_t stub[] = { 0xFA, 0xE9, 0xFF, 0xFF, 0xF4, 0xEB, 0xFD, 0x90 };
	uint32_t fs_off = LOADER_STUB_LIN - 0x03800000u;
	memcpy(b->mem + FS_BASE + fs_off, stub, sizeof stub);   /* via the FS chip */
	memcpy(b->mem + LOADER_STUB_LIN, stub, sizeof stub);    /* paging off */
	memcpy(b->mem + FS_BASE + 0xFFF0, stub, sizeof stub);   /* jump target: CS base 0x03000000 */
}

void pp_board_reset(PPBoard *b)
{
	/* RAM and unmapped space are cleared; both flash chips keep their contents. */
	memset(b->mem, 0, FS_BASE);
	memset(b->mem + FS_BASE + FS_SIZE, 0, FLASH_BASE - (FS_BASE + FS_SIZE));
	place_loader_stub(b);
	b->ldr_active = 0;
	b->ldr_len = 0;
	b->ldr_step = 0;
	memset(b->kb_rows, 0, sizeof b->kb_rows);
	b->kb_colsel = 0; b->kb_ctl = 0; b->kb_detect_fired = 0;
	b->wheel_pending = 0; b->wheel_phase = 0;
	memset(b->ioreg, 0, sizeof(b->ioreg));
	memset(b->seen, 0, sizeof(b->seen));
	b->pin[0] = b->pin[1] = b->pin[2] = 0xFF;
	memset(pp_iomem_r, 0, sizeof(pp_iomem_r));
	memset(pp_iomem_w, 0, sizeof(pp_iomem_w));
	memset(pp_iomem_w + (FLASH_BASE >> 16), 1, FLASH_SIZE >> 16);
	memset(pp_iomem_w + (FS_BASE >> 16), 1, FS_SIZE >> 16);
	b->amd_step = 0;
	amd_set_mode(b, AMD_ARRAY);
	memset(pp_iomem_w + (DSP_BASE >> 16), 1, 2);
	memset(pp_iomem_r + (DSP_BASE >> 16), 1, 2);
	memset(b->dsp_dm, 0, sizeof(b->dsp_dm));
	memset(b->dsp_pm, 0, sizeof(b->dsp_pm));
	b->dsp_addr = 0; b->dsp_pm_half = 0; b->dsp_running = 0; b->dsp_irq = 0; b->dsp_pwdack = 0;
	b->fl_cmd = 0;
	b->fl_status = 0x80;
	fl_set_mode_at(b, FL_ARRAY, 0);
	b->instr = 0;
	for (int n = 0; n < 2; n++) {
		UART *u = &b->uart[n];
		int rh = u->rh, rt = u->rt;
		memset(u, 0, sizeof(*u));
		u->rh = rh; u->rt = rt;         /* keep pending host data */
		u->msr = 0xB0;                  /* CTS, DSR, DCD */
		u->dll = 1;
	}
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
		if (b->ldr_active) {
			uint64_t step = 256 * 16;
			b->instr += step;
			ldr_tick(b);
			if (b->ldr_active == 2) {
				pp_board_reset(b);
			}
			continue;
		}
		if (b->trace) {
			for (int i = 0; i < 256 && !b->stop; i++) {
				CPUI386_State st;
				cpui386_get_state(b->cpu, &st);
				b->ring[b->ringpos++ & ((1 << 20) - 1)] = st.ip;
				for (int k = 0; k < b->nbp; k++)
					if (st.ip == b->bp[k])
						pp_log("bp %08x eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x [esp]=%08x",
						       st.ip, st.gpr[0], st.gpr[1], st.gpr[2], st.gpr[3], st.gpr[4],
						       *(uint32_t *)(b->mem + (st.gpr[4] & ADDR_MASK)));
				if (b->stop_at && st.ip == b->stop_at) { pp_log("hit stop address %08x", st.ip); dump_ring(b, "stop"); b->stop = 3; break; }
				cpui386_step(b->cpu, 1);
			}
			b->instr += 256;
			i8254_update_irq(b->pit);
			uart_tick(b);
			dsp_tick(b);
			kb_tick(b);
			ldr_check_entry(b);
			continue;
		}
		if (b->reset_pending) {
			b->reset_pending = 0;
			pp_board_reset(b);
		}
		int chunk = 256;
		cpui386_step(b->cpu, chunk);
		b->instr += chunk;
		i8254_update_irq(b->pit);
		uart_tick(b);
		dsp_tick(b);
		kb_tick(b);
		ldr_check_entry(b);
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
			/* The controller is page-addressed: each "row" the OS sends is
			 * a screen column, each byte 8 pixels down (bit 0 on top). */
			if (b->lcd_on && y < 160 && x < 160)
				bit = (b->lcd_ram[x * 20 + (y >> 3)] >> (y & 7)) & 1;
			argb[y * b->lcd_w + x] = bit ? on : off;
		}
	return changed;
}

void pp_key(PPBoard *b, int key, int down)
{
	uint16_t code;
	switch (key) {
	case PP_KEY_WHEEL_UP:   if (down) b->wheel_pending++; return;
	case PP_KEY_WHEEL_DOWN: if (down) b->wheel_pending--; return;
	case PP_KEY_WHEEL_CLICK: code = 0xFFFF; break;
	case PP_KEY_ESC:   code = 0x1B; break;
	case PP_KEY_ALT:   code = 0x101; break;
	case PP_KEY_SHIFT: code = 0x100; break;
	case PP_KEY_CAP:   code = 0x102; break;   /* left-side shift */
	case PP_KEY_POWER: code = 0x103; break;   /* backlight key */
	default:
		code = (key >= 'a' && key <= 'z') ? key - 32 : key;
		break;
	}
	kb_set_code(b, code, down);
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

void pp_board_get_flash(PPBoard *b, uint8_t *out)
{
	memcpy(out, b->mem + FLASH_BASE, FLASH_SIZE);
	memcpy(out + FLASH_SIZE, b->mem + FS_BASE, FS_SIZE);
	b->fl_dirty = 0;
}

int pp_board_set_flash(PPBoard *b, const uint8_t *in, size_t len)
{
	if (len != PP_FLASH_IMAGE_SIZE)
		return 0;
	memcpy(b->mem + FLASH_BASE, in, FLASH_SIZE);
	memcpy(b->mem + FS_BASE, in + FLASH_SIZE, FS_SIZE);
	b->fl_dirty = 0;
	return 1;
}

int pp_board_flash_dirty(PPBoard *b) { return b->fl_dirty; }
uint8_t *pp_board_mem(PPBoard *b) { return b->mem; }
