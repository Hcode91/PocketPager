/* PocketPager - BlackBerry OS family: RIM .cod module loader.
 *
 * A .cod is one linked Java module: a header, a code section holding every
 * routine's bytecode, and a data section holding names, class definitions,
 * type lists and the fixup tables that tie the code to other modules.  All
 * values are little endian.  Large modules are split into siblings
 * ("name", "name-1", ...), each its own .cod. */
#ifndef BBOS_COD_H
#define BBOS_COD_H
#include <stdint.h>
#include <stddef.h>

struct BBModule;
struct BBClass;
struct BBRoutine;

/* One entry of a type list. */
enum { BT_BOOL = 1, BT_BYTE = 2, BT_CHAR = 3, BT_SHORT = 4, BT_INT = 5,
       BT_LONG = 6, BT_CLASS = 7, BT_ARRAY = 8, BT_CLASS9 = 9,
       BT_TEN = 10, BT_FLOAT = 11, BT_DOUBLE = 12 };
typedef struct {
    uint8_t kind;   /* BT_* */
    uint8_t dims;   /* arrays: dimensions */
    uint8_t elem;   /* arrays: element base type (BT_*) */
    uint8_t m, c;   /* class reference: module ordinal, class ordinal */
} BBType;

typedef struct {
    uint16_t off;       /* code offset of the patched operand */
    uint8_t kind;       /* FX_* */
    uint8_t pad;
    uint32_t idx;       /* index into the module's member/class ref tables */
} BBFixup;

enum { FX_ROUTINE = 1, FX_SROUTINE, FX_VROUTINE, FX_CLASS, FX_FIELD,
       FX_LFIELD, FX_SFIELD, FX_MODULE };

typedef struct {
    uint16_t mod;       /* module ordinal in this module's table */
    const char *pkg, *cls;
    struct BBClass *resolved;
} BBClassRef;

typedef struct {
    int cref;           /* index into classrefs (-1 for local field refs) */
    const char *name;
    uint16_t tl;        /* type list (signature or field type) */
    uint16_t tl2;       /* routine refs: return type list */
    uint8_t lcls, lfield; /* local field refs: class ordinal, field index */
    void *resolved;
} BBMemberRef;

typedef struct BBField {
    const char *name;
    uint16_t tl;
    uint16_t addr;      /* statics: slot in the module static area */
    uint8_t attr;
    uint8_t wide;       /* long/double: two slots */
    uint16_t slot;      /* instance: resolved slot */
} BBField;

typedef struct BBRoutine {
    struct BBModule *mod;
    struct BBClass *cls;
    const char *name;
    uint16_t off;       /* code offset of first instruction */
    uint16_t size;      /* bytecode length */
    uint16_t attr;
    uint16_t proto, type; /* type lists: parameters (incl. this), return */
    uint8_t maxl, maxs, nstack;
    uint8_t isStatic;
    uint16_t exOff;     /* code offset of exception table, 0 if none */
    int16_t nargs;      /* parameter slots */
    int8_t ret;         /* 0 void, 1 one slot, 2 two slots */
    int16_t vindex;     /* vtable index for virtual routines, else -1 */
    void *native;       /* bound native implementation */
    void *prep;         /* interpreter scratch */
} BBRoutine;

typedef struct BBClass {
    struct BBModule *mod;
    int ord;
    const char *pkg, *name;
    char *full;          /* "pkg.name" */
    uint8_t supM, supC;
    struct BBClass *super;
    uint16_t attr, size, clinitOff, initOff;
    int nv, nnv, ns;
    BBRoutine **v, **nv_, **s;
    int nf, nsf;
    BBField *f, *sf;
    int nif;
    uint8_t (*ifs)[2];
    struct BBClass **ifaces;
    /* runtime */
    int linked;
    int nslots;         /* total instance slots incl. super */
    int vtsize;
    BBRoutine **vtable;
    int gidx;           /* VM-wide class number (per-process init state) */
    void *classObj;
    void *rt;           /* runtime extras */
} BBClass;

typedef struct BBModule {
    char name[96];
    uint8_t *raw; size_t rawSize;
    uint8_t *code; uint32_t codeSize;
    uint8_t *data; uint32_t dataSize;
    uint16_t flags, version;
    uint8_t dflags, dver;
    int nmod;
    uint16_t *modNameOff;
    struct BBModule **mods; /* resolved ordinals; [0] = self */
    int ncls;
    BBClass *cls;
    int nroutines;
    BBRoutine *routines;    /* sorted by offset */
    int ncref; BBClassRef *crefs;
    uint16_t *crefOff;      /* data offset of each class ref */
    int nmref; BBMemberRef *mrefs;
    int nfix; BBFixup *fix; /* sorted by offset */
    uint16_t staticSize;
    uint32_t *statics;      /* initial static area (each process gets a copy) */
    int gidx;               /* VM-wide module number */
    uint16_t o_typelists, o_ifrefs;
    struct BBModule *next;
} BBModule;

/* Parse a single .cod image (takes ownership of buf). NULL on error, with
 * a message in err. */
BBModule *cod_parse(const char *name, uint8_t *buf, size_t size, char *err, size_t errn);
void cod_free(BBModule *m);

/* Decode an identifier stored at data offset o ("" for 0). The string lives
 * in the module's name arena. */
const char *cod_name(BBModule *m, uint16_t o);
/* Raw latin-1 bytes at data offset o (NUL terminated). */
const char *cod_cstr(BBModule *m, uint16_t o);

/* Read a type list at data offset o; returns count, fills up to max. */
int cod_typelist(BBModule *m, uint16_t o, BBType *out, int max);
/* Slots taken by a type list (longs/doubles count 2). */
int cod_typeslots(BBModule *m, uint16_t o);

BBRoutine *cod_routine_at(BBModule *m, uint16_t off);
const BBFixup *cod_fixup_at(BBModule *m, uint16_t off);

/* Instruction size at pc (handles prefix and variable-length ops). */
int cod_insn_size(const uint8_t *code, uint32_t pc, int *opOut);
const char *cod_op_name(int op);

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

#endif
