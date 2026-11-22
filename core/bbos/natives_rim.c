/* PocketPager - BlackBerry OS family: natives for RIM's VM services
 * (processes, threads, code modules, device services). */
#include "natives.h"
#include <dirent.h>

/* ---------------- shared state ---------------- */

NatState *nat_state(BBVM *vm) {
    if (!vm->ext) {
        NatState *s = calloc(1, sizeof *s);
        s->nextPid = 1;
        s->bootMs = nat_now(vm);
        vm->ext = s;
        /* module handle table: every .cod in the search path, sorted */
        for (int d = 0; d < vm->ndirs; d++) {
            DIR *dir = opendir(vm->dirs[d]);
            if (!dir) continue;
            struct dirent *e;
            while ((e = readdir(dir))) {
                size_t l = strlen(e->d_name);
                if (l < 5 || strcmp(e->d_name + l - 4, ".cod")) continue;
                if (s->nmods == s->capmods) { s->capmods = s->capmods ? s->capmods * 2 : 1024; s->modNames = realloc(s->modNames, s->capmods * sizeof *s->modNames); }
                s->modNames[s->nmods] = strndup(e->d_name, l - 4);
                s->nmods++;
            }
            closedir(dir);
        }
        for (int i = 1; i < s->nmods; i++) {   /* insertion sort, fine for ~3k names */
            char *k = s->modNames[i]; int j = i - 1;
            while (j >= 0 && strcmp(s->modNames[j], k) > 0) { s->modNames[j + 1] = s->modNames[j]; j--; }
            s->modNames[j + 1] = k;
        }
    }
    return vm->ext;
}

uint64_t nat_now(BBVM *vm) {
    if (vm->host.nowMs) return vm->host.nowMs(vm->host.ctx);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int nat_module_handle(BBVM *vm, const char *name) {
    NatState *s = nat_state(vm);
    int lo = 0, hi = s->nmods - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(s->modNames[mid], name);
        if (!c) return mid + 1;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

BBModule *nat_module_by_handle(BBVM *vm, int h) {
    NatState *s = nat_state(vm);
    if (h < 1 || h > s->nmods) return NULL;
    return vm_module(vm, s->modNames[h - 1]);
}

Proc *nat_proc(BBVM *vm, int pid) {
    NatState *s = nat_state(vm);
    for (int i = 0; i < s->nprocs; i++) if (s->procs[i].id == pid) return &s->procs[i];
    return NULL;
}

static Proc *add_proc(BBVM *vm, ref_t obj) {
    NatState *s = nat_state(vm);
    Obj *o = vm_obj(vm, obj);
    int pid = o ? (int)o->d[0] : 0;
    Proc *p = nat_proc(vm, pid);
    if (p) { p->obj = obj; return p; }
    if (s->nprocs == s->capprocs) { s->capprocs = s->capprocs ? s->capprocs * 2 : 16; s->procs = realloc(s->procs, s->capprocs * sizeof *s->procs); }
    p = &s->procs[s->nprocs++];
    memset(p, 0, sizeof *p);
    p->id = pid; p->obj = obj;
    if (o) o->flags |= OF_PERM;
    return p;
}

Thread *nat_thread_of(BBVM *vm, ref_t threadObj) {
    for (Thread *t = vm->threads; t; t = t->next) if (t->obj == threadObj) return t;
    return NULL;
}

ref_t nat_thread_obj(BBVM *vm, Thread *t) {
    if (!t->obj) {
        BBClass *k = vm->cThread ? vm->cThread : vm_find_class_anywhere(vm, "java.lang.Thread");
        t->obj = vm_new_obj(vm, k);
        Obj *o = vm_obj(vm, t->obj);
        o->d[0] = 5;   /* priority */
    }
    return t->obj;
}

/* Find a no-argument virtual method by name on an object's class. */
BBRoutine *nat_virtual(BBVM *vm, ref_t obj, const char *name, const char *sig) {
    Obj *o = vm_obj(vm, obj);
    if (!o || o->kind != OK_INST) return NULL;
    return vm_find_method(vm, o->cls, name, sig);
}

/* Start a VM thread for a java.lang.Thread object, running its run(). */
Thread *nat_start_thread(BBVM *vm, ref_t threadObj, int pid) {
    Thread *nt = nat_thread_of(vm, threadObj);
    if (nt && nt->nfr) return nt;
    if (!nt) nt = vm_new_thread(vm, threadObj);
    nt->procId = pid;
    nt->state = TS_READY;
    BBRoutine *run = nat_virtual(vm, threadObj, "run", "");
    if (!run) { vm_log(vm, "thread object has no run()"); nt->state = TS_DEAD; return nt; }
    uint32_t a[1] = { threadObj };
    vm_push_call(vm, nt, run, a, 1);
    if (vm->trace & 4) vm_log(vm, "thread %d started (process %d)", nt->id, pid);
    return nt;
}

/* ---------------- java.lang.Thread ---------------- */

NAT(thr_currentThread) { UNUSED; ret[0] = nat_thread_obj(vm, t); return NAT_OK; }
NAT(thr_start0) {
    UNUSED;
    nat_start_thread(vm, a[0], t->procId);
    return NAT_OK;
}
NAT(thr_isAlive) {
    UNUSED;
    Thread *x = nat_thread_of(vm, a[0]);
    ret[0] = x && x->state != TS_DEAD && x->nfr > 0;
    return NAT_OK;
}
NAT(thr_activeCount) {
    UNUSED;
    int n = 0;
    for (Thread *x = vm->threads; x; x = x->next) if (x->state != TS_DEAD) n++;
    ret[0] = (uint32_t)n;
    return NAT_OK;
}
NAT(thr_interrupt0) {
    UNUSED;
    Thread *x = nat_thread_of(vm, a[0]);
    if (x) {
        x->interrupted = 1;
        if (x->state == TS_SLEEP) x->wakeAt = 0;
    }
    return NAT_OK;
}
NAT(nop) { UNUSED; return NAT_OK; }
NAT(cls_false) { UNUSED; ret[0] = 0; return NAT_OK; }
NAT(ret_true) { UNUSED; ret[0] = 1; return NAT_OK; }

/* ---------------- net.rim.vm.Process ---------------- */

NAT(proc_nextProcessId) { UNUSED; ret[0] = (uint32_t)nat_state(vm)->nextPid++; return NAT_OK; }
NAT(proc_addProcess) {
    UNUSED;
    Proc *p = add_proc(vm, a[0]);
    Thread *x = nat_thread_of(vm, a[1]);
    if (!x && a[1] == nat_thread_obj(vm, t)) x = t;
    if (x) x->procId = p->id;
    ret[0] = 1;
    return NAT_OK;
}
NAT(proc_createProcess) {
    UNUSED;
    Proc *p = add_proc(vm, a[0]);
    p->mainThread = a[1];
    ret[0] = 1;
    return NAT_OK;
}
NAT(proc_getProcessFromThread) {
    UNUSED;
    Thread *x = nat_thread_of(vm, a[0]);
    Proc *p = x ? nat_proc(vm, x->procId) : NULL;
    ret[0] = p ? p->obj : 0;
    return NAT_OK;
}
NAT(proc_getProcessFromId) { UNUSED; Proc *p = nat_proc(vm, (int)a[0]); ret[0] = p ? p->obj : 0; return NAT_OK; }
NAT(proc_loadModule) { UNUSED; ret[0] = 0; return NAT_OK; }   /* null = loaded */
/* start(pid, thread, args, flags): run the process module's entry point */
NAT(proc_start) {
    UNUSED;
    Proc *p = nat_proc(vm, (int)a[0]);
    Obj *po = p ? vm_obj(vm, p->obj) : NULL;
    if (!po) { ret[0] = 0; return NAT_OK; }
    int handle = (int)po->d[4];
    BBModule *m = nat_module_by_handle(vm, handle);
    if (!m) { vm_log(vm, "process %d: no module for handle %d", p->id, handle); ret[0] = 0; return NAT_OK; }
    /* entry point record in the data section: class ref, name, prototype */
    const uint8_t *d = m->data;
    int entry = (int)(a[3] & 1);
    uint16_t cref = rd16(d + 40 + 6 * entry), nameOff = rd16(d + 42 + 6 * entry);
    BBClass *k = NULL;
    for (int i = 0; i < m->ncref; i++) if (m->crefOff[i] == cref) { k = vm_resolve_cref(vm, m, i); break; }
    if (!k) { vm_log(vm, "process %d: %s has no entry point", p->id, m->name); ret[0] = 0; return NAT_OK; }
    BBRoutine *r = vm_find_method(vm, k, cod_name(m, nameOff), NULL);
    if (!r) { ret[0] = 0; return NAT_OK; }
    Thread *nt = nat_thread_of(vm, a[1]);
    if (!nt) nt = vm_new_thread(vm, a[1]);
    nt->procId = p->id;
    nt->state = TS_READY;
    uint32_t args[1] = { a[2] ? a[2] : vm_new_array(vm, BT_CLASS, vm->cString, 1, 0) };
    vm_push_call(vm, nt, r, args, r->nargs);
    if (vm->trace & 4) {
        char al[256] = ""; Obj *ao = vm_obj(vm, args[0]);
        for (uint32_t i = 0; ao && i < ao->len && strlen(al) < 200; i++) { char b[64]; vm_string_utf8(vm, ao->d[i], b, sizeof b); strcat(al, "\""); strcat(al, b); strcat(al, "\" "); }
        vm_log(vm, "process %d started %s.%s on thread %d args %s", p->id, k->full, r->name, nt->id, al);
    }
    ret[0] = 1;
    return NAT_OK;
}
NAT(proc_startThreadInProcess) { UNUSED; nat_start_thread(vm, a[1], (int)a[0]); return NAT_OK; }
NAT(proc_waitForIdle) {
    UNUSED;
    if (t->waitCount < 0) { t->waitCount = 0; return NAT_OK; }
    t->state = TS_SLEEP; t->wakeAt = nat_now(vm) + 10; t->waitCount = 0;
    return NAT_BLOCK;
}
NAT(proc_getAllProcesses) {
    UNUSED;
    NatState *s = nat_state(vm);
    BBClass *pk = vm_find_class_anywhere(vm, "net.rim.vm.Process");
    ref_t arr = vm_new_array(vm, BT_CLASS, pk, 1, (uint32_t)s->nprocs);
    for (int i = 0; i < s->nprocs; i++) vm_obj(vm, arr)->d[i] = s->procs[i].obj;
    ret[0] = arr;
    return NAT_OK;
}
NAT(proc_getAllThreads) {
    UNUSED;
    int n = 0;
    for (Thread *x = vm->threads; x; x = x->next) if (x->procId == (int)a[0] && x->state != TS_DEAD) n++;
    ref_t arr = vm_new_array(vm, BT_CLASS, vm->cThread, 1, (uint32_t)n);
    n = 0;
    for (Thread *x = vm->threads; x; x = x->next) if (x->procId == (int)a[0] && x->state != TS_DEAD) vm_obj(vm, arr)->d[n++] = nat_thread_obj(vm, x);
    ret[0] = arr;
    return NAT_OK;
}
NAT(proc_currentThreadSuicide) { UNUSED; t->state = TS_DEAD; t->nfr = 0; return NAT_BLOCK; }
NAT(proc_destroy) {
    UNUSED;
    for (Thread *x = vm->threads; x; x = x->next) if (x->procId == (int)a[0] && x != t) { x->state = TS_DEAD; x->nfr = 0; }
    return NAT_OK;
}
NAT(proc_getHeapSize) { UNUSED; ret[0] = 1u << 20; return NAT_OK; }
NAT(proc_getCodeSectionNumber) {
    UNUSED;
    char nm[256];
    vm_string_utf8(vm, a[0], nm, sizeof nm);
    ret[0] = (uint32_t)nat_module_handle(vm, nm);
    return NAT_OK;
}
NAT(proc_classForName) { /* classForName(String, int handle) */
    UNUSED;
    char nm[300];
    vm_string_utf8(vm, a[0], nm, sizeof nm);
    BBModule *m = nat_module_by_handle(vm, (int)a[1]);
    BBClass *k = m ? vm_class(vm, m->name, nm) : NULL;
    if (!k) k = vm_find_class_anywhere(vm, nm);
    ret[0] = k ? vm_class_obj(vm, k) : 0;
    return NAT_OK;
}

/* ---------------- CodeModuleManager ---------------- */

NAT(cmm_getModuleHandle) {
    UNUSED;
    char nm[256];
    vm_string_utf8(vm, a[0], nm, sizeof nm);
    ret[0] = (uint32_t)nat_module_handle(vm, nm);
    return NAT_OK;
}
NAT(cmm_getModuleHandleForClass) {
    UNUSED;
    Obj *o = vm_obj(vm, a[0]);
    BBClass *k = o && (o->flags & OF_CLASS) ? o->host : NULL;
    ret[0] = k ? (uint32_t)nat_module_handle(vm, k->mod->name) : 0;
    return NAT_OK;
}
NAT(cmm_getModuleHandleForObject) {
    UNUSED;
    Obj *o = vm_obj(vm, a[0]);
    ret[0] = o && o->kind == OK_INST ? (uint32_t)nat_module_handle(vm, o->cls->mod->name) : 0;
    return NAT_OK;
}
NAT(cmm_getModuleName) { /* (handle, index): index 0 is the module itself */
    UNUSED;
    NatState *s = nat_state(vm);
    int h = (int)a[0];
    ret[0] = (h >= 1 && h <= s->nmods && a[1] == 0) ? vm_new_string(vm, s->modNames[h - 1]) : 0;
    return NAT_OK;
}
NAT(cmm_getModuleAliasName) { /* (handle, index) -> alias or null */
    UNUSED;
    BBModule *m = nat_module_by_handle(vm, (int)a[0]);
    ret[0] = 0;
    if (!m) return NAT_OK;
    uint32_t p = rd16(m->data + 28) + 2u * a[1];
    if (p + 2 <= rd16(m->data + 6)) ret[0] = vm_new_string(vm, cod_cstr(m, rd16(m->data + p)));
    return NAT_OK;
}
NAT(cmm_getModuleVersion) {
    UNUSED;
    BBModule *m = nat_module_by_handle(vm, (int)a[0]);
    ret[0] = m && m->nmod ? vm_new_string(vm, cod_name(m, rd16(m->data + 52 + 2 * m->ncls + 2 * m->nmod))) : 0;
    if (m && !ret[0]) ret[0] = vm_new_string(vm, "5.0.0");
    return NAT_OK;
}
NAT(cmm_getModuleHandles) {
    UNUSED;
    NatState *s = nat_state(vm);
    ref_t arr = vm_new_array(vm, BT_INT, NULL, 1, (uint32_t)s->nmods);
    for (int i = 0; i < s->nmods; i++) vm_obj(vm, arr)->d[i] = (uint32_t)(i + 1);
    ret[0] = arr;
    return NAT_OK;
}
NAT(cmm_isValidHandle) { UNUSED; NatState *s = nat_state(vm); ret[0] = (int)a[0] >= 1 && (int)a[0] <= s->nmods; return NAT_OK; }
NAT(cmm_hasEntryPoint) {
    UNUSED;
    BBModule *m = nat_module_by_handle(vm, (int)a[0]);
    int e = (int)a[1] & 1;
    ret[0] = m && rd16(m->data + 40 + 6 * e) != 0;
    return NAT_OK;
}
NAT(cmm_isLibrary) {
    UNUSED;
    BBModule *m = nat_module_by_handle(vm, (int)a[0]);
    ret[0] = m ? (m->flags & 1) != 0 : 0;
    return NAT_OK;
}
NAT(cmm_isMidlet) {
    UNUSED;
    BBModule *m = nat_module_by_handle(vm, (int)a[0]);
    ret[0] = m ? (m->flags & 2) != 0 : 0;
    return NAT_OK;
}
NAT(cmm_getModuleCodeSize) { UNUSED; BBModule *m = nat_module_by_handle(vm, (int)a[0]); ret[0] = m ? m->codeSize : 0; return NAT_OK; }

/* ---------------- InternalServices ---------------- */

NAT(is_getUptime) { UNUSED; nat_ret64(ret, (int64_t)(nat_now(vm) - nat_state(vm)->bootMs)); return NAT_OK; }
NAT(is_isDeviceCapable) {
    UNUSED;
    /* Bold 9000: keyboard + trackball; no touch, no accelerometer.
     * Capability ids are logged until mapped. */
    static uint32_t seen[64]; static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == a[0]) known = 1;
    if (!known && nseen < 64) { seen[nseen++] = a[0]; if (vm->trace & 4) vm_log(vm, "isDeviceCapable(%d)", (int)a[0]); }
    ret[0] = 0;
    return NAT_OK;
}
NAT(is_catastrophicFailure) {
    UNUSED;
    char msg[300];
    vm_string_utf8(vm, a[1], msg, sizeof msg);
    vm_log(vm, "catastrophicFailure(%d): %s", (int)a[0], msg);
    vm_backtrace(vm, t);
    vm_fatal(vm, "catastrophicFailure: %s", msg);
    return NAT_THROWN;
}


/* ---------------- VM registries ---------------- */
static ref_t g_appRegistry;
NAT(proc_registerAppRegistry) { UNUSED; g_appRegistry = a[0]; Obj *o = vm_obj(vm, a[0]); if (o) o->flags |= OF_PERM; return NAT_OK; }
NAT(proc_getAppRegistry) { UNUSED; ret[0] = g_appRegistry; return NAT_OK; }

/* PersistentInteger: small persistent ints keyed by a long id */
typedef struct PInt { int64_t key; int32_t val; } PInt;
static PInt g_pint[512]; static int g_npint;
NAT(pint_getId) {
    UNUSED;
    int64_t k = nat_arg64(a);
    for (int i = 0; i < g_npint; i++) if (g_pint[i].key == k) { ret[0] = (uint32_t)i; return NAT_OK; }
    if (g_npint < 512) { g_pint[g_npint].key = k; g_pint[g_npint].val = (int32_t)a[2]; ret[0] = (uint32_t)g_npint++; }
    return NAT_OK;
}
NAT(pint_get) { UNUSED; int i = (int)a[0]; ret[0] = (i >= 0 && i < g_npint) ? (uint32_t)g_pint[i].val : 0; return NAT_OK; }
NAT(pint_set) { UNUSED; int i = (int)a[0]; if (i >= 0 && i < g_npint) g_pint[i].val = (int32_t)a[1]; return NAT_OK; }


/* ---------------- TraceBack: who is calling ---------------- */
/* Frames from the caller of the native outwards (the native itself has no frame). */
NAT(tb_getCallingModules) {
    UNUSED;
    int n = t->nfr;
    ref_t arr = vm_new_array(vm, BT_INT, NULL, 1, (uint32_t)n);
    Obj *o = vm_obj(vm, arr);
    for (int i = 0; i < n; i++) o->d[i] = (uint32_t)nat_module_handle(vm, t->fr[n - 1 - i].r->mod->name);
    ret[0] = arr;
    return NAT_OK;
}
NAT(tb_getCallingModule) {
    UNUSED;
    int d = (int)a[0], i = t->nfr - 1 - d;
    ret[0] = i >= 0 ? (uint32_t)nat_module_handle(vm, t->fr[i].r->mod->name) : 0;
    return NAT_OK;
}
NAT(tb_getCallingModuleName) {
    UNUSED;
    int d = (int)a[0], i = t->nfr - 1 - d;
    ret[0] = i >= 0 ? vm_new_string(vm, t->fr[i].r->mod->name) : 0;
    return NAT_OK;
}
NAT(tb_getCallingClasses) {
    UNUSED;
    int n = t->nfr;
    ref_t arr = vm_new_array(vm, BT_CLASS, vm->cClass, 1, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        BBClass *k = t->fr[n - 1 - i].r->cls;
        ref_t c = k ? vm_class_obj(vm, k) : 0;
        vm_obj(vm, arr)->d[i] = c;
    }
    ret[0] = arr;
    return NAT_OK;
}


/* ---------------- net.rim.vm.Message: the device event queue ---------------- */
typedef struct DevMsg { int32_t device, event, sub, len, ptr, d0, d1; ref_t o0, o1; } DevMsg;
static DevMsg g_q[1024]; static int g_qh, g_qt;

void nat_post_message(BBVM *vm, int device, int event, int sub, int d0, int d1, ref_t o0, ref_t o1) {
    int nt = (g_qt + 1) % 1024;
    if (nt == g_qh) return;   /* full: drop */
    DevMsg *m = &g_q[g_qt];
    memset(m, 0, sizeof *m);
    m->device = device; m->event = event; m->sub = sub; m->d0 = d0; m->d1 = d1; m->o0 = o0; m->o1 = o1;
    Obj *o;
    if ((o = vm_obj(vm, o0))) o->flags |= OF_PERM;
    if ((o = vm_obj(vm, o1))) o->flags |= OF_PERM;
    g_qt = nt;
    for (Thread *x = vm->threads; x; x = x->next) if (x->state == TS_EVENT) { x->state = TS_READY; break; }
}

int nat_messages_pending(void) { return g_qh != g_qt; }

NAT(msg_get) {
    UNUSED;
    if (t->interrupted == 2) { t->interrupted = 0; ret[0] = 0; return NAT_OK; }   /* abortGet */
    if (g_qh == g_qt) { t->state = TS_EVENT; return NAT_BLOCK; }
    DevMsg *m = &g_q[g_qh];
    g_qh = (g_qh + 1) % 1024;
    Obj *o = vm_obj(vm, a[0]);
    if (o) {
        o->d[0] = (uint32_t)m->device; o->d[1] = (uint32_t)m->event; o->d[2] = (uint32_t)m->sub;
        o->d[3] = (uint32_t)m->len; o->d[4] = (uint32_t)m->ptr; o->d[5] = (uint32_t)m->d0; o->d[6] = (uint32_t)m->d1;
        o->d[7] = m->o0; o->d[8] = m->o1;
    }
    Obj *x;
    if ((x = vm_obj(vm, m->o0))) x->flags &= ~OF_PERM;
    if ((x = vm_obj(vm, m->o1))) x->flags &= ~OF_PERM;
    if (vm->trace & 256) vm_log(vm, "msg get dev %d ev %d sub %d d0 %d d1 %d", m->device, m->event, m->sub, m->d0, m->d1);
    ret[0] = 1;
    return NAT_OK;
}
NAT(msg_post) {
    UNUSED;
    Obj *o = vm_obj(vm, a[0]);
    if (!o) return NAT_OK;
    if (vm->trace & 256) vm_log(vm, "msg post dev %d ev %d sub %d d0 %d d1 %d", (int)o->d[0], (int)o->d[1], (int)o->d[2], (int)o->d[5], (int)o->d[6]);
    nat_post_message(vm, (int)o->d[0], (int)o->d[1], (int)o->d[2], (int)o->d[5], (int)o->d[6], o->d[7], o->d[8]);
    return NAT_OK;
}
NAT(msg_abortGet) {
    UNUSED;
    Thread *x = nat_thread_of(vm, a[0]);
    if (x && x->state == TS_EVENT) { x->interrupted = 2; x->state = TS_READY; }
    return NAT_OK;
}


/* ---------------- net.rim.vm.Persistence (in memory for now) ---------------- */
static ref_t g_persistRoot, g_persistSync;
NAT(pers_getRoot) { UNUSED; ret[0] = g_persistRoot; return NAT_OK; }
NAT(pers_setRoot) { UNUSED; g_persistRoot = a[0]; Obj *o = vm_obj(vm, a[0]); if (o) o->flags |= OF_PERM; return NAT_OK; }
NAT(pers_getSynchObject) {
    UNUSED;
    if (!vm_obj(vm, g_persistSync)) { g_persistSync = vm_new_obj(vm, vm->cObject); vm_obj(vm, g_persistSync)->flags |= OF_PERM; }
    ret[0] = g_persistSync;
    return NAT_OK;
}


/* ---------------- code signing keys: every signer is "present" ---------------- */
static ref_t g_keys[64]; static int g_keyId[64]; static int g_nkeys;
static ref_t signing_key(BBVM *vm, int id) {
    for (int i = 0; i < g_nkeys; i++) if (g_keyId[i] == id) return g_keys[i];
    BBClass *k = vm_find_class_anywhere(vm, "net.rim.device.api.system.CodeSigningKey");
    if (!k) return 0;
    ref_t r = vm_new_obj(vm, k);
    Obj *o = vm_obj(vm, r);
    o->flags |= OF_PERM;
    o->d[0] = (uint32_t)id;
    ref_t pk = vm_new_array(vm, BT_BYTE, NULL, 1, 4);
    vm_obj(vm, r)->d[1] = pk;
    ((uint8_t *)vm_obj(vm, pk)->d)[0] = (uint8_t)id;
    vm_obj(vm, r)->d[2] = vm_new_string(vm, "RIM");
    if (g_nkeys < 64) { g_keys[g_nkeys] = r; g_keyId[g_nkeys] = id; g_nkeys++; }
    return r;
}
NAT(csk_get) { UNUSED; ret[0] = signing_key(vm, (int)a[1]); return NAT_OK; }
NAT(csk_getBuiltIn) { UNUSED; ret[0] = signing_key(vm, (int)a[0]); return NAT_OK; }


/* ---------------- CodeStore / module exported data ---------------- */
static void resize_int_array(BBVM *vm, ref_t r, uint32_t n) {
    Obj *old = vm_obj(vm, r);
    if (!old) return;
    Obj *o = calloc(1, sizeof(Obj) + (size_t)n * 4 + 8);
    memcpy(o, old, sizeof(Obj));
    o->len = n;
    vm->objs[r - REF_BASE] = o;
    free(old);
}
NAT(cs_getModuleHandles) {
    UNUSED;
    NatState *s = nat_state(vm);
    if (vm_obj(vm, a[0])) {
        resize_int_array(vm, a[0], (uint32_t)s->nmods);
        Obj *o = vm_obj(vm, a[0]);
        for (int i = 0; i < s->nmods; i++) o->d[i] = (uint32_t)(i + 1);
    }
    ret[0] = (uint32_t)s->nmods;
    return NAT_OK;
}
NAT(cs_isEldestSibling) {
    UNUSED;
    NatState *s = nat_state(vm);
    int h = (int)a[0];
    if (h < 1 || h > s->nmods) { ret[0] = 0; return NAT_OK; }
    const char *n = s->modNames[h - 1], *d = strrchr(n, '-');
    int sib = 0;
    if (d && d[1]) { char *e; strtol(d + 1, &e, 10); sib = !*e; }
    ret[0] = !sib;
    return NAT_OK;
}
NAT(cs_getSiblingHandles) {
    UNUSED;
    NatState *s = nat_state(vm);
    int h = (int)a[0];
    if (h < 1 || h > s->nmods) { ret[0] = 0; return NAT_OK; }
    char base[128];
    snprintf(base, sizeof base, "%s", s->modNames[h - 1]);
    char *d = strrchr(base, '-');
    if (d && d[1]) { char *e; strtol(d + 1, &e, 10); if (!*e) *d = 0; }
    int hs[256], n = 0;
    int b = nat_module_handle(vm, base);
    if (b) hs[n++] = b;
    for (int i = 1; i < 256 && n < 256; i++) {
        char nm[160]; snprintf(nm, sizeof nm, "%s-%d", base, i);
        int x = nat_module_handle(vm, nm);
        if (!x) break;
        hs[n++] = x;
    }
    ref_t arr = vm_new_array(vm, BT_INT, NULL, 1, (uint32_t)n);
    for (int i = 0; i < n; i++) vm_obj(vm, arr)->d[i] = (uint32_t)hs[i];
    ret[0] = arr;
    return NAT_OK;
}
/* exported data: entries (name, length, offset) between the alias table and the data bytes */
NAT(cmm_getModuleData) {
    UNUSED;
    BBModule *m = nat_module_by_handle(vm, (int)a[0]);
    Obj *key = vm_obj(vm, a[1]);
    ret[0] = 0;
    if (!m || !key) return NAT_OK;
    const uint8_t *d = m->data;
    uint32_t p = rd16(d + 6), end = rd16(d + 8);
    for (; p + 6 <= end; p += 6) {
        uint16_t no = rd16(d + p), len = rd16(d + p + 2), off = rd16(d + p + 4);
        const uint8_t *nm = d + no;
        uint32_t i = 0;
        while (i < key->len && no + i < m->dataSize && nm[i] == (uint8_t)nat_char(key, i)) i++;
        if (i != key->len || nm[i] != 0) continue;
        ref_t arr = vm_new_array(vm, BT_BYTE, NULL, 1, len);
        if ((uint32_t)off + len <= m->dataSize) memcpy(vm_obj(vm, arr)->d, d + off, len);
        ret[0] = arr;
        return NAT_OK;
    }
    return NAT_OK;
}


/* ---------------- ThreadSpecificData ---------------- */
NAT(tsd_get) { UNUSED; Thread *x = nat_thread_of(vm, a[0]); if (!x && a[0] == nat_thread_obj(vm, t)) x = t; ret[0] = x ? x->tsd : 0; return NAT_OK; }
NAT(tsd_set) { UNUSED; Thread *x = nat_thread_of(vm, a[0]); if (!x && a[0] == nat_thread_obj(vm, t)) x = t; if (x) x->tsd = a[1]; return NAT_OK; }

/* ---------------- OS timers: fire as messages on device 4 ---------------- */
typedef struct OsTimer { int handle, cookie, periodic; uint64_t due, period; } OsTimer;
static OsTimer g_tm[256]; static int g_ntm, g_nextTm = 1;
static void timers_tick(BBVM *vm) {
    uint64_t now = nat_now(vm);
    for (int i = 0; i < g_ntm; i++) {
        OsTimer *x = &g_tm[i];
        if (now < x->due) continue;
        nat_post_message(vm, 4, x->cookie, 0, 0, 0, 0, 0);
        if (x->periodic && x->period) x->due += x->period; else { g_tm[i] = g_tm[--g_ntm]; i--; }
    }
}
NAT(is_createTimer) { /* (long ms, boolean periodic, int cookie) -> handle */
    UNUSED;
    vm->tick = timers_tick;
    int64_t ms = nat_arg64(a);
    if (ms < 1) ms = 1;
    if (g_ntm < 256) {
        OsTimer *x = &g_tm[g_ntm++];
        x->handle = g_nextTm++; x->cookie = (int)a[3]; x->periodic = (int)a[2];
        x->period = (uint64_t)ms; x->due = nat_now(vm) + (uint64_t)ms;
        ret[0] = (uint32_t)x->handle;
    }
    return NAT_OK;
}
NAT(is_deleteTimerByHandle) { UNUSED; for (int i = 0; i < g_ntm; i++) if (g_tm[i].handle == (int)a[0]) { g_tm[i] = g_tm[--g_ntm]; break; } return NAT_OK; }
NAT(is_deleteTimerByCookie) { UNUSED; for (int i = 0; i < g_ntm; i++) if (g_tm[i].cookie == (int)a[0]) { g_tm[i] = g_tm[--g_ntm]; i--; } return NAT_OK; }
NAT(ret_two) { UNUSED; ret[0] = 2; return NAT_OK; }


/* module hash: SHA-1 over the whole .cod image */
int nat_sha1(const uint8_t *p, size_t n, uint8_t out[20]);
NAT(cmm_getModuleHash) {
    UNUSED;
    BBModule *m = nat_module_by_handle(vm, (int)a[0]);
    if (!m) { ret[0] = 0; return NAT_OK; }
    ref_t arr = vm_new_array(vm, BT_BYTE, NULL, 1, 20);
    nat_sha1(m->raw, m->rawSize, (uint8_t *)vm_obj(vm, arr)->d);
    ret[0] = arr;
    return NAT_OK;
}

const NatEnt kNativesRim[] = {
    { "java.lang.Thread.currentThread", thr_currentThread },
    { "java.lang.Thread.start0", thr_start0 },
    { "java.lang.Thread.isAlive", thr_isAlive },
    { "java.lang.Thread.activeCount", thr_activeCount },
    { "java.lang.Thread.interrupt0", thr_interrupt0 },
    { "java.lang.Thread.setPriority0", nop },
    { "net.rim.vm.Process.nextProcessId", proc_nextProcessId },
    { "net.rim.vm.Process.addProcess", proc_addProcess },
    { "net.rim.vm.Process.createProcess", proc_createProcess },
    { "net.rim.vm.Process.getProcessFromThread", proc_getProcessFromThread },
    { "net.rim.vm.Process.getProcessFromId", proc_getProcessFromId },
    { "net.rim.vm.Process.loadModule", proc_loadModule },
    { "net.rim.vm.Process.start(ILjava.lang.Thread;[Ljava.lang.String;I)", proc_start },
    { "net.rim.vm.Process.startThreadInProcess", proc_startThreadInProcess },
    { "net.rim.vm.Process.waitForIdle(J)", proc_waitForIdle },
    { "net.rim.vm.Process.getAllProcesses", proc_getAllProcesses },
    { "net.rim.vm.Process.getAllThreads(I)", proc_getAllThreads },
    { "net.rim.vm.Process.currentThreadSuicide", proc_currentThreadSuicide },
    { "net.rim.vm.Process.destroy", proc_destroy },
    { "net.rim.vm.Process.getHeapSize(I)", proc_getHeapSize },
    { "net.rim.vm.Process.getCodeSectionNumber(Ljava.lang.String;)", proc_getCodeSectionNumber },
    { "net.rim.vm.Process.classForName", proc_classForName },
    { "net.rim.vm.Process.markEventHandlerThread", nop },
    { "net.rim.vm.Process.bootComplete", nop },
    { "net.rim.vm.Process.killProcessIfThisThreadDies", nop },
    { "net.rim.device.api.system.CodeModuleManager.getModuleHandle(Ljava.lang.String;)", cmm_getModuleHandle },
    { "net.rim.device.api.system.CodeModuleManager.getModuleHandleForClassImpl", cmm_getModuleHandleForClass },
    { "net.rim.device.api.system.CodeModuleManager.getModuleHandleForObjectImpl", cmm_getModuleHandleForObject },
    { "net.rim.device.api.system.CodeModuleManager.getModuleName", cmm_getModuleName },
    { "net.rim.device.api.system.CodeModuleManager.getModuleAliasName", cmm_getModuleAliasName },
    { "net.rim.vm.Process.getModuleName", cmm_getModuleName },
    { "net.rim.device.api.system.CodeModuleManager.getModuleVersion", cmm_getModuleVersion },
    { "net.rim.device.api.system.CodeModuleManager.getModuleHandles", cmm_getModuleHandles },
    { "net.rim.device.api.system.CodeModuleManager.isValidHandle", cmm_isValidHandle },
    { "net.rim.device.api.system.CodeModuleManager.hasEntryPoint", cmm_hasEntryPoint },
    { "net.rim.device.api.system.CodeModuleManager.isLibrary", cmm_isLibrary },
    { "net.rim.device.api.system.CodeModuleManager.isMidlet", cmm_isMidlet },
    { "net.rim.device.api.system.CodeModuleManager.getModuleCodeSize", cmm_getModuleCodeSize },
    { "net.rim.device.internal.system.InternalServices.getUptime", is_getUptime },
    { "net.rim.device.internal.system.InternalServices.isDeviceCapable", is_isDeviceCapable },
    { "net.rim.device.internal.system.InternalServices.catastrophicFailure", is_catastrophicFailure },
    { "net.rim.vm.Process.registerAppRegistry", proc_registerAppRegistry },
    { "net.rim.vm.Process.getAppRegistry", proc_getAppRegistry },
    { "net.rim.vm.PersistentInteger.getId", pint_getId },
    { "net.rim.vm.PersistentInteger.get", pint_get },
    { "net.rim.vm.PersistentInteger.set", pint_set },
    { "net.rim.vm.EventLog.registerApp", nop },
    { "net.rim.vm.WeakReference.register", nop },
    { "net.rim.device.internal.system.InternalServices.isDeviceDevelopmentBuild", cls_false },
    { "net.rim.device.internal.system.InternalServices.isDateTimeValid", ret_true },
    { "net.rim.device.api.system.CodeModuleManager.verifySignature", ret_true },
    { "net.rim.vm.TraceBack.getCallingModules", tb_getCallingModules },
    { "net.rim.vm.TraceBack.getCallingModule", tb_getCallingModule },
    { "net.rim.vm.TraceBack.getCallingModuleName", tb_getCallingModuleName },
    { "net.rim.vm.TraceBack.getCallingClasses", tb_getCallingClasses },
    { "net.rim.vm.Message.get", msg_get },
    { "net.rim.vm.Message.post", msg_post },
    { "net.rim.vm.Message.abortGet", msg_abortGet },
    { "net.rim.vm.Persistence.getRoot", pers_getRoot },
    { "net.rim.vm.Persistence.setRoot", pers_setRoot },
    { "net.rim.vm.Persistence.getSynchObject", pers_getSynchObject },
    { "net.rim.device.api.system.CodeSigningKey.get(II)", csk_get },
    { "net.rim.device.api.system.CodeSigningKey.getBuiltInKey2", csk_getBuiltIn },
    { "net.rim.device.internal.system.CodeStore.getModuleHandles", cs_getModuleHandles },
    { "net.rim.device.internal.system.CodeStore.isEldestSiblingModule", cs_isEldestSibling },
    { "net.rim.device.internal.system.CodeStore.getSiblingHandles", cs_getSiblingHandles },
    { "net.rim.device.internal.system.CodeStore.getModuleData", cmm_getModuleData },
    { "net.rim.device.internal.system.CodeStore.checkModuleHash", ret_true },
    { "net.rim.device.api.system.CodeModuleManager.getModuleData", cmm_getModuleData },
    { "net.rim.vm.Process.getModuleData", cmm_getModuleData },
    { "net.rim.vm.Process.isModuleEldestSibling", cs_isEldestSibling },
    { "net.rim.vm.ThreadSpecificData.getData", tsd_get },
    { "net.rim.vm.ThreadSpecificData.setData", tsd_set },
    { "net.rim.device.internal.system.InternalServices.createTimer", is_createTimer },
    { "net.rim.device.internal.system.InternalServices.deleteTimerByHandle", is_deleteTimerByHandle },
    { "net.rim.device.internal.system.InternalServices.deleteTimerByCookie", is_deleteTimerByCookie },
    { "net.rim.device.internal.applicationcontrol.ApplicationControlImpl.getCallingModulesPermission", ret_two },
    { "net.rim.device.api.system.CodeModuleManager.getModuleHash(I)", cmm_getModuleHash },
    { NULL, NULL }
};
