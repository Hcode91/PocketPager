/* PocketPager - BlackBerry OS family: Java virtual machine for RIM .cod code.
 *
 * Runs RIM's own Java (the OS libraries and apps) the way the handheld's VM
 * does: modules are loaded from .cod files, linked through their fixup
 * tables, and interpreted. Natives (the C half of RIM's runtime) are bound by
 * class and method name, see natives.c. */
#ifndef BBOS_VM_H
#define BBOS_VM_H
#include "cod.h"
#include <stdint.h>

typedef uint32_t ref_t;
#define REF_BASE 0x60000000u
#define REF_MAX  0x0fffffffu

enum { OK_INST = 1, OK_ARRAY = 2 };
enum { OF_STRING = 1, OF_MARK = 2, OF_PERM = 4, OF_CLASS = 8 };

typedef struct Obj {
    BBClass *cls;       /* instances/strings: class; object arrays: element class */
    uint8_t kind, et, dims, flags;
    uint32_t len;       /* array length, or instance slot count */
    uint32_t hash;
    uint32_t mon;
    void *host;         /* native side data (pixels, class pointer, ...) */
    void (*hostFree)(void *);
    uint32_t d[];       /* slots or array payload */
} Obj;

typedef struct Ins {
    uint16_t op;        /* opcode (0x100+ for extended), | 0x8000 when resolved */
    uint16_t pc;        /* code offset */
    int32_t a, b;
    void *p;
} Ins;

typedef struct ExH { uint16_t start, end; int handler; uint8_t m, c; BBClass *cls; } ExH;

typedef struct Prep {
    int n;
    Ins *ins;
    int nlocals;
    int nex;
    ExH *ex;
} Prep;

typedef struct Frame {
    BBRoutine *r;
    Prep *p;
    int ip;
    uint32_t *lv;       /* locals */
    uint32_t *sp;       /* operand stack top (next free) */
    ref_t lock;         /* synchronized method monitor */
    uint8_t clinit;     /* frame is a class initializer */
    uint8_t nativeBase; /* frame where a nested vm_call returns */
    uint8_t tailRet;    /* 1 + slots to return after a tail call */
    BBClass *initCls;
} Frame;

enum { TS_READY = 0, TS_SLEEP, TS_WAIT, TS_MONITOR, TS_JOIN, TS_DEAD, TS_EVENT };

typedef struct Thread {
    int id;
    ref_t obj;          /* java.lang.Thread */
    uint32_t *stack, *stackEnd;
    Frame *fr;
    int nfr, maxfr;
    int state;
    uint64_t wakeAt;
    ref_t waitObj;      /* monitor waited on */
    int waitCount;      /* saved recursion count while waiting */
    ref_t pendingEx;
    ref_t joinTarget;
    int interrupted;
    int procId;
    ref_t tsd;          /* net.rim.vm.ThreadSpecificData */
    void *user;
    struct Thread *next;
} Thread;

typedef struct Monitor { ref_t obj; Thread *owner; int count; struct Monitor *next; } Monitor;

typedef struct BBVM BBVM;
typedef int (*NativeFn)(BBVM *vm, Thread *t, uint32_t *args, uint32_t *ret);
enum { NAT_OK = 0, NAT_THROWN = 1, NAT_BLOCK = 2, NAT_CALLED = 3 };

typedef struct Family {
    char base[96];
    int n;
    BBModule **mods;
    struct Family *next;
} Family;

typedef struct HostIf {
    void *ctx;
    void (*log)(void *ctx, const char *msg);
    uint64_t (*nowMs)(void *ctx);
} HostIf;

struct BBVM {
    char *dirs[8];
    int ndirs;
    BBModule *mods;
    Family *fams;
    HostIf host;

    /* heap */
    Obj **objs;
    uint32_t nobjs, capobjs;
    uint32_t *freeList; uint32_t nfree, capfree;
    size_t heapBytes, gcAt;

    /* threads */
    Thread *threads, *cur;
    int nextTid;
    Monitor *monitors;
    uint64_t timeMs;

    /* well-known classes */
    BBClass *cObject, *cString, *cClass, *cThread, *cThrowable;

    /* string literal cache: (module, offset) -> ref */
    struct LitEnt { BBModule *m; uint16_t off; ref_t r; } *lits;
    int nlits, caplits;

    /* tracing */
    int trace;
    const char *traceFilter;
    long long icount;
    int fatal;
    char fatalMsg[512];
    void *nat;          /* native registry */
    void (*tick)(BBVM *vm);   /* timers etc., called by the scheduler */
    void *ext;          /* natives.c state */
};

BBVM *vm_new(void);
void vm_free(BBVM *vm);
void vm_add_dir(BBVM *vm, const char *dir);
void vm_log(BBVM *vm, const char *fmt, ...);
void vm_fatal(BBVM *vm, const char *fmt, ...);

/* modules and classes */
BBModule *vm_module(BBVM *vm, const char *name);
Family *vm_family(BBVM *vm, const char *name);
BBClass *vm_class(BBVM *vm, const char *family, const char *fullName);
BBClass *vm_find_class_anywhere(BBVM *vm, const char *fullName);
BBClass *vm_cls_ref(BBVM *vm, BBModule *m, int mo, int co);
BBClass *vm_resolve_cref(BBVM *vm, BBModule *m, int i);
BBClass *vm_cls_direct(BBVM *vm, BBModule *m, int mo, int co);
BBModule *vm_mod_ord(BBVM *vm, BBModule *m, int ord);
int vm_link(BBVM *vm, BBClass *k);
int vm_is_subclass(BBClass *k, BBClass *of);
BBRoutine *vm_find_method(BBVM *vm, BBClass *k, const char *name, const char *sig);
const char *vm_sig(BBVM *vm, BBModule *m, uint16_t tl, int skipThis, char *buf, size_t n);

/* heap */
Obj *vm_obj(BBVM *vm, ref_t r);
ref_t vm_new_obj(BBVM *vm, BBClass *k);
ref_t vm_new_array(BBVM *vm, int et, BBClass *ecls, int dims, uint32_t len);
ref_t vm_new_string(BBVM *vm, const char *utf8);
ref_t vm_new_string16(BBVM *vm, const uint16_t *s, uint32_t n);
int vm_string_utf8(BBVM *vm, ref_t s, char *buf, size_t n);
int vm_elem_size(int et);
ref_t vm_class_obj(BBVM *vm, BBClass *k);
void vm_gc(BBVM *vm);
int vm_instanceof(BBVM *vm, ref_t r, BBClass *k);

/* threads and execution */
Thread *vm_new_thread(BBVM *vm, ref_t threadObj);
int vm_push_call(BBVM *vm, Thread *t, BBRoutine *r, const uint32_t *args, int nargs);
int vm_call(BBVM *vm, Thread *t, BBRoutine *r, const uint32_t *args, int nargs, uint32_t *ret);
void vm_throw(BBVM *vm, Thread *t, const char *cls, const char *msg);
int vm_run(BBVM *vm, long long budget);
int vm_run_thread(BBVM *vm, Thread *t, long long budget, int baseDepth);
int vm_init_class(BBVM *vm, Thread *t, BBClass *k);
void vm_backtrace(BBVM *vm, Thread *t);

/* monitors */
int vm_monitor_enter(BBVM *vm, Thread *t, ref_t o);
int vm_monitor_exit(BBVM *vm, Thread *t, ref_t o);
Monitor *vm_monitor(BBVM *vm, ref_t o, int create);

/* debugging */
#include <stdio.h>
void vm_disasm(BBVM *vm, BBRoutine *r, FILE *out);
void vm_disasm_class(BBVM *vm, BBClass *k, FILE *out);

/* natives */
void natives_init(BBVM *vm);
NativeFn natives_lookup(BBVM *vm, BBRoutine *r);

#endif
