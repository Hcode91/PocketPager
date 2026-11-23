/* PocketPager - BlackBerry OS family: RIM .cod module parser. */
#include "cod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "idtab.inc"
#include "optab.inc"

const char *cod_op_name(int op) { return (op >= 0 && op < 0x122) ? kOpName[op] : "?"; }

/* ---- name arena: data offset -> decoded string ---- */
typedef struct { uint16_t off; uint16_t used; char *s; } NameSlot;
typedef struct { NameSlot *slots; int cap, n; } NameMap;

static NameMap *names_of(BBModule *m);

/* store name maps outside the public struct */
typedef struct Extra { NameMap names, raw; } Extra;

/* A tiny side table: module -> extras. Modules are few, a list is fine. */
typedef struct ExtraNode { BBModule *m; Extra x; struct ExtraNode *next; } ExtraNode;
static ExtraNode *g_extra;
static Extra *extra(BBModule *m) {
    for (ExtraNode *e = g_extra; e; e = e->next) if (e->m == m) return &e->x;
    ExtraNode *e = calloc(1, sizeof *e);
    e->m = m; e->next = g_extra; g_extra = e;
    return &e->x;
}
static NameMap *names_of(BBModule *m) { return &extra(m)->names; }

static char **map_get(NameMap *nm, uint16_t off) {
    if (nm->cap == 0) { nm->cap = 256; nm->slots = calloc(nm->cap, sizeof(NameSlot)); }
    if (nm->n * 2 >= nm->cap) {
        NameMap big = { calloc(nm->cap * 2, sizeof(NameSlot)), nm->cap * 2, 0 };
        for (int i = 0; i < nm->cap; i++) if (nm->slots[i].used) {
            unsigned h = (nm->slots[i].off * 2654435761u) & (big.cap - 1);
            while (big.slots[h].used) h = (h + 1) & (big.cap - 1);
            big.slots[h] = nm->slots[i]; big.n++;
        }
        free(nm->slots); *nm = big;
    }
    unsigned h = (off * 2654435761u) & (nm->cap - 1);
    while (nm->slots[h].used && nm->slots[h].off != off) h = (h + 1) & (nm->cap - 1);
    if (!nm->slots[h].used) { nm->slots[h].used = 1; nm->slots[h].off = off; nm->slots[h].s = NULL; nm->n++; }
    return &nm->slots[h].s;
}

const char *cod_name(BBModule *m, uint16_t o) {
    if (o == 0 || o == 0xffff || o >= m->dataSize) return "";
    char **slot = map_get(names_of(m), o);
    if (*slot) return *slot;
    size_t cap = 64, n = 0;
    char *s = malloc(cap);
    for (uint32_t i = o; i < m->dataSize && m->data[i]; i++) {
        const char *piece; char one[2];
        uint8_t c = m->data[i];
        if (c == 0xff && i + 1 < m->dataSize) { one[0] = (char)m->data[++i]; one[1] = 0; piece = one; }
        else piece = kIdDecode[c];
        size_t l = strlen(piece);
        if (n + l + 1 > cap) { cap = (n + l + 1) * 2; s = realloc(s, cap); }
        memcpy(s + n, piece, l); n += l;
    }
    s[n] = 0;
    *slot = s;
    return s;
}

const char *cod_cstr(BBModule *m, uint16_t o) {
    if (o >= m->dataSize) return "";
    return (const char *)m->data + o;
}

/* ---- type lists ---- */
int cod_typelist(BBModule *m, uint16_t o, BBType *out, int max) {
    if (o == 0 || o == 0xffff || o >= m->dataSize) return 0;
    const uint8_t *d = m->data;
    uint32_t p = o;
    int i = d[p++], l;
    if (!(i & 0x80)) l = (i & 0x70) >> 4;
    else {
        l = i & 0x7f; i = d[p++];
        if (l & 0x40) { l &= ~0x40; l <<= 4; l += (i & 0xf0) >> 4; }
        l--;
    }
    if (l <= 0) return 0;
    l--;
    uint32_t end = p + l;
    int n = 0;
    int t = i;
    for (int first = 1;; first = 0) {
        if (!first) { if (p >= end) break; t = d[p++]; }
        BBType ty = {0};
        int k = t & 0xf;
        switch (k) {
        case 7: case 9: ty.kind = BT_CLASS; ty.m = d[p]; ty.c = d[p + 1]; p += 2; break;
        case 8:
            ty.kind = BT_ARRAY; ty.dims = d[p]; ty.elem = d[p + 1]; p += 2;
            if (ty.elem == 7) { ty.m = d[p]; ty.c = d[p + 1]; p += 2; }
            break;
        default: ty.kind = (uint8_t)k; break;
        }
        int rep = first ? 1 : (t >> 4) + 1;
        while (rep-- > 0) { if (n < max) out[n] = ty; n++; }
    }
    return n;
}

int cod_typeslots(BBModule *m, uint16_t o) {
    BBType t[64];
    int n = cod_typelist(m, o, t, 64), s = 0;
    for (int i = 0; i < n && i < 64; i++) s += (t[i].kind == BT_LONG || t[i].kind == BT_DOUBLE) ? 2 : 1;
    return s;
}

/* ---- instruction sizes ---- */
int cod_insn_size(const uint8_t *c, uint32_t pc, int *opOut) {
    int op = c[pc], pre = 0;
    if (op == 0xd8) { op = 0x100 + c[pc + 1]; pre = 1; }
    if (opOut) *opOut = op;
    int z;
    switch (op) {
    case 0xa3: z = 3 + rd16(c + pc + pre + 1) * 4 + 2; break;  /* lookupswitch_short */
    case 0xa4: z = 3 + rd16(c + pc + pre + 1) * 6 + 2; break;  /* lookupswitch */
    case 0x2f: z = 3 + 4 + 2 * rd16(c + pc + pre + 1); break;  /* tableswitch */
    case 0x11a: z = 3 + 2 * rd16(c + pc + pre + 1); break;     /* stringarrayinit */
    case 0x2d: z = 6; break;                                    /* arrayinit */
    default:
        z = op < 0x122 ? kOpSize[op] : 1;
        if (z <= 0) z = 1;
        break;
    }
    return z + pre;
}

/* ---- parsing ---- */
static int fail(char *err, size_t n, const char *fmt, ...) {
    if (err && n) { va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap); }
    return 0;
}

static uint32_t vint(const uint8_t *d, uint32_t *p) {
    uint32_t v = 0; int sh = 0; uint8_t k;
    do { k = d[(*p)++]; v |= (uint32_t)(k & 0x7f) << sh; sh += 7; } while (k & 0x80);
    return v & 0xffff;
}

typedef struct { BBFixup *a; int n, cap; } FixVec;
static void fix_add(FixVec *v, uint16_t off, int kind, uint32_t idx) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 256; v->a = realloc(v->a, v->cap * sizeof *v->a); }
    v->a[v->n].off = off; v->a[v->n].kind = (uint8_t)kind; v->a[v->n].pad = 0; v->a[v->n].idx = idx; v->n++;
}
static uint32_t g_limit;   /* data size of the module being parsed */
static uint32_t fix_entry(const uint8_t *d, uint32_t p, FixVec *v, int kind, uint32_t idx) {
    if (p >= g_limit) return g_limit + 1;
    uint32_t ext = vint(d, &p), end = p + ext, acc = 0;
    if (end > g_limit) return g_limit + 1;
    while (p < end) { acc += vint(d, &p); fix_add(v, (uint16_t)acc, kind, idx); }
    return end;
}

static int cref_index(BBModule *m, uint16_t off) {
    for (int i = 0; i < m->ncref; i++) if (m->crefOff[i] == off) return i;
    return -1;
}

static void mref_push(BBModule *m, BBMemberRef r) {
    m->mrefs = realloc(m->mrefs, (m->nmref + 1) * sizeof *m->mrefs);
    m->mrefs[m->nmref++] = r;
}

static uint32_t member_table(BBModule *m, uint32_t p, int adv, int kind, FixVec *fv) {
    const uint8_t *d = m->data;
    if (p + 2 > g_limit) return g_limit + 1;
    int n = (int16_t)rd16(d + p); p += 2;
    if (n < 0) n = -n;
    for (int i = 0; i < n && p < g_limit; i++) {
        p += p & 1;
        BBMemberRef r = {0};
        r.cref = cref_index(m, rd16(d + p));
        r.name = cod_name(m, rd16(d + p + 2));
        r.tl = rd16(d + p + 4);
        r.tl2 = adv ? rd16(d + p + 6) : 0;
        p += adv ? 8 : 6;
        mref_push(m, r);
        p = fix_entry(d, p, fv, kind, (uint32_t)(m->nmref - 1));
    }
    return p;
}

static int cmp_fix(const void *a, const void *b) {
    const BBFixup *x = a, *y = b;
    return (int)x->off - (int)y->off;
}
static int cmp_rt(const void *a, const void *b) {
    const BBRoutine *x = a, *y = b;
    return (int)x->off - (int)y->off;
}

static void fill_routine(BBModule *m, BBRoutine *r, uint16_t off) {
    const uint8_t *c = m->code;
    r->mod = m; r->off = off; r->vindex = -1; r->name = "";
    if (off < 9 || off > m->codeSize) return;
    if (c[off - 5] > 1) {
        uint32_t h = off - 9u;
        r->type = rd16(c + h); r->proto = rd16(c + h + 2);
        r->size = (uint16_t)(c[h + 4] - 2); r->attr = c[h + 5];
        r->name = cod_name(m, rd16(c + h + 6));
        uint8_t pk = c[h + 8];
        r->nstack = pk >> 6; r->maxl = (pk >> 4) & 3; r->maxs = pk & 3;
    } else {
        if (off < 14) return;
        uint32_t h = off - 14u;
        r->name = cod_name(m, rd16(c + h)); r->proto = rd16(c + h + 2); r->type = rd16(c + h + 4);
        r->size = rd16(c + h + 6); r->attr = rd16(c + h + 8);
        r->nstack = c[h + 10]; r->maxl = c[h + 11]; r->maxs = c[h + 13];
    }
    uint32_t e = (uint32_t)off + r->size;
    if (m->version < 78) e += e & 1;
    r->exOff = (r->attr & 0x40) ? (uint16_t)e : 0;
    r->nargs = (int16_t)cod_typeslots(m, r->proto);
    BBType t[2];
    int n = cod_typelist(m, r->type, t, 2);
    r->ret = n == 0 ? 0 : (t[0].kind == BT_LONG || t[0].kind == BT_DOUBLE) ? 2 : 1;
}

BBModule *cod_parse(const char *name, uint8_t *buf, size_t size, char *err, size_t errn) {
    if (size < 44 || rd32(buf) != 0xffffc0deu) { fail(err, errn, "%s: not a cod module", name); return NULL; }
    BBModule *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    /* pad the image so that malformed tables cannot read past the end
     * (files loaded into the module region come padded already) */
#ifdef BBARENA_H
    int owned = bb_code_owns(buf);
#else
    int owned = 0;
#endif
    if (!owned) {
        uint8_t *pb = realloc(buf, size + COD_PAD);
        if (!pb) { free(m); fail(err, errn, "%s: out of memory", name); return NULL; }
        memset(pb + size, 0, COD_PAD);
        buf = pb;
    }
    m->raw = buf; m->rawSize = size;
    m->version = rd16(buf + 36);
    m->codeSize = rd16(buf + 38);
    m->dataSize = rd16(buf + 40);
    m->flags = rd16(buf + 42);
    if (44u + m->codeSize + m->dataSize > size) { fail(err, errn, "%s: truncated", name); free(m); return NULL; }
    m->code = buf + 44;
    m->data = buf + 44 + m->codeSize;
    g_limit = m->dataSize;
    const uint8_t *d = m->data;
    m->dflags = d[0]; m->dver = d[1];
    m->nmod = d[4]; m->ncls = d[5];
    uint16_t o_export = rd16(d + 6), o_databytes = rd16(d + 8);
    uint16_t o_classdefs = rd16(d + 12);
    m->o_typelists = rd16(d + 14); m->o_ifrefs = rd16(d + 16);
    uint16_t o_classrefs = rd16(d + 18), o_rfix = rd16(d + 20), o_srfix = rd16(d + 22), o_vrfix = rd16(d + 24);
    uint16_t o_ccfix = rd16(d + 26), o_ffix = rd16(d + 30), o_lffix = rd16(d + 32);
    uint16_t o_sffix = rd16(d + 34), o_mcfix = rd16(d + 36);
    m->staticSize = rd16(d + 38);
    (void)o_export; (void)o_databytes; (void)o_classdefs;

    uint32_t p = 52;
    uint16_t *classOff = malloc((m->ncls + 1) * sizeof *classOff);
    for (int i = 0; i < m->ncls; i++, p += 2) classOff[i] = rd16(d + p);
    m->modNameOff = malloc((m->nmod + 1) * sizeof *m->modNameOff);
    for (int i = 0; i < m->nmod; i++, p += 2) m->modNameOff[i] = rd16(d + p);
    m->mods = calloc(m->nmod + 1, sizeof *m->mods);
    m->mods[0] = m;

    /* class refs */
    m->ncref = (o_rfix - o_classrefs) / 8;
    m->crefs = calloc(m->ncref + 1, sizeof *m->crefs);
    m->crefOff = calloc(m->ncref + 1, sizeof *m->crefOff);
    for (int i = 0; i < m->ncref; i++) {
        uint32_t q = o_classrefs + 8u * i;
        m->crefOff[i] = (uint16_t)q;
        m->crefs[i].mod = rd16(d + q);
        m->crefs[i].pkg = cod_name(m, rd16(d + q + 2));
        m->crefs[i].cls = cod_name(m, rd16(d + q + 4));
    }

    /* classes and their routines */
    m->cls = calloc(m->ncls + 1, sizeof *m->cls);
    int rcap = 64, rn = 0;
    uint16_t *roff = malloc(rcap * sizeof *roff);
    for (int ci = 0; ci < m->ncls; ci++) {
        BBClass *k = &m->cls[ci];
        uint32_t co = classOff[ci];
        k->mod = m; k->ord = ci;
        k->pkg = cod_name(m, rd16(d + co));
        k->name = cod_name(m, rd16(d + co + 2));
        size_t fl = strlen(k->pkg) + strlen(k->name) + 2;
        k->full = malloc(fl);
        snprintf(k->full, fl, "%s%s%s", k->pkg, *k->pkg ? "." : "", k->name);
        k->supM = d[co + 4]; k->supC = d[co + 5];
        k->clinitOff = rd16(d + co + 8); k->initOff = rd16(d + co + 10);
        k->size = rd16(d + co + 12);
        k->attr = rd16(d + co + 22);
        uint32_t of[8];
        for (int j = 0; j < 8; j++) of[j] = rd16(d + co + 24 + 2 * j) + co;
        k->nv = (of[1] - of[0]) / 2; k->nnv = (of[2] - of[1]) / 2; k->ns = (of[3] - of[2]) / 2;
        k->nf = (of[4] - of[3]) / 4; k->nsf = (of[5] - of[4]) / 6; k->nif = (of[6] - of[5]) / 2;
        k->f = calloc(k->nf + 1, sizeof *k->f);
        k->sf = calloc(k->nsf + 1, sizeof *k->sf);
        for (int j = 0; j < k->nf; j++) {
            uint32_t q = of[3] + 4u * j;
            k->f[j].name = cod_name(m, rd16(d + q)); k->f[j].tl = rd16(d + q + 2);
            k->f[j].attr = d[of[6] + j];
            k->f[j].wide = (uint8_t)(cod_typeslots(m, k->f[j].tl) == 2);
        }
        for (int j = 0; j < k->nsf; j++) {
            uint32_t q = of[4] + 6u * j;
            k->sf[j].name = cod_name(m, rd16(d + q)); k->sf[j].tl = rd16(d + q + 2); k->sf[j].addr = rd16(d + q + 4);
            k->sf[j].attr = d[of[7] + j];
            k->sf[j].wide = (uint8_t)(cod_typeslots(m, k->sf[j].tl) == 2);
        }
        k->ifs = calloc(k->nif + 1, 2);
        for (int j = 0; j < k->nif; j++) { k->ifs[j][0] = d[of[5] + 2 * j]; k->ifs[j][1] = d[of[5] + 2 * j + 1]; }
        for (uint32_t q = of[0]; q < of[3]; q += 2) {
            if (rn == rcap) { rcap *= 2; roff = realloc(roff, rcap * sizeof *roff); }
            roff[rn++] = rd16(d + q);
        }
    }
    /* unique routine offsets -> routine table */
    m->routines = calloc(rn + 1, sizeof *m->routines);
    for (int i = 0; i < rn; i++) {
        if (!roff[i]) continue;
        int dup = 0;
        for (int j = 0; j < m->nroutines; j++) if (m->routines[j].off == roff[i]) { dup = 1; break; }
        if (dup) continue;
        fill_routine(m, &m->routines[m->nroutines++], roff[i]);
    }
    qsort(m->routines, m->nroutines, sizeof *m->routines, cmp_rt);
    for (int ci = 0; ci < m->ncls; ci++) {
        BBClass *k = &m->cls[ci];
        uint32_t co = classOff[ci];
        uint32_t of0 = rd16(d + co + 24) + co;
        k->v = calloc(k->nv + 1, sizeof(BBRoutine *));
        k->nv_ = calloc(k->nnv + 1, sizeof(BBRoutine *));
        k->s = calloc(k->ns + 1, sizeof(BBRoutine *));
        uint32_t q = of0;
        for (int j = 0; j < k->nv; j++, q += 2) { BBRoutine *r = cod_routine_at(m, rd16(d + q)); k->v[j] = r; if (r) r->cls = k; }
        for (int j = 0; j < k->nnv; j++, q += 2) { BBRoutine *r = cod_routine_at(m, rd16(d + q)); k->nv_[j] = r; if (r) r->cls = k; }
        for (int j = 0; j < k->ns; j++, q += 2) { BBRoutine *r = cod_routine_at(m, rd16(d + q)); k->s[j] = r; if (r) { r->cls = k; r->isStatic = 1; } }
    }
    free(roff);
    free(classOff);

    /* fixup tables */
    FixVec fv = {0};
    int adv = m->dver >= 6;
    p = member_table(m, o_rfix, adv, FX_ROUTINE, &fv);
    p = member_table(m, o_srfix, adv, FX_SROUTINE, &fv);
    p = member_table(m, o_vrfix, adv, FX_VROUTINE, &fv);
    p = o_ccfix;
    { int n = rd16(d + p); p += 2;
      for (int i = 0; i < n && p < g_limit; i++) { p += p & 1; int ci = cref_index(m, rd16(d + p)); p += 2; p = fix_entry(d, p, &fv, FX_CLASS, (uint32_t)ci); } }
    p = member_table(m, o_ffix, 0, FX_FIELD, &fv);
    p = o_lffix;
    { int n = rd16(d + p); p += 2;
      for (int i = 0; i < n && p < g_limit; i++) {
          BBMemberRef r = {0}; r.cref = -1; r.lcls = d[p]; r.lfield = d[p + 1]; p += 2;
          mref_push(m, r);
          p = fix_entry(d, p, &fv, FX_LFIELD, (uint32_t)(m->nmref - 1));
      } }
    p = member_table(m, o_sffix, 0, FX_SFIELD, &fv);
    p = o_mcfix;
    { int n = rd16(d + p); p += 2;
      for (int i = 0; i < n && p < g_limit; i++) { int mo = d[p++]; p = fix_entry(d, p, &fv, FX_MODULE, (uint32_t)mo); } }
    if (p > m->dataSize) {
        /* keep what was read; such modules are rare (odd layouts) */
        if (getenv("BB_PARSE_WARN")) fprintf(stderr, "%s: fixup tables overrun\n", name);
    }
    qsort(fv.a, fv.n, sizeof *fv.a, cmp_fix);
    m->fix = fv.a; m->nfix = fv.n;
    m->statics = calloc(m->staticSize + 2, sizeof *m->statics);
    return m;
}

BBRoutine *cod_routine_at(BBModule *m, uint16_t off) {
    int lo = 0, hi = m->nroutines - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->routines[mid].off == off) return &m->routines[mid];
        if (m->routines[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

const BBFixup *cod_fixup_at(BBModule *m, uint16_t off) {
    int lo = 0, hi = m->nfix - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->fix[mid].off == off) return &m->fix[mid];
        if (m->fix[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void cod_free(BBModule *m) {
    if (!m) return;
    for (int i = 0; i < m->ncls; i++) {
        BBClass *k = &m->cls[i];
        free(k->full); free(k->f); free(k->sf); free(k->ifs); free(k->v); free(k->nv_); free(k->s);
        free(k->ifaces); free(k->vtable);
    }
    free(m->cls); free(m->routines); free(m->crefs); free(m->crefOff); free(m->mrefs);
    free(m->fix); free(m->modNameOff); free(m->mods); free(m->statics);
#ifdef BBARENA_H
    if (!bb_code_owns(m->raw))
#endif
    free(m->raw);
    free(m);
}
