/* bbhost: runs the BlackBerry OS for a front end (the Android app).
 *
 * Clock: while the OS boots, time follows the work done (about 25 million
 * instructions a second) and skips ahead whenever every thread waits, so
 * the boot takes as long as the CPU needs and no longer. Once booted the
 * clock follows real time. */
#include "bbhost.h"
#include "vm.h"
#include "natives.h"
#include "bbgfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static BBVM *g_vm;
static int g_booted, g_idleRun;
static uint64_t g_epoch = 1262304000000ull;   /* 2010-01-01 */
static uint64_t g_bootMs, g_skipMs, g_realAtBoot;
static char g_status[256];

static uint64_t real_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static uint64_t now_ms(void *ctx) {
    (void)ctx;
    if (!g_booted) return g_epoch + (uint64_t)g_vm->icount / 25000 + g_skipMs;
    return g_epoch + g_bootMs + (real_ms() - g_realAtBoot);
}
int bbh_uptime(void) { return g_vm ? (int)(now_ms(NULL) - g_epoch) : 0; }

static void host_log(void *ctx, const char *msg) { (void)ctx; fprintf(stderr, "%s\n", msg); }

int bbh_start(const char *dir, char *err, size_t errn) {
    if (g_vm) return 0;
    BBVM *vm = vm_new();
    g_vm = vm;
    vm->host.nowMs = now_ms;
    vm->host.log = host_log;
    vm_add_dir(vm, dir);
    natives_init(vm);
    vm->cObject = vm_class(vm, "net_rim_cldc", "java.lang.Object");
    vm->cString = vm_class(vm, "net_rim_cldc", "java.lang.String");
    vm->cClass = vm_class(vm, "net_rim_cldc", "java.lang.Class");
    vm->cThread = vm_class(vm, "net_rim_cldc", "java.lang.Thread");
    vm->cThrowable = vm_class(vm, "net_rim_cldc", "java.lang.Throwable");
    for (int k = 0; k < vm->nlits; k++) { Obj *o = vm_obj(vm, vm->lits[k].r); if (o) o->cls = vm->cString; }
    if (!vm->cObject || !vm->cString) { snprintf(err, errn, "the OS core module (net_rim_cldc) is missing"); return -1; }
    BBClass *k = vm_class(vm, "net_rim_app_manager", "net.rim.device.api.system.ApplicationManagerApp");
    BBRoutine *r = k ? vm_find_method(vm, k, "main", NULL) : NULL;
    if (!r) { snprintf(err, errn, "the OS application manager is missing"); return -1; }
    Thread *t = vm_new_thread(vm, 0);
    uint32_t args[4] = { 0 };
    if (r->nargs) args[0] = vm_new_array(vm, BT_CLASS, vm->cString, 1, 0);
    vm_push_call(vm, t, r, args, r->nargs);
    snprintf(g_status, sizeof g_status, "booting");
    return 0;
}

int bbh_run(int maxMs) {
    if (!g_vm) return -1;
    if (g_vm->fatal) return -1;
    uint64_t end = real_ms() + (uint64_t)maxMs;
    int idle = 0;
    do {
        int rc = g_vm->fatal ? -1 : vm_run(g_vm, 200000);
        if (rc == -1 || g_vm->fatal) { snprintf(g_status, sizeof g_status, "stopped: %s", g_vm->fatalMsg); return -1; }
        if (rc == 0 || rc == -3) {
            idle = 1;
            if (!g_booted) {
                g_skipMs += 10;
                /* settled: nothing left to do for a while */
                if (++g_idleRun > 50 || g_vm->icount > 160000000ll) {
                    g_bootMs = (uint64_t)g_vm->icount / 25000 + g_skipMs;
                    g_realAtBoot = real_ms();
                    g_booted = 1;
                    snprintf(g_status, sizeof g_status, "ready");
                }
            } else break;   /* let the caller sleep */
        } else if (!g_booted) g_idleRun = 0;
    } while (real_ms() < end);
    return idle ? 1 : 0;
}

int bbh_booted(void) { return g_booted; }
long long bbh_instructions(void) { return g_vm ? g_vm->icount : 0; }
const char *bbh_status(void) { return g_status; }

const uint16_t *bbh_screen(int *w, int *h, int *frame) { return bbgfx_framebuffer(w, h, frame); }

/* ------------------------------------------------------------------ */
/* applications                                                         */

static ref_t app_manager(Thread **lt) {
    BBClass *am = vm_find_class_anywhere(g_vm, "net.rim.device.api.system.ApplicationManager");
    BBRoutine *get = am ? vm_find_method(g_vm, am, "getApplicationManager", "") : NULL;
    *lt = vm_new_thread(g_vm, 0);
    (*lt)->procId = 1;
    uint32_t r[2] = { 0, 0 };
    if (!get || vm_call(g_vm, *lt, get, NULL, 0, r) < 0) return 0;
    return r[0];
}

int bbh_launch(const char *module) {
    if (!g_vm) return -1;
    Thread *lt;
    ref_t m = app_manager(&lt);
    if (!m) return -1;
    BBRoutine *launch = vm_find_method(g_vm, vm_obj(g_vm, m)->cls, "launch", "Ljava.lang.String;");
    uint32_t args[2] = { m, vm_new_string(g_vm, module) };
    return launch ? vm_call(g_vm, lt, launch, args, 2, NULL) : -1;
}

int bbh_pid_of(const char *module) {
    if (!g_vm) return -1;
    for (Thread *x = g_vm->threads; x; x = x->next)
        if (x->state != TS_DEAD && x->nfr) {
            /* the process's first frame is in the module or one of its siblings (name-N) */
            const char *n = x->fr[0].r->mod->name;
            size_t l = strlen(module);
            if (!strncmp(n, module, l) && (n[l] == 0 || (n[l] == '-' && n[l + 1] >= '0' && n[l + 1] <= '9'))) return x->procId;
        }
    return -1;
}

int bbh_foreground(const char *module) {
    int pid = bbh_pid_of(module);
    if (pid < 0) return -1;
    Thread *lt;
    ref_t m = app_manager(&lt);
    if (!m) return -1;
    BBRoutine *rf = vm_find_method(g_vm, vm_obj(g_vm, m)->cls, "requestForeground", "I");
    uint32_t args[2] = { m, (uint32_t)pid };
    if (!rf || vm_call(g_vm, lt, rf, args, 2, NULL) < 0) return -1;
    return pid;
}

/* ------------------------------------------------------------------ */
/* input: keypad (device 2) and trackball (device 27) messages          */

enum { DEV_KEYPAD = 2, DEV_NAV = 27, KEY_DOWN = 513, KEY_REPEAT = 514, KEY_UP = 515, KEY_CHAR = 32768,
       NAV_MOVE = 6913, NAV_CLICK = 6914, NAV_UNCLICK = 6915 };

void bbh_key(int ch, int action, int status) {
    if (!g_vm) return;
    int kc = (ch << 16) | (status & 0xffff), t = bbh_uptime();
    if (action == BBH_PRESS || action == BBH_DOWN) {
        nat_post_message(g_vm, DEV_KEYPAD, KEY_DOWN, ch, kc, t, 0, 0);
        if (ch < 0x1000) nat_post_message(g_vm, DEV_KEYPAD, KEY_CHAR, ch, status, t, 0, 0);
    }
    if (action == BBH_REPEAT) {
        nat_post_message(g_vm, DEV_KEYPAD, KEY_REPEAT, ch, kc, t, 0, 0);
        if (ch < 0x1000) nat_post_message(g_vm, DEV_KEYPAD, KEY_CHAR, ch, status | 0x10, t, 0, 0);
    }
    if (action == BBH_PRESS || action == BBH_UP) nat_post_message(g_vm, DEV_KEYPAD, KEY_UP, ch, kc, t, 0, 0);
}

void bbh_trackball(int dx, int dy) {
    if (!g_vm || (!dx && !dy)) return;
    /* sub: the movement (x low, -y high); data0: status; data1: time */
    nat_post_message(g_vm, DEV_NAV, NAV_MOVE, (-(dy << 16)) | (dx & 0xffff), 0x20000000, bbh_uptime(), 0, 0);
}

void bbh_click(int down) {
    if (!g_vm) return;
    nat_post_message(g_vm, DEV_NAV, down ? NAV_CLICK : NAV_UNCLICK, 0, 0x20000000, bbh_uptime(), 0, 0);
}
