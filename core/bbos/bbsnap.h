#ifndef BBSNAP_H
#define BBSNAP_H
#include <stddef.h>
#include <stdint.h>

/* Save or load everything the OS core holds: its memory region (bbarena)
 * and the writable variables of the binary that contains `anchor` (any
 * function or variable of the core). A slot stores only what differs from
 * the shared base file at basePath, which the first save creates. `build` names the core build; a state
 * only loads into the same build at the same addresses. `extra` is a small
 * caller record saved alongside (e.g. the clock). Returns 0 or -1 with a
 * message in err. Not safe while the core is running. */
int bbsnap_save(const char *path, const char *basePath, const void *anchor, const char *build,
                const void *extra, size_t extraLen, char *err, size_t errn);
int bbsnap_load(const char *path, const char *basePath, const void *anchor, const char *build,
                void *extra, size_t extraLen, char *err, size_t errn);
#endif
