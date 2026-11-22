/* PocketPager - BlackBerry OS family: net.rim.vm.Array / Memory natives. */
#include "natives.h"

static void resize_array(BBVM *vm, ref_t r, uint32_t n) {
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
    if (a[1] > o->len) resize_array(vm, a[0], a[1]);
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
    { NULL, NULL }
};
