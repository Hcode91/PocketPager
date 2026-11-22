/* PocketPager - BlackBerry OS family: natives.
 *
 * RIM's Java libraries declare some methods native; their bodies are a single
 * invokenative instruction. They are bound here by "package.Class.method"
 * (optionally followed by the parameter signature to pick an overload). */
#include "natives.h"

static uint64_t now_ms(BBVM *vm) { return nat_now(vm); }

static void ret64(uint32_t *ret, int64_t v) { ret[0] = (uint32_t)v; ret[1] = (uint32_t)((uint64_t)v >> 32); }
static int64_t arg64(uint32_t *a) { return (int64_t)((uint64_t)a[0] | (uint64_t)a[1] << 32); }

static Obj *need(BBVM *vm, Thread *t, ref_t r) {
    Obj *o = vm_obj(vm, r);
    if (!o) vm_throw(vm, t, "java.lang.NullPointerException", NULL);
    return o;
}

#define schar nat_char

/* ---------------- java.lang.Object ---------------- */
NAT(obj_getClass) {
    UNUSED;
    Obj *o = need(vm, t, a[0]); if (!o) return NAT_THROWN;
    BBClass *k = o->kind == OK_INST ? o->cls : (o->flags & OF_STRING) ? vm->cString : vm->cObject;
    ret[0] = vm_class_obj(vm, k);
    return NAT_OK;
}
NAT(obj_hashCode) { UNUSED; Obj *o = vm_obj(vm, a[0]); ret[0] = o ? o->hash : 0; return NAT_OK; }

static int monitor_owned(BBVM *vm, Thread *t, ref_t o) {
    Monitor *m = vm_monitor(vm, o, 0);
    return m && m->owner == t;
}

NAT(obj_notify) {
    UNUSED;
    if (!monitor_owned(vm, t, a[0])) { vm_throw(vm, t, "java.lang.IllegalMonitorStateException", NULL); return NAT_THROWN; }
    for (Thread *x = vm->threads; x; x = x->next)
        if (x->state == TS_WAIT && x->waitObj == a[0]) { x->state = TS_MONITOR; x->wakeAt = 0; break; }
    return NAT_OK;
}
NAT(obj_notifyAll) {
    UNUSED;
    if (!monitor_owned(vm, t, a[0])) { vm_throw(vm, t, "java.lang.IllegalMonitorStateException", NULL); return NAT_THROWN; }
    for (Thread *x = vm->threads; x; x = x->next)
        if (x->state == TS_WAIT && x->waitObj == a[0]) { x->state = TS_MONITOR; x->wakeAt = 0; }
    return NAT_OK;
}
/* wait(long): release the monitor fully, block, reacquire with the old count */
NAT(obj_wait) {
    UNUSED;
    ref_t o = a[0];
    if (t->waitCount < 0) {           /* resumed: monitor reacquired by the scheduler */
        t->waitCount = 0;
        return NAT_OK;
    }
    Monitor *m = vm_monitor(vm, o, 0);
    if (!m || m->owner != t) { vm_throw(vm, t, "java.lang.IllegalMonitorStateException", NULL); return NAT_THROWN; }
    int64_t ms = arg64(a + 1);
    t->waitCount = m->count;
    m->count = 0; m->owner = NULL;
    t->state = TS_WAIT; t->waitObj = o;
    t->wakeAt = ms > 0 ? now_ms(vm) + (uint64_t)ms : 0;
    return NAT_BLOCK;
}

/* ---------------- java.lang.String ---------------- */
NAT(str_length) { UNUSED; Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN; ret[0] = s->len; return NAT_OK; }
NAT(str_charAt) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    int32_t i = (int32_t)a[1];
    if (i < 0 || (uint32_t)i >= s->len) { vm_throw(vm, t, "java.lang.StringIndexOutOfBoundsException", NULL); return NAT_THROWN; }
    ret[0] = schar(s, (uint32_t)i);
    return NAT_OK;
}
NAT(str_equals) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *o = vm_obj(vm, a[1]);
    ret[0] = 0;
    if (!o || !(o->flags & OF_STRING) || o->len != s->len) return NAT_OK;
    for (uint32_t i = 0; i < s->len; i++) if (schar(s, i) != schar(o, i)) return NAT_OK;
    ret[0] = 1;
    return NAT_OK;
}
NAT(str_compareTo) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *o = need(vm, t, a[1]); if (!o) return NAT_THROWN;
    uint32_t n = s->len < o->len ? s->len : o->len;
    for (uint32_t i = 0; i < n; i++) { int d = (int)schar(s, i) - (int)schar(o, i); if (d) { ret[0] = (uint32_t)d; return NAT_OK; } }
    ret[0] = (uint32_t)((int)s->len - (int)o->len);
    return NAT_OK;
}
NAT(str_getChars) { /* getChars(srcBegin, srcEnd, char[] dst, dstBegin) */
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *d = need(vm, t, a[3]); if (!d) return NAT_THROWN;
    int32_t b = (int32_t)a[1], e = (int32_t)a[2], db = (int32_t)a[4];
    if (b < 0 || e > (int32_t)s->len || b > e || db < 0 || db + (e - b) > (int32_t)d->len) {
        vm_throw(vm, t, "java.lang.StringIndexOutOfBoundsException", NULL); return NAT_THROWN;
    }
    for (int32_t i = b; i < e; i++) ((uint16_t *)d->d)[db + i - b] = (uint16_t)schar(s, (uint32_t)i);
    return NAT_OK;
}
NAT(str_startsWith) { /* startsWith(String prefix, int off) */
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *p = need(vm, t, a[1]); if (!p) return NAT_THROWN;
    int32_t off = (int32_t)a[2];
    ret[0] = 0;
    if (off < 0 || (int64_t)off + p->len > s->len) return NAT_OK;
    for (uint32_t i = 0; i < p->len; i++) if (schar(s, off + i) != schar(p, i)) return NAT_OK;
    ret[0] = 1;
    return NAT_OK;
}
NAT(str_lastIndexOf) { /* lastIndexOf(int ch, int from) */
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    int32_t from = (int32_t)a[2];
    if (from >= (int32_t)s->len) from = (int32_t)s->len - 1;
    for (int32_t i = from; i >= 0; i--) if (schar(s, (uint32_t)i) == a[1]) { ret[0] = (uint32_t)i; return NAT_OK; }
    ret[0] = (uint32_t)-1;
    return NAT_OK;
}
NAT(str_indexOfStr) { /* indexOf(String, int from) */
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *p = need(vm, t, a[1]); if (!p) return NAT_THROWN;
    int32_t from = (int32_t)a[2]; if (from < 0) from = 0;
    for (int64_t i = from; i + p->len <= s->len; i++) {
        uint32_t j = 0;
        while (j < p->len && schar(s, (uint32_t)i + j) == schar(p, j)) j++;
        if (j == p->len) { ret[0] = (uint32_t)i; return NAT_OK; }
    }
    ret[0] = (uint32_t)-1;
    return NAT_OK;
}

/* ---------------- java.lang.System / Math ---------------- */
NAT(sys_currentTimeMillis) { UNUSED; ret64(ret, (int64_t)now_ms(vm)); return NAT_OK; }
NAT(sys_arraycopy) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *d = need(vm, t, a[2]); if (!d) return NAT_THROWN;
    int32_t sp = (int32_t)a[1], dp = (int32_t)a[3], n = (int32_t)a[4];
    if (s->kind != OK_ARRAY || d->kind != OK_ARRAY) { vm_throw(vm, t, "java.lang.ArrayStoreException", NULL); return NAT_THROWN; }
    if (n < 0 || sp < 0 || dp < 0 || (int64_t)sp + n > s->len || (int64_t)dp + n > d->len) {
        vm_throw(vm, t, "java.lang.ArrayIndexOutOfBoundsException", NULL); return NAT_THROWN;
    }
    int es = vm_elem_size(s->et), ed = vm_elem_size(d->et);
    if (es != ed) { vm_throw(vm, t, "java.lang.ArrayStoreException", NULL); return NAT_THROWN; }
    memmove((uint8_t *)d->d + (size_t)dp * ed, (uint8_t *)s->d + (size_t)sp * es, (size_t)n * es);
    return NAT_OK;
}
NAT(sys_identityHashCode) { UNUSED; Obj *o = vm_obj(vm, a[0]); ret[0] = o ? o->hash : 0; return NAT_OK; }

/* ---------------- java.lang.Thread ---------------- */
NAT(thr_sleep) {
    UNUSED;
    if (t->waitCount < 0) { t->waitCount = 0; return NAT_OK; }
    int64_t ms = arg64(a);
    t->state = TS_SLEEP;
    t->wakeAt = now_ms(vm) + (uint64_t)(ms > 0 ? ms : 0);
    t->waitCount = 0;
    return NAT_BLOCK;
}
NAT(thr_yield) { UNUSED; return NAT_OK; }


/* ---------------- strings (RIM keeps String as a byte[] or char[]) ---------------- */

/* Turn the object behind handle r into the array 'src' content (strings are
 * created with 'new String' and then given their characters). */
static void morph_to_string(BBVM *vm, ref_t r, int et, uint32_t len, const void *data) {
    uint32_t idx = r - REF_BASE;
    Obj *old = vm->objs[idx];
    size_t bytes = sizeof(Obj) + (size_t)len * vm_elem_size(et) + 8;
    Obj *o = calloc(1, bytes);
    o->kind = OK_ARRAY; o->et = (uint8_t)et; o->dims = 1; o->len = len;
    o->flags = OF_STRING | (old->flags & OF_PERM); o->cls = vm->cString; o->hash = old->hash; o->mon = old->mon;
    if (len) memcpy(o->d, data, (size_t)len * vm_elem_size(et));
    vm->objs[idx] = old == o ? o : o;
    free(old);
}

static ref_t copy_array(BBVM *vm, const Obj *s, uint32_t from, uint32_t n, int plain) {
    ref_t r = vm_new_array(vm, s->et, NULL, 1, n);
    Obj *o = vm_obj(vm, r);
    int es = vm_elem_size(s->et);
    memcpy(o->d, (const uint8_t *)s->d + (size_t)from * es, (size_t)n * es);
    if (!plain) { o->flags |= OF_STRING; o->cls = vm->cString; }
    return r;
}

NAT(str_usesBytes) { UNUSED; Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN; ret[0] = s->et != BT_CHAR; return NAT_OK; }
NAT(str_arrayToThisString) {
    UNUSED;
    Obj *src = need(vm, t, a[1]); if (!src) return NAT_THROWN;
    if (!vm_obj(vm, a[0])) return NAT_OK;
    int et = src->et == BT_CHAR ? BT_CHAR : BT_BYTE;
    morph_to_string(vm, a[0], et, src->len, src->d);
    return NAT_OK;
}
NAT(str_getCopyAsArray) { UNUSED; Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN; ret[0] = copy_array(vm, s, 0, s->len, 1); return NAT_OK; }
NAT(str_getSubArray) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    int32_t b = (int32_t)a[1], n = (int32_t)a[2];   /* (offset, count) */
    if (b < 0 || n < 0 || (int64_t)b + n > s->len) { vm_throw(vm, t, "java.lang.StringIndexOutOfBoundsException", NULL); return NAT_THROWN; }
    ret[0] = copy_array(vm, s, (uint32_t)b, (uint32_t)n, 1);
    return NAT_OK;
}
NAT(str_getBytesChar) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *d = need(vm, t, a[1]); if (!d) return NAT_THROWN;
    uint32_t off = a[2];
    for (uint32_t i = 0; i < s->len && off + i < d->len; i++) ((uint8_t *)d->d)[off + i] = (uint8_t)nat_char(s, i);
    return NAT_OK;
}
NAT(str_concatBytes) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *d = need(vm, t, a[1]); if (!d) return NAT_THROWN;
    Obj *o = need(vm, t, a[2]); if (!o) return NAT_THROWN;
    uint32_t k = 0;
    for (uint32_t i = 0; i < s->len && k < d->len; i++) ((uint8_t *)d->d)[k++] = (uint8_t)nat_char(s, i);
    for (uint32_t i = 0; i < o->len && k < d->len; i++) ((uint8_t *)d->d)[k++] = (uint8_t)nat_char(o, i);
    return NAT_OK;
}
NAT(str_doReplace) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *d = need(vm, t, a[1]); if (!d) return NAT_THROWN;
    unsigned from = a[2] & 0xffff, to = a[3] & 0xffff;
    for (uint32_t i = 0; i < s->len && i < d->len; i++) {
        unsigned c = nat_char(s, i);
        if (c == from) c = to;
        if (d->et == BT_CHAR) ((uint16_t *)d->d)[i] = (uint16_t)c; else ((uint8_t *)d->d)[i] = (uint8_t)c;
    }
    return NAT_OK;
}
NAT(str_newArrayWithPossibleDemotion) {
    UNUSED;
    Obj *c = need(vm, t, a[0]); if (!c) return NAT_THROWN;
    int32_t off = (int32_t)a[1], n = (int32_t)a[2];
    if (off < 0 || n < 0 || (int64_t)off + n > c->len) { vm_throw(vm, t, "java.lang.StringIndexOutOfBoundsException", NULL); return NAT_THROWN; }
    int wide = 0;
    for (int32_t i = 0; i < n; i++) if (nat_char(c, (uint32_t)(off + i)) > 0xff) { wide = 1; break; }
    ref_t r = vm_new_array(vm, wide ? BT_CHAR : BT_BYTE, NULL, 1, (uint32_t)n);
    Obj *o = vm_obj(vm, r);
    for (int32_t i = 0; i < n; i++) {
        unsigned ch = nat_char(c, (uint32_t)(off + i));
        if (wide) ((uint16_t *)o->d)[i] = (uint16_t)ch; else ((uint8_t *)o->d)[i] = (uint8_t)ch;
    }
    ret[0] = r;
    return NAT_OK;
}
static int str_case(BBVM *vm, Thread *t, uint32_t *a, uint32_t *ret, int upper) {
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    ref_t r = copy_array(vm, s, 0, s->len, 0);
    Obj *o = vm_obj(vm, r);
    for (uint32_t i = 0; i < o->len; i++) {
        unsigned c = nat_char(o, i);
        if (upper) { if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) c -= 32; }
        else { if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) c += 32; }
        if (o->et == BT_CHAR) ((uint16_t *)o->d)[i] = (uint16_t)c; else ((uint8_t *)o->d)[i] = (uint8_t)c;
    }
    ret[0] = r;
    return NAT_OK;
}
NAT(str_toLowerCase) { return str_case(vm, t, a, ret, 0); }
NAT(str_toUpperCase) { return str_case(vm, t, a, ret, 1); }

/* ---------------- StringBuffer: slot 0 = byte[]/char[] value, slot 1 = count ---------------- */

static Obj *sb_ensure(BBVM *vm, ref_t sb, uint32_t need, int wide) {
    Obj *b = vm_obj(vm, sb);
    Obj *v = vm_obj(vm, b->d[0]);
    int curWide = v && v->et == BT_CHAR;
    if (v && v->len >= need && (curWide || !wide)) return v;
    uint32_t cap = v ? v->len * 2 + 2 : 16;
    if (cap < need) cap = need;
    int et = (curWide || wide) ? BT_CHAR : BT_BYTE;
    ref_t nr = vm_new_array(vm, et, NULL, 1, cap);
    Obj *nv = vm_obj(vm, nr);
    b = vm_obj(vm, sb);
    v = vm_obj(vm, b->d[0]);
    uint32_t cnt = b->d[1];
    for (uint32_t i = 0; v && i < cnt && i < v->len; i++) {
        unsigned c = nat_char(v, i);
        if (et == BT_CHAR) ((uint16_t *)nv->d)[i] = (uint16_t)c; else ((uint8_t *)nv->d)[i] = (uint8_t)c;
    }
    b->d[0] = nr;
    return nv;
}

NAT(sb_doAppend) {
    UNUSED;
    Obj *b = need(vm, t, a[0]); if (!b) return NAT_THROWN;
    Obj *s = vm_obj(vm, a[1]);
    if (!s) return NAT_OK;
    int wide = 0;
    if (s->et == BT_CHAR) for (uint32_t i = 0; i < s->len; i++) if (((uint16_t *)s->d)[i] > 0xff) { wide = 1; break; }
    uint32_t cnt = b->d[1];
    Obj *v = sb_ensure(vm, a[0], cnt + s->len, wide);
    b = vm_obj(vm, a[0]);
    s = vm_obj(vm, a[1]);
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned c = nat_char(s, i);
        if (v->et == BT_CHAR) ((uint16_t *)v->d)[cnt + i] = (uint16_t)c; else ((uint8_t *)v->d)[cnt + i] = (uint8_t)c;
    }
    b->d[1] = cnt + s->len;
    return NAT_OK;
}
NAT(sb_promote) {
    UNUSED;
    Obj *b = need(vm, t, a[0]); if (!b) return NAT_THROWN;
    Obj *v = vm_obj(vm, b->d[0]);
    sb_ensure(vm, a[0], v ? v->len : 16, 1);
    return NAT_OK;
}
NAT(sb_reverse) {
    UNUSED;
    Obj *b = need(vm, t, a[0]); if (!b) return NAT_THROWN;
    Obj *v = vm_obj(vm, b->d[0]);
    uint32_t n = b->d[1];
    for (uint32_t i = 0; v && i < n / 2; i++) {
        unsigned x = nat_char(v, i), y = nat_char(v, n - 1 - i);
        if (v->et == BT_CHAR) { ((uint16_t *)v->d)[i] = (uint16_t)y; ((uint16_t *)v->d)[n - 1 - i] = (uint16_t)x; }
        else { ((uint8_t *)v->d)[i] = (uint8_t)y; ((uint8_t *)v->d)[n - 1 - i] = (uint8_t)x; }
    }
    ret[0] = a[0];
    return NAT_OK;
}
/* formatNumeric(buf, pos, radix, value) -> characters written; the buffer
 * array is grown in place when needed (RIM arrays can be resized). */
void nat_resize_array(BBVM *vm, ref_t r, uint32_t n);
NAT(sb_formatNumeric) {
    UNUSED;
    Obj *d = need(vm, t, a[0]); if (!d) return NAT_THROWN;
    uint32_t pos = a[1];
    int radix = (int)a[2];
    int64_t v = nat_arg64(a + 3);
    if (radix < 2 || radix > 36) radix = 10;
    char tmp[80]; int n = 0, neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do { int dgt = (int)(u % (uint64_t)radix); tmp[n++] = (char)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10); u /= (uint64_t)radix; } while (u);
    if (neg) tmp[n++] = '-';
    if (pos + (uint32_t)n > d->len) {
        uint32_t cap = d->len * 2 + 2;
        if (cap < pos + (uint32_t)n) cap = pos + (uint32_t)n;
        nat_resize_array(vm, a[0], cap);
        d = vm_obj(vm, a[0]);
    }
    for (int i = 0; i < n; i++, pos++) {
        char c = tmp[n - 1 - i];
        if (d->et == BT_CHAR) ((uint16_t *)d->d)[pos] = (uint16_t)c; else ((uint8_t *)d->d)[pos] = (uint8_t)c;
    }
    ret[0] = (uint32_t)n;
    return NAT_OK;
}

/* ---------------- java.lang.Class ---------------- */
static BBClass *class_of(BBVM *vm, ref_t r) { Obj *o = vm_obj(vm, r); return o && (o->flags & OF_CLASS) ? o->host : NULL; }
NAT(cls_isInstance) { UNUSED; BBClass *k = class_of(vm, a[0]); ret[0] = a[1] && k ? (uint32_t)vm_instanceof(vm, a[1], k) : 0; return NAT_OK; }
NAT(cls_isAssignableFrom) {
    UNUSED;
    BBClass *k = class_of(vm, a[0]), *o = class_of(vm, a[1]);
    if (!o) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    vm_link(vm, o);
    ret[0] = k && vm_is_subclass(o, k);
    return NAT_OK;
}
NAT(cls_isInterface) { UNUSED; BBClass *k = class_of(vm, a[0]); ret[0] = k && (k->attr & 0x200) ? 1 : 0; return NAT_OK; }
NAT(cls_newInstance0) {
    UNUSED;
    BBClass *k = class_of(vm, a[0]);
    if (!k) { vm_throw(vm, t, "java.lang.InstantiationException", NULL); return NAT_THROWN; }
    if (k->initState != 2) {
        uint32_t dummy[2];
        (void)dummy;
        if (!vm_init_class(vm, t, k)) {
            /* <clinit> pushed: run it to completion before constructing */
            int base = t->nfr - 1;
            while (t->nfr > base && !vm->fatal) { int rc = vm_run_thread(vm, t, 1000000, base); if (rc < 0) return NAT_THROWN; if (t->state != TS_READY) break; }
        }
    }
    ref_t o = vm_new_obj(vm, k);
    BBRoutine *init = vm_find_method(vm, k, "<init>", "");
    if (init && init->cls == k) {
        uint32_t args[1] = { o };
        if (vm_call(vm, t, init, args, 1, NULL) < 0) return NAT_THROWN;
    }
    ret[0] = o;
    return NAT_OK;
}
NAT(cls_getClassName) { UNUSED; BBClass *k = class_of(vm, a[0]); ret[0] = vm_new_string(vm, k ? k->name : "?"); return NAT_OK; }
NAT(cls_getPackageName) { UNUSED; BBClass *k = class_of(vm, a[0]); ret[0] = vm_new_string(vm, k ? k->pkg : ""); return NAT_OK; }
NAT(cls_forName0) {
    UNUSED;
    char nm[300];
    vm_string_utf8(vm, a[0], nm, sizeof nm);
    BBClass *k = vm_find_class_anywhere(vm, nm);
    if (!k) {
        /* try the family named after the package, e.g. net_rim_... modules */
        if (vm->trace & 4) vm_log(vm, "Class.forName(%s): not loaded", nm);
    }
    ret[0] = k ? vm_class_obj(vm, k) : 0;
    return NAT_OK;
}
NAT(cls_zero) { UNUSED; ret[0] = 0; return NAT_OK; }

/* ---------------- numbers and math ---------------- */
NAT(bits64) { UNUSED; ret[0] = a[0]; ret[1] = a[1]; return NAT_OK; }
NAT(bits32) { UNUSED; ret[0] = a[0]; return NAT_OK; }
NAT(m_sin) { UNUSED; nat_retd(ret, sin(nat_argd(a))); return NAT_OK; }
NAT(m_cos) { UNUSED; nat_retd(ret, cos(nat_argd(a))); return NAT_OK; }
NAT(m_tan) { UNUSED; nat_retd(ret, tan(nat_argd(a))); return NAT_OK; }
NAT(m_sqrt) { UNUSED; nat_retd(ret, sqrt(nat_argd(a))); return NAT_OK; }
NAT(m_ceil) { UNUSED; nat_retd(ret, ceil(nat_argd(a))); return NAT_OK; }
NAT(m_floor) { UNUSED; nat_retd(ret, floor(nat_argd(a))); return NAT_OK; }
NAT(rt_freeMemory) { UNUSED; nat_ret64(ret, 16 << 20); return NAT_OK; }
NAT(rt_totalMemory) { UNUSED; nat_ret64(ret, 32 << 20); return NAT_OK; }
NAT(rt_gc) { UNUSED; return NAT_OK; }
NAT(dbg_println0) { UNUSED; char b[1024]; vm_string_utf8(vm, a[0], b, sizeof b); vm_log(vm, "[out] %s", b); return NAT_OK; }
NAT(dbg_printInternal) {
    UNUSED;
    Obj *o = vm_obj(vm, a[0]);
    if (!o) return NAT_OK;
    char b[1024]; uint32_t n = o->len < sizeof b - 1 ? o->len : sizeof b - 1;
    memcpy(b, o->d, n); b[n] = 0;
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    if (n) vm_log(vm, "[out] %s", b);
    return NAT_OK;
}


/* ---------------- net.rim.device.api.util.StringUtilities ---------------- */
static unsigned lc(unsigned c) { if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) return c + 32; return c; }
NAT(su_hashCodeNative) { /* (String s, int from, int to, boolean ignoreCase) */
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    uint32_t from = a[1], to = a[2] > s->len ? s->len : a[2];
    int32_t h = 0;
    for (uint32_t i = from; i < to; i++) { unsigned c = schar(s, i); if (a[3]) c = lc(c); h = 31 * h + (int32_t)c; }
    ret[0] = (uint32_t)h;
    return NAT_OK;
}
NAT(su_indexOf) { /* (String s, int ch, int from, int to) */
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    int32_t from = (int32_t)a[2]; if (from < 0) from = 0;
    uint32_t to = a[3] > s->len ? s->len : a[3];
    for (uint32_t i = (uint32_t)from; i < to; i++) if (schar(s, i) == a[1]) { ret[0] = i; return NAT_OK; }
    ret[0] = (uint32_t)-1;
    return NAT_OK;
}
static int cmp_ic(Obj *x, Obj *y, int ic) {
    uint32_t n = x->len < y->len ? x->len : y->len;
    for (uint32_t i = 0; i < n; i++) {
        unsigned p = schar(x, i), q = schar(y, i);
        if (ic) { p = lc(p); q = lc(q); }
        if (p != q) return (int)p - (int)q;
    }
    return (int)x->len - (int)y->len;
}
NAT(su_compareToIgnoreCase) {
    UNUSED;
    Obj *x = need(vm, t, a[0]); if (!x) return NAT_THROWN;
    Obj *y = need(vm, t, a[1]); if (!y) return NAT_THROWN;
    ret[0] = (uint32_t)cmp_ic(x, y, 1);
    return NAT_OK;
}
NAT(su_isASCII) {
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    ret[0] = 1;
    for (uint32_t i = 0; i < s->len; i++) if (schar(s, i) > 0x7f) { ret[0] = 0; break; }
    return NAT_OK;
}
NAT(su_computeHashCode) { /* (StringBuffer) */
    UNUSED;
    Obj *b = need(vm, t, a[0]); if (!b) return NAT_THROWN;
    Obj *v = vm_obj(vm, b->d[0]);
    int32_t h = 0;
    for (uint32_t i = 0; v && i < b->d[1]; i++) h = 31 * h + (int32_t)schar(v, i);
    ret[0] = (uint32_t)h;
    return NAT_OK;
}
NAT(su_regionMatches) { /* (String s, boolean ic, int toff, String o, int ooff, int len, int flags?) */
    UNUSED;
    Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN;
    Obj *o = need(vm, t, a[3]); if (!o) return NAT_THROWN;
    int ic = (int)a[1];
    int32_t to = (int32_t)a[2], oo = (int32_t)a[4], len = (int32_t)a[5];
    ret[0] = 0;
    if (to < 0 || oo < 0 || (int64_t)to + len > s->len || (int64_t)oo + len > o->len) return NAT_OK;
    for (int32_t i = 0; i < len; i++) {
        unsigned p = schar(s, (uint32_t)(to + i)), q = schar(o, (uint32_t)(oo + i));
        if (ic) { p = lc(p); q = lc(q); }
        if (p != q) return NAT_OK;
    }
    ret[0] = 1;
    return NAT_OK;
}
void nat_sb_append_ascii(BBVM *vm, ref_t sb, const char *s) {
    Obj *b = vm_obj(vm, sb);
    if (!b) return;
    uint32_t n = (uint32_t)strlen(s), cnt = b->d[1];
    Obj *v = sb_ensure(vm, sb, cnt + n, 0);
    b = vm_obj(vm, sb);
    for (uint32_t i = 0; i < n; i++) {
        if (v->et == BT_CHAR) ((uint16_t *)v->d)[cnt + i] = (uint8_t)s[i]; else ((uint8_t *)v->d)[cnt + i] = (uint8_t)s[i];
    }
    b->d[1] = cnt + n;
}
static void sb_append_chars(BBVM *vm, ref_t sb, Obj *src, uint32_t from, uint32_t n) {
    Obj *b = vm_obj(vm, sb);
    int wide = 0;
    for (uint32_t i = 0; i < n; i++) if (schar(src, from + i) > 0xff) { wide = 1; break; }
    uint32_t cnt = b->d[1];
    Obj *v = sb_ensure(vm, sb, cnt + n, wide);
    b = vm_obj(vm, sb);
    for (uint32_t i = 0; i < n; i++) {
        unsigned c = schar(src, from + i);
        if (v->et == BT_CHAR) ((uint16_t *)v->d)[cnt + i] = (uint16_t)c; else ((uint8_t *)v->d)[cnt + i] = (uint8_t)c;
    }
    b->d[1] = cnt + n;
}
NAT(su_doAppendStr) { /* (StringBuffer, String|byte[], int off, int len) */
    UNUSED;
    if (!need(vm, t, a[0])) return NAT_THROWN;
    Obj *s = need(vm, t, a[1]); if (!s) return NAT_THROWN;
    uint32_t off = a[2], n = a[3];
    if (off > s->len) off = s->len;
    if (n > s->len - off) n = s->len - off;
    sb_append_chars(vm, a[0], s, off, n);
    return NAT_OK;
}
NAT(su_doAppendSb) { /* (StringBuffer dst, StringBuffer src, int off, int len) */
    UNUSED;
    if (!need(vm, t, a[0])) return NAT_THROWN;
    Obj *sb = need(vm, t, a[1]); if (!sb) return NAT_THROWN;
    Obj *v = vm_obj(vm, sb->d[0]);
    if (!v) return NAT_OK;
    uint32_t off = a[2], n = a[3];
    if (off > sb->d[1]) off = sb->d[1];
    if (n > sb->d[1] - off) n = sb->d[1] - off;
    sb_append_chars(vm, a[0], v, off, n);
    return NAT_OK;
}
static int su_case(BBVM *vm, Thread *t, uint32_t *a, uint32_t *ret, int upper) {
    uint32_t b[1] = { a[0] };
    return str_case(vm, t, b, ret, upper);
}
NAT(su_toLowerCase) { return su_case(vm, t, a, ret, 0); }
NAT(su_toUpperCase) { return su_case(vm, t, a, ret, 1); }
NAT(su_sbToLowerCase) { /* (StringBuffer, int from, int to, int locale) */
    UNUSED;
    Obj *b = need(vm, t, a[0]); if (!b) return NAT_THROWN;
    Obj *v = vm_obj(vm, b->d[0]);
    for (uint32_t i = a[1]; v && i < a[2] && i < b->d[1]; i++) {
        unsigned c = lc(schar(v, i));
        if (v->et == BT_CHAR) ((uint16_t *)v->d)[i] = (uint16_t)c; else ((uint8_t *)v->d)[i] = (uint8_t)c;
    }
    return NAT_OK;
}
NAT(su_getCharacterSize) { UNUSED; Obj *s = need(vm, t, a[0]); if (!s) return NAT_THROWN; ret[0] = s->et == BT_CHAR ? 2 : 1; return NAT_OK; }
NAT(nu_appendNumber) { /* NumberUtilities.appendNumber(StringBuffer, int|long value, int radix, int minDigits) */
    UNUSED;
    if (!need(vm, t, a[0])) return NAT_THROWN;
    return NAT_OK;
}

/* ---------------- registry ---------------- */
const NatEnt kNativesLang[] = {
    { "java.lang.Object.getClass", obj_getClass },
    { "java.lang.Object.hashCode", obj_hashCode },
    { "java.lang.Object.notify", obj_notify },
    { "java.lang.Object.notifyAll", obj_notifyAll },
    { "java.lang.Object.wait(J)", obj_wait },
    { "java.lang.String.length", str_length },
    { "java.lang.String.charAt", str_charAt },
    { "java.lang.String.equals", str_equals },
    { "java.lang.String.compareTo", str_compareTo },
    { "java.lang.String.getChars", str_getChars },
    { "java.lang.String.startsWith", str_startsWith },
    { "java.lang.String.lastIndexOf(II)", str_lastIndexOf },
    { "java.lang.String.indexOf(Ljava.lang.String;I)", str_indexOfStr },
    { "java.lang.System.currentTimeMillis", sys_currentTimeMillis },
    { "java.lang.System.arraycopy", sys_arraycopy },
    { "java.lang.System.identityHashCode", sys_identityHashCode },
    { "java.lang.Thread.sleep", thr_sleep },
    { "java.lang.Thread.yield", thr_yield },
    { "java.lang.StringBuffer.doAppend", sb_doAppend },
    { "java.lang.StringBuffer.promote", sb_promote },
    { "java.lang.StringBuffer.reverse", sb_reverse },
    { "java.lang.StringBuffer.formatNumeric", sb_formatNumeric },
    { "java.lang.String.toLowerCase", str_toLowerCase },
    { "java.lang.String.toUpperCase", str_toUpperCase },
    { "java.lang.String.usesBytes", str_usesBytes },
    { "java.lang.String.arrayToThisString", str_arrayToThisString },
    { "java.lang.String.getCopyAsArray", str_getCopyAsArray },
    { "java.lang.String.getSubArray", str_getSubArray },
    { "java.lang.String.getBytesChar", str_getBytesChar },
    { "java.lang.String.concatBytes", str_concatBytes },
    { "java.lang.String.doReplace", str_doReplace },
    { "java.lang.String.newArrayWithPossibleDemotion", str_newArrayWithPossibleDemotion },
    { "java.lang.String.indexOf", str_indexOfStr },
    { "java.lang.Class.isInstance", cls_isInstance },
    { "java.lang.Class.isAssignableFrom", cls_isAssignableFrom },
    { "java.lang.Class.isInterface", cls_isInterface },
    { "java.lang.Class.newInstance0", cls_newInstance0 },
    { "java.lang.Class.getClassName", cls_getClassName },
    { "java.lang.Class.getPackageName", cls_getPackageName },
    { "java.lang.Class.forName0", cls_forName0 },
    { "java.lang.Class.getArrayDims", cls_zero },
    { "java.lang.Class.getArrayType", cls_zero },
    { "java.lang.Double.doubleToLongBits", bits64 },
    { "java.lang.Double.longBitsToDouble", bits64 },
    { "java.lang.Float.floatToIntBits", bits32 },
    { "java.lang.Float.intBitsToFloat", bits32 },
    { "java.lang.Math.sin", m_sin }, { "java.lang.Math.cos", m_cos }, { "java.lang.Math.tan", m_tan },
    { "java.lang.Math.sqrt", m_sqrt }, { "java.lang.Math.ceil", m_ceil }, { "java.lang.Math.floor", m_floor },
    { "java.lang.Runtime.freeMemory", rt_freeMemory },
    { "java.lang.Runtime.totalMemory", rt_totalMemory },
    { "java.lang.Runtime.gc", rt_gc },
    { "java.lang.DebugPrintStream.println0", dbg_println0 },
    { "net.rim.device.api.util.StringUtilities.hashCodeNative", su_hashCodeNative },
    { "net.rim.device.api.util.StringUtilities.indexOf", su_indexOf },
    { "net.rim.device.api.util.StringUtilities.compareToIgnoreCase", su_compareToIgnoreCase },
    { "net.rim.device.api.util.StringUtilities.compareToHandleSpecialChars", str_compareTo },
    { "net.rim.device.api.util.StringUtilities.isASCII", su_isASCII },
    { "net.rim.device.api.util.StringUtilities.computeHashCode", su_computeHashCode },
    { "net.rim.device.api.util.StringUtilities.regionMatchesNative(Ljava.lang.String;ZILjava.lang.String;III)", su_regionMatches },
    { "net.rim.device.api.util.StringUtilities.doAppend(Ljava.lang.StringBuffer;Ljava.lang.String;II)", su_doAppendStr },
    { "net.rim.device.api.util.StringUtilities.doAppend(Ljava.lang.StringBuffer;[BII)", su_doAppendStr },
    { "net.rim.device.api.util.StringUtilities.doAppend(Ljava.lang.StringBuffer;Ljava.lang.StringBuffer;II)", su_doAppendSb },
    { "net.rim.device.api.util.StringUtilities.toLowerCase(Ljava.lang.String;I)", su_toLowerCase },
    { "net.rim.device.api.util.StringUtilities.toUpperCase(Ljava.lang.String;I)", su_toUpperCase },
    { "net.rim.device.api.util.StringUtilities.toLowerCase(Ljava.lang.StringBuffer;III)", su_sbToLowerCase },
    { "net.rim.device.api.util.StringUtilities.getCharacterSize", su_getCharacterSize },
    { "java.lang.DebugOutputStream.printInternal", dbg_printInternal },
    { NULL, NULL }
};

typedef struct Unbound { BBRoutine *r; char name[256]; } Unbound;
static Unbound g_unbound[4096];
static int g_nunbound;

static int nat_missing(BBVM *vm, Thread *t, uint32_t *a, uint32_t *ret) {
    (void)a;
    /* find which routine is executing: the caller's frame invoked it */
    BBRoutine *r = NULL;
    (void)t;
    ret[0] = ret[1] = 0;
    for (int i = 0; i < g_nunbound; i++) if (g_unbound[i].r == vm->ext) { r = g_unbound[i].r; break; }
    (void)r;
    return NAT_OK;
}

static int g_logMissing = 1;

NativeFn natives_lookup(BBVM *vm, BBRoutine *r) {
    char nm[512], sig[384], full[900];
    snprintf(nm, sizeof nm, "%s.%s", r->cls ? r->cls->full : "?", r->name);
    vm_sig(vm, r->mod, r->proto, !r->isStatic, sig, sizeof sig);
    snprintf(full, sizeof full, "%s(%s)", nm, sig);
    static const NatEnt *tables[] = { kNativesLang, kNativesRim, kNativesVm, kNativesSys, kNativesCrypto, NULL };
    for (int ti = 0; tables[ti]; ti++) for (const NatEnt *e = tables[ti]; e->name; e++) if (!strcmp(e->name, full)) return e->fn;
    for (int ti = 0; tables[ti]; ti++) for (const NatEnt *e = tables[ti]; e->name; e++) if (!strcmp(e->name, nm)) return e->fn;
    if (g_logMissing) { vm_log(vm, "native missing: %s", full); if ((vm->trace & 128) && vm->cur) vm_backtrace(vm, vm->cur); }
    if (g_nunbound < 4096) { g_unbound[g_nunbound].r = r; snprintf(g_unbound[g_nunbound].name, 256, "%s", full); g_nunbound++; }
    return nat_missing;
}

void natives_init(BBVM *vm) { (void)vm; }
