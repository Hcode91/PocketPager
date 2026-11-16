#ifndef PP_SCRAMBLE_H
#define PP_SCRAMBLE_H
#include <stddef.h>
#include <stdint.h>

int pp_is_scrambled(const uint8_t *img, size_t len);
long pp_descramble(const uint8_t *img, size_t len, uint8_t *out);

#endif
