/* PocketPager - BlackBerry OS family: drawing internals shared by the
 * graphics and text natives. */
#ifndef BBOS_GFXINT_H
#define BBOS_GFXINT_H
#include "natives.h"

typedef struct Surf {
    int w, h, bpp, stride, rpb, nbands;
    uint16_t *fb;           /* screen */
    Obj *one;               /* single band */
    Obj *bands;             /* byte[][] */
    BBVM *vm;
} Surf;

typedef struct GCtx {
    Obj *g;
    Surf s;
    int cx0, cy0, cx1, cy1;     /* clip, exclusive max */
    int tx, ty;
    uint32_t color;
    int alpha;
} GCtx;

enum { G_CLIPX = 19, G_CLIPY = 20, G_CLIPW = 21, G_CLIPH = 22, G_TX = 23, G_TY = 24,
       G_COLOR = 25, G_ALPHA = 26, G_BG = 27, G_STYLE = 29, G_FONT = 32, G_BITMAP = 2 };

int bbg_ctx(BBVM *vm, ref_t g, GCtx *c);
void bbg_plot(GCtx *c, int x, int y, uint32_t rgb, int a);
void bbg_fill(GCtx *c, int x, int y, int w, int h, uint32_t rgb, int a);
#endif
