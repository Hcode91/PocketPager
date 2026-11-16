/*
 * PocketPager - OS image descrambler.
 *
 * RIM OS images (PagerXXX.bin, 256 KB + 16-byte trailer) end in a trailer:
 *   u32 ?, u32 ?, u32 scrambled_len, "SCRM"
 * The first scrambled_len bytes are XORed with a 2048-byte repeating key
 * (and bit-inverted). The rest of the image is stored as-is.
 *
 * No key is stored here: the image contains long runs of erased flash
 * (0xFF), so the most common 2048-byte block in the scrambled area is
 * the key XOR 0xFF. We recover it from the image itself.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "scramble.h"

#define KEYLEN 2048

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int pp_is_scrambled(const uint8_t *img, size_t len)
{
	return len > 16 && memcmp(img + len - 4, "SCRM", 4) == 0;
}

/* Log-frequency (x2) of byte values in 386 OS code/data, used to pick the
 * most plausible key byte when the image has no blank 2 KB block. */
static const signed char byte_score[256] = { -5,-9,-10,-7,-9,-9,-11,-11,-10,-12,-11,-12,-11,-11,-12,-9,-10,-13,-12,-13,-11,-12,-12,-13,-12,-14,-13,-13,-12,-12,-13,-13,-10,-13,-13,-13,-11,-12,-14,-14,-12,-14,-14,-12,-13,-14,-14,-14,-11,-14,-13,-11,-13,-12,-14,-14,-12,-12,-13,-11,-12,-11,-14,-14,-10,-13,-13,-13,-12,-10,-12,-14,-12,-14,-14,-15,-13,-12,-13,-13,-10,-12,-13,-11,-12,-11,-11,-11,-12,-11,-13,-12,-13,-13,-11,-9,-8,-13,-14,-13,-12,-8,-9,-12,-12,-14,-10,-13,-13,-14,-14,-12,-11,-12,-12,-12,-9,-10,-13,-13,-12,-13,-14,-13,-12,-11,-12,-13,-10,-12,-13,-9,-12,-10,-13,-14,-12,-10,-12,-7,-13,-10,-14,-14,-11,-15,-14,-15,-14,-14,-14,-15,-14,-13,-13,-14,-13,-13,-15,-15,-11,-11,-14,-12,-14,-13,-15,-16,-13,-15,-15,-14,-14,-16,-15,-15,-12,-15,-16,-16,-14,-15,-13,-13,-13,-10,-12,-15,-15,-14,-13,-13,-10,-11,-12,-10,-12,-15,-12,-11,-11,-11,-14,-12,-14,-15,-13,-14,-11,-13,-13,-14,-14,-15,-14,-14,-13,-14,-15,-14,-14,-15,-15,-14,-11,-13,-14,-14,-13,-15,-13,-15,-8,-11,-14,-11,-11,-14,-13,-14,-10,-13,-13,-13,-12,-14,-12,-12,-11,-12,-12,-11,-11,-11,-10,-4 };

static void key_by_statistics(const uint8_t *img, size_t n, uint8_t *key)
{
	for (size_t i = 0; i < KEYLEN; i++) {
		int best = 0, bestscore = -1 << 30;
		for (int k = 0; k < 256; k++) {
			int sc = 0;
			for (size_t j = i; j < n; j += KEYLEN)
				sc += byte_score[img[j] ^ k];
			if (sc > bestscore) { bestscore = sc; best = k; }
		}
		key[i] = (uint8_t)(best ^ 0xFF);  /* stored like the block key: real key ^ 0xFF */
	}
}

/* Returns plaintext length written to out (len - 16) or -1 on failure. */
long pp_descramble(const uint8_t *img, size_t len, uint8_t *out)
{
	if (!pp_is_scrambled(img, len))
		return -1;
	size_t n = rd32(img + len - 8);
	size_t body = len - 16;
	if (n > body || n < KEYLEN * 4)
		return -1;

	/* Find the most frequent aligned 2048-byte block. */
	size_t nblk = n / KEYLEN;
	size_t best = 0, bestcnt = 0;
	for (size_t i = 0; i < nblk; i++) {
		size_t cnt = 0;
		for (size_t j = i; j < nblk; j++)
			if (memcmp(img + i * KEYLEN, img + j * KEYLEN, KEYLEN) == 0)
				cnt++;
		if (cnt > bestcnt) {
			bestcnt = cnt;
			best = i;
		}
		if (bestcnt > nblk / 4)
			break;
	}
	uint8_t statkey[KEYLEN];
	const uint8_t *key;
	if (bestcnt >= 4) {
		key = img + best * KEYLEN;      /* blank flash block = real key ^ 0xFF */
	} else {
		key_by_statistics(img, n, statkey);
		key = statkey;
	}

	for (size_t i = 0; i < n; i++)
		out[i] = img[i] ^ key[i % KEYLEN] ^ 0xFF;
	memcpy(out + n, img + n, body - n);

	/* Sanity check: 386EX start-up code begins with CLI. */
	if (out[0] != 0xFA)
		return -1;
	return (long)body;
}
