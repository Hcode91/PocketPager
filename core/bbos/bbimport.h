#ifndef BBIMPORT_H
#define BBIMPORT_H
#include <stddef.h>

typedef void (*bbimport_progress)(void *ctx, const char *what, int percent);

/* Unpack every .cod module from a BlackBerry OS Windows installer (.exe)
 * into outDir. tmpDir holds two large temporary files while it runs.
 * Returns the number of modules written, or -1 with a message in err. */
int bbimport_installer(const char *exe, const char *outDir, const char *tmpDir,
                       bbimport_progress cb, void *ctx, char *err, size_t errn);

/* Add modules from a .cod file or a .zip of them. Returns the count or -1. */
int bbimport_modules(const char *path, const char *outDir, char *err, size_t errn);

#endif
