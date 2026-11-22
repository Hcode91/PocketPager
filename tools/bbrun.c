/* bbrun: run a static method of a RIM Java module headlessly.
 *   bbrun -d dir [-d dir...] [-t mask] family class [method] */
#include "../core/bbos/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

int main(int argc, char **argv) {
    BBVM *vm = vm_new();
    int i = 1;
    long long budget = 50000000;
    int idleIters = 0, idleMax = 500;
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
                if (mr->cref < 0) continue;
                char full[512];
                snprintf(full, sizeof full, "%s.%s.%s", m->crefs[mr->cref].pkg, m->crefs[mr->cref].cls, mr->name);
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
        if (rc == -3 || rc == -1) break;
        if (rc == 0) {   /* idle: everyone waiting */
            struct timespec ts = { 0, 2 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            idleIters++;
            if (idleIters > idleMax) break;
        }
    }
    printf("vm_run -> %d, %lld instructions%s%s\n", rc, vm->icount, vm->fatal ? ", fatal: " : "", vm->fatal ? vm->fatalMsg : "");
    for (Thread *x = vm->threads; x; x = x->next) {
        printf("thread %d state %d frames %d\n", x->id, x->state, x->nfr);
        if (x->nfr) vm_backtrace(vm, x);
    }
    return 0;
}
