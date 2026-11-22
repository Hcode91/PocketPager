/* PocketPager - BlackBerry OS family: net.rim.vm.Array / Memory natives. */
#include "natives.h"
#include <ctype.h>

void nat_resize_array(BBVM *vm, ref_t r, uint32_t n);
static void resize_array(BBVM *vm, ref_t r, uint32_t n) { nat_resize_array(vm, r, n); }
void nat_resize_array(BBVM *vm, ref_t r, uint32_t n) {
    Obj *old = vm_obj(vm, r);
    if (!old || old->kind != OK_ARRAY) return;
    int es = vm_elem_size(old->et);
    size_t bytes = sizeof(Obj) + (size_t)n * es + 8;
    Obj *o = calloc(1, bytes);
    memcpy(o, old, sizeof(Obj));
    o->len = n;
    memcpy(o->d, old->d, (size_t)(old->len < n ? old->len : n) * es);
    vm->objs[r - REF_BASE] = o;
    free(old);
}

NAT(arr_resize) { UNUSED; if (!vm_obj(vm, a[0])) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; } resize_array(vm, a[0], a[1]); return NAT_OK; }
NAT(arr_extend) {
    UNUSED;
    Obj *o = vm_obj(vm, a[0]);
    if (!o) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    if ((int32_t)a[1] > 0) resize_array(vm, a[0], o->len + a[1]);   /* extend by a delta */
    return NAT_OK;
}
NAT(arr_isContiguous) { UNUSED; ret[0] = 1; return NAT_OK; }
NAT(arr_allocContiguousByteArray) { UNUSED; ret[0] = vm_new_array(vm, BT_BYTE, NULL, 1, a[0]); return NAT_OK; }
NAT(arr_maxContiguous) { UNUSED; ret[0] = 8u << 20; return NAT_OK; }
NAT(arr_getSectionSize) { UNUSED; Obj *o = vm_obj(vm, a[0]); ret[0] = o ? o->len : 0; return NAT_OK; }

NAT(mem_objectToInt) { UNUSED; ret[0] = a[0]; return NAT_OK; }
NAT(mem_intToObject) { UNUSED; ret[0] = vm_obj(vm, a[0]) ? a[0] : 0; return NAT_OK; }
NAT(mem_big) { UNUSED; ret[0] = 16u << 20; return NAT_OK; }
NAT(mem_zero) { UNUSED; ret[0] = 0; return NAT_OK; }
NAT(mem_one) { UNUSED; ret[0] = 1; return NAT_OK; }
NAT(mem_nop) { UNUSED; return NAT_OK; }
NAT(mem_objectSize) {
    UNUSED;
    Obj *o = vm_obj(vm, a[0]);
    ret[0] = !o ? 0 : o->kind == OK_INST ? 8 + 4 * o->len : 8 + o->len * (uint32_t)vm_elem_size(o->et);
    return NAT_OK;
}
NAT(mem_isStringAllBytes) { UNUSED; Obj *o = vm_obj(vm, a[0]); ret[0] = o && o->et != BT_CHAR; return NAT_OK; }

/* string interning: (String|byte[]|char[], off, len) -> canonical String */
static ref_t *g_interned; static int g_nint, g_capint;
static int str_eq_range(Obj *s, Obj *src, uint32_t off, uint32_t n) {
    if (s->len != n) return 0;
    for (uint32_t i = 0; i < n; i++) if (nat_char(s, i) != nat_char(src, off + i)) return 0;
    return 1;
}
NAT(mem_stringIntern) {
    UNUSED;
    Obj *src = vm_obj(vm, a[0]);
    if (!src) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint32_t off = a[1], n = a[2];
    if (off > src->len) off = src->len;
    if (n > src->len - off) n = src->len - off;
    for (int i = 0; i < g_nint; i++) {
        Obj *s = vm_obj(vm, g_interned[i]);
        if (s && str_eq_range(s, src, off, n)) { ret[0] = g_interned[i]; return NAT_OK; }
    }
    int wide = 0;
    for (uint32_t i = 0; i < n; i++) if (nat_char(src, off + i) > 0xff) wide = 1;
    ref_t r = vm_new_array(vm, wide ? BT_CHAR : BT_BYTE, NULL, 1, n);
    Obj *o = vm_obj(vm, r);
    src = vm_obj(vm, a[0]);
    for (uint32_t i = 0; i < n; i++) {
        unsigned c = nat_char(src, off + i);
        if (wide) ((uint16_t *)o->d)[i] = (uint16_t)c; else ((uint8_t *)o->d)[i] = (uint8_t)c;
    }
    o->flags |= OF_STRING | OF_PERM; o->cls = vm->cString;
    if (g_nint == g_capint) { g_capint = g_capint ? g_capint * 2 : 256; g_interned = realloc(g_interned, g_capint * sizeof *g_interned); }
    g_interned[g_nint++] = r;
    ret[0] = r;
    return NAT_OK;
}


/* ---------------- net.rim.device.api.util.Arrays ---------------- */
NAT(ar_equalsArray) { /* (a, aoff, b, boff, len) */
    UNUSED;
    Obj *x = vm_obj(vm, a[0]), *y = vm_obj(vm, a[2]);
    ret[0] = 0;
    if (!x || !y) { ret[0] = x == y; return NAT_OK; }
    uint32_t ao = a[1], bo = a[3], n = a[4];
    if ((int32_t)n < 0) {   /* -1: compare whole arrays, lengths included */
        if (x->len - ao != y->len - bo) return NAT_OK;
        n = x->len - ao;
    }
    if ((uint64_t)ao + n > x->len || (uint64_t)bo + n > y->len) return NAT_OK;
    int es = vm_elem_size(x->et);
    if (es != vm_elem_size(y->et)) return NAT_OK;
    ret[0] = !memcmp((uint8_t *)x->d + (size_t)ao * es, (uint8_t *)y->d + (size_t)bo * es, (size_t)n * es);
    return NAT_OK;
}
NAT(ar_fillArray) { /* (array, value, from, to or -1) */
    UNUSED;
    Obj *x = vm_obj(vm, a[0]);
    if (!x) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint32_t from = a[2], to = (int32_t)a[3] < 0 ? x->len : a[3];
    if (to > x->len) to = x->len;
    int es = vm_elem_size(x->et);
    for (uint32_t i = from; i < to; i++) {
        if (es == 1) ((uint8_t *)x->d)[i] = (uint8_t)a[1];
        else if (es == 2) ((uint16_t *)x->d)[i] = (uint16_t)a[1];
        else if (es == 4) x->d[i] = a[1];
        else { x->d[2 * i] = a[1]; x->d[2 * i + 1] = a[1]; }
    }
    return NAT_OK;
}
NAT(ar_fillLong) { /* (long[] a, long v, int from, int to) */
    UNUSED;
    Obj *x = vm_obj(vm, a[0]);
    if (!x) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint32_t from = a[3], to = (int32_t)a[4] < 0 ? x->len : a[4];
    if (to > x->len) to = x->len;
    for (uint32_t i = from; i < to; i++) { x->d[2 * i] = a[1]; x->d[2 * i + 1] = a[2]; }
    return NAT_OK;
}

/* ---------------- com.sun.cldc.i18n.Helper: UTF-8 <-> UTF-16 ---------------- */
static int utf8_next(const uint8_t *b, uint32_t *i, uint32_t end);
/* utf8ToUtf16(char[] dst, byte[] src, off, len): dst is grown to the decoded length */
NAT(h_utf8ToUtf16) {
    UNUSED;
    Obj *d = vm_obj(vm, a[0]), *s = vm_obj(vm, a[1]);
    if (!d || !s) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint32_t end = a[2] + a[3];
    if (end > s->len) end = s->len;
    uint32_t n = 0;
    for (uint32_t i = a[2]; i < end; n++) utf8_next((const uint8_t *)s->d, &i, end);
    if (d->len < n) resize_array(vm, a[0], n);
    d = vm_obj(vm, a[0]); s = vm_obj(vm, a[1]);
    uint32_t k = 0;
    for (uint32_t i = a[2]; i < end && k < d->len;) ((uint16_t *)d->d)[k++] = (uint16_t)utf8_next((const uint8_t *)s->d, &i, end);
    ret[0] = k;
    return NAT_OK;
}
/* getUtf16Chars(char[] dst, byte[] src, off, len, bigEndian) */
NAT(h_getUtf16Chars) {
    UNUSED;
    Obj *d = vm_obj(vm, a[0]), *s = vm_obj(vm, a[1]);
    if (!d || !s) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint32_t n = a[3] / 2;
    if (d->len < n) resize_array(vm, a[0], n);
    d = vm_obj(vm, a[0]); s = vm_obj(vm, a[1]);
    const uint8_t *b = (const uint8_t *)s->d + a[2];
    for (uint32_t i = 0; i < n && a[2] + 2 * i + 1 < s->len; i++)
        ((uint16_t *)d->d)[i] = (uint16_t)(a[4] ? (b[2 * i] << 8 | b[2 * i + 1]) : (b[2 * i] | b[2 * i + 1] << 8));
    ret[0] = n;
    return NAT_OK;
}
NAT(h_utf16ToUtf8) { /* (byte[] dst, char[] src, int off, int len) -> bytes */
    UNUSED;
    Obj *d = vm_obj(vm, a[0]), *s = vm_obj(vm, a[1]);
    if (!d || !s) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint32_t k = 0;
    uint8_t *o = (uint8_t *)d->d;
    for (uint32_t i = a[2]; i < a[2] + a[3] && i < s->len; i++) {
        unsigned c = nat_char(s, i);
        if (c < 0x80) { if (k < d->len) o[k] = (uint8_t)c; k++; }
        else if (c < 0x800) { if (k + 1 < d->len) { o[k] = (uint8_t)(0xc0 | c >> 6); o[k + 1] = (uint8_t)(0x80 | (c & 0x3f)); } k += 2; }
        else { if (k + 2 < d->len) { o[k] = (uint8_t)(0xe0 | c >> 12); o[k + 1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f)); o[k + 2] = (uint8_t)(0x80 | (c & 0x3f)); } k += 3; }
    }
    ret[0] = k;
    return NAT_OK;
}
NAT(kp_layout) { UNUSED; ret[0] = 0x51573332u; return NAT_OK; }   /* full QWERTY */


/* ---------------- TranscodingGateway: byte encodings <-> UTF-16 ----------------
 * L2U(enc, src, off, len, dst, dstOff, dstLen, state[], table, tableOff):
 * with dst == null it only counts and reports in state[1] whether the
 * result needs a char[] (1) or fits a byte[] (0). UTF-8 when there is no
 * conversion table; otherwise the table maps bytes (single-byte encodings). */
static int utf8_next(const uint8_t *b, uint32_t *i, uint32_t end) {
    uint32_t c = b[(*i)++];
    if (c >= 0xf0 && *i + 2 < end + 1) { c = ((c & 7) << 18) | ((b[*i] & 0x3f) << 12) | ((b[*i + 1] & 0x3f) << 6) | (b[*i + 2] & 0x3f); *i += 3; if (c > 0xffff) c = 0xfffd; }
    else if (c >= 0xe0 && *i + 1 < end + 1) { c = ((c & 0x0f) << 12) | ((b[*i] & 0x3f) << 6) | (b[*i + 1] & 0x3f); *i += 2; }
    else if (c >= 0xc0 && *i < end) { c = ((c & 0x1f) << 6) | (b[*i] & 0x3f); *i += 1; }
    return (int)c;
}
NAT(tg_L2U) {
    UNUSED;
    Obj *src = vm_obj(vm, a[1]), *dst = vm_obj(vm, a[4]), *st = vm_obj(vm, a[7]), *tab = vm_obj(vm, a[8]);
    if (!src) { ret[0] = (uint32_t)-1; return NAT_OK; }
    uint32_t i = a[2], end = a[2] + a[3], k = a[5];
    if (end > src->len) end = src->len;
    const uint8_t *b = (const uint8_t *)src->d;
    int wide = 0, n = 0;
    int useTable = tab && tab->len >= 512 && (int)a[0] != 0x7f;
    while (i < end) {
        int c;
        if (useTable) { uint32_t to = a[9] + 2u * b[i++]; c = to + 1 < tab->len ? (((uint8_t *)tab->d)[to] << 8 | ((uint8_t *)tab->d)[to + 1]) : '?'; }
        else c = utf8_next(b, &i, end);
        if (c > 0xff) wide = 1;
        if (dst) {
            if (k >= dst->len) break;
            if (dst->et == BT_CHAR) ((uint16_t *)dst->d)[k] = (uint16_t)c; else ((uint8_t *)dst->d)[k] = (uint8_t)c;
            k++;
        }
        n++;
    }
    if (!dst && st && st->len > 1) st->d[1] = (uint32_t)wide;
    if (vm->trace & 512) vm_log(vm, "L2U enc %d len %u -> %d chars (table %d)", (int)a[0], a[3], n, tab ? (int)tab->len : -1);
    ret[0] = (uint32_t)n;
    return NAT_OK;
}
NAT(tg_U2L) { /* U2L(enc, char[] src, off, len, byte[] dst, dstOff, dstLen, state[], table, tableOff) */
    UNUSED;
    Obj *src = vm_obj(vm, a[1]), *dst = vm_obj(vm, a[4]);
    if (!src) { ret[0] = (uint32_t)-1; return NAT_OK; }
    uint32_t k = a[5], n = 0;
    for (uint32_t i = a[2]; i < a[2] + a[3] && i < src->len; i++) {
        unsigned c = nat_char(src, i);
        uint8_t tmp[3]; int l;
        if (c < 0x80) { tmp[0] = (uint8_t)c; l = 1; }
        else if (c < 0x800) { tmp[0] = (uint8_t)(0xc0 | c >> 6); tmp[1] = (uint8_t)(0x80 | (c & 0x3f)); l = 2; }
        else { tmp[0] = (uint8_t)(0xe0 | c >> 12); tmp[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f)); tmp[2] = (uint8_t)(0x80 | (c & 0x3f)); l = 3; }
        for (int j = 0; j < l; j++) { if (dst && k < dst->len) ((uint8_t *)dst->d)[k] = tmp[j]; k++; n++; }
    }
    ret[0] = n;
    return NAT_OK;
}


/* ---------------- CharacterUtilities (Latin-1 plus basic Unicode ranges) ---------------- */
#include <wctype.h>
static int c_lower(unsigned c) { return (c >= 'a' && c <= 'z') || (c >= 0xdf && c <= 0xff && c != 0xf7) || (c > 0xff && iswlower(c)); }
static int c_upper(unsigned c) { return (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) || (c > 0xff && iswupper(c)); }
static int c_letter(unsigned c) { return c_lower(c) || c_upper(c) || c == 0xaa || c == 0xb5 || c == 0xba || (c > 0xff && iswalpha(c)); }
static int c_digit(unsigned c) { return (c >= '0' && c <= '9') || (c >= 0x660 && c <= 0x669) || (c >= 0xff10 && c <= 0xff19); }
static unsigned c_tolower(unsigned c) { if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) return c + 32; if (c > 0xff) return (unsigned)towlower(c); return c; }
static unsigned c_toupper(unsigned c) { if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) return c - 32; if (c == 0xff) return 0x178; if (c > 0xff) return (unsigned)towupper(c); return c; }
NAT(cu_isLowerCase) { UNUSED; ret[0] = (uint32_t)c_lower(a[0] & 0xffff); return NAT_OK; }
NAT(cu_isUpperCase) { UNUSED; ret[0] = (uint32_t)c_upper(a[0] & 0xffff); return NAT_OK; }
NAT(cu_isLetter) { UNUSED; ret[0] = (uint32_t)c_letter(a[0] & 0xffff); return NAT_OK; }
NAT(cu_isDigit) { UNUSED; ret[0] = (uint32_t)c_digit(a[0] & 0xffff); return NAT_OK; }
NAT(cu_isSpaceChar) { UNUSED; unsigned c = a[0] & 0xffff; ret[0] = c == ' ' || c == 0xa0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200b); return NAT_OK; }
NAT(cu_isISOControl) { UNUSED; unsigned c = a[0] & 0xffff; ret[0] = c < 0x20 || (c >= 0x7f && c < 0xa0); return NAT_OK; }
NAT(cu_isPrintable) { UNUSED; unsigned c = a[0] & 0xffff; ret[0] = !(c < 0x20 || (c >= 0x7f && c < 0xa0)); return NAT_OK; }
NAT(cu_isPunctuation) { UNUSED; unsigned c = a[0] & 0xffff; ret[0] = c < 0x80 && ispunct((int)c) && !strchr("$+<=>^`|~", (int)c); return NAT_OK; }
NAT(cu_isSymbol) { UNUSED; unsigned c = a[0] & 0xffff; ret[0] = c < 0x80 ? strchr("$+<=>^`|~", (int)c) != NULL : (c >= 0xa2 && c <= 0xa9) || c == 0xac || c == 0xae || c == 0xb0 || c == 0xb1 || c == 0xd7 || c == 0xf7; return NAT_OK; }
NAT(cu_toLowerCase) { UNUSED; ret[0] = c_tolower(a[0] & 0xffff) & 0xffff; return NAT_OK; }
NAT(cu_toUpperCase) { UNUSED; ret[0] = c_toupper(a[0] & 0xffff) & 0xffff; return NAT_OK; }
NAT(cu_identity) { UNUSED; ret[0] = a[0] & 0xffff; return NAT_OK; }
NAT(cu_isHan) { UNUSED; unsigned c = a[0] & 0xffff; ret[0] = c >= 0x4e00 && c <= 0x9fff; return NAT_OK; }
NAT(cu_getBidiType) { UNUSED; unsigned c = a[0] & 0xffff; ret[0] = c_digit(c) ? 2 : 0; return NAT_OK; }
NAT(cu_zero) { UNUSED; ret[0] = 0; return NAT_OK; }


/* ---------------- CRCs ---------------- */
static uint32_t crc32_tab[256];
static void crc32_init(void) { if (crc32_tab[1]) return; for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crc32_tab[i] = c; } }
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n) { crc32_init(); while (n--) c = crc32_tab[(c ^ *p++) & 0xff] ^ (c >> 8); return c; }
static uint32_t crc24_upd(uint32_t c, const uint8_t *p, size_t n) {
    while (n--) { c ^= (uint32_t)*p++ << 16; for (int k = 0; k < 8; k++) { c <<= 1; if (c & 0x1000000) c ^= 0x1864cfb; } }
    return c & 0xffffff;
}
static uint32_t crc16_upd(uint32_t c, const uint8_t *p, size_t n) {
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xa001 : c >> 1; }
    return c & 0xffff;
}
static int crc_args(BBVM *vm, Thread *t, uint32_t *a, const uint8_t **p, size_t *n) {
    Obj *o = vm_obj(vm, a[1]);
    if (!o) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return 0; }
    uint32_t off = a[2], len = a[3];
    if ((uint64_t)off + len > o->len) { vm_throw(vm, t, "java.lang.ArrayIndexOutOfBoundsException", NULL); return 0; }
    *p = (const uint8_t *)o->d + off; *n = len;
    return 1;
}
NAT(crc32_b) { UNUSED; const uint8_t *p; size_t n; if (!crc_args(vm, t, a, &p, &n)) return NAT_THROWN; ret[0] = crc32_upd(a[0], p, n); return NAT_OK; }
NAT(crc32_1) { UNUSED; uint8_t b = (uint8_t)a[1]; ret[0] = crc32_upd(a[0], &b, 1); return NAT_OK; }
NAT(crc24_b) { UNUSED; const uint8_t *p; size_t n; if (!crc_args(vm, t, a, &p, &n)) return NAT_THROWN; ret[0] = crc24_upd(a[0], p, n); return NAT_OK; }
NAT(crc24_1) { UNUSED; uint8_t b = (uint8_t)a[1]; ret[0] = crc24_upd(a[0], &b, 1); return NAT_OK; }
NAT(crc16_b) { UNUSED; const uint8_t *p; size_t n; if (!crc_args(vm, t, a, &p, &n)) return NAT_THROWN; ret[0] = crc16_upd(a[0], p, n); return NAT_OK; }
NAT(crc16_1) { UNUSED; uint8_t b = (uint8_t)a[1]; ret[0] = crc16_upd(a[0], &b, 1); return NAT_OK; }
NAT(crc32_int) { UNUSED; uint8_t b[4] = { (uint8_t)(a[1] >> 24), (uint8_t)(a[1] >> 16), (uint8_t)(a[1] >> 8), (uint8_t)a[1] }; ret[0] = crc32_upd(a[0], b, 4); return NAT_OK; }

/* ---------------- NumberUtilities ---------------- */
static int fmt_num(char *out, int64_t v, int radix, int minDigits) {
    if (radix < 2 || radix > 36) radix = 10;
    char tmp[80]; int n = 0, neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do { int d = (int)(u % (uint64_t)radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (uint64_t)radix; } while (u);
    while (n < minDigits && n < 70) tmp[n++] = '0';
    if (neg) tmp[n++] = '-';
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}
void nat_sb_append_ascii(BBVM *vm, ref_t sb, const char *s);
NAT(nu_appendInt) { UNUSED; char b[80]; fmt_num(b, (int32_t)a[1], (int)a[2], (int)a[3]); nat_sb_append_ascii(vm, a[0], b); return NAT_OK; }
NAT(nu_appendLong) { UNUSED; char b[80]; fmt_num(b, nat_arg64(a + 1), (int)a[3], (int)a[4]); nat_sb_append_ascii(vm, a[0], b); return NAT_OK; }
NAT(nu_appendBytes) { /* (byte[] buf, int pos, long v, int radix) -> chars written */
    UNUSED;
    Obj *o = vm_obj(vm, a[0]);
    char b[80]; int n = fmt_num(b, nat_arg64(a + 2), (int)a[4], 0);
    for (int i = 0; o && i < n && a[1] + (uint32_t)i < o->len; i++) ((uint8_t *)o->d)[a[1] + (uint32_t)i] = (uint8_t)b[i];
    ret[0] = (uint32_t)n;
    return NAT_OK;
}

/* ---------------- ITPolicy: no policy set, so every read gives its default ---------------- */
NAT(itp_default3) { UNUSED; ret[0] = a[2]; return NAT_OK; }
NAT(itp_null) { UNUSED; ret[0] = 0; return NAT_OK; }

const NatEnt kNativesVm[] = {
    { "net.rim.vm.Array.resize", arr_resize },
    { "net.rim.vm.Array.extend", arr_extend },
    { "net.rim.vm.Array.isContiguous", arr_isContiguous },
    { "net.rim.vm.Array.allocContiguousByteArray", arr_allocContiguousByteArray },
    { "net.rim.vm.Array.maxContiguousByteArraySize", arr_maxContiguous },
    { "net.rim.vm.Array.getSectionSize", arr_getSectionSize },
    { "net.rim.vm.Array.hasFileSupport", mem_zero },
    { "net.rim.vm.Memory.objectToInt", mem_objectToInt },
    { "net.rim.vm.Memory.intToObject", mem_intToObject },
    { "net.rim.vm.Memory.getFlashFree", mem_big },
    { "net.rim.vm.Memory.getFlashReady", mem_big },
    { "net.rim.vm.Memory.getFlashTotal", mem_big },
    { "net.rim.vm.Memory.getRAMReady", mem_big },
    { "net.rim.vm.Memory.getMaxHeap", mem_big },
    { "net.rim.vm.Memory.getMaxGroupSize", mem_big },
    { "net.rim.vm.Memory.getMaxNumCodeSections", mem_big },
    { "net.rim.vm.Memory.getGCGeneration", mem_zero },
    { "net.rim.vm.Memory.objectSize", mem_objectSize },
    { "net.rim.vm.Memory.objectFlashSize", mem_objectSize },
    { "net.rim.vm.Memory.isStringAllBytes", mem_isStringAllBytes },
    { "net.rim.vm.Memory.stringIntern", mem_stringIntern },
    { "net.rim.vm.Memory.createGroup", mem_one },
    { "net.rim.vm.Memory.moveToRAM", mem_one },
    { "net.rim.vm.Memory.getSnapshotAllowed", mem_zero },
    { "net.rim.vm.Memory.supressHourglass", mem_nop },
    { "net.rim.vm.Memory.allocRAMOnlyBytes", arr_allocContiguousByteArray },
    { "net.rim.vm.DebugSupport.isDesktopVM", mem_zero },
    { "net.rim.vm.DebugSupport.isDebuggerAttached", mem_zero },
    { "net.rim.device.api.util.Arrays.equalsArray", ar_equalsArray },
    { "net.rim.device.api.util.Arrays.fillArray", ar_fillArray },
    { "net.rim.device.api.util.Arrays.fill([JJII)", ar_fillLong },
    { "com.sun.cldc.i18n.Helper.utf8ToUtf16", h_utf8ToUtf16 },
    { "com.sun.cldc.i18n.Helper.utf16ToUtf8", h_utf16ToUtf8 },
    { "com.sun.cldc.i18n.Helper.getUtf16Chars", h_getUtf16Chars },
    { "net.rim.device.api.ui.Keypad.getHardwareLayout", kp_layout },
    { "com.sun.cldc.i18n.j2me.TranscodingGateway.L2U", tg_L2U },
    { "com.sun.cldc.i18n.j2me.TranscodingGateway.U2L", tg_U2L },
    { "net.rim.device.api.util.CharacterUtilities.isLowerCase", cu_isLowerCase },
    { "net.rim.device.api.util.CharacterUtilities.isUpperCase", cu_isUpperCase },
    { "net.rim.device.api.util.CharacterUtilities.isLetter", cu_isLetter },
    { "net.rim.device.api.util.CharacterUtilities.isDigit", cu_isDigit },
    { "net.rim.device.api.util.CharacterUtilities.isSpaceChar", cu_isSpaceChar },
    { "net.rim.device.api.util.CharacterUtilities.isISOControl", cu_isISOControl },
    { "net.rim.device.api.util.CharacterUtilities.isPrintable", cu_isPrintable },
    { "net.rim.device.api.util.CharacterUtilities.isPunctuation", cu_isPunctuation },
    { "net.rim.device.api.util.CharacterUtilities.isSymbol", cu_isSymbol },
    { "net.rim.device.api.util.CharacterUtilities.toLowerCase", cu_toLowerCase },
    { "net.rim.device.api.util.CharacterUtilities.toUpperCase", cu_toUpperCase },
    { "net.rim.device.api.util.CharacterUtilities.foldFullWidth", cu_identity },
    { "net.rim.device.api.util.CharacterUtilities.getOriginal", cu_identity },
    { "net.rim.device.api.util.CharacterUtilities.isHan", cu_isHan },
    { "net.rim.device.api.util.CharacterUtilities.getBidiType", cu_getBidiType },
    { "net.rim.device.api.util.CharacterUtilities.getPairedForm", cu_zero },
    { "net.rim.device.api.util.CRC32.update(I[BII)", crc32_b },
    { "net.rim.device.api.util.CRC32.update(II)", crc32_1 },
    { "net.rim.device.api.util.CRC32.updateInt", crc32_int },
    { "net.rim.device.api.util.CRC24.update(I[BII)", crc24_b },
    { "net.rim.device.api.util.CRC24.update(II)", crc24_1 },
    { "net.rim.device.api.util.CRC16.update(I[BII)", crc16_b },
    { "net.rim.device.api.util.CRC16.update(II)", crc16_1 },
    { "net.rim.device.api.util.NumberUtilities.appendNumber(Ljava.lang.StringBuffer;III)", nu_appendInt },
    { "net.rim.device.api.util.NumberUtilities.appendNumber(Ljava.lang.StringBuffer;JII)", nu_appendLong },
    { "net.rim.device.api.util.NumberUtilities.appendNumber([BIJI)", nu_appendBytes },
    { "net.rim.device.api.itpolicy.ITPolicy.readByte", itp_default3 },
    { "net.rim.device.api.itpolicy.ITPolicy.readInt", itp_default3 },
    { "net.rim.device.api.itpolicy.ITPolicy.readString", itp_null },
    { "net.rim.device.api.itpolicy.ITPolicy.readByteArray", itp_null },
    { NULL, NULL }
};
