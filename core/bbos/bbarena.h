/* bbarena: all of the BlackBerry OS core's memory comes from one region at
 * a fixed address, so a save state is that region plus the core's own
 * variables and every pointer in it stays valid when it is loaded back.
 *
 * The core's sources are compiled with "-include bbarena.h", which sends
 * malloc/calloc/realloc/free/strdup here. */
#ifndef BBARENA_H
#define BBARENA_H
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void *bb_malloc(size_t n);
void *bb_calloc(size_t n, size_t m);
void *bb_realloc(void *p, size_t n);
void bb_free(void *p);
char *bb_strdup(const char *s);

/* the region: [base, base + used) is what a save state must hold */
int bb_arena_init(void);
void *bb_arena_base(void);
size_t bb_arena_used(void);
size_t bb_arena_reserved(void);
/* make room for a region of `used` bytes before loading one */
int bb_arena_prepare(size_t used);

/* Module files live in a second fixed region that save states don't copy:
 * they list each file (path, place, size, checksum) and read it back on
 * load. bb_code_load reads a file there with `pad` zero bytes after it. */
typedef struct BBCodeEnt { char path[240]; uint64_t off, size; uint32_t crc, pad; } BBCodeEnt;
void *bb_code_load(const char *path, size_t *size, size_t pad);
int bb_code_owns(const void *p);
const BBCodeEnt *bb_code_list(int *n);
/* load-time: put the region back exactly as listed; 0 or -1 with err */
int bb_code_restore(const BBCodeEnt *e, int n, char *err, size_t errn);
#ifdef __cplusplus
}
#endif

#ifndef BBARENA_IMPL
#define malloc bb_malloc
#define calloc bb_calloc
#define realloc bb_realloc
#define free bb_free
#define strdup bb_strdup
#endif
#endif
