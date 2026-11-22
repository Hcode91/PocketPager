/* PocketPager - BlackBerry OS family: shared helpers for native methods. */
#ifndef BBOS_NATIVES_H
#define BBOS_NATIVES_H
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

typedef struct NatEnt { const char *name; NativeFn fn; } NatEnt;

#define NAT(fn) static int fn(BBVM *vm, Thread *t, uint32_t *a, uint32_t *ret)
#define UNUSED (void)vm; (void)t; (void)a; (void)ret

typedef struct Proc { int id; ref_t obj; ref_t mainThread; } Proc;

typedef struct NatState {
    int nextPid;
    uint64_t bootMs;
    Proc *procs; int nprocs, capprocs;
    char **modNames; int nmods, capmods;
    void *gfx;      /* natives_gfx.c */
    void *sys;      /* natives_sys.c */
} NatState;

NatState *nat_state(BBVM *vm);
uint64_t nat_now(BBVM *vm);
int nat_module_handle(BBVM *vm, const char *name);
BBModule *nat_module_by_handle(BBVM *vm, int h);
Proc *nat_proc(BBVM *vm, int pid);
Thread *nat_thread_of(BBVM *vm, ref_t threadObj);
ref_t nat_thread_obj(BBVM *vm, Thread *t);
Thread *nat_start_thread(BBVM *vm, ref_t threadObj, int pid);
BBRoutine *nat_virtual(BBVM *vm, ref_t obj, const char *name, const char *sig);

static inline void nat_ret64(uint32_t *ret, int64_t v) { ret[0] = (uint32_t)v; ret[1] = (uint32_t)((uint64_t)v >> 32); }
static inline int64_t nat_arg64(const uint32_t *a) { return (int64_t)((uint64_t)a[0] | (uint64_t)a[1] << 32); }
static inline double nat_argd(const uint32_t *a) { int64_t v = nat_arg64(a); double d; memcpy(&d, &v, 8); return d; }
static inline void nat_retd(uint32_t *ret, double d) { int64_t v; memcpy(&v, &d, 8); nat_ret64(ret, v); }
static inline unsigned nat_char(const Obj *s, uint32_t i) { return s->et == BT_CHAR ? ((const uint16_t *)s->d)[i] : ((const uint8_t *)s->d)[i]; }

void nat_post_message(BBVM *vm, int device, int event, int sub, int d0, int d1, ref_t o0, ref_t o1);
int nat_messages_pending(void);

extern const NatEnt kNativesLang[];
extern const NatEnt kNativesRim[];
extern const NatEnt kNativesVm[];
#endif
