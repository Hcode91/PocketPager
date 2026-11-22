/* PocketPager - BlackBerry OS family: AES and DES/3DES block ciphers. */
#ifndef BBOS_BLOCKCIPHER_H
#define BBOS_BLOCKCIPHER_H
#include <stdint.h>

typedef struct { int nr; uint32_t ek[60], dk[60]; } AesKey;
void aes_setkey(AesKey *k, const uint8_t *key, int keyBytes);   /* 16, 24 or 32 */
void aes_encrypt(const AesKey *k, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt(const AesKey *k, const uint8_t in[16], uint8_t out[16]);

typedef struct { uint32_t sk[3][32]; int triple; } DesKey;
void des_setkey(DesKey *k, const uint8_t *key, int keyBytes);   /* 8, 16 or 24 */
void des_encrypt(const DesKey *k, const uint8_t in[8], uint8_t out[8]);
void des_decrypt(const DesKey *k, const uint8_t in[8], uint8_t out[8]);
#endif
