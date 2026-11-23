/* bbimport: unpack a BlackBerry OS installer into a folder of .cod modules.
 *   bbimport installer.exe outdir      (the OS)
 *   bbimport -m app.zip|app.cod outdir (extra applications) */
#include "../core/bbos/bbimport.h"
#include <stdio.h>
#include <string.h>

static void progress(void *ctx, const char *what, int pct) {
    static int last = -1;
    (void)ctx;
    if (pct / 10 != last / 10) fprintf(stderr, "%s %d%%\n", what, pct);
    last = pct;
}

int main(int argc, char **argv) {
    char err[300] = "", names[2048] = "";
    int n;
    if (argc == 4 && !strcmp(argv[1], "-m")) { n = bbimport_modules(argv[2], argv[3], names, sizeof names, err, sizeof err); if (n >= 0) printf("applications: %s\n", names); }
    else if (argc == 3) n = bbimport_installer(argv[1], argv[2], "/tmp", progress, NULL, err, sizeof err);
    else { fprintf(stderr, "usage: bbimport installer.exe outdir | bbimport -m file outdir\n"); return 2; }
    if (n < 0) { fprintf(stderr, "error: %s\n", err); return 1; }
    printf("%d modules written\n", n);
    return 0;
}
