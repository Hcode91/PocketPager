/* PocketPager - BlackBerry OS family: module manager, linker, heap and
 * threads. The bytecode interpreter itself is in interp.c. */
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* logging                                                            */

void vm_log(BBVM *vm, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (vm->host.log) vm->host.log(vm->host.ctx, buf);
    else fprintf(stderr, "%s\n", buf);
}

void vm_fatal(BBVM *vm, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(vm->fatalMsg, sizeof vm->fatalMsg, fmt, ap); va_end(ap);
    vm->fatal = 1;
    vm_log(vm, "FATAL: %s", vm->fatalMsg);
}

/* ------------------------------------------------------------------ */
/* string hash map (name -> pointer)                                  */

typedef struct SMap { char **k; void **v; int cap, n; } SMap;

static unsigned shash(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }

static void smap_put(SMap *m, const char *k, void *v) {
    if (m->n * 2 >= m->cap) {
        SMap b = { 0 };
        b.cap = m->cap ? m->cap * 2 : 64;
        b.k = calloc(b.cap, sizeof *b.k); b.v = calloc(b.cap, sizeof *b.v);
        for (int i = 0; i < m->cap; i++) if (m->k[i]) {
            unsigned h = shash(m->k[i]) & (b.cap - 1);
            while (b.k[h]) h = (h + 1) & (b.cap - 1);
            b.k[h] = m->k[i]; b.v[h] = m->v[i]; b.n++;
        }
        free(m->k); free(m->v); *m = b;
    }
    unsigned h = shash(k) & (m->cap - 1);
    while (m->k[h] && strcmp(m->k[h], k)) h = (h + 1) & (m->cap - 1);
    if (!m->k[h]) { m->k[h] = strdup(k); m->n++; }
    m->v[h] = v;
}

static void *smap_get(SMap *m, const char *k) {
    if (!m->cap) return NULL;
    unsigned h = shash(k) & (m->cap - 1);
    while (m->k[h]) { if (!strcmp(m->k[h], k)) return m->v[h]; h = (h + 1) & (m->cap - 1); }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* modules                                                            */

BBVM *vm_new(void) {
    BBVM *vm = calloc(1, sizeof *vm);
    vm->capobjs = 4096;
    vm->objs = calloc(vm->capobjs, sizeof *vm->objs);
    vm->nobjs = 1;              /* index 0 is null */
    vm->gcAt = 48u << 20;
    vm->nextTid = 1;
    return vm;
}

void vm_add_dir(BBVM *vm, const char *dir) {
    if (vm->ndirs < 8) vm->dirs[vm->ndirs++] = strdup(dir);
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void base_name(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s", name);
    char *dash = strrchr(out, '-');
    if (dash && dash[1]) {
        char *e; strtol(dash + 1, &e, 10);
        if (!*e) *dash = 0;
    }
}

static void init_statics(BBVM *vm, BBModule *m);

static BBModule *load_module(BBVM *vm, const char *name) {
    for (int i = 0; i < vm->ndirs; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.cod", vm->dirs[i], name);
        size_t n;
#ifdef BBARENA_H
        /* module files go where save states can re-read them from disk */
        uint8_t *b = bb_code_load(path, &n, COD_PAD);
#else
        uint8_t *b = slurp(path, &n);
#endif
        if (!b) continue;
        char err[256];
        BBModule *m = cod_parse(name, b, n, err, sizeof err);
        if (!m) { vm_log(vm, "load %s: %s", name, err); return NULL; }
        m->next = vm->mods; vm->mods = m;
        m->gidx = vm->nmodsG++;
        for (int c = 0; c < m->ncls; c++) m->cls[c].gidx = vm->nclsG++;
        init_statics(vm, m);
        if (vm->trace & 4) vm_log(vm, "loaded module %s (%d classes)", name, m->ncls);
        return m;
    }
    return NULL;
}

BBModule *vm_module(BBVM *vm, const char *name) {
    for (BBModule *m = vm->mods; m; m = m->next) if (!strcmp(m->name, name)) return m;
    return load_module(vm, name);
}

Family *vm_family(BBVM *vm, const char *name) {
    char base[96];
    base_name(name, base, sizeof base);
    for (Family *f = vm->fams; f; f = f->next) if (!strcmp(f->base, base)) return f;
    Family *f = calloc(1, sizeof *f);
    snprintf(f->base, sizeof f->base, "%s", base);
    SMap *idx = calloc(1, sizeof *idx);
    f->next = vm->fams; vm->fams = f;
    BBModule *m0 = vm_module(vm, base);
    int misses = 0;
    for (int i = 0; misses < 3 && i < 200; i++) {
        BBModule *m;
        if (i == 0) m = m0;
        else { char nm[128]; snprintf(nm, sizeof nm, "%s-%d", base, i); m = vm_module(vm, nm); }
        if (!m) { if (i) misses++; continue; }
        misses = 0;
        f->mods = realloc(f->mods, (f->n + 1) * sizeof *f->mods);
        f->mods[f->n++] = m;
        for (int c = 0; c < m->ncls; c++) if (!smap_get(idx, m->cls[c].full)) smap_put(idx, m->cls[c].full, &m->cls[c]);
    }
    /* stash the index in a trailing module pointer slot */
    f->mods = realloc(f->mods, (f->n + 1) * sizeof *f->mods);
    f->mods[f->n] = (BBModule *)idx;
    return f;
}

static SMap *fam_index(Family *f) { return (SMap *)f->mods[f->n]; }

BBClass *vm_class(BBVM *vm, const char *family, const char *full) {
    Family *f = vm_family(vm, family);
    return f ? smap_get(fam_index(f), full) : NULL;
}

BBClass *vm_find_class_anywhere(BBVM *vm, const char *full) {
    for (Family *f = vm->fams; f; f = f->next) {
        BBClass *k = smap_get(fam_index(f), full);
        if (k) return k;
    }
    return NULL;
}

BBModule *vm_mod_ord(BBVM *vm, BBModule *m, int ord) {
    if (ord == 0) return m;
    if (ord < 0 || ord >= m->nmod) return NULL;
    if (!m->mods[ord]) {
        const char *nm = cod_cstr(m, m->modNameOff[ord]);
        m->mods[ord] = vm_module(vm, nm);
        if (!m->mods[ord]) vm_log(vm, "missing module %s (needed by %s)", nm, m->name);
    }
    return m->mods[ord];
}

static int same_family(BBModule *a, BBModule *b) {
    char x[96], y[96];
    base_name(a->name, x, sizeof x); base_name(b->name, y, sizeof y);
    return !strcmp(x, y);
}

BBClass *vm_resolve_cref(BBVM *vm, BBModule *m, int i) {
    BBClassRef *cr = &m->crefs[i];
    if (cr->resolved) return cr->resolved;
    char full[256];
    snprintf(full, sizeof full, "%s%s%s", cr->pkg, *cr->pkg ? "." : "", cr->cls);
    BBModule *t = vm_mod_ord(vm, m, cr->mod);
    BBClass *k = NULL;
    if (t) k = vm_class(vm, t->name, full);
    if (!k) k = vm_find_class_anywhere(vm, full);
    if (!k) vm_log(vm, "unresolved class %s (from %s)", full, m->name);
    cr->resolved = k;
    return k;
}

BBClass *vm_cls_direct(BBVM *vm, BBModule *m, int mo, int co) {
    BBModule *t = vm_mod_ord(vm, m, mo);
    if (!t || co >= t->ncls) return NULL;
    return &t->cls[co];
}

BBClass *vm_cls_ref(BBVM *vm, BBModule *m, int mo, int co) {
    if (mo == 255) return NULL;
    if (mo == 0) return co < m->ncls ? &m->cls[co] : NULL;
    BBModule *t = vm_mod_ord(vm, m, mo);
    if (!t) return NULL;
    if (same_family(t, m)) return co < t->ncls ? &t->cls[co] : NULL;
    for (int i = co; i < m->ncref; i += 256)
        if (m->crefs[i].mod == mo) return vm_resolve_cref(vm, m, i);
    return co < t->ncls ? &t->cls[co] : NULL;
}

/* ------------------------------------------------------------------ */
/* signatures                                                          */

const char *vm_sig(BBVM *vm, BBModule *m, uint16_t tl, int skipThis, char *buf, size_t n) {
    BBType t[96];
    int c = cod_typelist(m, tl, t, 96);
    size_t o = 0;
    buf[0] = 0;
    for (int i = skipThis ? 1 : 0; i < c && i < 96; i++) {
        char one[300];
        const char *code;
        BBType *y = &t[i];
        int dims = 0, k = y->kind;
        BBClass *cls = NULL;
        if (k == BT_ARRAY) { dims = y->dims; k = y->elem; }
        if (k == BT_CLASS || k == BT_CLASS9 || (y->kind == BT_ARRAY && y->elem == 7)) cls = vm_cls_ref(vm, m, y->m, y->c);
        switch (k) {
        case BT_BOOL: code = "Z"; break; case BT_BYTE: code = "B"; break; case BT_CHAR: code = "C"; break;
        case BT_SHORT: code = "S"; break; case BT_INT: code = "I"; break; case BT_LONG: code = "J"; break;
        case BT_FLOAT: code = "F"; break; case BT_DOUBLE: code = "D"; break;
        case 7: case 9: snprintf(one, sizeof one, "L%s;", cls ? cls->full : "?"); code = one; break;
        default: snprintf(one, sizeof one, "<%d>", k); code = one; break;
        }
        for (int d = 0; d < dims && o + 1 < n; d++) buf[o++] = '[';
        size_t l = strlen(code);
        if (o + l + 1 >= n) break;
        memcpy(buf + o, code, l); o += l; buf[o] = 0;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* class linking                                                       */

int vm_is_subclass(BBClass *k, BBClass *of) {
    for (; k; k = k->super) {
        if (k == of) return 1;
        for (int i = 0; i < k->nif; i++) if (k->ifaces && k->ifaces[i] && vm_is_subclass(k->ifaces[i], of)) return 1;
    }
    return 0;
}

static int same_method(BBVM *vm, BBRoutine *a, BBRoutine *b) {
    if (strcmp(a->name, b->name)) return 0;
    if (a->nargs != b->nargs) return 0;
    char sa[512], sb[512];
    vm_sig(vm, a->mod, a->proto, 1, sa, sizeof sa);
    vm_sig(vm, b->mod, b->proto, 1, sb, sizeof sb);
    return !strcmp(sa, sb);
}

int vm_link(BBVM *vm, BBClass *k) {
    if (!k) return 0;
    if (k->linked) return 1;
    k->linked = 1;
    BBModule *m = k->mod;
    k->super = vm_cls_ref(vm, m, k->supM, k->supC);
    if (k->super) vm_link(vm, k->super);
    k->ifaces = calloc(k->nif + 1, sizeof *k->ifaces);
    for (int i = 0; i < k->nif; i++) {
        k->ifaces[i] = vm_cls_ref(vm, m, k->ifs[i][0], k->ifs[i][1]);
        if (k->ifaces[i]) vm_link(vm, k->ifaces[i]);
    }
    int slot = k->super ? k->super->nslots : 0;
    for (int i = 0; i < k->nf; i++) { k->f[i].slot = (uint16_t)slot; slot += k->f[i].wide ? 2 : 1; }
    k->nslots = slot;
    /* vtable: inherit, override by name+signature, append new */
    int cap = (k->super ? k->super->vtsize : 0) + k->nv + 1;
    k->vtable = calloc(cap, sizeof *k->vtable);
    if (k->super && k->super->vtsize) {
        memcpy(k->vtable, k->super->vtable, k->super->vtsize * sizeof *k->vtable);
        k->vtsize = k->super->vtsize;
    }
    for (int i = 0; i < k->nv; i++) {
        BBRoutine *r = k->v[i];
        if (!r) continue;
        int found = -1;
        for (int j = 0; j < k->vtsize; j++)
            if (k->vtable[j] && same_method(vm, k->vtable[j], r)) { found = j; break; }
        if (found < 0) found = k->vtsize++;
        k->vtable[found] = r;
        r->vindex = (int16_t)found;
    }
    if (vm->trace & 8) vm_log(vm, "link %s: %d slots, vtable %d", k->full, k->nslots, k->vtsize);
    return 1;
}

BBRoutine *vm_find_method(BBVM *vm, BBClass *k, const char *name, const char *sig) {
    for (BBClass *c = k; c; c = c->super) {
        vm_link(vm, c);
        BBRoutine **lists[3] = { c->v, c->nv_, c->s };
        int cnt[3] = { c->nv, c->nnv, c->ns };
        for (int l = 0; l < 3; l++) for (int i = 0; i < cnt[l]; i++) {
            BBRoutine *r = lists[l][i];
            if (!r || strcmp(r->name, name)) continue;
            if (!sig) return r;
            char s[512];
            vm_sig(vm, r->mod, r->proto, !r->isStatic || !strcmp(r->name, "<init>"), s, sizeof s);
            if (!strcmp(s, sig)) return r;
        }
    }
    /* interfaces (abstract declarations) */
    for (BBClass *c = k; c; c = c->super)
        for (int i = 0; i < c->nif; i++) if (c->ifaces && c->ifaces[i]) {
            BBRoutine *r = vm_find_method(vm, c->ifaces[i], name, sig);
            if (r) return r;
        }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* heap                                                               */

Obj *vm_obj(BBVM *vm, ref_t r) {
    if (r <= REF_BASE) return NULL;
    uint32_t i = r - REF_BASE;
    if (i >= vm->nobjs) return NULL;
    return vm->objs[i];
}

static ref_t heap_add(BBVM *vm, Obj *o, size_t bytes) {
    uint32_t i;
    if (vm->nfree) i = vm->freeList[--vm->nfree];
    else {
        if (vm->nobjs == vm->capobjs) {
            vm->capobjs *= 2;
            vm->objs = realloc(vm->objs, vm->capobjs * sizeof *vm->objs);
        }
        i = vm->nobjs++;
    }
    vm->objs[i] = o;
    vm->heapBytes += bytes;
    o->hash = (i * 2654435761u) >> 1;
    return REF_BASE + i;
}

ref_t vm_new_obj(BBVM *vm, BBClass *k) {
    vm_link(vm, k);
    int n = k->nslots;
    size_t bytes = sizeof(Obj) + (size_t)(n + 1) * 4;
    Obj *o = calloc(1, bytes);
    o->cls = k; o->kind = OK_INST; o->len = (uint32_t)n;
    return heap_add(vm, o, bytes);
}

int vm_elem_size(int et) {
    switch (et) {
    case BT_BOOL: case BT_BYTE: return 1;
    case BT_CHAR: case BT_SHORT: return 2;
    case BT_LONG: case BT_DOUBLE: return 8;
    default: return 4;
    }
}

ref_t vm_new_array(BBVM *vm, int et, BBClass *ecls, int dims, uint32_t len) {
    size_t bytes = sizeof(Obj) + (size_t)len * vm_elem_size(et) + 8;
    Obj *o = calloc(1, bytes);
    o->kind = OK_ARRAY; o->et = (uint8_t)et; o->cls = ecls; o->dims = (uint8_t)dims; o->len = len;
    return heap_add(vm, o, bytes);
}

ref_t vm_new_string16(BBVM *vm, const uint16_t *s, uint32_t n) {
    int wide = 0;
    for (uint32_t i = 0; i < n; i++) if (s[i] > 0xff) { wide = 1; break; }
    ref_t r = vm_new_array(vm, wide ? BT_CHAR : BT_BYTE, NULL, 1, n);
    Obj *o = vm_obj(vm, r);
    o->flags |= OF_STRING; o->cls = vm->cString;
    for (uint32_t i = 0; i < n; i++) {
        if (wide) ((uint16_t *)o->d)[i] = s[i];
        else ((uint8_t *)o->d)[i] = (uint8_t)s[i];
    }
    return r;
}

ref_t vm_new_string(BBVM *vm, const char *u) {
    size_t n = strlen(u);
    uint16_t *b = malloc((n + 1) * 2);
    uint32_t k = 0;
    for (size_t i = 0; i < n;) {
        uint8_t c = (uint8_t)u[i];
        if (c < 0x80) { b[k++] = c; i++; }
        else if ((c & 0xe0) == 0xc0 && i + 1 < n) { b[k++] = (uint16_t)(((c & 0x1f) << 6) | (u[i + 1] & 0x3f)); i += 2; }
        else if ((c & 0xf0) == 0xe0 && i + 2 < n) { b[k++] = (uint16_t)(((c & 0x0f) << 12) | ((u[i + 1] & 0x3f) << 6) | (u[i + 2] & 0x3f)); i += 3; }
        else { b[k++] = c; i++; }
    }
    ref_t r = vm_new_string16(vm, b, k);
    free(b);
    return r;
}

int vm_string_utf8(BBVM *vm, ref_t s, char *buf, size_t n) {
    Obj *o = vm_obj(vm, s);
    if (!o || o->kind != OK_ARRAY) { snprintf(buf, n, "%s", o ? "<obj>" : "null"); return 0; }
    size_t k = 0;
    for (uint32_t i = 0; i < o->len; i++) {
        unsigned c = o->et == BT_CHAR ? ((uint16_t *)o->d)[i] : ((uint8_t *)o->d)[i];
        char tmp[4]; int l;
        if (c < 0x80) { tmp[0] = (char)c; l = 1; }
        else if (c < 0x800) { tmp[0] = (char)(0xc0 | c >> 6); tmp[1] = (char)(0x80 | (c & 0x3f)); l = 2; }
        else { tmp[0] = (char)(0xe0 | c >> 12); tmp[1] = (char)(0x80 | ((c >> 6) & 0x3f)); tmp[2] = (char)(0x80 | (c & 0x3f)); l = 3; }
        if (k + l + 1 >= n) break;
        memcpy(buf + k, tmp, l); k += l;
    }
    buf[k] = 0;
    return (int)k;
}

ref_t vm_class_obj(BBVM *vm, BBClass *k) {
    if (!k) return 0;
    if (k->classObj) return (ref_t)(uintptr_t)k->classObj;
    if (!vm->cClass) vm->cClass = vm_find_class_anywhere(vm, "java.lang.Class");
    ref_t r = vm->cClass ? vm_new_obj(vm, vm->cClass) : vm_new_array(vm, BT_INT, NULL, 1, 0);
    Obj *o = vm_obj(vm, r);
    o->host = k; o->flags |= OF_CLASS | OF_PERM;
    k->classObj = (void *)(uintptr_t)r;
    return r;
}

int vm_instanceof(BBVM *vm, ref_t r, BBClass *k) {
    Obj *o = vm_obj(vm, r);
    if (!o || !k) return 0;
    if (o->kind == OK_INST) { vm_link(vm, o->cls); return vm_is_subclass(o->cls, k); }
    if (o->flags & OF_STRING) return k == vm->cString || k == vm->cObject || vm_is_subclass(vm->cString, k);
    return k == vm->cObject;
}

/* ---- garbage collection (mark/sweep, conservative on stacks) ---- */

static void mark_ref(BBVM *vm, uint32_t v, uint32_t **stk, size_t *sn, size_t *sc) {
    if (v <= REF_BASE) return;
    uint32_t i = v - REF_BASE;
    if (i >= vm->nobjs) return;
    Obj *o = vm->objs[i];
    if (!o || (o->flags & OF_MARK)) return;
    o->flags |= OF_MARK;
    if (*sn == *sc) { *sc = *sc ? *sc * 2 : 4096; *stk = realloc(*stk, *sc * 4); }
    (*stk)[(*sn)++] = v;
}

void vm_gc(BBVM *vm) {
    uint32_t *stk = NULL; size_t sn = 0, sc = 0;
    for (uint32_t i = 1; i < vm->nobjs; i++) {
        Obj *o = vm->objs[i];
        if (o && (o->flags & OF_PERM)) { o->flags &= ~OF_MARK; mark_ref(vm, REF_BASE + i, &stk, &sn, &sc); }
    }
    for (Thread *t = vm->threads; t; t = t->next) {
        mark_ref(vm, t->obj, &stk, &sn, &sc);
        mark_ref(vm, t->pendingEx, &stk, &sn, &sc);
        mark_ref(vm, t->waitObj, &stk, &sn, &sc);
        mark_ref(vm, t->joinTarget, &stk, &sn, &sc);
        mark_ref(vm, t->tsd, &stk, &sn, &sc);
        uint32_t *top = t->nfr ? t->fr[t->nfr - 1].sp : t->stack;
        for (uint32_t *p = t->stack; p < top + 4 && p < t->stackEnd; p++) mark_ref(vm, *p, &stk, &sn, &sc);
        for (int f = 0; f < t->nfr; f++) mark_ref(vm, t->fr[f].lock, &stk, &sn, &sc);
    }
    for (BBModule *m = vm->mods; m; m = m->next)
        for (int i = 0; i < m->staticSize + 2; i++) mark_ref(vm, m->statics[i], &stk, &sn, &sc);
    for (int p = 0; p < vm->nps; p++) if (vm->ps[p]) {
        PState *ps = vm->ps[p];
        for (BBModule *m = vm->mods; m; m = m->next)
            if (m->gidx < ps->nst && ps->st[m->gidx])
                for (int i = 0; i < m->staticSize + 2; i++) mark_ref(vm, ps->st[m->gidx][i], &stk, &sn, &sc);
    }
    for (int i = 0; i < vm->nlits; i++) mark_ref(vm, vm->lits[i].r, &stk, &sn, &sc);
    for (Monitor *mo = vm->monitors; mo; mo = mo->next) mark_ref(vm, mo->obj, &stk, &sn, &sc);
    while (sn) {
        Obj *o = vm_obj(vm, stk[--sn]);
        if (o->kind == OK_INST) for (uint32_t i = 0; i < o->len; i++) mark_ref(vm, o->d[i], &stk, &sn, &sc);
        else if (o->et == BT_CLASS || o->et == BT_ARRAY) for (uint32_t i = 0; i < o->len; i++) mark_ref(vm, o->d[i], &stk, &sn, &sc);
    }
    free(stk);
    size_t live = 0; uint32_t freed = 0;
    for (uint32_t i = 1; i < vm->nobjs; i++) {
        Obj *o = vm->objs[i];
        if (!o) continue;
        if (o->flags & OF_MARK) {
            o->flags &= ~OF_MARK;
            live += sizeof(Obj) + (o->kind == OK_INST ? o->len * 4 : (size_t)o->len * vm_elem_size(o->et));
            continue;
        }
        if (o->hostFree) o->hostFree(o->host);
        free(o);
        vm->objs[i] = NULL;
        if (vm->nfree == vm->capfree) { vm->capfree = vm->capfree ? vm->capfree * 2 : 4096; vm->freeList = realloc(vm->freeList, vm->capfree * 4); }
        vm->freeList[vm->nfree++] = i;
        freed++;
    }
    vm->heapBytes = live;
    vm->gcAt = live * 2 + (32u << 20);
    if (vm->trace & 2) vm_log(vm, "gc: freed %u, live %zu KB", freed, live / 1024);
}

/* ------------------------------------------------------------------ */
/* statics                                                             */

PState *vm_ps(BBVM *vm, int pid) {
    if (pid < 0) pid = 0;
    if (pid >= vm->nps) {
        int n = pid + 64;
        vm->ps = realloc(vm->ps, (size_t)n * sizeof *vm->ps);
        memset(vm->ps + vm->nps, 0, (size_t)(n - vm->nps) * sizeof *vm->ps);
        vm->nps = n;
    }
    if (!vm->ps[pid]) vm->ps[pid] = calloc(1, sizeof(PState));
    return vm->ps[pid];
}

uint32_t *vm_statics(BBVM *vm, int pid, BBModule *m) {
    PState *p = vm_ps(vm, pid);
    if (m->gidx >= p->nst) {
        int n = vm->nmodsG + 256;
        p->st = realloc(p->st, (size_t)n * sizeof *p->st);
        memset(p->st + p->nst, 0, (size_t)(n - p->nst) * sizeof *p->st);
        p->nst = n;
    }
    if (!p->st[m->gidx]) {
        size_t sz = (size_t)(m->staticSize + 2) * sizeof(uint32_t);
        p->st[m->gidx] = malloc(sz);
        memcpy(p->st[m->gidx], m->statics, sz);
    }
    return p->st[m->gidx];
}

void vm_cinit_set(BBVM *vm, int pid, const BBClass *k, int state, Thread *by) {
    PState *p = vm_ps(vm, pid);
    if (k->gidx >= p->nci) {
        int n = vm->nclsG + 1024;
        p->ci = realloc(p->ci, (size_t)n);
        memset(p->ci + p->nci, 0, (size_t)(n - p->nci));
        p->cith = realloc(p->cith, (size_t)n * sizeof *p->cith);
        memset(p->cith + p->nci, 0, (size_t)(n - p->nci) * sizeof *p->cith);
        p->nci = n;
    }
    p->ci[k->gidx] = (uint8_t)state;
    p->cith[k->gidx] = by;
}

ref_t vm_class_lock(BBVM *vm, int pid, BBClass *k) {
    if (!k) return 0;
    PState *p = vm_ps(vm, pid);
    if (k->gidx >= p->nlocks) {
        int n = vm->nclsG + 1024;
        p->locks = realloc(p->locks, (size_t)n * sizeof *p->locks);
        memset(p->locks + p->nlocks, 0, (size_t)(n - p->nlocks) * sizeof *p->locks);
        p->nlocks = n;
    }
    if (!p->locks[k->gidx]) {
        ref_t o = vm_new_obj(vm, vm->cObject);
        vm_obj(vm, o)->flags |= OF_PERM;
        p->locks[k->gidx] = o;
    }
    return p->locks[k->gidx];
}

Thread *vm_cinit_thread(BBVM *vm, int pid, const BBClass *k) {
    PState *p = (unsigned)pid < (unsigned)vm->nps ? vm->ps[pid] : NULL;
    return p && k->gidx < p->nci ? p->cith[k->gidx] : NULL;
}

static void init_statics(BBVM *vm, BBModule *m) {
    const uint8_t *d = m->data;
    uint32_t p = rd16(d + 10), end = rd16(d + 12);
    int strs = 0;
    while (p + 6 <= end) {
        uint16_t addr = rd16(d + p);
        uint32_t val = rd32(d + p + 2);
        p += 6;
        if (addr == 0xffff) { strs = 1; continue; }
        if (addr >= m->staticSize + 2) continue;
        if (!strs) m->statics[addr] = val;
        else {
            /* deferred: string literal; created on first use by the interpreter */
            extern ref_t interp_literal(BBVM *vm, BBModule *m, uint16_t off, int unicode);
            m->statics[addr] = interp_literal(vm, m, (uint16_t)val, -1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* monitors                                                            */

Monitor *vm_monitor(BBVM *vm, ref_t o, int create) {
    for (Monitor *m = vm->monitors; m; m = m->next) if (m->obj == o) return m;
    if (!create) return NULL;
    Monitor *m = calloc(1, sizeof *m);
    m->obj = o; m->next = vm->monitors; vm->monitors = m;
    return m;
}

int vm_monitor_enter(BBVM *vm, Thread *t, ref_t o) {
    Monitor *m = vm_monitor(vm, o, 1);
    if (!m->owner || m->owner == t) { m->owner = t; m->count++; return 1; }
    t->state = TS_MONITOR; t->waitObj = o;
    return 0;
}

int vm_monitor_exit(BBVM *vm, Thread *t, ref_t o) {
    Monitor *m = vm_monitor(vm, o, 0);
    if (!m || m->owner != t) return 0;
    if (--m->count == 0) {
        m->owner = NULL;
        /* drop idle monitors */
        int waiting = 0;
        for (Thread *x = vm->threads; x; x = x->next)
            if ((x->state == TS_MONITOR || x->state == TS_WAIT) && x->waitObj == o) { waiting = 1; break; }
        if (!waiting) {
            Monitor **pp = &vm->monitors;
            while (*pp && *pp != m) pp = &(*pp)->next;
            if (*pp) { *pp = m->next; free(m); }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* threads                                                             */

Thread *vm_new_thread(BBVM *vm, ref_t threadObj) {
    Thread *t = calloc(1, sizeof *t);
    t->id = vm->nextTid++;
    t->obj = threadObj;
    size_t slots = 64 * 1024;
    t->stack = calloc(slots, 4);
    t->stackEnd = t->stack + slots;
    t->maxfr = 1024;
    t->fr = calloc(t->maxfr, sizeof *t->fr);
    Thread **pp = &vm->threads;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    return t;
}

void vm_free(BBVM *vm) {
    if (!vm) return;
    for (uint32_t i = 1; i < vm->nobjs; i++) if (vm->objs[i]) {
        if (vm->objs[i]->hostFree) vm->objs[i]->hostFree(vm->objs[i]->host);
        free(vm->objs[i]);
    }
    free(vm->objs); free(vm->freeList);
    while (vm->threads) { Thread *t = vm->threads; vm->threads = t->next; free(t->stack); free(t->fr); free(t); }
    while (vm->mods) { BBModule *m = vm->mods; vm->mods = m->next; cod_free(m); }
    for (int i = 0; i < vm->ndirs; i++) free(vm->dirs[i]);
    free(vm->lits);
    free(vm);
}
