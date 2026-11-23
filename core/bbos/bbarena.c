/* The fixed-address memory region behind save states (see bbarena.h).
 *
 * Layout: a header (allocator state) at the base, then blocks. Each block
 * has a 16-byte header holding its capacity. Small sizes use free lists by
 * 16-byte class up to 1 KB and by power of two up to 1 MB; bigger blocks
 * go on one first-fit list. New memory comes from bumping `top`. Everything
 * the allocator knows lives inside the region, so it is saved with it. */
#define BBARENA_IMPL
#include "bbarena.h"
/* the allocator itself, and its temporary buffers, use the C library */
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#if UINTPTR_MAX > 0xffffffffu
#define ARENA_ADDR ((uintptr_t)0x5800000000ull)   /* fits a 39-bit address space */
#define ARENA_SIZE ((size_t)3 << 30)
#else   /* 32-bit: wherever the system puts it (save states then stay within one run) */
#define ARENA_ADDR ((uintptr_t)0)
#define ARENA_SIZE ((size_t)1 << 30)
#endif
#define NSMALL 64                                  /* 16..1024 in steps of 16 */
#define NPOW 6                                     /* 2 KB .. 64 KB */

typedef struct Blk { size_t cap; size_t pad; } Blk;   /* 16 bytes before the data */
typedef struct FreeBig { size_t cap; size_t pad; struct FreeBig *next; } FreeBig;

typedef struct ArenaHdr {
    uint64_t magic;
    uintptr_t top;                 /* next unused byte */
    void *small[NSMALL];
    void *pow[NPOW];
    FreeBig *big;
} ArenaHdr;

static ArenaHdr *A;

int bb_arena_init(void) {
    if (A) return 1;
    void *p = mmap((void *)ARENA_ADDR, ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "bbarena: cannot reserve memory\n"); abort(); }
    if (ARENA_ADDR && (uintptr_t)p != ARENA_ADDR) fprintf(stderr, "bbarena: region not at its fixed address; saves won't load in another run\n");
    A = (ArenaHdr *)p;
    A->magic = 0x4242415245414e41ull;
    A->top = ((uintptr_t)p + sizeof(ArenaHdr) + 15) & ~(uintptr_t)15;
    return !ARENA_ADDR || (uintptr_t)p == ARENA_ADDR;
}
void *bb_arena_base(void) { if (!A) bb_arena_init(); return A; }
size_t bb_arena_used(void) { if (!A) bb_arena_init(); return A->top - (uintptr_t)A; }
size_t bb_arena_reserved(void) { return ARENA_SIZE; }
int bb_arena_prepare(size_t used) { if (!A) bb_arena_init(); return used <= ARENA_SIZE && (!ARENA_ADDR || (uintptr_t)A == ARENA_ADDR); }

static size_t class_cap(size_t n, int *small, int *pw) {
    *small = -1; *pw = -1;
    if (n <= 1024) { size_t c = (n + 15) & ~(size_t)15; if (!c) c = 16; *small = (int)(c / 16 - 1); return c; }
    size_t c = 2048; int i = 0;
    while (c < n && i < NPOW - 1) { c <<= 1; i++; }
    if (c >= n) { *pw = i; return c; }
    return (n + 4095) & ~(size_t)4095;   /* bigger: whole pages, on the big list */
}

static void *bump(size_t cap) {
    uintptr_t b = A->top;
    if (b + sizeof(Blk) + cap > (uintptr_t)A + ARENA_SIZE) { fprintf(stderr, "bbarena: out of memory\n"); abort(); }
    A->top = b + sizeof(Blk) + cap;
    Blk *h = (Blk *)b;
    h->cap = cap;
    return h + 1;
}

void *bb_malloc(size_t n) {
    if (!A) bb_arena_init();
    int s, pw;
    size_t cap = class_cap(n, &s, &pw);
    void **list = s >= 0 ? &A->small[s] : pw >= 0 ? &A->pow[pw] : NULL;
    if (list) {
        if (*list) { void *p = *list; *list = *(void **)p; return p; }
        return bump(cap);
    }
    /* big: best fit, within a quarter of the size */
    FreeBig **best = NULL;
    for (FreeBig **pp = &A->big; *pp; pp = &(*pp)->next) {
        FreeBig *f = *pp;
        if (f->cap >= cap && f->cap <= cap + cap / 4 && (!best || f->cap < (*best)->cap)) best = pp;
    }
    if (best) { FreeBig *f = *best; *best = f->next; return (Blk *)f + 1; }
    return bump(cap);
}

void bb_free(void *p) {
    if (!p) return;
    Blk *h = (Blk *)p - 1;
    int s, pw;
    size_t cap = h->cap;
    class_cap(cap, &s, &pw);
    if (s >= 0 && (size_t)(s + 1) * 16 == cap) { *(void **)p = A->small[s]; A->small[s] = p; return; }
    if (pw >= 0 && ((size_t)2048 << pw) == cap) { *(void **)p = A->pow[pw]; A->pow[pw] = p; return; }
    /* big: give the pages back (they read as zero, which also keeps
     * save states small) and keep the block for reuse */
    uintptr_t a = ((uintptr_t)p + 4095) & ~(uintptr_t)4095, e = ((uintptr_t)p + cap) & ~(uintptr_t)4095;
    if (e > a) madvise((void *)a, e - a, MADV_DONTNEED);
    FreeBig *f = (FreeBig *)h;
    f->next = A->big;
    A->big = f;
}

void *bb_calloc(size_t n, size_t m) {
    size_t t = n * m;
    if (m && t / m != n) return NULL;
    void *p = bb_malloc(t);
    memset(p, 0, t);
    return p;
}

void *bb_realloc(void *p, size_t n) {
    if (!p) return bb_malloc(n);
    if (!n) { bb_free(p); return NULL; }
    Blk *h = (Blk *)p - 1;
    if (n <= h->cap) return p;
    void *q = bb_malloc(n);
    memcpy(q, p, h->cap);
    bb_free(p);
    return q;
}

char *bb_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = bb_malloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* module files                                                         */

#include <zlib.h>
#if UINTPTR_MAX > 0xffffffffu
#define CODE_ADDR ((uintptr_t)0x5000000000ull)
#define CODE_SIZE ((size_t)1 << 30)
#else
#define CODE_ADDR ((uintptr_t)0)
#define CODE_SIZE ((size_t)384 << 20)
#endif
#define CODE_MAXENT 8192

typedef struct CodeHdr { uintptr_t top; int n; BBCodeEnt e[CODE_MAXENT]; } CodeHdr;
static CodeHdr *C;

static int code_init(void) {
    if (C) return !CODE_ADDR || (uintptr_t)C == CODE_ADDR;
    void *p = mmap((void *)CODE_ADDR, CODE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "bbarena: cannot reserve module memory\n"); abort(); }
    C = p;
    C->top = ((uintptr_t)p + sizeof(CodeHdr) + 4095) & ~(uintptr_t)4095;
    return !CODE_ADDR || (uintptr_t)p == CODE_ADDR;
}

int bb_code_owns(const void *p) { return C && (uintptr_t)p >= (uintptr_t)C && (uintptr_t)p < (uintptr_t)C + CODE_SIZE; }

void *bb_code_load(const char *path, size_t *size, size_t pad) {
    code_init();
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || C->n >= CODE_MAXENT || C->top + (size_t)sz + pad > (uintptr_t)C + CODE_SIZE) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)C->top;
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); memset(b, 0, (size_t)sz); return NULL; }
    fclose(f);
    BBCodeEnt *e = &C->e[C->n++];
    snprintf(e->path, sizeof e->path, "%s", path);
    e->off = (uint64_t)((uintptr_t)b - (uintptr_t)C);
    e->size = (uint64_t)sz;
    e->pad = (uint32_t)pad;
    e->crc = (uint32_t)crc32(0, b, (uInt)sz);
    C->top = ((uintptr_t)b + (size_t)sz + pad + 15) & ~(uintptr_t)15;
    *size = (size_t)sz;
    return b;
}

const BBCodeEnt *bb_code_list(int *n) { code_init(); *n = C->n; return C->e; }

static int same_ent(const BBCodeEnt *a, const BBCodeEnt *b) {
    return a->off == b->off && a->size == b->size && a->crc == b->crc && a->pad == b->pad && !strcmp(a->path, b->path);
}

int bb_code_restore(const BBCodeEnt *e, int n, char *err, size_t errn) {
    code_init();
    if (CODE_ADDR && (uintptr_t)C != CODE_ADDR) { snprintf(err, errn, "the emulator's module memory is not at its usual address"); return -1; }
    if (n < 0 || n > CODE_MAXENT) { snprintf(err, errn, "save file is damaged"); return -1; }
    /* first check every file that isn't already in place, writing nothing,
     * so a failed load leaves the running OS alone */
    uint8_t *tmp = NULL; size_t tmpCap = 0;
    for (int i = 0; i < n; i++) {
        if (i < C->n && same_ent(&C->e[i], &e[i])) continue;
        if (e[i].off + e[i].size + e[i].pad > CODE_SIZE) { free(tmp); snprintf(err, errn, "save file is damaged"); return -1; }
        if (e[i].size > tmpCap) { free(tmp); tmpCap = (size_t)e[i].size; tmp = malloc(tmpCap); }
        FILE *f = fopen(e[i].path, "rb");
        size_t got = f && tmp ? fread(tmp, 1, (size_t)e[i].size, f) : 0;
        if (f) fclose(f);
        if (got != e[i].size || (uint32_t)crc32(0, tmp, (uInt)e[i].size) != e[i].crc) {
            const char *nm = strrchr(e[i].path, '/');
            snprintf(err, errn, "module %s is missing or has changed since this save", nm ? nm + 1 : e[i].path);
            free(tmp);
            return -1;
        }
    }
    free(tmp);
    uintptr_t top = ((uintptr_t)C + sizeof(CodeHdr) + 4095) & ~(uintptr_t)4095;
    for (int i = 0; i < n; i++) {
        uint8_t *b = (uint8_t *)C + e[i].off;
        if (!(i < C->n && same_ent(&C->e[i], &e[i]))) {
            FILE *f = fopen(e[i].path, "rb");
            if (f) { if (fread(b, 1, (size_t)e[i].size, f) != e[i].size) memset(b, 0, (size_t)e[i].size); fclose(f); }
            memset(b + e[i].size, 0, e[i].pad);
        }
        uintptr_t t = ((uintptr_t)b + (size_t)e[i].size + e[i].pad + 15) & ~(uintptr_t)15;
        if (t > top) top = t;
    }
    memcpy(C->e, e, sizeof *e * (size_t)n);
    C->n = n;
    C->top = top;
    return 0;
}

size_t bb_arena_free_bytes(void) {
    size_t t = 0;
    for (int i = 0; i < NSMALL; i++) for (void *p = A->small[i]; p; p = *(void **)p) t += (size_t)(i + 1) * 16;
    for (int i = 0; i < NPOW; i++) for (void *p = A->pow[i]; p; p = *(void **)p) t += (size_t)2048 << i;
    for (FreeBig *f = A->big; f; f = f->next) t += f->cap;
    return t;
}
