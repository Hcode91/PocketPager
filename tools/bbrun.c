/* bbrun: run a static method of a RIM Java module headlessly.
 *   bbrun -d dir [-d dir...] [-t mask] family class [method] */
#include "../core/bbos/vm.h"
#include "../core/bbos/natives.h"
#include "../core/bbos/bbgfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

/* Virtual clock: time advances with executed instructions (about 25 M per
 * second, near the device's speed) and jumps ahead while every thread
 * waits, so runs are reproducible and idle waits cost nothing. */
static BBVM *g_vm;
static uint64_t g_idleMs, g_epoch = 1262304000000ull;   /* 2010-01-01 */
static uint64_t virt_now(void *ctx) { (void)ctx; return g_epoch + (uint64_t)g_vm->icount / 25000 + g_idleMs; }

static void save_screen(const char *out) {
    int w, h, fr;
    const uint16_t *fb = bbgfx_framebuffer(&w, &h, &fr);
    FILE *f = fopen(out, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int k = 0; k < w * h; k++) {
            uint16_t c = fb[k];
            unsigned char px[3] = { (unsigned char)((c >> 8) & 0xf8), (unsigned char)((c >> 3) & 0xfc), (unsigned char)(c << 3) };
            fwrite(px, 1, 3, f);
        }
        fclose(f);
    }
    fprintf(stderr, "screen: %d frames, saved %s\n", fr, out);
}

/* Scripted input (BBINPUT): comma-separated "<idle ticks>:<action>" items,
 * run once the launched app is in front. Actions: k<c> types character c,
 * enter, space, esc, click, up/down/left/right (trackball), shot (screenshot
 * to /tmp/bbshot<N>.ppm). */
enum { DEV_KEYPAD = 2, DEV_NAV = 27, KEY_DOWN = 513, KEY_REPEAT = 514, KEY_UP = 515, KEY_CHAR = 32768,
       NAV_MOVE = 6913, NAV_CLICK = 6914, NAV_UNCLICK = 6915 };
static int uptime_ms(void) { return (int)(virt_now(NULL) - g_epoch); }
static void post_key(BBVM *vm, int ch) {
    int kc = ch << 16;
    nat_post_message(vm, DEV_KEYPAD, KEY_DOWN, ch, kc, uptime_ms(), 0, 0);
    nat_post_message(vm, DEV_KEYPAD, KEY_CHAR, ch, 0, uptime_ms(), 0, 0);
    nat_post_message(vm, DEV_KEYPAD, KEY_UP, ch, kc, uptime_ms() + 50, 0, 0);
}
static void post_nav(BBVM *vm, int ev, int dx, int dy) {
    nat_post_message(vm, DEV_NAV, ev, (-(dy << 16)) | (dx & 0xffff), 0x20000000, uptime_ms(), 0, 0);
}
static void run_input(BBVM *vm, const char *act) {
    static int nshot;
    if (!strcmp(act, "enter")) post_key(vm, '\n');
    else if (!strcmp(act, "space")) post_key(vm, ' ');
    else if (!strcmp(act, "esc")) post_key(vm, 27);
    else if (!strcmp(act, "menu")) post_key(vm, 0x1002);
    else if (!strcmp(act, "click")) { post_nav(vm, NAV_CLICK, 0, 0); post_nav(vm, NAV_UNCLICK, 0, 0); }
    else if (!strcmp(act, "up")) post_nav(vm, NAV_MOVE, 0, -1);
    else if (!strcmp(act, "down")) post_nav(vm, NAV_MOVE, 0, 1);
    else if (!strcmp(act, "left")) post_nav(vm, NAV_MOVE, -8, 0);
    else if (!strcmp(act, "right")) post_nav(vm, NAV_MOVE, 8, 0);
    else if (!strcmp(act, "shot")) { char p[64]; snprintf(p, sizeof p, "/tmp/bbshot%d.ppm", nshot++); save_screen(p); }
    else if (act[0] == 'k' && act[1]) post_key(vm, (unsigned char)act[1]);
    fprintf(stderr, "input: %s\n", act);
}

int main(int argc, char **argv) {
    BBVM *vm = vm_new();
    g_vm = vm;
    if (!getenv("BBREALTIME")) vm->host.nowMs = virt_now;
    vm->traceFilter = getenv("BBTRACE");
    int i = 1;
    long long budget = 50000000;
    int idleIters = 0, idleMax = 200000;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) vm_add_dir(vm, argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) vm->trace = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) budget = atoll(argv[++i]);
        i++;
    }
    if (argc - i < 2) { fprintf(stderr, "usage: bbrun -d dir family class [method]\n"); return 1; }
    const char *fam = argv[i], *cls = argv[i + 1], *meth = argc - i > 2 ? argv[i + 2] : "main";
    if (!strcmp(cls, "-dis")) meth = "-dis";
    if (!strcmp(cls, "-natives")) meth = "-natives";
    if (!strcmp(cls, "-xref")) meth = "-xref";
    natives_init(vm);
    vm->cObject = vm_class(vm, "net_rim_cldc", "java.lang.Object");
    vm->cString = vm_class(vm, "net_rim_cldc", "java.lang.String");
    vm->cClass = vm_class(vm, "net_rim_cldc", "java.lang.Class");
    vm->cThread = vm_class(vm, "net_rim_cldc", "java.lang.Thread");
    vm->cThrowable = vm_class(vm, "net_rim_cldc", "java.lang.Throwable");
    for (int k = 0; k < vm->nlits; k++) { Obj *o = vm_obj(vm, vm->lits[k].r); if (o) o->cls = vm->cString; }
    if (!vm->cObject || !vm->cString) { fprintf(stderr, "core classes missing\n"); return 1; }
    if (!strcmp(cls, "-xref")) {
        /* bbrun -d dir any -xref Class.method : find callers in every module */
        const char *want = argc - i > 2 ? argv[i + 2] : "";
        DIR *dir = opendir(vm->dirs[0]);
        struct dirent *e;
        while (dir && (e = readdir(dir))) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".cod")) continue;
            char nm[256]; snprintf(nm, sizeof nm, "%.*s", (int)(l - 4), e->d_name);
            BBModule *m = vm_module(vm, nm);
            if (!m) continue;
            for (int fi = 0; fi < m->nfix; fi++) {
                BBFixup *f = &m->fix[fi];
                if (f->kind != FX_ROUTINE && f->kind != FX_SROUTINE && f->kind != FX_VROUTINE) continue;
                BBMemberRef *mr = &m->mrefs[f->idx];
                char full[512];
                if (mr->cref < 0) {
                    BBClass *lc = mr->lcls < m->ncls ? &m->cls[mr->lcls] : NULL;
                    snprintf(full, sizeof full, "%s.%s", lc ? lc->full : "?", mr->name);
                } else snprintf(full, sizeof full, "%s.%s.%s", m->crefs[mr->cref].pkg, m->crefs[mr->cref].cls, mr->name);
                if (getenv("XREFDBG") && strstr(full, getenv("XREFDBG"))) printf("dbg %s: %s\n", m->name, full);
                if (strcmp(full, want)) continue;
                BBRoutine *best = NULL;
                for (int ri = 0; ri < m->nroutines; ri++) if (m->routines[ri].off <= f->off) best = &m->routines[ri];
                printf("%s @%u in %s.%s\n", m->name, f->off, best && best->cls ? best->cls->full : "?", best ? best->name : "?");
            }
        }
        return 0;
    }
    if (!strcmp(cls, "-natives")) {
        Family *f = vm_family(vm, fam);
        for (int mi = 0; mi < f->n; mi++) {
            BBModule *m = f->mods[mi];
            for (int ri = 0; ri < m->nroutines; ri++) {
                BBRoutine *r = &m->routines[ri];
                uint8_t op = r->size ? m->code[r->off] : 0;
                if (op < 9 || op > 11) continue;
                char sig[400], rs[100];
                vm_sig(vm, m, r->proto, !r->isStatic, sig, sizeof sig);
                vm_sig(vm, m, r->type, 0, rs, sizeof rs);
                printf("%s%s.%s(%s)%s\n", r->isStatic ? "static " : "", r->cls ? r->cls->full : "?", r->name, sig, *rs ? rs : "V");
            }
        }
        return 0;
    }
    if (!strcmp(cls, "-classes")) {
        for (int j = 0; j < 64; j++) {
            char nm[128];
            if (j) snprintf(nm, sizeof nm, "%s-%d", fam, j); else snprintf(nm, sizeof nm, "%s", fam);
            BBModule *m = vm_module(vm, nm);
            if (!m) { if (j) break; continue; }
            for (int c = 0; c < m->ncls; c++) printf("%s %s\n", nm, m->cls[c].full ? m->cls[c].full : m->cls[c].name);
        }
        return 0;
    }
    if (!strcmp(meth, "-dis")) {
        for (int j = i + 1; j < argc; j++) {
            if (!strcmp(argv[j], "-dis")) continue;
            BBClass *c = vm_class(vm, fam, argv[j]);
            if (!c) c = vm_find_class_anywhere(vm, argv[j]);
            if (!c) { printf("no class %s\n", argv[j]); continue; }
            vm_disasm_class(vm, c, stdout);
        }
        return 0;
    }
    BBClass *k = vm_class(vm, fam, cls);
    if (!k) { fprintf(stderr, "class %s not found in %s\n", cls, fam); return 1; }
    BBRoutine *r = vm_find_method(vm, k, meth, NULL);
    if (!r) { fprintf(stderr, "no method %s\n", meth); return 1; }
    Thread *t = vm_new_thread(vm, 0);
    uint32_t args[4] = { 0 };
    if (r->nargs) args[0] = vm_new_array(vm, BT_CLASS, vm->cString, 1, 0);
    vm_push_call(vm, t, r, args, r->nargs);
    int rc = 0;
    long long done = 0;
    for (int iter = 0; iter < 100000 && done < budget && !vm->fatal; iter++) {
        long long before = vm->icount;
        rc = vm_run(vm, 200000);
        done += vm->icount - before;
        {
            /* BBLAUNCH=module: once the boot has settled (BBLAUNCHAT
             * instructions, default 120M), ask the application manager to
             * launch that module, as tapping its icon would. */
            static int launched;
            const char *lm = getenv("BBLAUNCH");
            long long at = getenv("BBLAUNCHAT") ? atoll(getenv("BBLAUNCHAT")) : 120000000;
            if (lm && !launched && (vm->icount >= at || idleIters > 50)) {
                idleIters = 0;
                launched = 1;
                BBClass *am = vm_find_class_anywhere(vm, "net.rim.device.api.system.ApplicationManager");
                BBRoutine *get = am ? vm_find_method(vm, am, "getApplicationManager", "") : NULL;
                Thread *lt = vm_new_thread(vm, 0);
                lt->procId = 1;
                uint32_t r[2] = { 0, 0 };
                if (get && vm_call(vm, lt, get, NULL, 0, r) >= 0 && r[0]) {
                    Obj *o = vm_obj(vm, r[0]);
                    BBRoutine *launch = vm_find_method(vm, o->cls, "launch", "Ljava.lang.String;");
                    uint32_t args[2] = { r[0], vm_new_string(vm, lm) };
                    int rc2 = launch ? vm_call(vm, lt, launch, args, 2, NULL) : -9;
                    fprintf(stderr, "launch %s -> %d\n", lm, rc2);
                } else fprintf(stderr, "launch %s: no application manager\n", lm);
            }
            /* then bring it to the front, as the home screen would */
            static int fronted;
            if (lm && launched && !fronted && idleIters > 50) {
                idleIters = 0;
                fronted = 1;
                int pid = -1;
                for (Thread *x = vm->threads; x; x = x->next)
                    if (x->state != TS_DEAD && x->nfr && !strncmp(x->fr[0].r->mod->name, lm, strlen(lm))) { pid = x->procId; break; }
                BBClass *am = vm_find_class_anywhere(vm, "net.rim.device.api.system.ApplicationManager");
                BBRoutine *get = am ? vm_find_method(vm, am, "getApplicationManager", "") : NULL;
                Thread *lt = vm_new_thread(vm, 0);
                lt->procId = 1;
                uint32_t r[2] = { 0, 0 };
                if (pid >= 0 && get && vm_call(vm, lt, get, NULL, 0, r) >= 0 && r[0]) {
                    BBRoutine *rf = vm_find_method(vm, vm_obj(vm, r[0])->cls, "requestForeground", "I");
                    uint32_t args[2] = { r[0], (uint32_t)pid };
                    int rc2 = rf ? vm_call(vm, lt, rf, args, 2, NULL) : -9;
                    fprintf(stderr, "foreground pid %d -> %d\n", pid, rc2);
                } else fprintf(stderr, "foreground: %s not running\n", lm);
            }
            static const char *inp;
            static int sinceInput;
            if (!inp) inp = getenv("BBINPUT");
            sinceInput++;
            if (inp && *inp && (!lm || fronted)) {
                /* delays count loop passes: about 10 ms of device time each */
                int delay = atoi(inp);
                const char *c = strchr(inp, ':');
                if (c && sinceInput >= delay) {
                    sinceInput = 0;
                    char act[32]; int k = 0;
                    for (c++; *c && *c != ',' && k < 31; c++) act[k++] = *c;
                    act[k] = 0;
                    run_input(vm, act);
                    idleIters = 0;
                    inp = *c == ',' ? c + 1 : c;
                }
            }
        }
        {
            static long long dumpAt = -2;
            if (dumpAt == -2) dumpAt = getenv("BBDUMPAT") ? atoll(getenv("BBDUMPAT")) : -1;
            if (dumpAt >= 0 && vm->icount >= dumpAt) {
                dumpAt = -1;
                fflush(stderr);
                for (Thread *x = vm->threads; x; x = x->next) if (x->state != TS_DEAD) {
                    printf("dump: thread %d pid %d state %d\n", x->id, x->procId, x->state); fflush(stdout);
                    vm_backtrace(vm, x);
                }
            }
        }
        if (rc == -3 || rc == -1) break;
        if (rc == 0) {   /* idle: everyone waiting */
            if (vm->host.nowMs) g_idleMs += 10;
            else { struct timespec ts = { 0, 2 * 1000 * 1000 }; nanosleep(&ts, NULL); }
            idleIters++;
            if (idleIters > idleMax) break;
        }
    }
    save_screen(getenv("BBSHOT") ? getenv("BBSHOT") : "/tmp/bbscreen.ppm");
    fflush(stderr); printf("vm_run -> %d, %lld instructions%s%s\n", rc, vm->icount, vm->fatal ? ", fatal: " : "", vm->fatal ? vm->fatalMsg : "");
    for (Thread *x = vm->threads; x; x = x->next) {
        printf("thread %d state %d frames %d\n", x->id, x->state, x->nfr); fflush(stdout);
        if (x->nfr) vm_backtrace(vm, x);
    }
    return 0;
}
