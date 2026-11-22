/* PocketPager - BlackBerry OS family: AES (FIPS-197) and DES/3DES (FIPS 46-3),
 * small table-free implementations. */
#include "blockcipher.h"
#include <string.h>

/* ---------------- AES ---------------- */
static uint8_t g_sbox[256], g_inv[256];
static int g_aesInit;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) { uint8_t p = 0; while (b) { if (b & 1) p ^= a; a = xt(a); b >>= 1; } return p; }

static void aes_tables(void) {
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
        g_sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_inv[g_sbox[i]] = (uint8_t)i;
    g_aesInit = 1;
}

void aes_setkey(AesKey *k, const uint8_t *key, int kb) {
    if (!g_aesInit) aes_tables();
    int nk = kb / 4;
    k->nr = nk + 6;
    int total = 4 * (k->nr + 1);
    for (int i = 0; i < nk; i++) k->ek[i] = (uint32_t)key[4 * i] << 24 | key[4 * i + 1] << 16 | key[4 * i + 2] << 8 | key[4 * i + 3];
    uint8_t rc = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = k->ek[i - 1];
        if (i % nk == 0) {
            t = (t << 8) | (t >> 24);
            t = (uint32_t)g_sbox[t >> 24] << 24 | (uint32_t)g_sbox[(t >> 16) & 255] << 16 | (uint32_t)g_sbox[(t >> 8) & 255] << 8 | g_sbox[t & 255];
            t ^= (uint32_t)rc << 24; rc = xt(rc);
        } else if (nk > 6 && i % nk == 4) {
            t = (uint32_t)g_sbox[t >> 24] << 24 | (uint32_t)g_sbox[(t >> 16) & 255] << 16 | (uint32_t)g_sbox[(t >> 8) & 255] << 8 | g_sbox[t & 255];
        }
        k->ek[i] = k->ek[i - nk] ^ t;
    }
    memcpy(k->dk, k->ek, sizeof k->dk);
}

static void add_rk(uint8_t s[16], const uint32_t *rk) {
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) s[4 * c + r] ^= (uint8_t)(rk[c] >> (24 - 8 * r));
}

void aes_encrypt(const AesKey *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_rk(s, k->ek);
    for (int rnd = 1; rnd <= k->nr; rnd++) {
        for (int i = 0; i < 16; i++) s[i] = g_sbox[s[i]];
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) & 3) + r];
        if (rnd != k->nr) for (int c = 0; c < 4; c++) {
            uint8_t *a = t + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            a[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
            a[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
            a[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
            a[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
        }
        memcpy(s, t, 16);
        add_rk(s, k->ek + 4 * rnd);
    }
    memcpy(out, s, 16);
}

void aes_decrypt(const AesKey *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_rk(s, k->dk + 4 * k->nr);
    for (int rnd = k->nr - 1; rnd >= 0; rnd--) {
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[4 * ((c + r) & 3) + r] = s[4 * c + r];
        for (int i = 0; i < 16; i++) s[i] = g_inv[t[i]];
        add_rk(s, k->dk + 4 * rnd);
        if (rnd) for (int c = 0; c < 4; c++) {
            uint8_t *a = s + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            a[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
            a[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
            a[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
            a[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
        }
    }
    memcpy(out, s, 16);
}

/* ---------------- DES ---------------- */
static const uint8_t IP[64] = { 58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
    57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7 };
static const uint8_t FP[64] = { 40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25 };
static const uint8_t E_[48] = { 32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,16,17,18,19,20,21,20,21,22,23,24,25,
    24,25,26,27,28,29,28,29,30,31,32,1 };
static const uint8_t P_[32] = { 16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25 };
static const uint8_t PC1[56] = { 57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,
    63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4 };
static const uint8_t PC2[48] = { 14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,41,52,31,37,47,55,30,40,
    51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32 };
static const uint8_t SH[16] = { 1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1 };
static const uint8_t SB[8][64] = {
 {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
 {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
 {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
 {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
 {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
 {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
 {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
 {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11} };

static uint64_t perm(uint64_t v, const uint8_t *t, int n, int inBits) {
    uint64_t r = 0;
    for (int i = 0; i < n; i++) r = (r << 1) | ((v >> (inBits - t[i])) & 1);
    return r;
}
/* round keys as 48-bit values packed in two 32-bit halves */
static void des_sched(uint32_t sk[32], const uint8_t key[8]) {
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = k << 8 | key[i];
    uint64_t cd = perm(k, PC1, 56, 64);
    uint32_t c = (uint32_t)(cd >> 28), d = (uint32_t)(cd & 0xfffffff);
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < SH[r]; s++) { c = ((c << 1) | (c >> 27)) & 0xfffffff; d = ((d << 1) | (d >> 27)) & 0xfffffff; }
        uint64_t sub = perm(((uint64_t)c << 28) | d, PC2, 48, 56);
        sk[2 * r] = (uint32_t)(sub >> 24); sk[2 * r + 1] = (uint32_t)(sub & 0xffffff);
    }
}
static uint32_t des_f(uint32_t r, const uint32_t *sk) {
    uint64_t e = perm(r, E_, 48, 32) ^ ((uint64_t)sk[0] << 24 | sk[1]);
    uint32_t o = 0;
    for (int i = 0; i < 8; i++) {
        int b = (int)((e >> (42 - 6 * i)) & 63);
        int row = ((b >> 4) & 2) | (b & 1), col = (b >> 1) & 15;
        o = (o << 4) | SB[i][row * 16 + col];
    }
    return (uint32_t)perm(o, P_, 32, 32);
}
static void des_block(const uint32_t sk[32], int dec, const uint8_t in[8], uint8_t out[8]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | in[i];
    v = perm(v, IP, 64, 64);
    uint32_t l = (uint32_t)(v >> 32), r = (uint32_t)v;
    for (int i = 0; i < 16; i++) {
        int k = dec ? 15 - i : i;
        uint32_t t = r; r = l ^ des_f(r, sk + 2 * k); l = t;
    }
    v = perm(((uint64_t)r << 32) | l, FP, 64, 64);
    for (int i = 7; i >= 0; i--) { out[i] = (uint8_t)v; v >>= 8; }
}
void des_setkey(DesKey *k, const uint8_t *key, int kb) {
    k->triple = kb >= 16;
    des_sched(k->sk[0], key);
    if (k->triple) { des_sched(k->sk[1], key + 8); des_sched(k->sk[2], kb >= 24 ? key + 16 : key); }
}
void des_encrypt(const DesKey *k, const uint8_t in[8], uint8_t out[8]) {
    if (!k->triple) { des_block(k->sk[0], 0, in, out); return; }
    uint8_t a[8], b[8];
    des_block(k->sk[0], 0, in, a); des_block(k->sk[1], 1, a, b); des_block(k->sk[2], 0, b, out);
}
void des_decrypt(const DesKey *k, const uint8_t in[8], uint8_t out[8]) {
    if (!k->triple) { des_block(k->sk[0], 1, in, out); return; }
    uint8_t a[8], b[8];
    des_block(k->sk[2], 1, in, a); des_block(k->sk[1], 0, a, b); des_block(k->sk[0], 1, b, out);
}
