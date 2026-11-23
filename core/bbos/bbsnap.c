/* Save states for the BlackBerry OS core: see bbsnap.h.
 *
 * What is saved is an "image": the core's writable variables followed by
 * its memory region (bbarena), plus the list of module files to re-read.
 * Most of the image hardly changes once the OS is up, so a save slot holds
 * only the 4 KB pages that differ from a shared base image:
 *
 *   base  (gzip): "BBBASE2", build[64], id, ranges, arena base + used,
 *                 the image bytes
 *   base.idx    : id, page count, one 64-bit hash per page (raw)
 *   slot  (gzip): "BBSLOT2", build[64], base id, ranges, code list,
 *                 arena base + used, extra record, then (page, 4 KB) pairs
 *                 ending with page 0xffffffff
 *
 * The base is written on the first save and kept while the build stays the
 * same. Loading reads the base into place, then the slot's pages. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "bbsnap.h"
#include "bbarena.h"
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

/* this file's own buffers must not come from the region it saves and loads */
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup

#define PAGE 4096u
#define END_PAGE 0xffffffffu

typedef struct Range { uintptr_t a; size_t n; } Range;
typedef struct Img { Range r[9]; int n; size_t len; } Img;   /* variables, then the arena */
typedef struct Find { uintptr_t anchor; Range r[8]; int n; } Find;

/* writable PT_LOAD segments of the binary holding the anchor, minus the
 * parts made read-only after relocation (RELRO) */
static int find_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    Find *f = data;
    int mine = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t s = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && f->anchor >= s && f->anchor < s + ph->p_memsz) mine = 1;
    }
    if (!mine) return 0;
    uintptr_t rs = 0, re = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_GNU_RELRO) { rs = info->dlpi_addr + ph->p_vaddr; re = rs + ph->p_memsz; }
    }
    for (int i = 0; i < info->dlpi_phnum && f->n < 8; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W)) continue;
        uintptr_t s = info->dlpi_addr + ph->p_vaddr, e = s + ph->p_memsz;
        if (rs < e && re > s) {
            if (s < rs) f->r[f->n++] = (Range){ s, rs - s };
            if (re < e && f->n < 8) f->r[f->n++] = (Range){ re, e - re };
        } else f->r[f->n++] = (Range){ s, e - s };
    }
    return 1;
}

static int image(const void *anchor, Img *im, size_t arenaUsed) {
    Find f;
    memset(&f, 0, sizeof f);
    f.anchor = (uintptr_t)anchor;
    dl_iterate_phdr(find_cb, &f);
    if (!f.n) return 0;
    im->n = 0; im->len = 0;
    for (int i = 0; i < f.n; i++) { im->r[im->n++] = f.r[i]; im->len += f.r[i].n; }
    im->r[im->n++] = (Range){ (uintptr_t)bb_arena_base(), arenaUsed };
    im->len += arenaUsed;
    return 1;
}

/* copy [off, off+n) of the image out (wr = 0) or in (wr = 1) */
static void img_io(const Img *im, size_t off, uint8_t *buf, size_t n, int wr) {
    for (int i = 0; i < im->n && n; i++) {
        if (off >= im->r[i].n) { off -= im->r[i].n; continue; }
        size_t k = im->r[i].n - off < n ? im->r[i].n - off : n;
        uint8_t *p = (uint8_t *)im->r[i].a + off;
        if (wr) memcpy(p, buf, k); else memcpy(buf, p, k);
        buf += k; n -= k; off = 0;
    }
    if (!wr && n) memset(buf, 0, n);
}

static uint64_t page_hash(const uint8_t *p, size_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h ^ (h >> 29);
}

static int fail(char *err, size_t n, const char *msg) { if (err && n) snprintf(err, n, "%s", msg); return -1; }

static int wr(gzFile g, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        unsigned k = n > (1u << 30) ? (1u << 30) : (unsigned)n;
        if (gzwrite(g, b, k) != (int)k) return 0;
        b += k; n -= k;
    }
    return 1;
}
static int rd(gzFile g, void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        unsigned k = n > (1u << 30) ? (1u << 30) : (unsigned)n;
        if (gzread(g, b, k) != (int)k) return 0;
        b += k; n -= k;
    }
    return 1;
}

static int wr_ranges(gzFile g, const Img *im) {
    uint64_t n = (uint64_t)im->n;
    if (!wr(g, &n, 8)) return 0;
    for (int i = 0; i < im->n; i++) {
        uint64_t a = im->r[i].a, l = im->r[i].n;
        if (!wr(g, &a, 8) || !wr(g, &l, 8)) return 0;
    }
    return 1;
}
/* the stored ranges must be this build's: same variables, same arena base
 * (the arena's length may differ) */
static int rd_ranges(gzFile g, const Img *cur, Img *saved) {
    uint64_t n;
    if (!rd(g, &n, 8) || n != (uint64_t)cur->n) return 0;
    saved->n = (int)n; saved->len = 0;
    for (int i = 0; i < saved->n; i++) {
        uint64_t a, l;
        if (!rd(g, &a, 8) || !rd(g, &l, 8)) return 0;
        if (a != cur->r[i].a || (i < saved->n - 1 && l != cur->r[i].n)) return 0;
        saved->r[i] = (Range){ (uintptr_t)a, (size_t)l };
        saved->len += (size_t)l;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the base image                                                       */

typedef struct Base { uint64_t id; uint64_t npages; uint64_t *hash; } Base;

static int read_index(const char *basePath, Base *b) {
    char p[1100];
    snprintf(p, sizeof p, "%s.idx", basePath);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    int ok = fread(&b->id, 8, 1, f) == 1 && fread(&b->npages, 8, 1, f) == 1 && b->npages < (1u << 24);
    if (ok) {
        b->hash = malloc(8 * (size_t)(b->npages ? b->npages : 1));
        ok = b->hash && fread(b->hash, 8, (size_t)b->npages, f) == b->npages;
    }
    fclose(f);
    if (!ok) { free(b->hash); b->hash = NULL; }
    return ok;
}

static int base_matches(const char *basePath, const char *build, uint64_t id) {
    gzFile g = gzopen(basePath, "rb");
    if (!g) return 0;
    char magic[8], b[64], want[64] = { 0 };
    uint64_t bid;
    snprintf(want, sizeof want, "%s", build);
    int ok = rd(g, magic, 8) && !memcmp(magic, "BBBASE2", 8) && rd(g, b, 64) && !memcmp(b, want, 64) && rd(g, &bid, 8) && bid == id;
    gzclose(g);
    return ok;
}

static int write_base(const char *basePath, const Img *im, const char *build, Base *out, char *err, size_t errn) {
    char tmp[1100], idx[1100], idxTmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", basePath);
    snprintf(idx, sizeof idx, "%s.idx", basePath);
    snprintf(idxTmp, sizeof idxTmp, "%s.idx.tmp", basePath);
    gzFile g = gzopen(tmp, "wb1");
    if (!g) return fail(err, errn, "cannot write the save file");
    char b[64] = { 0 };
    snprintf(b, sizeof b, "%s", build);
    uint64_t id = ((uint64_t)time(NULL) << 20) ^ (uint64_t)(uintptr_t)&id ^ (uint64_t)clock();
    uint64_t abase = im->r[im->n - 1].a, aused = im->r[im->n - 1].n;
    int ok = wr(g, "BBBASE2", 8) && wr(g, b, 64) && wr(g, &id, 8) && wr_ranges(g, im) && wr(g, &abase, 8) && wr(g, &aused, 8);
    for (int i = 0; ok && i < im->n; i++) ok = wr(g, (void *)im->r[i].a, im->r[i].n);
    if (gzclose(g) != Z_OK) ok = 0;
    /* the page hashes, for later saves to compare against */
    uint64_t npages = (im->len + PAGE - 1) / PAGE;
    uint64_t *h = malloc(8 * (size_t)(npages ? npages : 1));
    uint8_t *pg = malloc(PAGE);
    for (uint64_t p = 0; ok && p < npages; p++) {
        size_t n = im->len - p * PAGE < PAGE ? im->len - p * PAGE : PAGE;
        img_io(im, p * PAGE, pg, n, 0);
        h[p] = page_hash(pg, n);
    }
    free(pg);
    FILE *f = ok ? fopen(idxTmp, "wb") : NULL;
    if (f) {
        ok = fwrite(&id, 8, 1, f) == 1 && fwrite(&npages, 8, 1, f) == 1 && fwrite(h, 8, (size_t)npages, f) == npages;
        if (fclose(f)) ok = 0;
    } else ok = 0;
    if (!ok || rename(tmp, basePath) || rename(idxTmp, idx)) {
        remove(tmp); remove(idxTmp); free(h);
        return fail(err, errn, "could not write the save file (storage full?)");
    }
    out->id = id; out->npages = npages; out->hash = h;
    return 0;
}

/* ------------------------------------------------------------------ */

int bbsnap_save(const char *path, const char *basePath, const void *anchor, const char *build,
                const void *extra, size_t extraLen, char *err, size_t errn) {
    Img im;
    if (!image(anchor, &im, bb_arena_used())) return fail(err, errn, "cannot find the core's variables");
    Base base = { 0 };
    if (!read_index(basePath, &base) || !base_matches(basePath, build, base.id)) {
        free(base.hash); base.hash = NULL;
        if (write_base(basePath, &im, build, &base, err, errn) < 0) return -1;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    gzFile g = gzopen(tmp, "wb1");
    if (!g) { free(base.hash); return fail(err, errn, "cannot write the save file"); }
    char b[64] = { 0 };
    snprintf(b, sizeof b, "%s", build);
    int nc;
    const BBCodeEnt *ce = bb_code_list(&nc);
    uint64_t ncode = (uint64_t)nc, abase = im.r[im.n - 1].a, aused = im.r[im.n - 1].n, el = extraLen;
    int ok = wr(g, "BBSLOT2", 8) && wr(g, b, 64) && wr(g, &base.id, 8) && wr_ranges(g, &im)
          && wr(g, &ncode, 8) && wr(g, ce, sizeof *ce * (size_t)nc)
          && wr(g, &abase, 8) && wr(g, &aused, 8) && wr(g, &el, 8) && (!extraLen || wr(g, extra, extraLen));
    uint8_t *pg = malloc(PAGE);
    uint64_t npages = (im.len + PAGE - 1) / PAGE;
    for (uint64_t p = 0; ok && p < npages; p++) {
        size_t n = im.len - p * PAGE < PAGE ? im.len - p * PAGE : PAGE;
        img_io(&im, p * PAGE, pg, n, 0);
        if (n < PAGE) memset(pg + n, 0, PAGE - n);
        if (p < base.npages && page_hash(pg, n) == base.hash[p]) continue;
        uint32_t pi = (uint32_t)p;
        ok = wr(g, &pi, 4) && wr(g, pg, PAGE);
    }
    uint32_t endp = END_PAGE;
    ok = ok && wr(g, &endp, 4);
    free(pg); free(base.hash);
    if (gzclose(g) != Z_OK) ok = 0;
    if (!ok) { remove(tmp); return fail(err, errn, "could not write the save file (storage full?)"); }
    if (rename(tmp, path)) { remove(tmp); return fail(err, errn, "could not write the save file"); }
    return 0;
}

int bbsnap_load(const char *path, const char *basePath, const void *anchor, const char *build,
                void *extra, size_t extraLen, char *err, size_t errn) {
    Img cur;
    if (!image(anchor, &cur, bb_arena_used())) return fail(err, errn, "cannot find the core's variables");
    char want[64] = { 0 };
    snprintf(want, sizeof want, "%s", build);

    /* 1. read and check the slot's header */
    gzFile g = gzopen(path, "rb");
    if (!g) return fail(err, errn, "no save in this slot");
    gzbuffer(g, 1 << 20);
    char magic[8], b[64];
    uint64_t baseId, ncode, abase, aused, el;
    Img slot;
    if (!rd(g, magic, 8) || memcmp(magic, "BBSLOT2", 8) || !rd(g, b, 64)) { gzclose(g); return fail(err, errn, "not a save file"); }
    if (memcmp(b, want, 64)) { gzclose(g); return fail(err, errn, "this save was made by a different version of the app"); }
    if (!rd(g, &baseId, 8) || !rd_ranges(g, &cur, &slot)) {
        gzclose(g); return fail(err, errn, "the emulator could not load at its usual address; restart the app and try again");
    }
    if (!rd(g, &ncode, 8) || ncode > 100000) { gzclose(g); return fail(err, errn, "save file is damaged"); }
    BBCodeEnt *ce = malloc(sizeof *ce * (size_t)(ncode ? ncode : 1));
    if (!rd(g, ce, sizeof *ce * (size_t)ncode) || !rd(g, &abase, 8) || !rd(g, &aused, 8) || !rd(g, &el, 8)
        || el != extraLen || (extraLen && !rd(g, extra, extraLen))) {
        free(ce); gzclose(g); return fail(err, errn, "save file is damaged");
    }
    if (abase != (uint64_t)(uintptr_t)bb_arena_base() || !bb_arena_prepare((size_t)aused)) {
        free(ce); gzclose(g); return fail(err, errn, "the emulator's memory is not at its usual address");
    }

    /* 2. the base: must be the one this slot was made against */
    if (!base_matches(basePath, build, baseId)) { free(ce); gzclose(g); return fail(err, errn, "this save's base file is missing"); }
    gzFile bg = gzopen(basePath, "rb");
    gzbuffer(bg, 1 << 20);
    uint64_t bid, babase, baused;
    Img bimg;
    char bm[8], bb[64];
    if (!rd(bg, bm, 8) || !rd(bg, bb, 64) || !rd(bg, &bid, 8) || !rd_ranges(bg, &cur, &bimg) || !rd(bg, &babase, 8) || !rd(bg, &baused, 8)
        || babase != abase || !bb_arena_prepare((size_t)baused)) {
        gzclose(bg); free(ce); gzclose(g); return fail(err, errn, "this save's base file is damaged");
    }

    /* 3. module files (checked before anything changes) */
    if (bb_code_restore(ce, (int)ncode, err, errn) < 0) { gzclose(bg); free(ce); gzclose(g); return -1; }
    free(ce);

    /* 4. from here on the running OS is replaced: base image, then pages */
    Img dst = cur;
    dst.r[dst.n - 1].n = (size_t)(aused > baused ? aused : baused);
    dst.len = 0;
    for (int i = 0; i < dst.n; i++) dst.len += dst.r[i].n;
    int ok = 1;
    for (int i = 0; ok && i < bimg.n; i++) ok = rd(bg, (void *)bimg.r[i].a, bimg.r[i].n);
    gzclose(bg);
    uint8_t *pg = malloc(PAGE);
    for (;;) {
        uint32_t pi;
        if (!ok || !rd(g, &pi, 4)) { ok = 0; break; }
        if (pi == END_PAGE) break;
        if (!rd(g, pg, PAGE) || (size_t)pi * PAGE >= dst.len) { ok = 0; break; }
        size_t n = dst.len - (size_t)pi * PAGE < PAGE ? dst.len - (size_t)pi * PAGE : PAGE;
        img_io(&dst, (size_t)pi * PAGE, pg, n, 1);
    }
    free(pg);
    gzclose(g);
    if (!ok) return fail(err, errn, "save file is damaged (the emulator must restart)");
    return 0;
}
