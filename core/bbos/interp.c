/* PocketPager - BlackBerry OS family: RIM bytecode interpreter.
 *
 * Routines are decoded once into an Ins array (operands parsed, branch
 * targets turned into instruction indices); references through the
 * module's fixup tables are resolved the first time each instruction runs. */
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define RESOLVED 0x8000
#define OPMASK 0x1ff

/* ------------------------------------------------------------------ */
/* literals                                                            */

ref_t interp_literal(BBVM *vm, BBModule *m, uint16_t off, int unicode) {
    for (int i = 0; i < vm->nlits; i++) if (vm->lits[i].m == m && vm->lits[i].off == off) return vm->lits[i].r;
    if (off < 4 || off >= m->dataSize) return 0;
    uint32_t hdr = rd32(m->data + off - 4);
    int type = (hdr >> 17) & 0xf;
    uint32_t len = hdr & 0x1ffff;
    if (unicode < 0) unicode = type == 3;
    ref_t r;
    if (unicode || type == 3) {
        r = vm_new_array(vm, BT_CHAR, NULL, 1, len);
        Obj *o = vm_obj(vm, r);
        for (uint32_t i = 0; i < len && off + 2 * i + 1 < m->dataSize; i++) ((uint16_t *)o->d)[i] = rd16(m->data + off + 2 * i);
    } else {
        r = vm_new_array(vm, BT_BYTE, NULL, 1, len);
        Obj *o = vm_obj(vm, r);
        if (off + len <= m->dataSize) memcpy(o->d, m->data + off, len);
    }
    Obj *o = vm_obj(vm, r);
    o->flags |= OF_STRING | OF_PERM;
    o->cls = vm->cString;
    if (vm->nlits == vm->caplits) { vm->caplits = vm->caplits ? vm->caplits * 2 : 256; vm->lits = realloc(vm->lits, vm->caplits * sizeof *vm->lits); }
    vm->lits[vm->nlits].m = m; vm->lits[vm->nlits].off = off; vm->lits[vm->nlits].r = r; vm->nlits++;
    return r;
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */

typedef struct Switch { int n; int32_t low; int32_t *keys; int *tgt; int def; } Switch;
typedef struct StrArr { int n; uint16_t *off; } StrArr;
typedef struct Iref { BBClass *cls; const char *name; char sig[384]; uint8_t m, c; uint16_t proto; BBClass *cacheCls; BBRoutine *cacheR; } Iref;
static BBClass **g_side;
static int g_nside, g_capside;
static int side_add(BBClass *k) {
    if (g_nside == g_capside) { g_capside = g_capside ? g_capside * 2 : 1024; g_side = realloc(g_side, g_capside * sizeof *g_side); }
    g_side[g_nside] = k;
    return g_nside++;
}

static int modbyte(BBModule *m, uint32_t pos) {
    const BBFixup *f = cod_fixup_at(m, (uint16_t)pos);
    if (f && f->kind == FX_MODULE) return (int)f->idx;
    return m->code[pos];
}

static int rel_target(int *map, uint32_t base, int32_t disp, uint32_t start, uint32_t size) {
    int64_t t = (int64_t)base + disp - start;
    if (t < 0 || t >= size) return -1;
    return map[t];
}

static Prep *prepare(BBVM *vm, BBRoutine *r) {
    BBModule *m = r->mod;
    const uint8_t *c = m->code;
    uint32_t start = r->off, end = r->off + r->size;
    int *map = malloc((r->size + 1) * sizeof *map);
    for (uint32_t i = 0; i <= r->size; i++) map[i] = -1;
    int n = 0;
    for (uint32_t pc = start; pc < end;) { map[pc - start] = n++; int op; pc += cod_insn_size(c, pc, &op); }
    map[r->size] = n;
    Prep *p = calloc(1, sizeof *p);
    p->ins = calloc(n + 1, sizeof *p->ins);
    p->n = n;
    int maxLocal = r->nargs;
    int k = 0;
    for (uint32_t pc = start; pc < end; k++) {
        int op;
        int z = cod_insn_size(c, pc, &op);
        uint32_t o = pc + 1 + (op >= 0x100);
        Ins *in = &p->ins[k];
        in->op = (uint16_t)op; in->pc = (uint16_t)pc;
        switch (op) {
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98:
        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: case 0xa0: case 0xa1:
            in->a = rel_target(map, o, (int8_t)c[o], start, r->size); break;
        case 0xa2: in->a = rel_target(map, o, (int16_t)rd16(c + o), start, r->size); break;
        case 0x31: case 0x33: case 0x35: case 0x47: case 0x49: case 0x4b:
            in->a = c[o];
            if (in->a + 2 > maxLocal) maxLocal = in->a + 2;
            break;
        case 0xa5: in->a = c[o]; break;
        case 0x15: case 0x24: in->a = (int8_t)c[o]; break;
        case 0x16: case 0x25: in->a = (int16_t)rd16(c + o); break;
        case 0x32: case 0x34: case 0x36: case 0x48: case 0x4a: case 0x4c:
            in->a = rd16(c + o);
            if (in->a + 2 > maxLocal) maxLocal = in->a + 2;
            break;
        case 0x17: case 0x26: in->a = (int32_t)rd32(c + o); break;
        case 0x27: in->a = (int32_t)rd32(c + o); in->b = (int32_t)rd32(c + o + 4); break;
        case 0x28: in->a = rd16(c + o); break;
        case 0x2a: in->a = rd16(c + o + 2); in->b = rd16(c + o); break;
        case 0x2d: in->a = c[o]; in->b = rd16(c + o + 1); in->p = (void *)(uintptr_t)rd16(c + o + 3); break;
        case 0x11a: {
            StrArr *s = calloc(1, sizeof *s);
            s->n = rd16(c + o); s->off = calloc(s->n + 1, 2);
            for (int i = 0; i < s->n; i++) s->off[i] = rd16(c + o + 2 + 2 * i);
            in->p = s; break;
        }
        case 0x2f: {
            Switch *s = calloc(1, sizeof *s);
            int cnt = rd16(c + o);
            s->n = cnt; s->low = (int32_t)rd32(c + o + 2);
            s->tgt = calloc(cnt + 1, sizeof *s->tgt);
            uint32_t q = o + 6;
            for (int i = 0; i < cnt; i++, q += 2) s->tgt[i] = rel_target(map, q, (int16_t)rd16(c + q), start, r->size);
            in->p = s; break;
        }
        case 0xa3: case 0xa4: {
            Switch *s = calloc(1, sizeof *s);
            int cnt = rd16(c + o), w = op == 0xa3 ? 2 : 4;
            s->n = cnt;
            s->keys = calloc(cnt + 1, sizeof *s->keys); s->tgt = calloc(cnt + 1, sizeof *s->tgt);
            uint32_t q = o + 2;
            for (int i = 0; i < cnt; i++) {
                s->keys[i] = w == 2 ? (int16_t)rd16(c + q) : (int32_t)rd32(c + q);
                q += w;
                s->tgt[i] = rel_target(map, q, (int16_t)rd16(c + q), start, r->size);
                q += 2;
            }
            s->def = rel_target(map, q, (int16_t)rd16(c + q), start, r->size);
            in->p = s; break;
        }
        case 0x78: in->a = c[o]; in->b = (int8_t)c[o + 1]; if (in->a + 1 > maxLocal) maxLocal = in->a + 1; break;
        case 0x79: in->a = rd16(c + o); in->b = (int16_t)rd16(c + o + 2); if (in->a + 1 > maxLocal) maxLocal = in->a + 1; break;
        case 0xbd: case 0xbe: in->a = c[o]; in->b = (int8_t)c[o + 1]; break;
        case 0xa6: in->a = c[o]; in->b = c[o + 1] | c[o + 2] << 8; break;
        default: break;
        }
        pc += z;
    }
    p->nlocals = maxLocal > r->maxl ? maxLocal : r->maxl;
    if (p->nlocals < r->nargs) p->nlocals = r->nargs;
    /* exception table */
    if (r->exOff) {
        uint32_t q = r->exOff;
        int cap = 4;
        p->ex = calloc(cap, sizeof *p->ex);
        while (q + 2 <= m->codeSize) {
            uint16_t s = rd16(c + q);
            if (s == 0xffff) break;
            if (p->nex == cap) { cap *= 2; p->ex = realloc(p->ex, cap * sizeof *p->ex); }
            ExH *e = &p->ex[p->nex++];
            e->start = s; e->end = rd16(c + q + 2);
            uint16_t h = rd16(c + q + 4);
            e->handler = (h >= start && h < end) ? map[h - start] : -1;
            e->m = c[q + 6]; e->c = c[q + 7];
            e->cls = NULL;
            q += 8;
        }
    }
    free(map);
    return p;
}

/* ------------------------------------------------------------------ */
/* frames                                                              */

static inline Frame *top(Thread *t) { return &t->fr[t->nfr - 1]; }

static NativeFn bind_native(BBVM *vm, BBRoutine *r) {
    if (r->native) return (NativeFn)r->native;
    NativeFn f = natives_lookup(vm, r);
    r->native = (void *)f;
    return f;
}

static int is_native_routine(BBRoutine *r) {
    uint8_t op = r->mod->code[r->off];
    return r->size && (op == 0x09 || op == 0x0a || op == 0x0b);
}

/* Push a frame for r whose arguments are already at args (the caller's
 * stack, or copied there). Returns 0 on stack overflow. */
static int push_frame(BBVM *vm, Thread *t, BBRoutine *r, uint32_t *args) {
    if (!r->prep) r->prep = prepare(vm, r);
    Prep *p = r->prep;
    if (t->nfr >= t->maxfr || args + p->nlocals + 512 > t->stackEnd) {
        vm_throw(vm, t, "java.lang.StackOverflowError", NULL);
        return 0;
    }
    Frame *f = &t->fr[t->nfr++];
    memset(f, 0, sizeof *f);
    f->r = r; f->p = p; f->ip = 0;
    f->lv = args;
    for (int i = r->nargs; i < p->nlocals; i++) args[i] = 0;
    f->sp = args + p->nlocals;
    return 1;
}

int vm_push_call(BBVM *vm, Thread *t, BBRoutine *r, const uint32_t *args, int nargs) {
    uint32_t *base = t->nfr ? top(t)->sp : t->stack;
    memcpy(base, args, nargs * 4);
    if (is_native_routine(r)) {
        /* run natives directly through a tiny trampoline frame */
    }
    return push_frame(vm, t, r, base);
}

/* ------------------------------------------------------------------ */
/* exceptions                                                          */

static BBClass *cldc_class(BBVM *vm, const char *full) {
    BBClass *k = vm_class(vm, "net_rim_cldc", full);
    if (!k) k = vm_find_class_anywhere(vm, full);
    return k;
}

void vm_throw(BBVM *vm, Thread *t, const char *cls, const char *msg) {
    BBClass *k = cldc_class(vm, cls);
    {
        static const char *exf = (const char *)-1;
        if (exf == (const char *)-1) exf = getenv("BBEXTRACE");
        if ((vm->trace & 1) || (exf && strstr(cls, exf))) { vm_log(vm, "throw %s%s%s [t%d]", cls, msg ? ": " : "", msg ? msg : "", t->id); vm_backtrace(vm, t); }
    }
    if (!k) k = cldc_class(vm, "java.lang.Error");
    if (!k) { vm_fatal(vm, "no exception class %s (%s)", cls, msg ? msg : ""); t->pendingEx = 0; return; }
    ref_t e = vm_new_obj(vm, k);
    if (msg) {
        /* Throwable keeps its message in the first String field */
        for (BBClass *c = k; c; c = c->super) if (!strcmp(c->full, "java.lang.Throwable")) {
            for (int i = 0; i < c->nf; i++) {
                char s[64];
                vm_sig(vm, c->mod, c->f[i].tl, 0, s, sizeof s);
                if (!strcmp(s, "Ljava.lang.String;")) { vm_obj(vm, e)->d[c->f[i].slot] = vm_new_string(vm, msg); break; }
            }
        }
    }
    t->pendingEx = e;
}

void vm_backtrace(BBVM *vm, Thread *t) {
    for (int i = t->nfr - 1; i >= 0 && i >= t->nfr - 24; i--) {
        Frame *f = &t->fr[i];
        int pc = f->ip < f->p->n ? f->p->ins[f->ip].pc : -1;
        vm_log(vm, "   at %s.%s (%s @%d)", f->r->cls ? f->r->cls->full : "?", f->r->name, f->r->mod->name, pc);
    }
}

static void release_frame(BBVM *vm, Thread *t, Frame *f) {
    if (f->lock) vm_monitor_exit(vm, t, f->lock);
    if (f->clinit && f->initCls && vm_cinit_get(vm, t->procId, f->initCls) == 1) vm_cinit_set(vm, t->procId, f->initCls, 2, NULL);
}

/* Unwind to a handler for t->pendingEx. Returns 1 if one was found,
 * 0 when the exception escaped past baseDepth. */
static int unwind(BBVM *vm, Thread *t, int baseDepth) {
    ref_t ex = t->pendingEx;
    Obj *eo = vm_obj(vm, ex);
    while (t->nfr > baseDepth) {
        Frame *f = top(t);
        Prep *p = f->p;
        int ip = f->ip < p->n ? f->ip : p->n - 1;
        uint16_t pc = p->ins[ip].pc;
        for (int i = 0; i < p->nex; i++) {
            ExH *h = &p->ex[i];
            if (pc < h->start || pc >= h->end || h->handler < 0) continue;
            if (!h->cls && !(h->m == 255 && h->c == 255)) h->cls = vm_cls_ref(vm, f->r->mod, h->m, h->c);
            if (h->cls && eo && !vm_instanceof(vm, ex, h->cls)) continue;
            f->sp = f->lv + p->nlocals;
            *f->sp++ = ex;
            f->ip = h->handler;
            t->pendingEx = 0;
            return 1;
        }
        release_frame(vm, t, f);
        t->nfr--;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* class init                                                          */

/* Returns 1 if k is ready; 0 if a <clinit> frame was pushed (re-run the
 * instruction afterwards). */
int vm_init_class(BBVM *vm, Thread *t, BBClass *k) {
    if (!k) return 1;
    int pid = t->procId;
    int st = vm_cinit_get(vm, pid, k);
    if (st == 2) return 1;
    vm_link(vm, k);
    if (st == 1) {
        /* in progress: recursive use on the same thread proceeds, other
         * threads of the process wait until the initialiser finishes */
        Thread *it = vm_cinit_thread(vm, pid, k);
        if (!it || it == t || it->state == TS_DEAD) return 1;
        t->state = TS_CLINIT; t->initWait = k;
        return 0;
    }
    if (k->super && vm_cinit_get(vm, pid, k->super) != 2) {
        if (!vm_init_class(vm, t, k->super)) return 0;
    }
    vm_cinit_set(vm, pid, k, 1, t);
    if (!k->clinitOff) { vm_cinit_set(vm, pid, k, 2, NULL); return 1; }
    BBRoutine *r = cod_routine_at(k->mod, k->clinitOff);
    if (!r) { vm_cinit_set(vm, pid, k, 2, NULL); return 1; }
    if (vm->trace & 16) vm_log(vm, "clinit %s (pid %d)", k->full, pid);
    uint32_t *base = t->nfr ? top(t)->sp : t->stack;
    if (!push_frame(vm, t, r, base)) return 0;
    top(t)->clinit = 1;
    top(t)->initCls = k;
    return 0;
}

/* ------------------------------------------------------------------ */
/* resolution helpers                                                  */

static BBRoutine *resolve_member_routine(BBVM *vm, BBModule *m, BBMemberRef *mr, int isStatic) {
    if (mr->resolved) return mr->resolved;
    BBClass *k = mr->cref >= 0 ? (m->crefs[mr->cref].resolved ? m->crefs[mr->cref].resolved : NULL) : NULL;
    if (!k && mr->cref >= 0) {
        BBClassRef *cr = &m->crefs[mr->cref];
        char full[256];
        snprintf(full, sizeof full, "%s%s%s", cr->pkg, *cr->pkg ? "." : "", cr->cls);
        BBModule *t = vm_mod_ord(vm, m, cr->mod);
        if (t) k = vm_class(vm, t->name, full);
        if (!k) k = vm_find_class_anywhere(vm, full);
        cr->resolved = k;
    }
    if (!k) { vm_log(vm, "member %s: class unresolved", mr->name); return NULL; }
    char sig[512];
    vm_sig(vm, m, mr->tl, !isStatic || !strcmp(mr->name, "<init>"), sig, sizeof sig);
    BBRoutine *r = vm_find_method(vm, k, mr->name, sig);
    if (!r) {
        r = vm_find_method(vm, k, mr->name, NULL);
        vm_log(vm, "member %s.%s%s: no exact match%s", k->full, mr->name, sig, r ? " (using name)" : "");
    }
    mr->resolved = r;
    return r;
}

static BBClass *member_class(BBVM *vm, BBModule *m, BBMemberRef *mr) {
    if (mr->cref < 0) return NULL;
    BBClassRef *cr = &m->crefs[mr->cref];
    if (cr->resolved) return cr->resolved;
    char full[256];
    snprintf(full, sizeof full, "%s%s%s", cr->pkg, *cr->pkg ? "." : "", cr->cls);
    BBModule *t = vm_mod_ord(vm, m, cr->mod);
    BBClass *k = t ? vm_class(vm, t->name, full) : NULL;
    if (!k) k = vm_find_class_anywhere(vm, full);
    cr->resolved = k;
    return k;
}

static int field_slot_by_name(BBVM *vm, BBClass *k, const char *name) {
    for (BBClass *c = k; c; c = c->super) {
        vm_link(vm, c);
        for (int i = 0; i < c->nf; i++) if (!strcmp(c->f[i].name, name)) return c->f[i].slot;
    }
    return -1;
}

static BBField *static_by_name(BBVM *vm, BBClass *k, const char *name, BBClass **owner) {
    for (BBClass *c = k; c; c = c->super) {
        vm_link(vm, c);
        for (int i = 0; i < c->nsf; i++) if (!strcmp(c->sf[i].name, name)) { *owner = c; return &c->sf[i]; }
        for (int j = 0; j < c->nif; j++) if (c->ifaces[j]) {
            BBField *f = static_by_name(vm, c->ifaces[j], name, owner);
            if (f) return f;
        }
    }
    return NULL;
}

static BBRoutine *routine_in(BBVM *vm, BBModule *m, int mo, uint16_t off) {
    BBModule *t = vm_mod_ord(vm, m, mo);
    if (!t) return NULL;
    BBRoutine *r = cod_routine_at(t, off);
    if (r && !r->cls) {
        /* routine listed by no class (shouldn't happen); keep going */
    }
    return r;
}

static BBRoutine *iface_dispatch(BBVM *vm, BBClass *k, Iref *ir) {
    for (BBClass *c = k; c; c = c->super) {
        vm_link(vm, c);
        BBRoutine **lists[2] = { c->v, c->nv_ };
        int cnt[2] = { c->nv, c->nnv };
        for (int l = 0; l < 2; l++) for (int i = 0; i < cnt[l]; i++) {
            BBRoutine *r = lists[l][i];
            if (!r || strcmp(r->name, ir->name)) continue;
            char s[384];
            vm_sig(vm, r->mod, r->proto, 1, s, sizeof s);
            if (!strcmp(s, ir->sig)) return r;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* array helpers                                                       */

static Obj *arr_check(BBVM *vm, Thread *t, ref_t a, int32_t i) {
    Obj *o = vm_obj(vm, a);
    if (!o) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NULL; }
    if (i < 0 || (uint32_t)i >= o->len) {
        char b[48]; snprintf(b, sizeof b, "%d", i);
        vm_throw(vm, t, "java.lang.ArrayIndexOutOfBoundsException", b);
        return NULL;
    }
    return o;
}

static ref_t multi_array(BBVM *vm, int et, BBClass *ecls, int nest, int ndims, int32_t *lens) {
    int32_t len = lens[0];
    if (ndims == 1) return vm_new_array(vm, nest > 1 ? BT_ARRAY : et, ecls, nest, (uint32_t)len);
    ref_t r = vm_new_array(vm, BT_ARRAY, ecls, nest, (uint32_t)len);
    vm_obj(vm, r)->et = BT_ARRAY;
    for (int32_t i = 0; i < len; i++) {
        ref_t s = multi_array(vm, et, ecls, nest - 1, ndims - 1, lens + 1);
        vm_obj(vm, r)->d[i] = s;
    }
    return r;
}

/* element type of an array whose nesting is "nest" and leaf type et */
static int arr_is(BBVM *vm, ref_t r, int nest, int et, BBClass *ecls) {
    Obj *o = vm_obj(vm, r);
    if (!o) return 0;
    if (o->kind != OK_ARRAY) return 0;
    if (o->flags & OF_STRING) return nest == 1 && (et == o->et) ? 0 : 0;
    int odims = o->dims ? o->dims : 1;
    if (odims != nest) {
        if (odims > nest && ecls == vm->cObject) return 1;
        return 0;
    }
    if (ecls) {
        if (o->et != BT_CLASS && o->et != BT_ARRAY) return 0;
        if (!o->cls) return 1;
        vm_link(vm, o->cls);
        return vm_is_subclass(o->cls, ecls);
    }
    return o->et == et || (nest > 1 && o->et == BT_ARRAY);
}

/* ------------------------------------------------------------------ */
/* the interpreter                                                     */

static inline int64_t get64(const uint32_t *p) { return (int64_t)((uint64_t)p[0] | (uint64_t)p[1] << 32); }
static inline void put64(uint32_t *p, int64_t v) { p[0] = (uint32_t)v; p[1] = (uint32_t)((uint64_t)v >> 32); }
static inline float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t u32f(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline double f64(int64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline int64_t i64d(double d) { int64_t v; memcpy(&v, &d, 8); return v; }

static int32_t f2i(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)d;
}
static int64_t d2l(double d) {
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}

#define THROW(cls, msg) do { vm_throw(vm, t, cls, msg); goto exception; } while (0)
#define NPE() THROW("java.lang.NullPointerException", NULL)

/* Invoke routine r with nargs slots on the caller's stack. */
static int do_invoke(BBVM *vm, Thread *t, Frame *f, BBRoutine *r, int nargs) {
    uint32_t *args = f->sp - nargs;
    if (is_native_routine(r)) {
        NativeFn fn = bind_native(vm, r);
        uint32_t ret[2] = { 0, 0 };
        int st = fn(vm, t, args, ret);
        if (st == NAT_BLOCK) return 2;       /* re-run this instruction later */
        if (st == NAT_THROWN) return -1;
        if (st == NAT_CALLED) { f->sp = args; f->ip++; return 3; }
        f->sp = args;
        if (r->ret >= 1) *f->sp++ = ret[0];
        if (r->ret == 2) *f->sp++ = ret[1];
        f->ip++;
        return 1;
    }
    f->ip++;
    f->sp = args;
    if (!push_frame(vm, t, r, args)) { f->ip--; return -1; }
    return 0;
}

static void do_return(BBVM *vm, Thread *t, int slots) {
    Frame *f = top(t);
    uint32_t v0 = slots ? f->sp[-slots] : 0, v1 = slots == 2 ? f->sp[-1] : 0;
    uint32_t *dst = f->lv;
    release_frame(vm, t, f);
    t->nfr--;
    if (t->nfr) {
        Frame *c = top(t);
        c->sp = dst;
        if (slots >= 1) *c->sp++ = v0;
        if (slots == 2) *c->sp++ = v1;
    } else {
        t->stack[0] = v0; t->stack[1] = v1;
    }
}

int vm_run_thread(BBVM *vm, Thread *t, long long budget, int baseDepth) {
    vm->cur = t;
    if (t->pendingEx && t->nfr > baseDepth) goto exception;
    while (budget-- > 0) {
        if (t->nfr <= baseDepth) return 1;
        if (t->state != TS_READY) return 0;
        Frame *f = top(t);
        if (f->ip >= f->p->n) {
            /* past the end: after a tail call, return what it returned */
            do_return(vm, t, f->tailRet ? f->tailRet - 1 : 0);
            continue;
        }
        Ins *in = &f->p->ins[f->ip];
        BBModule *m = f->r->mod;
        uint32_t *sp = f->sp;
        int op = in->op & OPMASK;
        vm->icount++;
        if ((vm->trace & 32) || (vm->traceFilter && f->r->cls && strstr(f->r->cls->full, vm->traceFilter)))
            vm_log(vm, "[%d] %s.%s @%u %s sp=%d top=%08x %08x", t->id, f->r->cls ? f->r->cls->name : "?", f->r->name, in->pc, cod_op_name(op),
                   (int)(sp - f->lv - f->p->nlocals), sp > f->lv + f->p->nlocals ? sp[-1] : 0, sp > f->lv + f->p->nlocals + 1 ? sp[-2] : 0);
        uint32_t o = in->pc + 1u + (op >= 0x100);
        switch (op) {
        /* ---- frame entry markers ---- */
        case 0x0e: case 0x0f: case 0x10: case 0x11: case 0xdd: case 0x14:
            if (op == 0x14) {
                /* clinit_wait: class already initialised by someone else? */
                if (f->clinit && f->initCls && vm_cinit_get(vm, t->procId, f->initCls) == 2) { do_return(vm, t, 0); continue; }
            }
            f->ip++; continue;
        case 0x12: /* synch: synchronized instance method */
            if (!vm_monitor_enter(vm, t, f->lv[0])) return 0;
            f->lock = f->lv[0]; f->ip++; continue;
        case 0x13: { /* synch_static */
            BBClass *k = vm_cls_ref(vm, m, 0, m->code[o]);
            /* static state is per process, and so is the lock guarding it */
            ref_t co = vm_class_lock(vm, t->procId, k);
            if (!vm_monitor_enter(vm, t, co)) return 0;
            f->lock = co; f->ip++; continue;
        }
        case 0xcc: case 0xd7: f->ip++; continue;      /* nop, isreal */

        /* ---- constants ---- */
        case 0x22: case 0x23: *sp++ = 0; break;
        case 0x2c: *sp++ = 1; break;
        case 0x24: case 0x25: case 0x26: *sp++ = (uint32_t)in->a; break;
        case 0x27: *sp++ = (uint32_t)in->a; *sp++ = (uint32_t)in->b; break;
        case 0x11d: *sp++ = 0; *sp++ = 0; break;                                   /* dconst_0 */
        case 0x11e: { int64_t v = i64d(1.0); put64(sp, v); sp += 2; break; }       /* dconst_1 */
        case 0x11b: case 0x11c: case 0x11f: *sp++ = 0; break;
        case 0x120: *sp++ = u32f(1.0f); break;                                     /* fconst_1 */
        case 0x121: *sp++ = u32f(2.0f); break;                                     /* fconst_2 */
        case 0x28: *sp++ = interp_literal(vm, m, (uint16_t)in->a, 0); break;
        case 0x2a: *sp++ = interp_literal(vm, m, (uint16_t)in->a, 1); break;
        case 0xdf: {
            static ref_t empty;
            if (!vm_obj(vm, empty)) { empty = vm_new_string(vm, ""); vm_obj(vm, empty)->flags |= OF_PERM; }
            *sp++ = empty; break;
        }
        case 0x11a: {
            StrArr *s = in->p;
            ref_t a = vm_new_array(vm, BT_CLASS, vm->cString, 1, (uint32_t)s->n);
            for (int i = 0; i < s->n; i++) vm_obj(vm, a)->d[i] = s->off[i] ? interp_literal(vm, m, s->off[i], -1) : 0;
            *sp++ = a; break;
        }
        case 0x2d: { /* arrayinit: push a new array initialised from the data section */
            uint16_t off = (uint16_t)(uintptr_t)in->p;
            uint32_t n = (uint32_t)in->b;
            ref_t ar = vm_new_array(vm, in->a, NULL, 1, n);
            Obj *ao = vm_obj(vm, ar);
            uint32_t bytes = n * (uint32_t)vm_elem_size(in->a);
            if ((uint32_t)off + bytes <= m->dataSize) memcpy(ao->d, m->data + off, bytes);
            *sp++ = ar;
            break;
        }

        /* ---- locals ---- */
        case 0x31: case 0x32: case 0x33: case 0x34: *sp++ = f->lv[in->a]; break;
        case 0x35: case 0x36: *sp++ = f->lv[in->a]; *sp++ = f->lv[in->a + 1]; break;
        case 0x37: case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e:
            *sp++ = f->lv[op - 0x37]; break;
        case 0x3f: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46:
            *sp++ = f->lv[op - 0x3f]; break;
        case 0x47: case 0x48: case 0x49: case 0x4a: f->lv[in->a] = *--sp; break;
        case 0x4b: case 0x4c: sp -= 2; f->lv[in->a] = sp[0]; f->lv[in->a + 1] = sp[1]; break;
        case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51: case 0x52: case 0x53: case 0x54:
            f->lv[op - 0x4d] = *--sp; break;
        case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c:
            f->lv[op - 0x55] = *--sp; break;
        case 0x78: case 0x79: f->lv[in->a] += (uint32_t)in->b; break;

        /* ---- fields ---- */
        case 0x19: case 0x1a: case 0x1c: case 0x1d: case 0x5d: case 0x5e: case 0x5f: case 0x60:
        case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: {
            if (!(in->op & RESOLVED)) {
                int wide = (op == 0x1a || op == 0x1d || op == 0x5e || op == 0x60 || op == 0x62 || op == 0x64 || op == 0x66 || op == 0x68);
                const BBFixup *fx = cod_fixup_at(m, (uint16_t)o);
                int slot = m->code[o] + (wide ? 256 : 0);
                if (fx && fx->kind == FX_LFIELD) {
                    BBMemberRef *mr = &m->mrefs[fx->idx];
                    BBClass *k = &m->cls[mr->lcls];
                    vm_link(vm, k);
                    slot = mr->lfield < k->nf ? k->f[mr->lfield].slot : -1;
                } else if (fx && fx->kind == FX_FIELD) {
                    BBMemberRef *mr = &m->mrefs[fx->idx];
                    BBClass *k = member_class(vm, m, mr);
                    slot = k ? field_slot_by_name(vm, k, mr->name) : -1;
                }
                if (slot < 0) { vm_log(vm, "unresolved field at %s @%u", m->name, in->pc); THROW("java.lang.NoSuchFieldError", m->name); }
                in->a = slot;
                in->op |= RESOLVED;
            }
            int s = in->a;
            switch (op) {
            case 0x19: case 0x1a: case 0x1c: case 0x1d: { /* ireturn_field / areturn_field */
                Obj *ob = vm_obj(vm, f->lv[0]); if (!ob) NPE();
                *sp++ = ob->d[s]; f->sp = sp; do_return(vm, t, 1); continue;
            }
            case 0x5d: case 0x5e: { /* putfield_return: this.f = arg1 */
                Obj *ob = vm_obj(vm, f->lv[0]); if (!ob) NPE();
                ob->d[s] = f->lv[1]; f->sp = sp; do_return(vm, t, 0); continue;
            }
            case 0x5f: case 0x60: { Obj *ob = vm_obj(vm, sp[-2]); if (!ob) NPE(); ob->d[s] = sp[-1]; sp -= 2; break; }
            case 0x61: case 0x62: { Obj *ob = vm_obj(vm, sp[-3]); if (!ob) NPE(); ob->d[s] = sp[-2]; ob->d[s + 1] = sp[-1]; sp -= 3; break; }
            case 0x63: case 0x64: { Obj *ob = vm_obj(vm, sp[-1]); if (!ob) NPE(); sp[-1] = ob->d[s]; break; }
            case 0x65: case 0x66: { Obj *ob = vm_obj(vm, sp[-1]); if (!ob) NPE(); sp[-1] = ob->d[s]; *sp++ = ob->d[s + 1]; break; }
            case 0x67: case 0x68: { Obj *ob = vm_obj(vm, f->lv[0]); if (!ob) NPE(); *sp++ = ob->d[s]; break; }
            }
            break;
        }

        /* ---- statics ---- */
        case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: case 0x6f: case 0x70: {
            if (!(in->op & RESOLVED)) {
                int lib = op == 0x6a || op == 0x6c || op == 0x6e || op == 0x70;
                const BBFixup *fx = cod_fixup_at(m, (uint16_t)o);
                BBModule *sm = NULL; BBClass *k = NULL; int addr = -1;
                if (fx && fx->kind == FX_SFIELD) {
                    BBMemberRef *mr = &m->mrefs[fx->idx];
                    BBClass *c = member_class(vm, m, mr), *owner = NULL;
                    BBField *sf = c ? static_by_name(vm, c, mr->name, &owner) : NULL;
                    if (sf) { sm = owner->mod; k = owner; addr = sf->addr; }
                    else vm_log(vm, "unresolved static %s.%s", c ? c->full : "?", mr->name);
                } else {
                    int mo = lib ? modbyte(m, o) : 0;
                    int co = m->code[o + lib];
                    addr = rd16(m->code + o + lib + 1);
                    sm = vm_mod_ord(vm, m, mo);
                    k = vm_cls_direct(vm, m, mo, co);
                }
                if (!sm || addr < 0) { vm_log(vm, "unresolved static at %s @%u", m->name, in->pc); THROW("java.lang.NoSuchFieldError", m->name); }
                in->p = sm;
                in->b = addr;
                in->a = side_add(k);
                in->op |= RESOLVED;
            }
            {
                BBClass *k = g_side[in->a];
                if (k && vm_cinit_get(vm, t->procId, k) != 2 && !(f->clinit && f->initCls == k)) {
                    f->sp = sp;
                    if (!vm_init_class(vm, t, k)) { if (t->pendingEx) goto exception; continue; }
                }
            }
            uint32_t *p = vm_statics(vm, t->procId, (BBModule *)in->p) + in->b;
            switch (op) {
            case 0x69: case 0x6a: p[0] = *--sp; break;
            case 0x6b: case 0x6c: sp -= 2; p[0] = sp[0]; p[1] = sp[1]; break;
            case 0x6d: case 0x6e: *sp++ = p[0]; break;
            case 0x6f: case 0x70: *sp++ = p[0]; *sp++ = p[1]; break;
            }
            break;
        }

        /* ---- conversions and arithmetic ---- */
        case 0x71: sp[-1] = (uint32_t)(int32_t)(int8_t)sp[-1]; break;
        case 0x72: sp[-1] = (uint32_t)(int32_t)(int16_t)sp[-1]; break;
        case 0x73: sp[-1] = (uint16_t)sp[-1]; break;
        case 0x74: { int64_t v = (int32_t)sp[-1]; put64(sp - 1, v); sp++; break; }
        case 0x75: sp--; break;   /* l2i: keep low word */
        case 0x76: sp[-1] = (uint32_t)-(int32_t)sp[-1]; break;
        case 0x77: put64(sp - 2, -get64(sp - 2)); break;
        case 0x7a: sp--; sp[-1] += sp[0]; break;
        case 0x7b: sp -= 2; put64(sp - 2, get64(sp - 2) + get64(sp)); break;
        case 0x7c: sp--; sp[-1] -= sp[0]; break;
        case 0x7d: sp -= 2; put64(sp - 2, get64(sp - 2) - get64(sp)); break;
        case 0x7e: sp--; sp[-1] = (uint32_t)((int32_t)sp[-1] * (int32_t)sp[0]); break;
        case 0x7f: sp -= 2; put64(sp - 2, (int64_t)((uint64_t)get64(sp - 2) * (uint64_t)get64(sp))); break;
        case 0x80: case 0x82: {
            int32_t b = (int32_t)sp[-1], a = (int32_t)sp[-2];
            if (!b) THROW("java.lang.ArithmeticException", "/ by zero");
            sp--;
            if (a == INT32_MIN && b == -1) sp[-1] = op == 0x80 ? (uint32_t)a : 0;
            else sp[-1] = (uint32_t)(op == 0x80 ? a / b : a % b);
            break;
        }
        case 0x81: case 0x83: {
            int64_t b = get64(sp - 2), a = get64(sp - 4);
            if (!b) THROW("java.lang.ArithmeticException", "/ by zero");
            sp -= 2;
            if (a == INT64_MIN && b == -1) put64(sp - 2, op == 0x81 ? a : 0);
            else put64(sp - 2, op == 0x81 ? a / b : a % b);
            break;
        }
        case 0x84: sp--; sp[-1] &= sp[0]; break;
        case 0x85: sp -= 2; sp[-2] &= sp[0]; sp[-1] &= sp[1]; break;
        case 0x86: sp--; sp[-1] |= sp[0]; break;
        case 0x87: sp -= 2; sp[-2] |= sp[0]; sp[-1] |= sp[1]; break;
        case 0x88: sp--; sp[-1] ^= sp[0]; break;
        case 0x89: sp -= 2; sp[-2] ^= sp[0]; sp[-1] ^= sp[1]; break;
        case 0x8a: sp--; sp[-1] <<= (sp[0] & 31); break;
        case 0x8b: { int s = sp[-1] & 63; sp--; put64(sp - 2, (int64_t)((uint64_t)get64(sp - 2) << s)); break; }
        case 0x8c: sp--; sp[-1] = (uint32_t)((int32_t)sp[-1] >> (sp[0] & 31)); break;
        case 0x8d: { int s = sp[-1] & 63; sp--; put64(sp - 2, get64(sp - 2) >> s); break; }
        case 0x8e: sp--; sp[-1] >>= (sp[0] & 31); break;
        case 0x8f: { int s = sp[-1] & 63; sp--; put64(sp - 2, (int64_t)((uint64_t)get64(sp - 2) >> s)); break; }
        case 0x90: { int64_t b = get64(sp - 2), a = get64(sp - 4); sp -= 3; sp[-1] = (uint32_t)(a < b ? -1 : a > b ? 1 : 0); break; }

        /* ---- branches ---- */
        case 0x91: case 0x92: sp -= 2; if (sp[0] == sp[1]) goto branch; break;
        case 0x94: case 0x95: sp -= 2; if (sp[0] != sp[1]) goto branch; break;
        case 0x97: sp -= 2; if ((int32_t)sp[0] > (int32_t)sp[1]) goto branch; break;
        case 0x99: sp -= 2; if ((int32_t)sp[0] >= (int32_t)sp[1]) goto branch; break;
        case 0x9b: sp -= 2; if ((int32_t)sp[0] < (int32_t)sp[1]) goto branch; break;
        case 0x9d: sp -= 2; if ((int32_t)sp[0] <= (int32_t)sp[1]) goto branch; break;
        case 0x93: case 0x9f: if (*--sp == 0) goto branch; break;
        case 0x96: case 0xa0: if (*--sp != 0) goto branch; break;
        case 0x98: if ((int32_t)*--sp > 0) goto branch; break;
        case 0x9a: if ((int32_t)*--sp >= 0) goto branch; break;
        case 0x9c: if ((int32_t)*--sp < 0) goto branch; break;
        case 0x9e: if ((int32_t)*--sp <= 0) goto branch; break;
        case 0xa1: case 0xa2: goto branch;
        case 0x2f: {
            Switch *s = in->p;
            int32_t v = (int32_t)*--sp;
            int64_t idx = (int64_t)v - s->low;
            int tg = (idx > 0 && idx < s->n) ? s->tgt[idx] : s->tgt[0];
            f->sp = sp; f->ip = tg; continue;
        }
        case 0xa3: case 0xa4: {
            Switch *s = in->p;
            int32_t v = (int32_t)*--sp;
            int tg = s->def;
            for (int i = 0; i < s->n; i++) if (s->keys[i] == v) { tg = s->tgt[i]; break; }
            f->sp = sp; f->ip = tg; continue;
        }

        /* ---- arrays ---- */
        case 0xa5: {
            int32_t n = (int32_t)sp[-1];
            if (n < 0) THROW("java.lang.NegativeArraySizeException", NULL);
            sp[-1] = vm_new_array(vm, in->a, NULL, 1, (uint32_t)n);
            break;
        }
        case 0xa6: { /* multianewarray: dims on stack, nesting, leaf type */
            int nd = in->a, nest = in->b & 0xff, et = in->b >> 8;
            int32_t lens[32];
            sp -= nd;
            for (int i = 0; i < nd && i < 32; i++) { lens[i] = (int32_t)sp[i]; if (lens[i] < 0) THROW("java.lang.NegativeArraySizeException", NULL); }
            *sp++ = multi_array(vm, et, NULL, nest, nd, lens);
            break;
        }
        case 0xa8: case 0xa9: case 0xaa: case 0xab: {
            int lib = op == 0xa9 || op == 0xab;
            if (!(in->op & RESOLVED)) {
                const BBFixup *fx = cod_fixup_at(m, (uint16_t)o);
                BBClass *k;
                if (fx && fx->kind == FX_CLASS) k = vm_resolve_cref(vm, m, (int)fx->idx);
                else k = vm_cls_ref(vm, m, lib ? modbyte(m, o) : 0, m->code[o + lib]);
                in->p = k;
                if (op >= 0xaa) { in->a = m->code[o + lib + 1]; in->b = m->code[o + lib + 2]; }
                in->op |= RESOLVED;
            }
            BBClass *k = in->p;
            if (op <= 0xa9) {
                int32_t n = (int32_t)sp[-1];
                if (n < 0) THROW("java.lang.NegativeArraySizeException", NULL);
                sp[-1] = vm_new_array(vm, BT_CLASS, k, 1, (uint32_t)n);
            } else {
                int nd = in->a, nest = in->b;
                int32_t lens[32];
                sp -= nd;
                for (int i = 0; i < nd && i < 32; i++) lens[i] = (int32_t)sp[i];
                *sp++ = multi_array(vm, BT_CLASS, k, nest, nd, lens);
            }
            break;
        }
        case 0xa7: { Obj *a = vm_obj(vm, sp[-1]); if (!a) NPE(); sp[-1] = a->len; break; }
        case 0xd9: { Obj *a = vm_obj(vm, sp[-1]); if (!a) NPE(); sp[-1] = a->len; break; }   /* stringlength */
        case 0xda: { /* stringaload */
            Obj *a = arr_check(vm, t, sp[-2], (int32_t)sp[-1]); if (!a) goto exception;
            uint32_t i = sp[-1]; sp--;
            sp[-1] = a->et == BT_CHAR ? ((uint16_t *)a->d)[i] : ((uint8_t *)a->d)[i];
            break;
        }
        case 0xac: { Obj *a = arr_check(vm, t, sp[-2], (int32_t)sp[-1]); if (!a) goto exception; sp--; sp[-1] = (uint32_t)(int32_t)((int8_t *)a->d)[sp[0]]; break; }
        case 0xad: { Obj *a = arr_check(vm, t, sp[-2], (int32_t)sp[-1]); if (!a) goto exception; sp--; sp[-1] = (uint32_t)(int32_t)((int16_t *)a->d)[sp[0]]; break; }
        case 0xae: { Obj *a = arr_check(vm, t, sp[-2], (int32_t)sp[-1]); if (!a) goto exception; sp--;
            sp[-1] = a->et == BT_BYTE ? ((uint8_t *)a->d)[sp[0]] : ((uint16_t *)a->d)[sp[0]]; break; }
        case 0xaf: case 0xb0: { Obj *a = arr_check(vm, t, sp[-2], (int32_t)sp[-1]); if (!a) goto exception; sp--; sp[-1] = a->d[sp[0]]; break; }
        case 0xb1: { Obj *a = arr_check(vm, t, sp[-2], (int32_t)sp[-1]); if (!a) goto exception; uint32_t i = sp[-1];
            sp[-2] = a->d[2 * i]; sp[-1] = a->d[2 * i + 1]; break; }
        case 0xb2: { Obj *a = arr_check(vm, t, sp[-3], (int32_t)sp[-2]); if (!a) goto exception; ((uint8_t *)a->d)[sp[-2]] = (uint8_t)sp[-1]; sp -= 3; break; }
        case 0xb3: case 0xb4: { Obj *a = arr_check(vm, t, sp[-3], (int32_t)sp[-2]); if (!a) goto exception;
            if (a->et == BT_BYTE || a->et == BT_BOOL) ((uint8_t *)a->d)[sp[-2]] = (uint8_t)sp[-1];
            else ((uint16_t *)a->d)[sp[-2]] = (uint16_t)sp[-1];
            sp -= 3; break; }
        case 0xb5: case 0xb6: { Obj *a = arr_check(vm, t, sp[-3], (int32_t)sp[-2]); if (!a) goto exception;
            if (op == 0xb6 && sp[-1] && a->cls && a->dims <= 1 && a->et == BT_CLASS && !vm_instanceof(vm, sp[-1], a->cls))
                THROW("java.lang.ArrayStoreException", a->cls->full);
            a->d[sp[-2]] = sp[-1]; sp -= 3; break; }
        case 0xb7: { Obj *a = arr_check(vm, t, sp[-4], (int32_t)sp[-3]); if (!a) goto exception; uint32_t i = sp[-3];
            a->d[2 * i] = sp[-2]; a->d[2 * i + 1] = sp[-1]; sp -= 4; break; }

        /* ---- objects ---- */
        case 0xb8: case 0xb9: case 0xba: case 0xbb:
        case 0xbf: case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4:
        case 0xc6: case 0xc7: case 0xc8: case 0xc9: {
            int lib = op == 0xb9 || op == 0xbb || op == 0xc0 || op == 0xc2 || op == 0xc4 || op == 0xc7 || op == 0xc9;
            if (!(in->op & RESOLVED)) {
                const BBFixup *fx = cod_fixup_at(m, (uint16_t)o);
                BBClass *k;
                if (fx && fx->kind == FX_CLASS) k = vm_resolve_cref(vm, m, (int)fx->idx);
                else k = vm_cls_ref(vm, m, lib ? modbyte(m, o) : 0, m->code[o + lib]);
                if (!k) { vm_log(vm, "unresolved class operand at %s @%u", m->name, in->pc); THROW("java.lang.NoClassDefFoundError", m->name); }
                in->p = k;
                if (op == 0xc3 || op == 0xc4) {
                    uint32_t q = o + lib + 1;
                    in->a = (int)((int64_t)q + (int16_t)rd16(m->code + q));
                    /* convert to instruction index */
                    int idx = -1;
                    for (int i = 0; i < f->p->n; i++) if (f->p->ins[i].pc == (uint16_t)in->a) { idx = i; break; }
                    in->a = idx;
                }
                if (op >= 0xc6) in->b = m->code[o + lib + 1];
                in->op |= RESOLVED;
            }
            BBClass *k = in->p;
            switch (op) {
            case 0xb8: case 0xb9:
                if (vm_cinit_get(vm, t->procId, k) != 2) { f->sp = sp; if (!vm_init_class(vm, t, k)) { if (t->pendingEx) goto exception; continue; } }
                *sp++ = vm_new_obj(vm, k);
                break;
            case 0xba: case 0xbb:
                if (vm_cinit_get(vm, t->procId, k) != 2) { f->sp = sp; if (!vm_init_class(vm, t, k)) { if (t->pendingEx) goto exception; continue; } }
                break;
            case 0xbf: case 0xc0: sp[-1] = sp[-1] ? (uint32_t)vm_instanceof(vm, sp[-1], k) : 0; break;
            case 0xc1: case 0xc2:
                if (sp[-1] && !vm_instanceof(vm, sp[-1], k)) {
                    Obj *ob = vm_obj(vm, sp[-1]);
                    char msg[300]; snprintf(msg, sizeof msg, "%s -> %s", ob && ob->cls ? ob->cls->full : "array", k->full);
                    THROW("java.lang.ClassCastException", msg);
                }
                break;
            case 0xc3: case 0xc4:
                /* checkcastbranch: falls through when the cast holds, else pops and branches */
                if (!sp[-1] || !vm_instanceof(vm, sp[-1], k)) { sp--; f->sp = sp; f->ip = in->a; continue; }
                break;
            case 0xc6: case 0xc7: sp[-1] = sp[-1] ? (uint32_t)arr_is(vm, sp[-1], in->b, BT_CLASS, k) : 0; break;
            case 0xc8: case 0xc9:
                if (sp[-1] && !arr_is(vm, sp[-1], in->b, BT_CLASS, k)) THROW("java.lang.ClassCastException", k->full);
                break;
            }
            break;
        }
        case 0xbd: sp[-1] = sp[-1] ? (uint32_t)arr_is(vm, sp[-1], in->a, in->b, NULL) : 0; break;
        case 0xbe:
            if (sp[-1] && !arr_is(vm, sp[-1], in->a, in->b, NULL)) {
                Obj *ob = vm_obj(vm, sp[-1]);
                if (!(ob && (ob->flags & OF_STRING))) THROW("java.lang.ClassCastException", "array");
            }
            break;
        case 0xc5: {
            if (!(in->op & RESOLVED)) {
                uint32_t q = o + 2;
                int tgt = (int)((int64_t)q + (int16_t)rd16(m->code + q));
                in->a = m->code[o]; in->b = (int8_t)m->code[o + 1];
                int idx = -1;
                for (int i = 0; i < f->p->n; i++) if (f->p->ins[i].pc == (uint16_t)tgt) { idx = i; break; }
                in->p = (void *)(intptr_t)idx;
                in->op |= RESOLVED;
            }
            if (!sp[-1] || !arr_is(vm, sp[-1], in->a, in->b, NULL)) { sp--; f->sp = sp; f->ip = (int)(intptr_t)in->p; continue; }
            break;
        }
        case 0xbc: { /* athrow */
            ref_t e = *--sp;
            if (!e) NPE();
            t->pendingEx = e;
            {
                Obj *eo = vm_obj(vm, e);
                static const char *exf = (const char *)-1;
                if (exf == (const char *)-1) exf = getenv("BBEXTRACE");
                if ((vm->trace & 1) || (exf && eo && eo->cls && strstr(eo->cls->full, exf))) { vm_log(vm, "athrow %s [t%d]", eo && eo->cls ? eo->cls->full : "?", t->id); vm_backtrace(vm, t); }
            }
            f->sp = sp;
            goto exception;
        }
        case 0xca: { ref_t ob = sp[-1]; if (!ob) NPE(); f->sp = sp;
            if (!vm_monitor_enter(vm, t, ob)) return 0;
            sp--; break; }
        case 0xcb: { ref_t ob = *--sp; if (!ob) NPE(); vm_monitor_exit(vm, t, ob); break; }

        /* ---- stack ---- */
        case 0xcd: sp--; break;
        case 0xce: sp -= 2; break;
        case 0xcf: sp[0] = sp[-1]; sp++; break;
        case 0xd0: sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; break;
        case 0xd1: { uint32_t a = sp[-1], b = sp[-2]; sp[-2] = a; sp[-1] = b; sp[0] = a; sp++; break; }
        case 0xd2: { uint32_t a = sp[-1], b = sp[-2], c = sp[-3]; sp[-3] = a; sp[-2] = c; sp[-1] = b; sp[0] = a; sp++; break; }
        case 0xd3: { uint32_t a = sp[-1], b = sp[-2], c = sp[-3]; sp[-3] = b; sp[-2] = a; sp[-1] = c; sp[0] = b; sp[1] = a; sp += 2; break; }
        case 0xd4: { uint32_t a = sp[-1], b = sp[-2], c = sp[-3], d = sp[-4];
            sp[-4] = b; sp[-3] = a; sp[-2] = d; sp[-1] = c; sp[0] = b; sp[1] = a; sp += 2; break; }
        case 0xd5: { uint32_t a = sp[-1]; sp[-1] = sp[-2]; sp[-2] = a; break; }

        /* ---- floating point ---- */
        case 0x100: sp--; sp[-1] = u32f(f32(sp[-1]) + f32(sp[0])); break;
        case 0x102: sp--; sp[-1] = u32f(f32(sp[-1]) - f32(sp[0])); break;
        case 0x104: sp--; sp[-1] = u32f(f32(sp[-1]) * f32(sp[0])); break;
        case 0x106: sp--; sp[-1] = u32f(f32(sp[-1]) / f32(sp[0])); break;
        case 0x108: sp--; sp[-1] = u32f(fmodf(f32(sp[-1]), f32(sp[0]))); break;
        case 0x101: sp -= 2; put64(sp - 2, i64d(f64(get64(sp - 2)) + f64(get64(sp)))); break;
        case 0x103: sp -= 2; put64(sp - 2, i64d(f64(get64(sp - 2)) - f64(get64(sp)))); break;
        case 0x105: sp -= 2; put64(sp - 2, i64d(f64(get64(sp - 2)) * f64(get64(sp)))); break;
        case 0x107: sp -= 2; put64(sp - 2, i64d(f64(get64(sp - 2)) / f64(get64(sp)))); break;
        case 0x109: sp -= 2; put64(sp - 2, i64d(fmod(f64(get64(sp - 2)), f64(get64(sp))))); break;
        case 0x10a: sp[-1] = u32f(-f32(sp[-1])); break;
        case 0x10b: put64(sp - 2, i64d(-f64(get64(sp - 2)))); break;
        case 0x10c: sp[-1] = u32f((float)(int32_t)sp[-1]); break;
        case 0x10d: { double d = (int32_t)sp[-1]; put64(sp - 1, i64d(d)); sp++; break; }
        case 0x10e: { float v = (float)get64(sp - 2); sp--; sp[-1] = u32f(v); break; }
        case 0x10f: put64(sp - 2, i64d((double)get64(sp - 2))); break;
        case 0x110: sp[-1] = (uint32_t)f2i(f32(sp[-1])); break;
        case 0x111: { int64_t v = d2l(f32(sp[-1])); put64(sp - 1, v); sp++; break; }
        case 0x112: { double d = f32(sp[-1]); put64(sp - 1, i64d(d)); sp++; break; }
        case 0x113: { int32_t v = f2i(f64(get64(sp - 2))); sp--; sp[-1] = (uint32_t)v; break; }
        case 0x114: put64(sp - 2, d2l(f64(get64(sp - 2)))); break;
        case 0x115: { float v = (float)f64(get64(sp - 2)); sp--; sp[-1] = u32f(v); break; }
        case 0x116: case 0x117: { float b = f32(sp[-1]), a = f32(sp[-2]); sp--;
            sp[-1] = (uint32_t)(a > b ? 1 : a < b ? -1 : a == b ? 0 : (op == 0x117 ? 1 : -1)); break; }
        case 0x118: case 0x119: { double b = f64(get64(sp - 2)), a = f64(get64(sp - 4)); sp -= 3;
            sp[-1] = (uint32_t)(a > b ? 1 : a < b ? -1 : a == b ? 0 : (op == 0x119 ? 1 : -1)); break; }

        /* ---- returns ---- */
        case 0x15: case 0x16: case 0x17: *sp++ = (uint32_t)in->a; f->sp = sp; do_return(vm, t, 1); continue;
        case 0x18: case 0x1b: f->sp = sp; do_return(vm, t, 1); continue;
        case 0x1e: f->sp = sp; do_return(vm, t, 2); continue;
        case 0x1f: case 0x21: f->sp = sp; do_return(vm, t, 0); continue;
        case 0x20: /* clinit_return */
            if (f->initCls) vm_cinit_set(vm, t->procId, f->initCls, 2, NULL);
            f->sp = sp; do_return(vm, t, 0); continue;

        /* ---- invocation ---- */
        case 0x01: case 0xde: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
        case 0x0c: case 0x0d: case 0x02: case 0x09: case 0x0a: case 0x0b: {
            f->sp = sp;
            if (!(in->op & RESOLVED)) {
                const BBFixup *fx = cod_fixup_at(m, (uint16_t)o);
                BBMemberRef *mr = (fx && fx->kind != FX_CLASS && fx->kind != FX_MODULE) ? &m->mrefs[fx->idx] : NULL;
                switch (op) {
                case 0x01:
                    in->b = m->code[o + 2];
                    if (mr) {
                        BBClass *k = member_class(vm, m, mr);
                        BBRoutine *r = k ? resolve_member_routine(vm, m, mr, 0) : NULL;
                        if (!r || r->vindex < 0) {
                            /* resolve through the referencing class's vtable by name */
                            char sig[512]; vm_sig(vm, m, mr->tl, 1, sig, sizeof sig);
                            int found = -1;
                            if (k) { vm_link(vm, k);
                                for (int i = 0; i < k->vtsize; i++) if (k->vtable[i] && !strcmp(k->vtable[i]->name, mr->name)) {
                                    char s2[512]; vm_sig(vm, k->vtable[i]->mod, k->vtable[i]->proto, 1, s2, sizeof s2);
                                    if (!strcmp(s2, sig)) { found = i; break; }
                                } }
                            if (found < 0) {
                                if (r) { in->op = 0x03 | RESOLVED; in->p = r; in->b = r->nargs; goto resolved_inv; }
                                vm_log(vm, "unresolved virtual %s.%s%s", k ? k->full : "?", mr->name, sig); THROW("java.lang.NoSuchMethodError", mr->name);
                            }
                            in->a = found;
                        } else in->a = r->vindex;
                    } else in->a = (int16_t)rd16(m->code + o);
                    break;
                case 0xde: in->a = m->code[o] >> 2; in->b = (m->code[o] & 3) + 1; break;
                case 0x03: case 0x05: case 0x04: case 0x06: {
                    int lib = op == 0x04 || op == 0x06;
                    BBRoutine *r = mr ? resolve_member_routine(vm, m, mr, 0)
                                      : routine_in(vm, m, lib ? modbyte(m, o) : 0, rd16(m->code + o + lib));
                    in->p = r; in->b = m->code[o + lib + 2];
                    if (!r) { vm_log(vm, "unresolved call at %s @%u", m->name, in->pc); THROW("java.lang.NoSuchMethodError", m->name); }
                    break;
                }
                case 0x07: case 0x08: {
                    int lib = op == 0x08;
                    BBRoutine *r = mr ? resolve_member_routine(vm, m, mr, 1)
                                      : routine_in(vm, m, lib ? modbyte(m, o) : 0, rd16(m->code + o + lib + 1));
                    if (!r) { vm_log(vm, "unresolved static call at %s @%u", m->name, in->pc); THROW("java.lang.NoSuchMethodError", m->name); }
                    in->p = r; in->b = r->nargs;
                    break;
                }
                case 0x0c: case 0x0d: {
                    int lib = op == 0x0d;
                    BBRoutine *r = mr ? resolve_member_routine(vm, m, mr, 0)
                                      : routine_in(vm, m, lib ? modbyte(m, o) : 0, rd16(m->code + o + lib));
                    if (!r) { vm_log(vm, "unresolved jumpspecial at %s @%u", m->name, in->pc); THROW("java.lang.NoSuchMethodError", m->name); }
                    in->p = r;
                    break;
                }
                case 0x02: {
                    uint16_t io = rd16(m->code + o);
                    Iref *ir = calloc(1, sizeof *ir);
                    ir->m = m->data[io]; ir->c = m->data[io + 1];
                    ir->name = cod_name(m, rd16(m->data + io + 2));
                    ir->proto = rd16(m->data + io + 4);
                    ir->cls = vm_cls_ref(vm, m, ir->m, ir->c);
                    vm_sig(vm, m, ir->proto, 1, ir->sig, sizeof ir->sig);
                    in->p = ir; in->b = m->code[o + 2];
                    in->a = 0;
                    break;
                }
                case 0x09: case 0x0a: case 0x0b:
                    in->p = f->r; in->b = m->code[o];
                    break;
                }
                in->op |= RESOLVED;
            }
        resolved_inv:;
            int rc;
            switch (in->op & OPMASK) {
            case 0x01: case 0xde: {
                int nargs = in->b;
                ref_t self = sp[-nargs];
                Obj *ob = vm_obj(vm, self);
                if (!ob) NPE();
                BBClass *k = ob->kind == OK_INST ? ob->cls : (vm->cString && (ob->flags & OF_STRING) ? vm->cString : vm->cObject);
                if (!k) { vm_fatal(vm, "virtual call on array without Object class"); return -1; }
                vm_link(vm, k);
                if (in->a >= k->vtsize || !k->vtable[in->a]) {
                    vm_fatal(vm, "vtable index %d out of range for %s (%d) at %s.%s @%u", in->a, k->full, k->vtsize,
                             f->r->cls ? f->r->cls->full : "?", f->r->name, in->pc);
                    return -1;
                }
                BBRoutine *r = k->vtable[in->a];
                if (r->nargs != nargs && (vm->trace & 64))
                    vm_log(vm, "warn: vcall %s.%s nargs %d vs %d", k->full, r->name, r->nargs, nargs);
                rc = do_invoke(vm, t, f, r, nargs);
                break;
            }
            case 0x02: {
                Iref *ir = in->p;
                int nargs = in->b;
                ref_t self = sp[-nargs];
                Obj *ob = vm_obj(vm, self);
                if (!ob) NPE();
                BBClass *k = ob->kind == OK_INST ? ob->cls : vm->cObject;
                BBRoutine *r;
                if (ir->cacheCls == k) r = ir->cacheR;
                else { r = iface_dispatch(vm, k, ir); ir->cacheCls = k; ir->cacheR = r; }
                if (!r) {
                    vm_log(vm, "no implementation of %s.%s%s in %s", ir->cls ? ir->cls->full : "?", ir->name, ir->sig, k ? k->full : "?");
                    THROW("java.lang.AbstractMethodError", ir->name);
                }
                rc = do_invoke(vm, t, f, r, nargs);
                break;
            }
            case 0x03: case 0x04: case 0x05: case 0x06:
                if (!sp[-in->b]) NPE();
                rc = do_invoke(vm, t, f, (BBRoutine *)in->p, in->b);
                break;
            case 0x07: case 0x08: {
                BBRoutine *r = in->p;
                if (r->cls && vm_cinit_get(vm, t->procId, r->cls) != 2 && !(f->clinit && f->initCls == r->cls)) {
                    if (!vm_init_class(vm, t, r->cls)) { if (t->pendingEx) goto exception; continue; }
                }
                rc = do_invoke(vm, t, f, r, r->nargs);
                break;
            }
            case 0x0c: case 0x0d: {
                /* tail call with this routine's own arguments */
                BBRoutine *r = in->p;
                int n = f->r->nargs;
                memmove(f->sp, f->lv, n * 4);
                f->sp += n;
                f->tailRet = (uint8_t)(f->r->ret + 1);
                rc = do_invoke(vm, t, f, r, r->nargs);
                if (rc == 1) { do_return(vm, t, f->r->ret); continue; }
                if (rc == 0) f->ip = f->p->n;
                break;
            }
            case 0x09: case 0x0a: case 0x0b: {
                /* executing a native body directly (shouldn't happen: natives run at the call site) */
                BBRoutine *r = in->p;
                NativeFn fn = bind_native(vm, r);
                uint32_t ret[2] = { 0, 0 };
                int st = fn(vm, t, f->lv, ret);
                if (st == NAT_BLOCK) return 0;
                if (st == NAT_THROWN) goto exception;
                f->sp = f->lv + f->p->nlocals;
                if (r->ret >= 1) *f->sp++ = ret[0];
                if (r->ret == 2) *f->sp++ = ret[1];
                do_return(vm, t, r->ret);
                continue;
            }
            default: rc = 0; break;
            }
            if (rc == -1) goto exception;
            if (rc == 2) return 0;       /* blocked in a native */
            continue;
        }

        case 0xfa: /* halt */
            vm_log(vm, "halt in %s.%s @%u", f->r->cls ? f->r->cls->full : "?", f->r->name, in->pc);
            vm_backtrace(vm, t);
            THROW("java.lang.Error", "halt");
        case 0xfb: /* threaddeath */
            t->state = TS_DEAD; return 0;
        case 0x00:
            vm_fatal(vm, "breakpoint opcode in %s.%s @%u", f->r->cls ? f->r->cls->full : "?", f->r->name, in->pc);
            return -1;
        default:
            vm_fatal(vm, "unimplemented opcode %s (0x%x) in %s.%s @%u", cod_op_name(op), op, f->r->cls ? f->r->cls->full : "?", f->r->name, in->pc);
            return -1;
        }
        f->sp = sp;
        f->ip++;
        continue;
    branch:
        f->sp = sp;
        if (in->a < 0) { vm_fatal(vm, "bad branch in %s @%u", f->r->name, in->pc); return -1; }
        f->ip = in->a;
        continue;
    exception:
        if (!t->pendingEx) { if (vm->fatal) return -1; continue; }
        if (!unwind(vm, t, baseDepth)) {
            char msg[256] = "";
            Obj *eo = vm_obj(vm, t->pendingEx);
            vm_log(vm, "uncaught %s in thread %d", eo && eo->cls ? eo->cls->full : "?", t->id);
            (void)msg;
            return -2;
        }
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* scheduling                                                          */

static uint64_t sched_now(BBVM *vm) {
    if (vm->host.nowMs) return vm->host.nowMs(vm->host.ctx);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wake threads whose condition is met. */
static void wake(BBVM *vm) {
    if (vm->tick) vm->tick(vm);
    uint64_t now = sched_now(vm);
    for (Thread *t = vm->threads; t; t = t->next) {
        switch (t->state) {
        case TS_SLEEP:
            if (now >= t->wakeAt) { t->state = TS_READY; t->waitCount = -1; }
            break;
        case TS_CLINIT: {
            BBClass *k = t->initWait;
            Thread *it = k ? vm_cinit_thread(vm, t->procId, k) : NULL;
            if (!k || vm_cinit_get(vm, t->procId, k) != 1 || !it || it->state == TS_DEAD) { t->state = TS_READY; t->initWait = NULL; }
            break;
        }
        case TS_WAIT:
            if (t->wakeAt && now >= t->wakeAt) t->state = TS_MONITOR;
            if (t->interrupted) t->state = TS_MONITOR;
            break;
        case TS_MONITOR: {
            Monitor *m = vm_monitor(vm, t->waitObj, 1);
            if (m->owner && m->owner != t) break;
            if (t->waitCount > 0) {            /* returning from wait(): reacquire */
                m->owner = t; m->count = t->waitCount;
                t->waitCount = -1;
            }
            t->state = TS_READY; t->waitObj = 0;
            break;
        }
        case TS_JOIN: {
            int alive = 0;
            for (Thread *x = vm->threads; x; x = x->next) if (x->obj == t->joinTarget && x->state != TS_DEAD) alive = 1;
            if (!alive) { t->state = TS_READY; t->waitCount = -1; }
            break;
        }
        default: break;
        }
    }
}

static void thread_finished(BBVM *vm, Thread *t, int rc) {
    t->state = TS_DEAD;
    if (rc == -2 && t->pendingEx) {
        Obj *eo = vm_obj(vm, t->pendingEx);
        vm_log(vm, "thread %d died: %s", t->id, eo && eo->cls ? eo->cls->full : "?");
    }
}

/* Run all threads for up to budget instructions. Returns the number of
 * runnable threads left (0 = everything idle or dead). */
int vm_run(BBVM *vm, long long budget) {
    while (budget > 0 && !vm->fatal) {
        wake(vm);
        int ran = 0;
        for (Thread *t = vm->threads; t && budget > 0 && !vm->fatal; t = t->next) {
            if (t->state != TS_READY) continue;
            if (!t->nfr) { t->state = TS_DEAD; continue; }
            long long slice = budget < 20000 ? budget : 20000;
            long long before = vm->icount;
            int rc = vm_run_thread(vm, t, slice, 0);
            budget -= (vm->icount - before) + 1;
            ran++;
            if (rc == -1) return -1;
            if (rc == -2 || (rc == 1 && t->nfr == 0)) thread_finished(vm, t, rc);
        }
        if (vm->heapBytes > vm->gcAt) vm_gc(vm);
        if (!ran) {
            int sleeping = 0;
            for (Thread *t = vm->threads; t; t = t->next) if (t->state != TS_DEAD) sleeping++;
            return sleeping ? 0 : -3;
        }
    }
    int n = 0;
    for (Thread *t = vm->threads; t; t = t->next) if (t->state == TS_READY) n++;
    return n;
}

int vm_call(BBVM *vm, Thread *t, BBRoutine *r, const uint32_t *args, int nargs, uint32_t *ret) {
    int base = t->nfr;
    uint32_t *at = t->nfr ? top(t)->sp : t->stack;
    if (is_native_routine(r)) {
        memcpy(at, args, nargs * 4);
        NativeFn fn = bind_native(vm, r);
        uint32_t rv[2] = { 0, 0 };
        int st = fn(vm, t, at, rv);
        if (ret) { ret[0] = rv[0]; ret[1] = rv[1]; }
        return st == NAT_OK ? 0 : -1;
    }
    uint32_t *savedSp = t->nfr ? top(t)->sp : NULL;
    if (!vm_push_call(vm, t, r, args, nargs)) return -1;
    while (t->nfr > base && !vm->fatal) {
        int rc = vm_run_thread(vm, t, 1000000, base);
        if (rc == -1) return -1;
        if (rc == -2) { /* uncaught within the nested call: leave it pending */
            while (t->nfr > base) t->nfr--;
            return -2;
        }
        if (t->nfr > base && t->state != TS_READY) {
            /* blocked inside a nested call: let the others run */
            wake(vm);
            for (Thread *x = vm->threads; x; x = x->next)
                if (x != t && x->state == TS_READY && x->nfr) vm_run_thread(vm, x, 20000, 0);
            vm->cur = t;
        }
    }
    if (ret) {
        uint32_t *res = at;
        ret[0] = res[0]; ret[1] = r->ret == 2 ? res[1] : 0;
    }
    if (base) top(t)->sp = savedSp;
    return 0;
}
