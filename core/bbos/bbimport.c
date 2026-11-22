/* Import a BlackBerry OS from RIM's Windows installer (e.g. the Bold 9000
 * "9000M_PBr5.0.0_..._Wind.exe"): pull out every .cod module and write them
 * to one folder, named by the module name each file declares.
 *
 * The installer nests four formats:
 *   InstallShield setup stream (in the .exe) -> the .msi (lightly scrambled)
 *   .msi = OLE compound file -> the "Data1.cab" stream
 *   cabinet (MSZIP) -> loader files, some .cod, some zips of sibling .cod
 *   zip -> sibling .cod files (name-1.cod, name-2.cod...)
 */
#include "bbimport.h"
#include "cod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <zlib.h>

typedef struct Imp {
    const char *outDir;
    bbimport_progress cb;
    void *ctx;
    char *err; size_t errn;
    int written;
} Imp;

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int fail(Imp *im, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (im->err && im->errn) vsnprintf(im->err, im->errn, fmt, ap);
    va_end(ap);
    return -1;
}
static void progress(Imp *im, const char *what, int pct) { if (im->cb) im->cb(im->ctx, what, pct); }

/* ------------------------------------------------------------------ */
/* output: one .cod module                                             */

static int is_cod(const uint8_t *p, size_t n) { return n >= 44 && r32(p) == 0xffffc0deu; }

/* the module's own name is the first entry of its module table */
static int cod_selfname(const uint8_t *p, size_t n, char *name, size_t nn) {
    uint8_t *copy = malloc(n);
    if (!copy) return 0;
    memcpy(copy, p, n);
    char e[128];
    BBModule *m = cod_parse("import", copy, n, e, sizeof e);
    if (!m) return 0;
    int ok = 0;
    if (m->nmod > 0) {
        const char *s = cod_cstr(m, m->modNameOff[0]);
        if (s && *s && strlen(s) < nn && !strchr(s, '/') && !strchr(s, '\\')) { snprintf(name, nn, "%s", s); ok = 1; }
    }
    cod_free(m);
    return ok;
}

static int write_cod(Imp *im, const char *name, const uint8_t *p, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.cod", im->outDir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return fail(im, "cannot write %s", path);
    size_t w = fwrite(p, 1, n, f);
    fclose(f);
    if (w != n) return fail(im, "cannot write %s (disk full?)", path);
    im->written++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* zip: sibling modules bundled in one ".cod"                          */

static int inflate_raw(const uint8_t *src, size_t n, uint8_t *dst, size_t dn) {
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return 0;
    z.next_in = (Bytef *)src; z.avail_in = (uInt)n;
    z.next_out = dst; z.avail_out = (uInt)dn;
    int rc = inflate(&z, Z_FINISH);
    size_t got = z.total_out;
    inflateEnd(&z);
    return (rc == Z_STREAM_END || rc == Z_OK || rc == Z_BUF_ERROR) && got == dn;
}

static int do_file(Imp *im, const uint8_t *p, size_t n);

/* entries come from the central directory, which also covers zips written
 * with data descriptors */
static int do_zip(Imp *im, const uint8_t *p, size_t n) {
    if (n < 22) return 0;
    size_t eocd = n - 22, lim = n > 65557 ? n - 65557 : 0;
    while (eocd > lim && r32(p + eocd) != 0x06054b50u) eocd--;
    if (r32(p + eocd) != 0x06054b50u) return 0;
    uint16_t cnt = r16(p + eocd + 10);
    size_t cd = r32(p + eocd + 16);
    for (int i = 0; i < cnt && cd + 46 <= n && r32(p + cd) == 0x02014b50u; i++) {
        uint16_t method = r16(p + cd + 10);
        uint32_t csize = r32(p + cd + 20), usize = r32(p + cd + 24);
        uint16_t nl = r16(p + cd + 28), xl = r16(p + cd + 30), cl = r16(p + cd + 32);
        size_t lho = r32(p + cd + 42);
        char nm[256];
        snprintf(nm, sizeof nm, "%.*s", nl < 255 ? (int)nl : 255, (const char *)p + cd + 46);
        cd += 46 + nl + xl + cl;
        size_t l = strlen(nm);
        int want = (l > 4 && (!strcmp(nm + l - 4, ".cod") || !strcmp(nm + l - 4, ".COD") || !strcmp(nm + l - 4, ".zip")));
        if (!want || lho + 30 > n || r32(p + lho) != 0x04034b50u) continue;
        size_t data = lho + 30 + r16(p + lho + 26) + r16(p + lho + 28);
        if (data + csize > n || usize > (256u << 20)) continue;
        uint8_t *buf = malloc(usize ? usize : 1);
        int ok = buf != NULL;
        if (ok && method == 0 && csize == usize) memcpy(buf, p + data, usize);
        else if (ok && method == 8) ok = inflate_raw(p + data, csize, buf, usize);
        else ok = 0;
        int rc = 0;
        if (ok) {
            if (is_cod(buf, usize)) {
                char name[128];
                const char *base = strrchr(nm, '/'); base = base ? base + 1 : nm;
                if (!cod_selfname(buf, usize, name, sizeof name)) snprintf(name, sizeof name, "%.*s", (int)(strlen(base) - 4), base);
                rc = write_cod(im, name, buf, usize);
            } else rc = do_file(im, buf, usize);   /* a nested bundle of siblings */
        }
        free(buf);
        if (rc < 0) return -1;
    }
    return 0;
}

static int do_file(Imp *im, const uint8_t *p, size_t n) {
    if (is_cod(p, n)) {
        char name[128];
        if (cod_selfname(p, n, name, sizeof name)) return write_cod(im, name, p, n);
        return 0;
    }
    if (n >= 4 && r32(p) == 0x04034b50u) return do_zip(im, p, n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* cabinet (MSZIP)                                                      */

typedef struct Rd { FILE *f; long base; } Rd;
static int rd_at(Rd *r, long off, void *dst, size_t n) {
    if (fseek(r->f, r->base + off, SEEK_SET)) return 0;
    return fread(dst, 1, n, r->f) == n;
}

static int do_cab(Imp *im, FILE *f, long base) {
    Rd r = { f, base };
    uint8_t h[64];
    if (!rd_at(&r, 0, h, 36) || memcmp(h, "MSCF", 4)) return fail(im, "the installer's cabinet is damaged");
    uint32_t coffFiles = r32(h + 16);
    uint16_t nfold = r16(h + 26), nfile = r16(h + 28), flags = r16(h + 30);
    long pos = 36;
    int resHdr = 0, resFold = 0, resData = 0;
    if (flags & 4) {
        uint8_t x[4];
        if (!rd_at(&r, 36, x, 4)) return fail(im, "bad cabinet");
        resHdr = r16(x); resFold = x[2]; resData = x[3];
        pos = 40 + resHdr;
    }
    if (flags & 3) {   /* previous / next cabinet names: skip two strings each */
        int strings = ((flags & 1) ? 2 : 0) + ((flags & 2) ? 2 : 0);
        while (strings--) { int c; fseek(f, base + pos, SEEK_SET); do { c = fgetc(f); pos++; } while (c > 0); }
    }
    typedef struct Fold { uint32_t start; uint16_t nblk, type; } Fold;
    Fold *fo = calloc(nfold ? nfold : 1, sizeof *fo);
    for (int i = 0; i < nfold; i++) {
        uint8_t b[8];
        if (!rd_at(&r, pos, b, 8)) { free(fo); return fail(im, "bad cabinet folder"); }
        fo[i].start = r32(b); fo[i].nblk = r16(b + 4); fo[i].type = r16(b + 6);
        pos += 8 + resFold;
    }
    typedef struct CFile { uint32_t size, off; uint16_t fold; } CFile;
    CFile *fi = calloc(nfile ? nfile : 1, sizeof *fi);
    pos = coffFiles;
    for (int i = 0; i < nfile; i++) {
        uint8_t b[16];
        if (!rd_at(&r, pos, b, 16)) { free(fo); free(fi); return fail(im, "bad cabinet file list"); }
        fi[i].size = r32(b); fi[i].off = r32(b + 4); fi[i].fold = r16(b + 8);
        pos += 16;
        fseek(f, base + pos, SEEK_SET);
        int c; do { c = fgetc(f); pos++; } while (c > 0);
    }
    uint8_t *cbuf = malloc(65536 + 64);
    int rc = 0;
    for (int fd = 0; fd < nfold && rc == 0; fd++) {
        progress(im, "Unpacking modules", 10 + 90 * fd / (nfold ? nfold : 1));
        if ((fo[fd].type & 15) != 1) continue;   /* only MSZIP (the format RIM used) */
        /* uncompressed size: blocks hold at most 32 KB each */
        size_t cap = (size_t)fo[fd].nblk * 32768 + 1, len = 0;
        uint8_t *out = malloc(cap);
        if (!out) { rc = fail(im, "out of memory"); break; }
        long bp = fo[fd].start;
        z_stream z; memset(&z, 0, sizeof z);
        inflateInit2(&z, -15);
        for (int b = 0; b < fo[fd].nblk; b++) {
            uint8_t bh[8];
            if (!rd_at(&r, bp, bh, 8)) { rc = fail(im, "bad cabinet data"); break; }
            uint16_t cb = r16(bh + 4), ub = r16(bh + 6);
            bp += 8 + resData;
            if (cb > 65536 || !rd_at(&r, bp, cbuf, cb)) { rc = fail(im, "bad cabinet block"); break; }
            bp += cb;
            if (cb < 2 || cbuf[0] != 'C' || cbuf[1] != 'K') { rc = fail(im, "unexpected cabinet compression"); break; }
            /* each block is its own deflate stream that may refer back to
             * the previous 32 KB of output */
            inflateReset(&z);
            if (len) { size_t dl = len < 32768 ? len : 32768; inflateSetDictionary(&z, out + len - dl, (uInt)dl); }
            z.next_in = cbuf + 2; z.avail_in = cb - 2;
            z.next_out = out + len; z.avail_out = (uInt)(cap - len);
            int zr = inflate(&z, Z_FINISH);
            if (zr != Z_STREAM_END && zr != Z_OK && zr != Z_BUF_ERROR) { rc = fail(im, "cabinet data corrupt"); break; }
            len += ub;
        }
        inflateEnd(&z);
        for (int i = 0; i < nfile && rc == 0; i++)
            if (fi[i].fold == fd && (size_t)fi[i].off + fi[i].size <= len) rc = do_file(im, out + fi[i].off, fi[i].size);
        free(out);
    }
    free(cbuf); free(fo); free(fi);
    return rc;
}

/* ------------------------------------------------------------------ */
/* OLE compound file (.msi): find the cabinet stream                   */

static int do_msi(Imp *im, const char *msiPath, const char *tmpCab) {
    FILE *f = fopen(msiPath, "rb");
    if (!f) return fail(im, "cannot read %s", msiPath);
    uint8_t h[512];
    if (fread(h, 1, 512, f) != 512 || r32(h) != 0xe011cfd0u) { fclose(f); return fail(im, "the installer has no Windows Installer package"); }
    int shift = r16(h + 0x1e);
    uint32_t ssz = 1u << shift, nfat = r32(h + 0x2c), dir0 = r32(h + 0x30), difat = r32(h + 0x44), ndifat = r32(h + 0x48);
    if (shift < 9 || shift > 16) { fclose(f); return fail(im, "bad package"); }
    #define SECOFF(s) ((long)((uint64_t)((s) + 1) << shift))
    /* the FAT's own sectors: 109 in the header, then the DIFAT chain */
    uint32_t *fatSecs = malloc(sizeof(uint32_t) * (nfat + 1));
    uint32_t nf = 0;
    for (int i = 0; i < 109 && nf < nfat; i++) fatSecs[nf++] = r32(h + 0x4c + 4 * i);
    uint8_t *sec = malloc(ssz);
    for (uint32_t d = 0, s = difat; d < ndifat && nf < nfat && s < 0xfffffffau; d++) {
        fseek(f, SECOFF(s), SEEK_SET);
        if (fread(sec, 1, ssz, f) != ssz) break;
        for (uint32_t i = 0; i + 1 < ssz / 4 && nf < nfat; i++) fatSecs[nf++] = r32(sec + 4 * i);
        s = r32(sec + ssz - 4);
    }
    uint32_t perSec = ssz / 4, nent = nf * perSec;
    uint32_t *fat = malloc(sizeof(uint32_t) * (nent ? nent : 1));
    for (uint32_t i = 0; i < nf; i++) {
        fseek(f, SECOFF(fatSecs[i]), SEEK_SET);
        if (fread(sec, 1, ssz, f) != ssz) memset(sec, 0xff, ssz);
        for (uint32_t k = 0; k < perSec; k++) fat[i * perSec + k] = r32(sec + 4 * k);
    }
    free(fatSecs);
    /* directory: pick the biggest stream that starts like a cabinet */
    uint32_t best = 0xffffffffu, bestSize = 0;
    for (uint32_t s = dir0, guard = 0; s < nent && guard < nent; s = fat[s], guard++) {
        fseek(f, SECOFF(s), SEEK_SET);
        if (fread(sec, 1, ssz, f) != ssz) break;
        for (uint32_t e = 0; e < ssz / 128; e++) {
            const uint8_t *d = sec + e * 128;
            if (d[66] != 2) continue;
            uint32_t start = r32(d + 116), size = r32(d + 120);
            if (size < 4096 || size <= bestSize || start >= nent) continue;
            uint8_t m4[4];
            fseek(f, SECOFF(start), SEEK_SET);
            if (fread(m4, 1, 4, f) == 4 && !memcmp(m4, "MSCF", 4)) { best = start; bestSize = size; }
        }
    }
    if (best == 0xffffffffu) { free(sec); free(fat); fclose(f); return fail(im, "no module cabinet found in the installer"); }
    /* copy the stream out (following its sector chain) */
    FILE *o = fopen(tmpCab, "wb");
    if (!o) { free(sec); free(fat); fclose(f); return fail(im, "cannot write %s", tmpCab); }
    uint32_t left = bestSize;
    for (uint32_t s = best, guard = 0; left && s < nent && guard < nent; s = fat[s], guard++) {
        fseek(f, SECOFF(s), SEEK_SET);
        uint32_t k = left < ssz ? left : ssz;
        if (fread(sec, 1, k, f) != k) break;
        fwrite(sec, 1, k, o);
        left -= k;
    }
    fclose(o); free(sec); free(fat); fclose(f);
    if (left) return fail(im, "the installer's package is truncated");
    #undef SECOFF
    return 0;
}

/* ------------------------------------------------------------------ */
/* InstallShield setup stream inside the .exe                          */

static const uint8_t kIsMagic[14] = { 'I','n','s','t','a','l','l','S','h','i','e','l','d',0 };

/* a real stream header: a sane entry count and a first entry whose name is
 * text and whose data fits in the file */
static int is_stream_at(FILE *f, long off, long fsize) {
    uint8_t h[0x2e + 312];
    fseek(f, off, SEEK_SET);
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, kIsMagic, 14)) return 0;
    int n = r16(h + 14);
    if (n < 1 || n > 4096) return 0;
    const uint8_t *e = h + 0x2e;
    int nl = 0;
    while (nl < 260 && e[nl]) { if (e[nl] < 32 || e[nl] > 126) return 0; nl++; }
    if (nl == 0 || nl == 260) return 0;
    uint32_t len = r32(e + 268);
    return off + 0x2e + 312 + (long)len <= fsize;
}

static long find_is_stream(FILE *f) {
    uint8_t *buf = malloc(65536 + 16);
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f), off = 0, found = -1;
    size_t got;
    while (found < 0 && off < fsize) {
        fseek(f, off, SEEK_SET);
        got = fread(buf, 1, 65536 + 14, f);
        if (got < 14) break;
        for (size_t i = 0; i + 14 <= got && found < 0; i++)
            if (buf[i] == 'I' && !memcmp(buf + i, kIsMagic, 14) && is_stream_at(f, off + (long)i, fsize)) found = off + (long)i;
        off += 65536;
    }
    free(buf);
    return found;
}

static int do_exe(Imp *im, const char *exe, const char *tmpMsi) {
    FILE *f = fopen(exe, "rb");
    if (!f) return fail(im, "cannot open %s", exe);
    progress(im, "Looking for the OS package", 0);
    long is = find_is_stream(f);
    if (is < 0) { fclose(f); return fail(im, "this is not a BlackBerry OS installer (no InstallShield data)"); }
    uint8_t hdr[0x2e];
    fseek(f, is, SEEK_SET);
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return fail(im, "bad installer"); }
    int n = r16(hdr + 14);
    long off = is + 0x2e;
    for (int i = 0; i < n; i++) {
        uint8_t eh[312];
        fseek(f, off, SEEK_SET);
        if (fread(eh, 1, 312, f) != 312) break;
        char name[261];
        memcpy(name, eh, 260); name[260] = 0;
        uint32_t flags = r32(eh + 260), len = r32(eh + 268);
        off += 312;
        size_t nl = strlen(name);
        if (nl > 4 && !strcmp(name + nl - 4, ".msi")) {
            /* descramble: key = name XOR 13 35 86 07; nibble-swapped, xored, inverted */
            static const uint8_t mg[4] = { 0x13, 0x35, 0x86, 0x07 };
            size_t kl = nl;
            uint8_t key[260];
            for (size_t k = 0; k < kl; k++) key[k] = (uint8_t)name[k] ^ mg[k % 4];
            FILE *o = fopen(tmpMsi, "wb");
            if (!o) { fclose(f); return fail(im, "cannot write %s", tmpMsi); }
            uint8_t *buf = malloc(1 << 20);
            uint32_t done = 0;
            int enc = (flags & 6) != 0, blocky = (flags & 4) != 0;
            while (done < len) {
                uint32_t k = len - done < (1u << 20) ? len - done : (1u << 20);
                if (fread(buf, 1, k, f) != k) break;
                if (enc && kl) for (uint32_t j = 0; j < k; j++) {
                    uint32_t pos = done + j, ki = blocky ? (pos % 1024) % (uint32_t)kl : pos % (uint32_t)kl;
                    uint8_t b = buf[j];
                    buf[j] = (uint8_t)~(key[ki] ^ (uint8_t)((b << 4) | (b >> 4)));
                }
                fwrite(buf, 1, k, o);
                done += k;
                progress(im, "Reading the installer", (int)(10ull * done / len));
            }
            free(buf); fclose(o); fclose(f);
            if (done < len) return fail(im, "the installer is truncated");
            return 0;
        }
        off += len;
    }
    fclose(f);
    return fail(im, "no Windows Installer package inside this installer");
}

int bbimport_installer(const char *exe, const char *outDir, const char *tmpDir,
                       bbimport_progress cb, void *ctx, char *err, size_t errn) {
    Imp im = { outDir, cb, ctx, err, errn, 0 };
    char msi[1024], cab[1024];
    snprintf(msi, sizeof msi, "%s/bbimport.msi", tmpDir);
    snprintf(cab, sizeof cab, "%s/bbimport.cab", tmpDir);
    int rc = do_exe(&im, exe, msi);
    if (rc == 0) { progress(&im, "Opening the package", 10); rc = do_msi(&im, msi, cab); }
    remove(msi);
    if (rc == 0) {
        FILE *f = fopen(cab, "rb");
        if (!f) rc = fail(&im, "cannot read %s", cab);
        else { rc = do_cab(&im, f, 0); fclose(f); }
    }
    remove(cab);
    if (rc == 0 && im.written == 0) rc = fail(&im, "no OS modules found in this installer");
    return rc < 0 ? -1 : im.written;
}

/* Add loose modules: a .cod (single or a zip of siblings) or a .zip / .jad
 * bundle containing .cod files. */
int bbimport_modules(const char *path, const char *outDir, char *err, size_t errn) {
    Imp im = { outDir, NULL, NULL, err, errn, 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return fail(&im, "cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (256l << 20)) { fclose(f); return fail(&im, "file too large"); }
    uint8_t *p = malloc((size_t)n);
    size_t got = p ? fread(p, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) { free(p); return fail(&im, "cannot read %s", path); }
    int rc = do_file(&im, p, (size_t)n);
    free(p);
    if (rc < 0) return -1;
    if (im.written == 0) return fail(&im, "no BlackBerry modules (.cod) found");
    return im.written;
}
