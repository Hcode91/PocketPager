/* PocketPager - BlackBerry OS family: message digests behind
 * net.rim.device.api.crypto.NativeDigest (SHA-1, SHA-256, MD5). */
#include "natives.h"
#include "blockcipher.h"

enum { D_SHA1 = 1, D_SHA256, D_MD5 };

typedef struct Dg {
    int alg;
    uint32_t h[8];
    uint8_t buf[64];
    uint32_t nbuf;
    uint64_t total;
} Dg;

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void dg_reset(Dg *d) {
    d->nbuf = 0; d->total = 0;
    static const uint32_t s1[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    static const uint32_t s2[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    if (d->alg == D_SHA256) memcpy(d->h, s2, sizeof s2);
    else memcpy(d->h, s1, sizeof s1);
}

static void blk_sha1(Dg *d, const uint8_t *p) {
    uint32_t w[80], a = d->h[0], b = d->h[1], c = d->h[2], e = d->h[3], f = d->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t k, fn;
        if (i < 20) { fn = (b & c) | (~b & e); k = 0x5a827999; }
        else if (i < 40) { fn = b ^ c ^ e; k = 0x6ed9eba1; }
        else if (i < 60) { fn = (b & c) | (b & e) | (c & e); k = 0x8f1bbcdc; }
        else { fn = b ^ c ^ e; k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + fn + f + k + w[i];
        f = e; e = c; c = rol(b, 30); b = a; a = t;
    }
    d->h[0] += a; d->h[1] += b; d->h[2] += c; d->h[3] += e; d->h[4] += f;
}

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

static void blk_sha256(Dg *d, const uint8_t *p) {
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, d->h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(s[4], 6) ^ ror(s[4], 11) ^ ror(s[4], 25), ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
        uint32_t t1 = s[7] + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror(s[0], 2) ^ ror(s[0], 13) ^ ror(s[0], 22), mj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
        uint32_t t2 = S0 + mj;
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1; s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) d->h[i] += s[i];
}

static void blk_md5(Dg *d, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const int R[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                               4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    uint32_t m[16], a = d->h[0], b = d->h[1], c = d->h[2], e = d->h[3];
    for (int i = 0; i < 16; i++) m[i] = (uint32_t)p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & e); g = i; }
        else if (i < 32) { f = (e & b) | (~e & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ e; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~e); g = (7 * i) % 16; }
        uint32_t t = e; e = c; c = b;
        b = b + rol(a + f + K[i] + m[g], R[i]);
        a = t;
    }
    d->h[0] += a; d->h[1] += b; d->h[2] += c; d->h[3] += e;
}

static void dg_block(Dg *d, const uint8_t *p) {
    if (d->alg == D_SHA256) blk_sha256(d, p); else if (d->alg == D_MD5) blk_md5(d, p); else blk_sha1(d, p);
}

static void dg_update(Dg *d, const uint8_t *p, size_t n) {
    d->total += n;
    while (n) {
        size_t k = 64 - d->nbuf; if (k > n) k = n;
        memcpy(d->buf + d->nbuf, p, k); d->nbuf += (uint32_t)k; p += k; n -= k;
        if (d->nbuf == 64) { dg_block(d, d->buf); d->nbuf = 0; }
    }
}

static int dg_final(Dg *d, uint8_t *out) {
    Dg c = *d;
    uint64_t bits = c.total * 8;
    uint8_t pad = 0x80, z = 0;
    dg_update(&c, &pad, 1);
    while (c.nbuf != 56) dg_update(&c, &z, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = c.alg == D_MD5 ? (uint8_t)(bits >> (8 * i)) : (uint8_t)(bits >> (56 - 8 * i));
    dg_update(&c, len, 8);
    int words = c.alg == D_SHA256 ? 8 : c.alg == D_MD5 ? 4 : 5;
    for (int i = 0; i < words; i++) for (int j = 0; j < 4; j++)
        out[4 * i + j] = c.alg == D_MD5 ? (uint8_t)(c.h[i] >> (8 * j)) : (uint8_t)(c.h[i] >> (24 - 8 * j));
    return words * 4;
}

static ref_t new_digest(BBVM *vm, int alg) {
    BBClass *k = vm_find_class_anywhere(vm, "net.rim.device.api.crypto.NativeDigest");
    ref_t r = vm_new_obj(vm, k);
    Dg *d = calloc(1, sizeof *d);
    d->alg = alg; dg_reset(d);
    Obj *o = vm_obj(vm, r);
    o->host = d; o->hostFree = free;
    return r;
}
static Dg *dg_of(BBVM *vm, ref_t r) { Obj *o = vm_obj(vm, r); return o ? o->host : NULL; }

NAT(nd_sha1) { UNUSED; ret[0] = new_digest(vm, D_SHA1); return NAT_OK; }
NAT(nd_sha256) { UNUSED; ret[0] = new_digest(vm, D_SHA256); return NAT_OK; }
NAT(nd_md5) { UNUSED; ret[0] = new_digest(vm, D_MD5); return NAT_OK; }
NAT(nd_reset) { UNUSED; Dg *d = dg_of(vm, a[0]); if (d) dg_reset(d); return NAT_OK; }
NAT(nd_update1) { UNUSED; Dg *d = dg_of(vm, a[0]); uint8_t b = (uint8_t)a[1]; if (d) dg_update(d, &b, 1); return NAT_OK; }
NAT(nd_update) {
    UNUSED;
    Dg *d = dg_of(vm, a[0]);
    Obj *b = vm_obj(vm, a[1]);
    if (!d || !b) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint32_t off = a[2], n = a[3];
    if ((uint64_t)off + n > b->len) { vm_throw(vm, t, "java.lang.ArrayIndexOutOfBoundsException", NULL); return NAT_THROWN; }
    dg_update(d, (uint8_t *)b->d + off, n);
    return NAT_OK;
}
NAT(nd_getDigest) {
    UNUSED;
    Dg *d = dg_of(vm, a[0]);
    Obj *b = vm_obj(vm, a[1]);
    if (!d || !b) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint8_t out[32];
    int n = dg_final(d, out);
    uint32_t off = a[2];
    if ((uint64_t)off + (uint32_t)n > b->len) { vm_throw(vm, t, "java.lang.ArrayIndexOutOfBoundsException", NULL); return NAT_THROWN; }
    memcpy((uint8_t *)b->d + off, out, (size_t)n);
    if (a[3]) dg_reset(d);
    return NAT_OK;
}

int nat_sha1(const uint8_t *p, size_t n, uint8_t out[20]) {
    Dg d = { 0 }; d.alg = D_SHA1; dg_reset(&d); dg_update(&d, p, n); return dg_final(&d, out);
}

/* ------------------------------------------------------------------ */
/* FIPS 186-2 general purpose random number generator (Appendix 3.1)
 * with the SHA-1 based G function. The context is a byte[]:
 * [0] b (key bytes), [1] buffered count, [2..21] buffered output,
 * [24..87] XKEY, [88..151] XSEED, [152] XSEED in use, [153] reuse XSEED. */

#define FX_B 0
#define FX_N 1
#define FX_BUF 2
#define FX_KEY 24
#define FX_SEED 88
#define FX_HAS 152
#define FX_EVERY 153
#define FX_SIZE 156

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static void fips_g(const uint8_t *c, int b, uint8_t out[20]) {
    uint8_t blk[64] = { 0 };
    memcpy(blk, c, (size_t)b);
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[4 * i] << 24 | blk[4 * i + 1] << 16 | blk[4 * i + 2] << 8 | blk[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint32_t A = h[0], B = h[1], C = h[2], D = h[3], E = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (B & C) | (~B & D); k = 0x5A827999; }
        else if (i < 40) { f = B ^ C ^ D; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (B & C) | (B & D) | (C & D); k = 0x8F1BBCDC; }
        else { f = B ^ C ^ D; k = 0xCA62C1D6; }
        uint32_t tmp = rol32(A, 5) + f + E + k + w[i];
        E = D; D = C; C = rol32(B, 30); B = A; A = tmp;
    }
    h[0] += A; h[1] += B; h[2] += C; h[3] += D; h[4] += E;
    for (int i = 0; i < 5; i++) { out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16); out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i]; }
}
/* big-endian add of a (n bytes) into acc (b bytes, right aligned) */
static void be_add(uint8_t *acc, int b, const uint8_t *x, int n, unsigned carry) {
    for (int i = 0; i < b; i++) {
        unsigned v = acc[b - 1 - i] + carry + (i < n ? x[n - 1 - i] : 0);
        acc[b - 1 - i] = (uint8_t)v; carry = v >> 8;
    }
}
static void fips_step(uint8_t *ctx) {
    int b = ctx[FX_B];
    uint8_t xval[64], x[20];
    memcpy(xval, ctx + FX_KEY, (size_t)b);
    if (ctx[FX_HAS]) { be_add(xval, b, ctx + FX_SEED, b, 0); if (!ctx[FX_EVERY]) ctx[FX_HAS] = 0; }
    fips_g(xval, b, x);
    be_add(ctx + FX_KEY, b, x, 20, 1);    /* XKEY = 1 + XKEY + x mod 2^b */
    memcpy(ctx + FX_BUF, x, 20);
    ctx[FX_N] = 20;
}

NAT(fips_initialize) {   /* static (byte[] key, off, len, byte[] seed, off, len, boolean) */
    UNUSED;
    Obj *k = vm_obj(vm, a[0]), *sd = vm_obj(vm, a[3]);
    BBClass *cls = vm_find_class_anywhere(vm, "net.rim.device.api.crypto.NativeFIPSPRNG");
    if (!cls) { ret[0] = 0; return NAT_OK; }
    ref_t cr = vm_new_array(vm, BT_BYTE, NULL, 1, FX_SIZE);
    uint8_t *ctx = (uint8_t *)vm_obj(vm, cr)->d;
    int len = (int)a[2];
    if (len > 64) len = 64;
    if (len < 0) len = 0;
    int b = len ? len : 20;
    ctx[FX_B] = (uint8_t)b;
    if (k && len) memcpy(ctx + FX_KEY, (uint8_t *)k->d + a[1], (size_t)len);
    else for (int i = 0; i < b; i++) ctx[FX_KEY + i] = (uint8_t)rand();
    if (sd && (int)a[5] > 0) {
        int sl = (int)a[5] > b ? b : (int)a[5];
        memcpy(ctx + FX_SEED + (b - sl), (uint8_t *)sd->d + a[4], (size_t)sl);
        ctx[FX_HAS] = 1; ctx[FX_EVERY] = (uint8_t)(a[6] != 0);
    }
    ref_t o = vm_new_obj(vm, cls);
    vm_obj(vm, o)->d[0] = cr;
    if (getenv("BBFIPS")) { char hx[200] = ""; for (int i = 0; i < b; i++) sprintf(hx + 2 * i, "%02x", ctx[FX_KEY + i]); vm_log(vm, "fips init len=%d key=%s seed=%d", len, hx, ctx[FX_HAS]); }
    ret[0] = o;
    return NAT_OK;
}
NAT(fips_xorBytes) {   /* (byte[] buf, off, len) */
    UNUSED;
    Obj *self = vm_obj(vm, a[0]);
    Obj *c = self ? vm_obj(vm, self->d[0]) : NULL;
    Obj *buf = vm_obj(vm, a[1]);
    if (!c || !buf) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    uint8_t *ctx = (uint8_t *)c->d, *p = (uint8_t *)buf->d;
    uint32_t off = a[2], n = a[3];
    if (off > buf->len || n > buf->len - off) { vm_throw(vm, t, "java.lang.ArrayIndexOutOfBoundsException", NULL); return NAT_THROWN; }
    for (uint32_t i = 0; i < n; i++) {
        if (!ctx[FX_N]) fips_step(ctx);
        p[off + i] ^= ctx[FX_BUF + 20 - ctx[FX_N]];
        ctx[FX_N]--;
    }
    if (getenv("BBFIPS")) { char hx[100] = ""; for (uint32_t i = 0; i < n && i < 32; i++) sprintf(hx + 2 * i, "%02x", p[off + i]); vm_log(vm, "fips xor %u: %s", n, hx); }
    return NAT_OK;
}
NAT(fips_version) { UNUSED; ret[0] = 1; return NAT_OK; }

/* ------------------------------------------------------------------ */
/* CryptoBlockNatives: content protection blocks. Algorithm 1 is 3DES and
 * 2 is AES, both CBC with a zero IV and zero padding to 16 bytes.
 * "Compression" is a 4-byte length prefix so that decryptDecompress gives
 * back the exact original length. */

static ref_t cb_crypt(BBVM *vm, Obj *key, int alg, const uint8_t *in, uint32_t n, int dec) {
    uint32_t bs = alg == 1 ? 8 : 16, out = dec ? n - n % bs : (n + 15) / 16 * 16;
    ref_t r = vm_new_array(vm, BT_BYTE, NULL, 1, out);
    uint8_t *o = (uint8_t *)vm_obj(vm, r)->d, iv[16] = { 0 }, blk[16], prev[16];
    uint32_t kl = key ? key->len : 0;
    const uint8_t *kb = key ? (const uint8_t *)key->d : iv;
    AesKey ak; DesKey dk;
    if (alg == 1) des_setkey(&dk, kb, kl >= 24 ? 24 : kl >= 16 ? 16 : 8);
    else aes_setkey(&ak, kb, kl >= 32 ? 32 : kl >= 24 ? 24 : 16);
    for (uint32_t p = 0; p < out; p += bs) {
        memset(blk, 0, sizeof blk);
        memcpy(blk, in + p, p + bs <= n ? bs : (p < n ? n - p : 0));
        if (!dec) {
            for (uint32_t i = 0; i < bs; i++) blk[i] ^= iv[i];
            if (alg == 1) des_encrypt(&dk, blk, o + p); else aes_encrypt(&ak, blk, o + p);
            memcpy(iv, o + p, bs);
        } else {
            memcpy(prev, blk, bs);
            if (alg == 1) des_decrypt(&dk, blk, o + p); else aes_decrypt(&ak, blk, o + p);
            for (uint32_t i = 0; i < bs; i++) o[p + i] ^= iv[i];
            memcpy(iv, prev, bs);
        }
    }
    return r;
}
static int cb_args(BBVM *vm, Thread *t, uint32_t *a, Obj **key, const uint8_t **p, uint32_t *n) {
    Obj *d = vm_obj(vm, a[2]);
    *key = vm_obj(vm, a[0]);
    if (!d || a[3] > d->len || a[4] > d->len - a[3]) { vm_throw(vm, t, "java.lang.IllegalArgumentException", NULL); return 0; }
    *p = (const uint8_t *)d->d + a[3]; *n = a[4];
    return 1;
}
NAT(cb_encrypt) {   /* (key, alg, data, off, len, flags) */
    UNUSED; Obj *k; const uint8_t *p; uint32_t n;
    if (!cb_args(vm, t, a, &k, &p, &n)) return NAT_THROWN;
    ret[0] = cb_crypt(vm, k, (int)(int8_t)a[1], p, n, 0);
    return NAT_OK;
}
NAT(cb_decrypt) {
    UNUSED; Obj *k; const uint8_t *p; uint32_t n;
    if (!cb_args(vm, t, a, &k, &p, &n)) return NAT_THROWN;
    ret[0] = cb_crypt(vm, k, (int)(int8_t)a[1], p, n, 1);
    return NAT_OK;
}
NAT(cb_compressEncrypt) {
    UNUSED; Obj *k; const uint8_t *p; uint32_t n;
    if (!cb_args(vm, t, a, &k, &p, &n)) return NAT_THROWN;
    uint8_t *tmp = malloc(n + 4);
    tmp[0] = (uint8_t)(n >> 24); tmp[1] = (uint8_t)(n >> 16); tmp[2] = (uint8_t)(n >> 8); tmp[3] = (uint8_t)n;
    memcpy(tmp + 4, p, n);
    ret[0] = cb_crypt(vm, k, (int)(int8_t)a[1], tmp, n + 4, 0);
    free(tmp);
    return NAT_OK;
}
NAT(cb_decryptDecompress) {
    UNUSED; Obj *k; const uint8_t *p; uint32_t n;
    if (!cb_args(vm, t, a, &k, &p, &n)) return NAT_THROWN;
    ref_t r = cb_crypt(vm, k, (int)(int8_t)a[1], p, n, 1);
    Obj *o = vm_obj(vm, r);
    const uint8_t *q = (const uint8_t *)o->d;
    uint32_t len = o->len >= 4 ? (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3] : 0;
    if (len > o->len - 4) len = o->len >= 4 ? o->len - 4 : 0;
    ref_t out = vm_new_array(vm, BT_BYTE, NULL, 1, len);
    memcpy(vm_obj(vm, out)->d, (const uint8_t *)vm_obj(vm, r)->d + 4, len);
    ret[0] = out;
    return NAT_OK;
}
NAT(cb_copy) {   /* compress/decompress (byte[], off, len): identity */
    UNUSED;
    Obj *d = vm_obj(vm, a[0]);
    if (!d || a[1] > d->len || a[2] > d->len - a[1]) { vm_throw(vm, t, "java.lang.IllegalArgumentException", NULL); return NAT_THROWN; }
    ref_t r = vm_new_array(vm, BT_BYTE, NULL, 1, a[2]);
    memcpy(vm_obj(vm, r)->d, (uint8_t *)vm_obj(vm, a[0])->d + a[1], a[2]);
    ret[0] = r;
    return NAT_OK;
}

const NatEnt kNativesCrypto[] = {
    { "net.rim.device.internal.crypto.CryptoBlockNatives.encrypt", cb_encrypt },
    { "net.rim.device.internal.crypto.CryptoBlockNatives.decrypt", cb_decrypt },
    { "net.rim.device.internal.crypto.CryptoBlockNatives.compressEncrypt", cb_compressEncrypt },
    { "net.rim.device.internal.crypto.CryptoBlockNatives.decryptDecompress", cb_decryptDecompress },
    { "net.rim.device.internal.crypto.CryptoBlockNatives.compress", cb_copy },
    { "net.rim.device.internal.crypto.CryptoBlockNatives.decompress", cb_copy },
    { "net.rim.device.api.crypto.NativeFIPSPRNG.initialize", fips_initialize },
    { "net.rim.device.api.crypto.NativeFIPSPRNG.xorBytes", fips_xorBytes },
    { "net.rim.device.api.crypto.NativeFIPSPRNG.getVersion", fips_version },
    { "net.rim.device.api.crypto.NativeDigest.initializeSHA1", nd_sha1 },
    { "net.rim.device.api.crypto.NativeDigest.initializeSHA256", nd_sha256 },
    { "net.rim.device.api.crypto.NativeDigest.initializeMD5", nd_md5 },
    { "net.rim.device.api.crypto.NativeDigest.reset", nd_reset },
    { "net.rim.device.api.crypto.NativeDigest.update(B)", nd_update1 },
    { "net.rim.device.api.crypto.NativeDigest.update([BII)", nd_update },
    { "net.rim.device.api.crypto.NativeDigest.getDigest", nd_getDigest },
    { NULL, NULL }
};
