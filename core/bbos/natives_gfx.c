/* PocketPager - BlackBerry OS family: graphics natives.
 *
 * net.rim.device.api.ui.Graphics draws either to the LCD frame buffer or into
 * a Bitmap. Bitmaps keep their pixels in Java byte arrays, as on the device:
 * rows of 32-bit aligned scanlines, split into bands of at most 16 KB when
 * large. The default type (0xc5) is 16 bpp RGB565 little endian; alpha masks
 * are separate bitmaps of 1, 4 or 8 bpp.
 *
 * Graphics state fields: 19..22 clip x/y/w/h (device), 23/24 translation,
 * 25 colour, 26 global alpha, 27 background colour, 29 drawing style,
 * 32 font, 2 target bitmap (null = screen). */
#include "natives.h"
#include "bbgfx.h"
#include "gfxint.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

/* ------------------------------------------------------------------ */
/* the LCD                                                             */

static uint16_t g_fb[BB_SCREEN_W * BB_SCREEN_H];
static volatile int g_frame;

const uint16_t *bbgfx_framebuffer(int *w, int *h, int *frame) {
    if (w) *w = BB_SCREEN_W;
    if (h) *h = BB_SCREEN_H;
    if (frame) *frame = g_frame;
    return g_fb;
}

/* ------------------------------------------------------------------ */
/* surfaces                                                            */

enum { B_TYPE = 0, B_W = 1, B_H = 2, B_STRIDE = 3, B_DATA = 4, B_BANDS = 5, B_RPB = 6, B_ALPHA = 7, B_TRANS = 9 };



static int surf_bitmap(BBVM *vm, ref_t bmp, Surf *s) {
    Obj *b = vm_obj(vm, bmp);
    memset(s, 0, sizeof *s);
    s->vm = vm;
    if (!b || b->kind != OK_INST || b->len < 8) return 0;
    int type = (int)b->d[B_TYPE];
    s->w = (int)b->d[B_W]; s->h = (int)b->d[B_H];
    s->bpp = 1 << ((type & 7) - 1);
    s->stride = (int)b->d[B_STRIDE];
    s->nbands = (int)b->d[B_BANDS];
    s->rpb = (int)b->d[B_RPB];
    Obj *d = vm_obj(vm, b->d[B_DATA]);
    if (!d) return 0;
    if (s->nbands <= 1 || d->et == BT_BYTE) { s->one = d; s->nbands = 1; if (s->rpb <= 0) s->rpb = s->h; }
    else s->bands = d;
    if (s->rpb <= 0) s->rpb = s->h ? s->h : 1;
    return 1;
}

static uint8_t *surf_row(Surf *s, int y) {
    if (s->fb) return (uint8_t *)(s->fb + (size_t)y * s->w);
    if (s->one) {
        size_t off = (size_t)y * s->stride;
        return off + s->stride <= s->one->len ? (uint8_t *)s->one->d + off : NULL;
    }
    int band = y / s->rpb, row = y % s->rpb;
    if (band >= (int)s->bands->len) return NULL;
    Obj *b = vm_obj(s->vm, s->bands->d[band]);
    if (!b) return NULL;
    size_t off = (size_t)row * s->stride;
    return off + s->stride <= b->len ? (uint8_t *)b->d + off : NULL;
}

static inline uint32_t rgb565_to_rgb(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}
static inline uint16_t rgb_to_565(uint32_t c) {
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

/* read a pixel as 0xRRGGBB (16 bpp) or an intensity/alpha 0..255 (1/4/8 bpp) */
static uint32_t surf_get(Surf *s, int x, int y) {
    uint8_t *r = surf_row(s, y);
    if (!r) return 0;
    switch (s->bpp) {
    case 16: return rgb565_to_rgb((uint16_t)(r[2 * x] | r[2 * x + 1] << 8));
    case 8: return r[x];
    case 4: { int v = (r[x >> 1] >> ((x & 1) * 4)) & 15; return (uint32_t)(v * 17); }
    case 1: { int v = (r[x >> 3] >> (x & 7)) & 1; return v ? 255u : 0u; }
    default: return 0;
    }
}

static void surf_put(Surf *s, int x, int y, uint32_t v) {
    uint8_t *r = surf_row(s, y);
    if (!r) return;
    switch (s->bpp) {
    case 16: { uint16_t c = rgb_to_565(v); r[2 * x] = (uint8_t)c; r[2 * x + 1] = (uint8_t)(c >> 8); break; }
    case 8: r[x] = (uint8_t)v; break;
    case 4: { int sh = (x & 1) * 4; r[x >> 1] = (uint8_t)((r[x >> 1] & ~(15 << sh)) | (((v >> 4) & 15) << sh)); break; }
    case 1: if (v & 0x80) r[x >> 3] |= (uint8_t)(1 << (x & 7)); else r[x >> 3] &= (uint8_t)~(1 << (x & 7)); break;
    }
}

static inline uint32_t blend(uint32_t dst, uint32_t src, int a) {
    if (a >= 255) return src;
    if (a <= 0) return dst;
    uint32_t rb = ((src & 0xff00ff) * (uint32_t)a + (dst & 0xff00ff) * (uint32_t)(255 - a)) >> 8;
    uint32_t g = ((src & 0x00ff00) * (uint32_t)a + (dst & 0x00ff00) * (uint32_t)(255 - a)) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

/* ------------------------------------------------------------------ */
/* graphics context                                                     */



static void note_screen_bitmap(BBVM *vm, int pid, ref_t bmp);
static int gctx(BBVM *vm, ref_t gr, GCtx *c) {
    Obj *g = vm_obj(vm, gr);
    memset(c, 0, sizeof *c);
    if (!g || g->kind != OK_INST || g->len < 64) return 0;
    c->g = g;
    ref_t bmp = g->d[G_BITMAP];
    if (bmp) {
        if (!surf_bitmap(vm, bmp, &c->s)) return 0;
        Obj *bo = vm_obj(vm, bmp);
        if (bo && bo->len > B_ALPHA && bo->d[B_ALPHA]) c->hasA = surf_bitmap(vm, bo->d[B_ALPHA], &c->a);
        if (c->s.w == BB_SCREEN_W && c->s.h == BB_SCREEN_H && vm->cur) note_screen_bitmap(vm, vm->cur->procId, bmp);
    }
    else { c->s.fb = g_fb; c->s.w = BB_SCREEN_W; c->s.h = BB_SCREEN_H; c->s.bpp = 16; c->s.stride = BB_SCREEN_W * 2; c->s.vm = vm; }
    c->cx0 = (int)g->d[G_CLIPX]; c->cy0 = (int)g->d[G_CLIPY];
    c->cx1 = c->cx0 + (int)g->d[G_CLIPW]; c->cy1 = c->cy0 + (int)g->d[G_CLIPH];
    if (c->cx0 < 0) c->cx0 = 0;
    if (c->cy0 < 0) c->cy0 = 0;
    if (c->cx1 > c->s.w) c->cx1 = c->s.w;
    if (c->cy1 > c->s.h) c->cy1 = c->s.h;
    c->tx = (int)g->d[G_TX]; c->ty = (int)g->d[G_TY];
    c->color = g->d[G_COLOR] & 0xffffff;
    c->alpha = (int)g->d[G_ALPHA];
    if (c->alpha < 0 || c->alpha > 255) c->alpha = 255;
    return 1;
}

/* drawing into a bitmap with an alpha plane also makes the pixels opaque */
static inline void mark_alpha(GCtx *c, int x, int y, int a) {
    if (a >= 255) surf_put(&c->a, x, y, 255);
    else { int o = (int)surf_get(&c->a, x, y); surf_put(&c->a, x, y, (uint32_t)(o + (255 - o) * a / 255)); }
}
static inline void plot(GCtx *c, int x, int y, uint32_t rgb, int a) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    if (a >= 255) surf_put(&c->s, x, y, rgb);
    else if (a > 0) surf_put(&c->s, x, y, blend(surf_get(&c->s, x, y), rgb, a));
    if (c->hasA && a > 0) mark_alpha(c, x, y, a);
}

static void fill(GCtx *c, int x, int y, int w, int h, uint32_t rgb, int a) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    if (x0 >= x1 || y0 >= y1) return;
    if (c->s.bpp == 16 && a >= 255) {
        if (c->hasA) for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) surf_put(&c->a, xx, yy, 255);
        uint16_t px = rgb_to_565(rgb);
        for (int yy = y0; yy < y1; yy++) {
            uint8_t *r = surf_row(&c->s, yy);
            if (!r) continue;
            for (int xx = x0; xx < x1; xx++) { r[2 * xx] = (uint8_t)px; r[2 * xx + 1] = (uint8_t)(px >> 8); }
        }
        return;
    }
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) plot(c, xx, yy, rgb, a);
}

static void hline(GCtx *c, int x0, int x1, int y, uint32_t rgb, int a) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    fill(c, x0, y, x1 - x0 + 1, 1, rgb, a);
}

static void line(GCtx *c, int x0, int y0, int x1, int y1, uint32_t rgb, int a) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int n = 0; n < 100000; n++) {
        plot(c, x0, y0, rgb, a);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void ellipse(GCtx *c, int cx, int cy, int rx, int ry, int filled, uint32_t rgb, int a) {
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (!rx || !ry) { line(c, cx - rx, cy - ry, cx + rx, cy + ry, rgb, a); return; }
    for (int y = -ry; y <= ry; y++) {
        double f = 1.0 - (double)y * y / ((double)ry * ry);
        int w = (int)(rx * sqrt(f > 0 ? f : 0) + 0.5);
        if (filled) hline(c, cx - w, cx + w, cy + y, rgb, a);
        else { plot(c, cx - w, cy + y, rgb, a); plot(c, cx + w, cy + y, rgb, a); }
    }
    if (!filled) for (int x = -rx; x <= rx; x++) {
        double f = 1.0 - (double)x * x / ((double)rx * rx);
        int h = (int)(ry * sqrt(f > 0 ? f : 0) + 0.5);
        plot(c, cx + x, cy - h, rgb, a); plot(c, cx + x, cy + h, rgb, a);
    }
}

static void round_rect(GCtx *c, int x, int y, int w, int h, int aw, int ah, int filled, uint32_t rgb, int a) {
    int rx = aw / 2, ry = ah / 2;
    if (rx * 2 > w) rx = w / 2;
    if (ry * 2 > h) ry = h / 2;
    if (rx <= 0 || ry <= 0) {
        if (filled) fill(c, x, y, w, h, rgb, a);
        else { hline(c, x, x + w - 1, y, rgb, a); hline(c, x, x + w - 1, y + h - 1, rgb, a); fill(c, x, y, 1, h, rgb, a); fill(c, x + w - 1, y, 1, h, rgb, a); }
        return;
    }
    for (int yy = 0; yy < h; yy++) {
        int inset = 0;
        int dy = yy < ry ? ry - yy : (yy >= h - ry ? yy - (h - ry - 1) : 0);
        if (dy) { double f = 1.0 - (double)dy * dy / ((double)ry * ry); inset = rx - (int)(rx * sqrt(f > 0 ? f : 0) + 0.5); }
        if (filled) hline(c, x + inset, x + w - 1 - inset, y + yy, rgb, a);
        else if (yy == 0 || yy == h - 1) hline(c, x + inset, x + w - 1 - inset, y + yy, rgb, a);
        else { plot(c, x + inset, y + yy, rgb, a); plot(c, x + w - 1 - inset, y + yy, rgb, a); }
    }
}

int bbg_ctx(BBVM *vm, ref_t g, GCtx *c) { return gctx(vm, g, c); }
void bbg_plot(GCtx *c, int x, int y, uint32_t rgb, int a) { plot(c, x, y, rgb, a); }
void bbg_fill(GCtx *c, int x, int y, int w, int h, uint32_t rgb, int a) { fill(c, x, y, w, h, rgb, a); }

/* ------------------------------------------------------------------ */
/* context stack (kept natively per Graphics object)                   */

typedef struct GState { int cx, cy, cw, ch, tx, ty; uint32_t color, alpha, bg, style, font; } GState;
typedef struct GStack { int n, cap; GState *s; } GStack;

static void gstack_free(void *p) { GStack *s = p; if (s) { free(s->s); free(s); } }
static GStack *gstack(Obj *g) {
    if (!g->host) { g->host = calloc(1, sizeof(GStack)); g->hostFree = gstack_free; }
    return g->host;
}

static int push_ctx(BBVM *vm, ref_t gr, int x, int y, int w, int h, int dx, int dy, int region) {
    Obj *g = vm_obj(vm, gr);
    if (!g || g->len < 64) return 0;
    GStack *st = gstack(g);
    if (st->n == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 16;
        st->s = realloc(st->s, (size_t)st->cap * sizeof *st->s);
    }
    GState *p = &st->s[st->n++];
    p->cx = (int)g->d[G_CLIPX]; p->cy = (int)g->d[G_CLIPY]; p->cw = (int)g->d[G_CLIPW]; p->ch = (int)g->d[G_CLIPH];
    p->tx = (int)g->d[G_TX]; p->ty = (int)g->d[G_TY];
    /* the context also keeps the colours, drawing style and font */
    p->color = g->d[G_COLOR]; p->alpha = g->d[G_ALPHA]; p->bg = g->d[G_BG]; p->style = g->d[G_STYLE]; p->font = g->d[G_FONT];
    int ax = x + p->tx, ay = y + p->ty;
    int x0 = ax > p->cx ? ax : p->cx, y0 = ay > p->cy ? ay : p->cy;
    int x1 = ax + w < p->cx + p->cw ? ax + w : p->cx + p->cw;
    int y1 = ay + h < p->cy + p->ch ? ay + h : p->cy + p->ch;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    g->d[G_CLIPX] = (uint32_t)x0; g->d[G_CLIPY] = (uint32_t)y0;
    g->d[G_CLIPW] = (uint32_t)(x1 - x0); g->d[G_CLIPH] = (uint32_t)(y1 - y0);
    if (region) { g->d[G_TX] = (uint32_t)(ax - dx); g->d[G_TY] = (uint32_t)(ay - dy); }
    else { g->d[G_TX] = (uint32_t)(p->tx + dx); g->d[G_TY] = (uint32_t)(p->ty + dy); }
    g->d[12] = (uint32_t)st->n;
    return x1 > x0 && y1 > y0;
}

static void pop_ctx(BBVM *vm, ref_t gr) {
    Obj *g = vm_obj(vm, gr);
    if (!g || !g->host) return;
    GStack *st = g->host;
    if (!st->n) return;
    GState *p = &st->s[--st->n];
    g->d[G_CLIPX] = (uint32_t)p->cx; g->d[G_CLIPY] = (uint32_t)p->cy; g->d[G_CLIPW] = (uint32_t)p->cw; g->d[G_CLIPH] = (uint32_t)p->ch;
    g->d[G_TX] = (uint32_t)p->tx; g->d[G_TY] = (uint32_t)p->ty;
    g->d[G_COLOR] = p->color; g->d[G_ALPHA] = p->alpha; g->d[G_BG] = p->bg; g->d[G_STYLE] = p->style; g->d[G_FONT] = p->font;
    g->d[12] = (uint32_t)st->n;
}

static int rect_of(BBVM *vm, ref_t r, int *x, int *y, int *w, int *h) {
    Obj *o = vm_obj(vm, r);
    if (!o || o->len < 4) return 0;
    *x = (int)o->d[0]; *y = (int)o->d[1]; *w = (int)o->d[2]; *h = (int)o->d[3];
    return 1;
}

/* ------------------------------------------------------------------ */
/* Graphics natives                                                     */

#define GC(c) GCtx c; if (!gctx(vm, a[0], &c)) return NAT_OK

/* Each application process paints into its own backbuffer bitmap; the LCD
 * shows the backbuffer of the visible process (InternalServices
 * .setVisibleProcess), refreshed whenever the UI engine finishes a paint. */
typedef struct BackBuf { int pid; ref_t bmp; } BackBuf;
static BackBuf g_bb[256]; static int g_nbb;
static int g_visiblePid = -1;

/* hardware overlays: bitmaps the display controller draws over the LCD */
typedef struct Overlay { int used, id, x, y, visible; ref_t bmp; } Overlay;
static Overlay g_ov[16];
static void draw_overlays(BBVM *vm) {
    for (int i = 0; i < 16; i++) if (g_ov[i].used && g_ov[i].visible) {
        Surf s, al;
        Obj *bo = vm_obj(vm, g_ov[i].bmp);
        if (!bo || !surf_bitmap(vm, g_ov[i].bmp, &s)) continue;
        int hasA = surf_bitmap(vm, bo->d[B_ALPHA], &al);
        for (int y = 0; y < s.h; y++) for (int x = 0; x < s.w; x++) {
            int dx = g_ov[i].x + x, dy = g_ov[i].y + y;
            if (dx < 0 || dy < 0 || dx >= BB_SCREEN_W || dy >= BB_SCREEN_H) continue;
            uint32_t v = surf_get(&s, x, y);
            int a8 = hasA ? (int)surf_get(&al, x, y) : 255;
            uint16_t *p = &g_fb[dy * BB_SCREEN_W + dx];
            *p = rgb_to_565(blend(rgb565_to_rgb(*p), v, a8));
        }
    }
}

static void present_why(BBVM *vm, const char *why) {
    if (getenv("BBGFXLOG")) { for (int i = 0; i < g_nbb; i++) vm_log(vm, "present(%s): pid %d bmp %x%s", why, g_bb[i].pid, g_bb[i].bmp, g_bb[i].pid == g_visiblePid ? " (visible)" : ""); }
    for (int i = 0; i < g_nbb; i++) if (g_bb[i].pid == g_visiblePid) {
        Surf s;
        if (!surf_bitmap(vm, g_bb[i].bmp, &s) || s.bpp != 16) return;
        int w = s.w < BB_SCREEN_W ? s.w : BB_SCREEN_W, h = s.h < BB_SCREEN_H ? s.h : BB_SCREEN_H;
        for (int y = 0; y < h; y++) {
            uint8_t *r = surf_row(&s, y);
            if (r) memcpy(g_fb + (size_t)y * BB_SCREEN_W, r, (size_t)w * 2);
        }
        draw_overlays(vm);
        g_frame++;
        return;
    }
}

/* the screen-sized bitmap a process painted last is what it shows */
static void note_screen_bitmap(BBVM *vm, int pid, ref_t bmp) {
    int i;
    for (i = 0; i < g_nbb; i++) if (g_bb[i].pid == pid) break;
    if (i == g_nbb) { if (g_nbb == 256) return; g_nbb++; }
    if (g_bb[i].bmp != bmp) {
        Obj *b = vm_obj(vm, bmp);
        if (b) b->flags |= OF_PERM;
    }
    g_bb[i].pid = pid; g_bb[i].bmp = bmp;
}

NAT(g_createFrontbuffer) { UNUSED; return NAT_OK; }
NAT(g_createBackbuffer) {   /* (Bitmap) on a Graphics: draw into it */
    UNUSED;
    Obj *g = vm_obj(vm, a[0]);
    if (g && g->len > G_BITMAP) g->d[G_BITMAP] = a[1];
    return NAT_OK;
}
NAT(g_getDirtyRegion) {   /* (XYRect): the engine finished painting */
    UNUSED;
    Obj *r = vm_obj(vm, a[1]);
    if (r && r->len >= 4) { r->d[0] = 0; r->d[1] = 0; r->d[2] = BB_SCREEN_W; r->d[3] = BB_SCREEN_H; }
    if (t->procId == g_visiblePid) present_why(vm, "dirty");
    return NAT_OK;
}
NAT(g_updateFromBackBuffer) {   /* (x, y, w, h, ?, ?) on the backbuffer's Graphics */
    UNUSED;
    Obj *g = vm_obj(vm, a[0]);
    if (g && g->len > G_BITMAP && g->d[G_BITMAP]) note_screen_bitmap(vm, t->procId, g->d[G_BITMAP]);
    if (t->procId == g_visiblePid) present_why(vm, "backbuffer");
    return NAT_OK;
}
static Overlay *ov_find(int id, int create) {
    for (int i = 0; i < 16; i++) if (g_ov[i].used && g_ov[i].id == id) return &g_ov[i];
    if (create) for (int i = 0; i < 16; i++) if (!g_ov[i].used) { memset(&g_ov[i], 0, sizeof g_ov[i]); g_ov[i].used = 1; g_ov[i].id = id; return &g_ov[i]; }
    return NULL;
}
NAT(ov_set) {   /* static (display, id, Bitmap, x, y, visible) */
    UNUSED;
    Overlay *o = ov_find((int)a[1], 1);
    if (o) { o->bmp = a[2]; o->x = (int)a[3]; o->y = (int)a[4]; o->visible = (int)a[5]; Obj *b = vm_obj(vm, a[2]); if (b) b->flags |= OF_PERM; }
    if (getenv("BBGFXLOG")) {
        Surf s; Obj *b = vm_obj(vm, a[2]);
        int ok = surf_bitmap(vm, a[2], &s);
        vm_log(vm, "overlay %d bmp %x %dx%d bpp %d alpha %x at %d,%d vis %d", (int)a[1], a[2], ok ? s.w : -1, ok ? s.h : -1, ok ? s.bpp : -1, b && b->len > B_ALPHA ? b->d[B_ALPHA] : 0, (int)a[3], (int)a[4], (int)a[5]);
    }
    return NAT_OK;
}
NAT(ov_delete) { UNUSED; Overlay *o = ov_find((int)a[1], 0); if (o) o->used = 0; return NAT_OK; }
NAT(ov_isSet) { UNUSED; ret[0] = ov_find((int)a[1], 0) != NULL; return NAT_OK; }
NAT(ov_handle) { UNUSED; ret[0] = a[1]; return NAT_OK; }
NAT(ov_position) { UNUSED; Overlay *o = ov_find((int)a[1], 0); if (o) { o->x = (int)a[2]; o->y = (int)a[3]; } ret[0] = o != NULL; return NAT_OK; }
NAT(ov_state) { UNUSED; Overlay *o = ov_find((int)a[1], 0); if (o) o->visible = (int)a[2]; ret[0] = o != NULL; return NAT_OK; }
NAT(ov_update) { UNUSED; Overlay *o = ov_find((int)a[1], 0); if (o) o->bmp = a[2]; ret[0] = o != NULL; return NAT_OK; }
NAT(ov_disable) { UNUSED; for (int i = 0; i < 16; i++) g_ov[i].visible = 0; return NAT_OK; }

NAT(g_setVisibleProcess) { UNUSED; g_visiblePid = (int)a[1]; if (getenv("BBGFXLOG")) vm_log(vm, "visible pid %d (%d) caller pid %d, %d backbuffers", (int)a[0], (int)a[1], t->procId, g_nbb); present_why(vm, "visible"); return NAT_OK; }
NAT(g_resetStack) {
    UNUSED;
    Obj *g = vm_obj(vm, a[0]);
    if (g && g->host) ((GStack *)g->host)->n = 0;
    if (g && g->len > 12) g->d[12] = 0;
    return NAT_OK;
}
NAT(g_pushContext6) { UNUSED; ret[0] = (uint32_t)push_ctx(vm, a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5], (int)a[6], 0); return NAT_OK; }
NAT(g_pushContextR) {
    UNUSED; int x, y, w, h;
    if (!rect_of(vm, a[1], &x, &y, &w, &h)) { ret[0] = 0; return NAT_OK; }
    ret[0] = (uint32_t)push_ctx(vm, a[0], x, y, w, h, (int)a[2], (int)a[3], 0);
    return NAT_OK;
}
NAT(g_pushRegion6) { UNUSED; ret[0] = (uint32_t)push_ctx(vm, a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5], (int)a[6], 1); return NAT_OK; }
NAT(g_pushRegionR2) {
    UNUSED; int x, y, w, h;
    if (!rect_of(vm, a[1], &x, &y, &w, &h)) { ret[0] = 0; return NAT_OK; }
    ret[0] = (uint32_t)push_ctx(vm, a[0], x, y, w, h, (int)a[2], (int)a[3], 1);
    return NAT_OK;
}
NAT(g_pushRegionR) {
    UNUSED; int x, y, w, h;
    if (!rect_of(vm, a[1], &x, &y, &w, &h)) { ret[0] = 0; return NAT_OK; }
    ret[0] = (uint32_t)push_ctx(vm, a[0], x, y, w, h, 0, 0, 1);
    return NAT_OK;
}
NAT(g_popContext) { UNUSED; pop_ctx(vm, a[0]); return NAT_OK; }

NAT(g_fillRect) { UNUSED; GC(c); if (getenv("BBGFXLOG")) vm_log(vm, "fillRect %d,%d %dx%d clip %d,%d-%d,%d t %d,%d col %06x a %d bmp %d", (int)a[1], (int)a[2], (int)a[3], (int)a[4], c.cx0, c.cy0, c.cx1, c.cy1, c.tx, c.ty, c.color, c.alpha, c.s.fb == NULL); fill(&c, (int)a[1] + c.tx, (int)a[2] + c.ty, (int)a[3], (int)a[4], c.color, c.alpha); return NAT_OK; }
NAT(g_drawRect) {
    UNUSED; GC(c);
    int x = (int)a[1] + c.tx, y = (int)a[2] + c.ty, w = (int)a[3], h = (int)a[4];
    if (w <= 0 || h <= 0) return NAT_OK;
    fill(&c, x, y, w, 1, c.color, c.alpha); fill(&c, x, y + h - 1, w, 1, c.color, c.alpha);
    fill(&c, x, y + 1, 1, h - 2, c.color, c.alpha); fill(&c, x + w - 1, y + 1, 1, h - 2, c.color, c.alpha);
    return NAT_OK;
}
NAT(g_clear) { UNUSED; GC(c); uint32_t bg = c.g->d[G_BG] & 0xffffff; fill(&c, c.cx0, c.cy0, c.cx1 - c.cx0, c.cy1 - c.cy0, bg, 255); return NAT_OK; }
NAT(g_clear4) { UNUSED; GC(c); uint32_t bg = c.g->d[G_BG] & 0xffffff; fill(&c, (int)a[1] + c.tx, (int)a[2] + c.ty, (int)a[3], (int)a[4], bg, 255); return NAT_OK; }
NAT(g_clearR) {
    UNUSED; GC(c); int x, y, w, h;
    if (!rect_of(vm, a[1], &x, &y, &w, &h)) return NAT_OK;
    fill(&c, x + c.tx, y + c.ty, w, h, c.g->d[G_BG] & 0xffffff, 255);
    return NAT_OK;
}
NAT(g_drawLine) { UNUSED; GC(c); line(&c, (int)a[1] + c.tx, (int)a[2] + c.ty, (int)a[3] + c.tx, (int)a[4] + c.ty, c.color, c.alpha); return NAT_OK; }
NAT(g_drawPoint) { UNUSED; GC(c); plot(&c, (int)a[1] + c.tx, (int)a[2] + c.ty, c.color, c.alpha); return NAT_OK; }
NAT(g_fillRoundRect) { UNUSED; GC(c); round_rect(&c, (int)a[1] + c.tx, (int)a[2] + c.ty, (int)a[3], (int)a[4], (int)a[5], (int)a[6], 1, c.color, c.alpha); return NAT_OK; }
NAT(g_drawRoundRect) { UNUSED; GC(c); round_rect(&c, (int)a[1] + c.tx, (int)a[2] + c.ty, (int)a[3], (int)a[4], (int)a[5], (int)a[6], 0, c.color, c.alpha); return NAT_OK; }
NAT(g_fillArc) { /* (x, y, w, h, start, extent): full ellipses only for now */
    UNUSED; GC(c);
    int w = (int)a[3], h = (int)a[4];
    ellipse(&c, (int)a[1] + c.tx + w / 2, (int)a[2] + c.ty + h / 2, w / 2, h / 2, 1, c.color, c.alpha);
    return NAT_OK;
}
NAT(g_drawArc) {
    UNUSED; GC(c);
    int w = (int)a[3], h = (int)a[4];
    ellipse(&c, (int)a[1] + c.tx + w / 2, (int)a[2] + c.ty + h / 2, w / 2, h / 2, 0, c.color, c.alpha);
    return NAT_OK;
}
/* fillEllipse32(cx, cy, px, py, qx, qy, start, end): fixed 16.16 */
NAT(g_fillEllipse32) {
    UNUSED; GC(c);
    int cx = ((int32_t)a[1] >> 16) + c.tx, cy = ((int32_t)a[2] >> 16) + c.ty;
    int rx = abs(((int32_t)a[3] >> 16) - ((int32_t)a[1] >> 16)) + abs(((int32_t)a[5] >> 16) - ((int32_t)a[1] >> 16));
    int ry = abs(((int32_t)a[4] >> 16) - ((int32_t)a[2] >> 16)) + abs(((int32_t)a[6] >> 16) - ((int32_t)a[2] >> 16));
    ellipse(&c, cx, cy, rx, ry, 1, c.color, c.alpha);
    return NAT_OK;
}
NAT(g_drawEllipse32) {
    UNUSED; GC(c);
    int cx = ((int32_t)a[1] >> 16) + c.tx, cy = ((int32_t)a[2] >> 16) + c.ty;
    int rx = abs(((int32_t)a[3] >> 16) - ((int32_t)a[1] >> 16)) + abs(((int32_t)a[5] >> 16) - ((int32_t)a[1] >> 16));
    int ry = abs(((int32_t)a[4] >> 16) - ((int32_t)a[2] >> 16)) + abs(((int32_t)a[6] >> 16) - ((int32_t)a[2] >> 16));
    ellipse(&c, cx, cy, rx, ry, 0, c.color, c.alpha);
    return NAT_OK;
}
NAT(g_fillRect32) { /* 16.16 coordinates */
    UNUSED; GC(c);
    fill(&c, ((int32_t)a[1] >> 16) + c.tx, ((int32_t)a[2] >> 16) + c.ty, (int32_t)a[3] >> 16, (int32_t)a[4] >> 16, c.color, c.alpha);
    return NAT_OK;
}
NAT(g_invert) {
    UNUSED; GC(c);
    int x0 = (int)a[1] + c.tx, y0 = (int)a[2] + c.ty, x1 = x0 + (int)a[3], y1 = y0 + (int)a[4];
    if (x0 < c.cx0) x0 = c.cx0;
    if (y0 < c.cy0) y0 = c.cy0;
    if (x1 > c.cx1) x1 = c.cx1;
    if (y1 > c.cy1) y1 = c.cy1;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) surf_put(&c.s, x, y, surf_get(&c.s, x, y) ^ 0xffffff);
    return NAT_OK;
}
NAT(g_invertR) {
    UNUSED; int x, y, w, h;
    if (!rect_of(vm, a[1], &x, &y, &w, &h)) return NAT_OK;
    uint32_t b[5] = { a[0], (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    return g_invert(vm, t, b, ret);
}
NAT(g_copyArea) { /* (x, y, w, h, dx, dy) */
    UNUSED; GC(c);
    int x = (int)a[1] + c.tx, y = (int)a[2] + c.ty, w = (int)a[3], h = (int)a[4], dx = (int)a[5], dy = (int)a[6];
    if (w <= 0 || h <= 0) { ret[0] = 1; return NAT_OK; }
    uint32_t *tmp = malloc((size_t)w * h * 4);
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++)
        tmp[yy * w + xx] = (x + xx >= 0 && y + yy >= 0 && x + xx < c.s.w && y + yy < c.s.h) ? surf_get(&c.s, x + xx, y + yy) : 0;
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) plot(&c, x + xx + dx, y + yy + dy, tmp[yy * w + xx], 255);
    free(tmp);
    ret[0] = 1;
    return NAT_OK;
}
NAT(g_updateDisplay) {
    UNUSED;
    Obj *g = vm_obj(vm, a[0]);
    if (g && g->len > G_BITMAP && g->d[G_BITMAP]) { if (t->procId == g_visiblePid) present_why(vm, "update"); }
    else g_frame++;
    return NAT_OK;
}
NAT(g_isRopSupported) { UNUSED; ret[0] = 1; return NAT_OK; }
NAT(g_getNearestColor) { UNUSED; ret[0] = rgb565_to_rgb(rgb_to_565(a[1])); return NAT_OK; }
NAT(g_fitsInCache) { UNUSED; ret[0] = 1; return NAT_OK; }

/* Blit a bitmap region: (x, y, w, h, bitmap, left, top) */
static void blit(BBVM *vm, GCtx *c, int x, int y, int w, int h, ref_t bmp, int left, int top) {
    Surf src, al;
    if (!surf_bitmap(vm, bmp, &src)) return;
    Obj *bo = vm_obj(vm, bmp);
    int hasAlpha = surf_bitmap(vm, bo->d[B_ALPHA], &al);
    int trans = (int)bo->d[B_TRANS];
    if (getenv("BBGFXLOG")) vm_log(vm, "blit %x %dx%d bpp %d alpha %d trans %x -> [%x %dx%d] %d,%d %dx%d from %d,%d ga %d", bmp, src.w, src.h, src.bpp, hasAlpha, trans, c->g->d[G_BITMAP], c->s.w, c->s.h, x, y, w, h, left, top, c->alpha);
    for (int yy = 0; yy < h; yy++) {
        int sy = top + yy, dy = y + yy;
        if (sy < 0 || sy >= src.h || dy < c->cy0 || dy >= c->cy1) continue;
        for (int xx = 0; xx < w; xx++) {
            int sx = left + xx, dx = x + xx;
            if (sx < 0 || sx >= src.w || dx < c->cx0 || dx >= c->cx1) continue;
            uint32_t v = surf_get(&src, sx, sy);
            int a8 = c->alpha;
            if (src.bpp < 16) {
                /* monochrome source: set pixels draw in the current colour */
                if (!v) continue;
                v = c->color;
            }
            if (hasAlpha) {
                int av = sx < al.w && sy < al.h ? (int)surf_get(&al, sx, sy) : 255;
                a8 = a8 * av / 255;
            } else if (trans >= 0 && src.bpp == 16 && (v & 0xffffff) == (rgb565_to_rgb(rgb_to_565((uint32_t)trans)) & 0xffffff)) continue;
            plot(c, dx, dy, v, a8);
        }
    }
}

NAT(g_drawBitmap) { UNUSED; GC(c); blit(vm, &c, (int)a[1] + c.tx, (int)a[2] + c.ty, (int)a[3], (int)a[4], a[5], (int)a[6], (int)a[7]); return NAT_OK; }
NAT(g_drawBitmapR) {
    UNUSED; GC(c); int x, y, w, h;
    if (!rect_of(vm, a[1], &x, &y, &w, &h)) return NAT_OK;
    blit(vm, &c, x + c.tx, y + c.ty, w, h, a[2], (int)a[3], (int)a[4]);
    return NAT_OK;
}
NAT(g_rop) { /* rop(rop, x, y, w, h, bitmap, left, top) */
    UNUSED; GC(c);
    blit(vm, &c, (int)a[2] + c.tx, (int)a[3] + c.ty, (int)a[4], (int)a[5], a[6], (int)a[7], (int)a[8]);
    return NAT_OK;
}
NAT(g_tileRop) { /* tileRop(rop, x, y, w, h, bitmap, dx, dy) */
    UNUSED; GC(c);
    Surf src;
    if (!surf_bitmap(vm, a[6], &src) || src.w <= 0 || src.h <= 0) return NAT_OK;
    int x = (int)a[2] + c.tx, y = (int)a[3] + c.ty, w = (int)a[4], h = (int)a[5], ox = (int)a[7], oy = (int)a[8];
    for (int yy = 0; yy < h; yy += src.h) for (int xx = 0; xx < w; xx += src.w) {
        int bw = src.w < w - xx ? src.w : w - xx, bh = src.h < h - yy ? src.h : h - yy;
        blit(vm, &c, x + xx, y + yy, bw, bh, a[6], ((ox % src.w) + src.w) % src.w, ((oy % src.h) + src.h) % src.h);
    }
    return NAT_OK;
}
/* drawRGB/drawARGB(int[] data, offset, scanLength, x, y, w, h) */
static int draw_rgb(BBVM *vm, uint32_t *a, int useAlpha) {
    GCtx c;
    if (!gctx(vm, a[0], &c)) return NAT_OK;
    Obj *d = vm_obj(vm, a[1]);
    if (!d) return NAT_OK;
    int off = (int)a[2], scan = (int)a[3], x = (int)a[4] + c.tx, y = (int)a[5] + c.ty, w = (int)a[6], h = (int)a[7];
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        int64_t i = (int64_t)off + (int64_t)yy * scan + xx;
        if (i < 0 || i >= d->len) continue;
        uint32_t v = d->d[i];
        int al = useAlpha ? (int)(v >> 24) : 255;
        plot(&c, x + xx, y + yy, v & 0xffffff, al * c.alpha / 255);
    }
    return NAT_OK;
}
NAT(g_drawRGB) { UNUSED; return draw_rgb(vm, a, 0); }
NAT(g_drawARGB) { UNUSED; return draw_rgb(vm, a, 1); }

/* ------------------------------------------------------------------ */
/* Bitmap natives                                                       */

/* allocateRAMBitmap(size, byte[] init, byte fill) */
NAT(bm_allocateRAM) {
    UNUSED;
    Obj *b = vm_obj(vm, a[0]);
    if (!b) return NAT_OK;
    uint32_t size = a[1];
    ref_t arr = vm_new_array(vm, BT_BYTE, NULL, 1, size + 4);
    b = vm_obj(vm, a[0]);
    Obj *o = vm_obj(vm, arr);
    Obj *init = vm_obj(vm, a[2]);
    if (init) memcpy(o->d, init->d, init->len < size ? init->len : size);
    else memset(o->d, (int)(int8_t)a[3], size);
    b->d[B_DATA] = arr;
    b->d[B_BANDS] = 1;
    b->d[B_RPB] = b->d[B_H];
    return NAT_OK;
}
/* getARGB(int[] argb, offset, scanLength, x, y, w, h [, boolean]) */
NAT(bm_getARGB) {
    UNUSED;
    Surf s, al;
    if (!surf_bitmap(vm, a[0], &s)) return NAT_OK;
    Obj *bo = vm_obj(vm, a[0]);
    int hasAlpha = surf_bitmap(vm, bo->d[B_ALPHA], &al);
    Obj *d = vm_obj(vm, a[1]);
    if (!d) { vm_throw(vm, t, "java.lang.NullPointerException", NULL); return NAT_THROWN; }
    int off = (int)a[2], scan = (int)a[3], x = (int)a[4], y = (int)a[5], w = (int)a[6], h = (int)a[7];
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        int64_t i = (int64_t)off + (int64_t)yy * scan + xx;
        if (i < 0 || i >= d->len) continue;
        int sx = x + xx, sy = y + yy;
        if (sx < 0 || sy < 0 || sx >= s.w || sy >= s.h) { d->d[i] = 0; continue; }
        uint32_t v = surf_get(&s, sx, sy);
        if (s.bpp < 16) v = v ? 0x000000 : 0xffffff;
        uint32_t av = hasAlpha ? surf_get(&al, sx, sy) : 255;
        d->d[i] = (av << 24) | (v & 0xffffff);
    }
    return NAT_OK;
}
/* copyARGB(int[] argb, offset, scanLength, x, y, w, h, ?, ?) - write into this bitmap */
NAT(bm_copyARGB) {
    UNUSED;
    Surf s, al;
    if (!surf_bitmap(vm, a[0], &s)) return NAT_OK;
    Obj *bo = vm_obj(vm, a[0]);
    int hasAlpha = surf_bitmap(vm, bo->d[B_ALPHA], &al);
    Obj *d = vm_obj(vm, a[1]);
    if (!d) return NAT_OK;
    int off = (int)a[2], scan = (int)a[3], x = (int)a[4], y = (int)a[5], w = (int)a[6], h = (int)a[7];
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        int64_t i = (int64_t)off + (int64_t)yy * scan + xx;
        int sx = x + xx, sy = y + yy;
        if (i < 0 || i >= d->len || sx < 0 || sy < 0 || sx >= s.w || sy >= s.h) continue;
        uint32_t v = d->d[i];
        surf_put(&s, sx, sy, s.bpp == 16 ? (v & 0xffffff) : ((v & 0xffffff) < 0x808080 ? 255u : 0u));
        if (hasAlpha) surf_put(&al, sx, sy, v >> 24);
    }
    return NAT_OK;
}
NAT(bm_getBppFromARGB) { UNUSED; ret[0] = 16; return NAT_OK; }
NAT(bm_compare) {
    UNUSED;
    Surf x, y;
    ret[0] = 0;
    if (!surf_bitmap(vm, a[0], &x) || !surf_bitmap(vm, a[1], &y) || x.w != y.w || x.h != y.h || x.bpp != y.bpp) return NAT_OK;
    for (int j = 0; j < x.h; j++) for (int i = 0; i < x.w; i++) if (surf_get(&x, i, j) != surf_get(&y, i, j)) return NAT_OK;
    ret[0] = 1;
    return NAT_OK;
}
/* getScaledBitmap(srcX, srcY, srcW, srcH, Bitmap dst, dstX, dstY, dstW, dstH, filter) on this */
NAT(bm_getScaledBitmap) {
    UNUSED;
    Surf s, d;
    if (!surf_bitmap(vm, a[0], &s) || !surf_bitmap(vm, a[5], &d)) return NAT_OK;
    int sx = (int)a[1], sy = (int)a[2], sw = (int)a[3], sh = (int)a[4], dx = (int)a[6], dy = (int)a[7], dw = (int)a[8], dh = (int)a[9];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return NAT_OK;
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        int px = sx + x * sw / dw, py = sy + y * sh / dh;
        if (px < s.w && py < s.h && dx + x < d.w && dy + y < d.h) surf_put(&d, dx + x, dy + y, surf_get(&s, px, py));
    }
    return NAT_OK;
}

/* ------------------------------------------------------------------ */
/* encoded images (stb_image)                                          */

enum { IMG_GIF = 1, IMG_PNG = 2, IMG_JPEG = 3, IMG_WBMP = 4, IMG_BMP = 5 };

static int image_type(const uint8_t *p, int n) {
    if (n >= 8 && !memcmp(p, "\x89PNG", 4)) return IMG_PNG;
    if (n >= 3 && p[0] == 0xff && p[1] == 0xd8) return IMG_JPEG;
    if (n >= 6 && !memcmp(p, "GIF8", 4)) return IMG_GIF;
    if (n >= 2 && p[0] == 'B' && p[1] == 'M') return IMG_BMP;
    return 0;
}
NAT(ei_getImageType) {
    UNUSED;
    Obj *d = vm_obj(vm, a[0]);
    if (!d) { ret[0] = 0; return NAT_OK; }
    uint32_t off = a[1], len = a[2];
    if (off > d->len) off = d->len;
    if (len > d->len - off) len = d->len - off;
    ret[0] = (uint32_t)image_type((uint8_t *)d->d + off, (int)len);
    return NAT_OK;
}

/* EncodedImage fields: 5 _data, 6 _offset, 7 _length, 10 info (ImageInfo subclass) */
static int ei_bytes(BBVM *vm, ref_t ei, const uint8_t **p, int *n) {
    Obj *e = vm_obj(vm, ei);
    if (!e || e->len < 11) return 0;
    Obj *d = vm_obj(vm, e->d[5]);
    if (!d) return 0;
    uint32_t off = e->d[6], len = e->d[7];
    if (off > d->len) off = d->len;
    if (len > d->len - off) len = d->len - off;
    *p = (const uint8_t *)d->d + off; *n = (int)len;
    return 1;
}

static int ei_populate(BBVM *vm, ref_t ei, int type) {
    const uint8_t *p; int n;
    Obj *e = vm_obj(vm, ei);
    if (!e || !ei_bytes(vm, ei, &p, &n)) return NAT_OK;
    Obj *info = vm_obj(vm, e->d[10]);
    if (!info) return NAT_OK;
    int w = 0, h = 0, comp = 0;
    stbi_info_from_memory(p, n, &w, &h, &comp);
    int alpha = comp == 2 || comp == 4;
    if (type == IMG_PNG && n > 26) {
        int ct = p[25];
        if (ct == 4 || ct == 6) alpha = 1;
        /* tRNS chunk also means transparency */
        for (int i = 8; i + 8 < n; ) {
            uint32_t L = (uint32_t)p[i] << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3];
            if (!memcmp(p + i + 4, "tRNS", 4)) { alpha = 1; break; }
            if (!memcmp(p + i + 4, "IDAT", 4)) break;
            i += 12 + (int)L;
        }
    }
    if (type == IMG_GIF) alpha = 1;
    info->d[0] = (uint32_t)type; info->d[1] = (uint32_t)w; info->d[2] = (uint32_t)h; info->d[3] = 1;
    info->d[4] = 0; info->d[5] = (uint32_t)alpha;
    if (type == IMG_PNG && info->len >= 10) {
        info->d[6] = n > 25 ? p[25] : 2; info->d[7] = n > 24 ? p[24] : 8;
        info->d[8] = (uint32_t)alpha; info->d[9] = alpha ? 8 : 0;
    }
    return NAT_OK;
}
NAT(ei_populatePNG) { UNUSED; return ei_populate(vm, a[0], IMG_PNG); }
NAT(ei_populateJPEG) { UNUSED; return ei_populate(vm, a[0], IMG_JPEG); }
NAT(ei_populateBMP) { UNUSED; return ei_populate(vm, a[0], IMG_BMP); }
NAT(ei_populateGIF) { UNUSED; return ei_populate(vm, a[0], IMG_GIF); }

/* decode into dst (scaled to its size) and alpha */
static int ei_decode(BBVM *vm, ref_t ei, ref_t dst, ref_t alpha) {
    const uint8_t *p; int n;
    if (!ei_bytes(vm, ei, &p, &n)) return NAT_OK;
    int w, h, comp;
    uint8_t *px = stbi_load_from_memory(p, n, &w, &h, &comp, 4);
    if (!px) { vm_log(vm, "image decode failed: %s", stbi_failure_reason()); return NAT_OK; }
    Surf d, al;
    int hasD = surf_bitmap(vm, dst, &d), hasA = surf_bitmap(vm, alpha, &al);
    if (hasD && d.w > 0 && d.h > 0) {
        for (int y = 0; y < d.h; y++) for (int x = 0; x < d.w; x++) {
            int sx = x * w / d.w, sy = y * h / d.h;
            const uint8_t *q = px + ((size_t)sy * w + sx) * 4;
            surf_put(&d, x, y, (uint32_t)q[0] << 16 | q[1] << 8 | q[2]);
            if (hasA && x < al.w && y < al.h) surf_put(&al, x, y, q[3]);
        }
    }
    stbi_image_free(px);
    return NAT_OK;
}
NAT(ei_getPNGImage) { UNUSED; return ei_decode(vm, a[0], a[1], a[2]); }
NAT(ei_getGIFImage) { UNUSED; return ei_decode(vm, a[0], a[1], a[2]); }
NAT(ei_getJPEGImage) { UNUSED; return ei_decode(vm, a[0], a[1], 0); }
NAT(ei_getBMPImage) { UNUSED; return ei_decode(vm, a[0], a[1], 0); }

/* ------------------------------------------------------------------ */

NAT(k_true) { UNUSED; ret[0] = 1; return NAT_OK; }
NAT(g_nop) { UNUSED; return NAT_OK; }
NAT(k_false) { UNUSED; ret[0] = 0; return NAT_OK; }
NAT(disp_props) { UNUSED; ret[0] = 0; return NAT_OK; }

const NatEnt kNativesGfx[] = {
    { "net.rim.device.api.ui.Graphics.createFrontbufferSurface", g_createFrontbuffer },
    { "net.rim.device.api.ui.Graphics.createBackbufferSurface", g_createBackbuffer },
    { "net.rim.device.api.ui.GraphicsInternal.getDirtyRegion0", g_getDirtyRegion },
    { "net.rim.device.api.ui.GraphicsInternal.updateDisplayFromBackBuffer0", g_updateFromBackBuffer },
    { "net.rim.device.api.ui.GraphicsInternal.resetDirtyRegion0", g_nop },
    { "net.rim.device.api.ui.GraphicsInternal.setOverlay0Impl", ov_set },
    { "net.rim.device.api.ui.GraphicsInternal.deleteOverlay0Impl", ov_delete },
    { "net.rim.device.api.ui.GraphicsInternal.isOverlaySet0Impl", ov_isSet },
    { "net.rim.device.api.ui.GraphicsInternal.getOverlayHandle0Impl", ov_handle },
    { "net.rim.device.api.ui.GraphicsInternal.setOverlayPosition0Impl", ov_position },
    { "net.rim.device.api.ui.GraphicsInternal.setOverlayState0Impl", ov_state },
    { "net.rim.device.api.ui.GraphicsInternal.updateOverlay0Impl", ov_update },
    { "net.rim.device.api.ui.GraphicsInternal.disableOverlays0", ov_disable },
    { "net.rim.device.internal.system.InternalServices.setVisibleProcessImpl", g_setVisibleProcess },
    { "net.rim.device.api.ui.Graphics.resetStack", g_resetStack },
    { "net.rim.device.api.ui.Graphics.pushContext(IIIIII)", g_pushContext6 },
    { "net.rim.device.api.ui.Graphics.pushContext(Lnet.rim.device.api.ui.XYRect;II)", g_pushContextR },
    { "net.rim.device.api.ui.Graphics.pushRegion(IIIIII)", g_pushRegion6 },
    { "net.rim.device.api.ui.Graphics.pushRegion(Lnet.rim.device.api.ui.XYRect;II)", g_pushRegionR2 },
    { "net.rim.device.api.ui.Graphics.pushRegion(Lnet.rim.device.api.ui.XYRect;)", g_pushRegionR },
    { "net.rim.device.api.ui.Graphics.popContext", g_popContext },
    { "net.rim.device.api.ui.Graphics.fillRect(IIII)", g_fillRect },
    { "net.rim.device.api.ui.Graphics.fillRect32", g_fillRect32 },
    { "net.rim.device.api.ui.Graphics.drawRect(IIII)", g_drawRect },
    { "net.rim.device.api.ui.Graphics.drawRect32", g_drawRect },
    { "net.rim.device.api.ui.Graphics.clear()", g_clear },
    { "net.rim.device.api.ui.Graphics.clear(IIII)", g_clear4 },
    { "net.rim.device.api.ui.Graphics.clear(Lnet.rim.device.api.ui.XYRect;)", g_clearR },
    { "net.rim.device.api.ui.Graphics.drawLine", g_drawLine },
    { "net.rim.device.api.ui.Graphics.drawPoint", g_drawPoint },
    { "net.rim.device.api.ui.Graphics.fillRoundRect", g_fillRoundRect },
    { "net.rim.device.api.ui.Graphics.drawRoundRect", g_drawRoundRect },
    { "net.rim.device.api.ui.Graphics.fillArc", g_fillArc },
    { "net.rim.device.api.ui.Graphics.drawArc", g_drawArc },
    { "net.rim.device.api.ui.Graphics.fillEllipse32", g_fillEllipse32 },
    { "net.rim.device.api.ui.Graphics.drawEllipse32", g_drawEllipse32 },
    { "net.rim.device.api.ui.Graphics.invert(IIII)", g_invert },
    { "net.rim.device.api.ui.Graphics.invert(Lnet.rim.device.api.ui.XYRect;)", g_invertR },
    { "net.rim.device.api.ui.Graphics.copyArea(IIIIII)", g_copyArea },
    { "net.rim.device.api.ui.Graphics.updateDisplay0", g_updateDisplay },
    { "net.rim.device.api.ui.Graphics.isRopSupported", g_isRopSupported },
    { "net.rim.device.api.ui.Graphics.getNearestColorImpl", g_getNearestColor },
    { "net.rim.device.api.ui.Graphics.fitsInCache", g_fitsInCache },
    { "net.rim.device.api.ui.Graphics.drawBitmap(IIIILnet.rim.device.api.system.Bitmap;II)", g_drawBitmap },
    { "net.rim.device.api.ui.Graphics.drawBitmap(Lnet.rim.device.api.ui.XYRect;Lnet.rim.device.api.system.Bitmap;II)", g_drawBitmapR },
    { "net.rim.device.api.ui.Graphics.rop", g_rop },
    { "net.rim.device.api.ui.Graphics.tileRop", g_tileRop },
    { "net.rim.device.api.ui.Graphics.drawRGB", g_drawRGB },
    { "net.rim.device.api.ui.Graphics.drawARGB", g_drawARGB },
    { "net.rim.device.api.system.Bitmap.allocateRAMBitmap", bm_allocateRAM },
    { "net.rim.device.api.system.Bitmap.getARGB", bm_getARGB },
    { "net.rim.device.api.system.Bitmap.copyARGB", bm_copyARGB },
    { "net.rim.device.api.system.Bitmap.getBppFromARGB", bm_getBppFromARGB },
    { "net.rim.device.api.system.Bitmap.compareBitmapData", bm_compare },
    { "net.rim.device.api.system.Bitmap.getScaledBitmap", bm_getScaledBitmap },
    { "net.rim.device.api.system.EncodedImage.getImageType", ei_getImageType },
    { "net.rim.device.api.system.EncodedImage.isCFISupported", k_false },
    { "net.rim.device.api.system.PNGEncodedImage.populatePNGInfo", ei_populatePNG },
    { "net.rim.device.api.system.PNGEncodedImage.getPNGImage", ei_getPNGImage },
    { "net.rim.device.api.system.JPEGEncodedImage.populateJPEGInfo", ei_populateJPEG },
    { "net.rim.device.api.system.JPEGEncodedImage.getJPEGImage", ei_getJPEGImage },
    { "net.rim.device.api.system.BMPEncodedImage.populateBMPInfo", ei_populateBMP },
    { "net.rim.device.api.system.BMPEncodedImage.getBMPImage", ei_getBMPImage },
    { "net.rim.device.api.system.BMPEncodedImage.isBMPSupported", k_true },
    { "net.rim.device.api.system.GIFEncodedImage.populateGIFInfo", ei_populateGIF },
    { "net.rim.device.api.system.GIFEncodedImage.getGIFImage", ei_getGIFImage },
    { "net.rim.device.api.ui.DisplayInstance.getPropertiesImpl", disp_props },
    { NULL, NULL }
};
