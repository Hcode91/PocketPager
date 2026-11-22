/* PocketPager - BlackBerry OS family: fonts and text.
 *
 * The OS ships its system fonts (the BBAlpha families) as TrueType files
 * split into resources of net_rim_font_latin_truetype ("BBAlphaSans.001",
 * ".002", ...). On the device the native font engine reads them; here they
 * are fetched through the module's own Resource class and rendered with
 * stb_truetype. The proprietary "sff4" bitmap fonts other modules register
 * are accepted and mapped onto the nearest TrueType family.
 *
 * Font fields: 0 style, 1 height (px), 2 requested height, 3 leading,
 * 4 ascent, 5 descent, 6 family name, 7 effects, 8 antialias mode,
 * 9..14 transform, 15 FontFamily. */
#include "natives.h"
#include "gfxint.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

enum { F_STYLE = 0, F_HEIGHT = 1, F_LEADING = 3, F_ASCENT = 4, F_DESCENT = 5, F_NAME = 6 };
enum { ST_BOLD = 1, ST_ITALIC = 2 };

enum { FK_SANS, FK_SANS_B, FK_SANS_I, FK_SERIF, FK_SERIF_B, FK_SERIF_I, FK_MONO, FK_MONO_B, FK_COND, FK_COND_B, FK_COUNT };
static const char *kFontRes[FK_COUNT] = {
    "BBAlphaSans", "BBAlphaSansBold", "BBAlphaSansItalic", "BBAlphaSerif", "BBAlphaSerifBold",
    "BBAlphaSerifItalic", "BBAlphaSansMono", "BBAlphaSansMonoBold", "BBAlphaSansCondensed", "BBAlphaSansCondensedBold",
};

typedef struct TTF { uint8_t *data; int ok; stbtt_fontinfo info; int asc, desc, gap; } TTF;
static TTF g_ttf[FK_COUNT];
static int g_loaded;

/* glyph cache */
typedef struct Glyph { uint32_t key; uint16_t size; uint8_t font, used; int16_t w, h, xo, yo; int16_t adv; uint8_t *bmp; } Glyph;
#define GCACHE 8192
static Glyph g_glyphs[GCACHE];

/* ------------------------------------------------------------------ */
/* loading                                                             */

static ref_t java_resource(BBVM *vm, Thread *t, ref_t res, BBRoutine *get, const char *key) {
    uint32_t args[2] = { res, vm_new_string(vm, key) }, r[2] = { 0, 0 };
    if (vm_call(vm, t, get, args, 2, r) < 0) { t->pendingEx = 0; return 0; }
    return r[0];
}

static void fonts_load(BBVM *vm, Thread *t) {
    if (g_loaded) return;
    g_loaded = 1;
    BBClass *ri = vm_find_class_anywhere(vm, "net.rim.device.resources.Resource$Internal");
    BBRoutine *grc = ri ? vm_find_method(vm, ri, "getResourceClass", "Ljava.lang.String;") : NULL;
    if (!grc) { vm_log(vm, "fonts: no Resource$Internal.getResourceClass"); return; }
    uint32_t args[1] = { vm_new_string(vm, "net_rim_font_latin_truetype") }, r[2] = { 0, 0 };
    if (vm_call(vm, t, grc, args, 1, r) < 0 || !r[0]) { t->pendingEx = 0; vm_log(vm, "fonts: resource class not found"); return; }
    ref_t res = r[0];
    vm_obj(vm, res)->flags |= OF_PERM;
    Obj *ro = vm_obj(vm, res);
    BBRoutine *get = vm_find_method(vm, ro->cls, "getResource", NULL);
    if (!get) { vm_log(vm, "fonts: no getResource"); return; }
    for (int f = 0; f < FK_COUNT; f++) {
        size_t n = 0, cap = 0; uint8_t *buf = NULL;
        for (int part = 1; part < 100; part++) {
            char key[64];
            snprintf(key, sizeof key, "%s.%03d", kFontRes[f], part);
            ref_t b = java_resource(vm, t, res, get, key);
            Obj *bo = vm_obj(vm, b);
            if (!bo) break;
            if (n + bo->len > cap) { cap = (n + bo->len) * 2; buf = realloc(buf, cap); }
            memcpy(buf + n, bo->d, bo->len);
            n += bo->len;
        }
        if (!n) { vm_log(vm, "fonts: %s missing", kFontRes[f]); free(buf); continue; }
        TTF *tf = &g_ttf[f];
        tf->data = buf;
        if (stbtt_InitFont(&tf->info, buf, stbtt_GetFontOffsetForIndex(buf, 0))) {
            tf->ok = 1;
            stbtt_GetFontVMetrics(&tf->info, &tf->asc, &tf->desc, &tf->gap);
        }
        vm_log(vm, "fonts: %s %zu bytes%s", kFontRes[f], n, tf->ok ? "" : " (bad)");
    }
}

/* pick a TrueType face for a Font object */
static int font_pick(BBVM *vm, Obj *fo) {
    int style = (int)fo->d[F_STYLE];
    char name[64] = "";
    Obj *ns = vm_obj(vm, fo->d[F_NAME]);
    if (ns) { uint32_t n = ns->len < 63 ? ns->len : 63; for (uint32_t i = 0; i < n; i++) name[i] = (char)nat_char(ns, i); }
    int fam = 0;   /* 0 sans 1 serif 2 mono 3 condensed */
    if (strstr(name, "Mono") || strstr(name, "Fixed") || strstr(name, "Courier")) fam = 2;
    else if (strstr(name, "Condensed") || strstr(name, "Tall")) fam = 3;
    else if (strstr(name, "Serif") && !strstr(name, "Sans")) fam = 1;
    int b = (style & ST_BOLD) != 0, it = (style & ST_ITALIC) != 0;
    int k;
    switch (fam) {
    case 1: k = b ? FK_SERIF_B : it ? FK_SERIF_I : FK_SERIF; break;
    case 2: k = b ? FK_MONO_B : FK_MONO; break;
    case 3: k = b ? FK_COND_B : FK_COND; break;
    default: k = b ? FK_SANS_B : it ? FK_SANS_I : FK_SANS; break;
    }
    if (!g_ttf[k].ok) k = FK_SANS;
    return g_ttf[k].ok ? k : -1;
}

typedef struct FontSel { int k; int px; float scale; int asc, desc; } FontSel;

static int font_sel(BBVM *vm, Thread *t, ref_t f, FontSel *s) {
    fonts_load(vm, t);
    Obj *fo = vm_obj(vm, f);
    memset(s, 0, sizeof *s);
    if (!fo || fo->len < 16) return 0;
    s->k = font_pick(vm, fo);
    s->px = (int)fo->d[F_HEIGHT];
    if (s->px <= 0 || s->px > 400) s->px = 16;
    if (s->k < 0) { s->asc = s->px * 4 / 5; s->desc = s->px - s->asc; return 1; }
    TTF *tf = &g_ttf[s->k];
    s->scale = stbtt_ScaleForPixelHeight(&tf->info, (float)s->px);
    s->asc = (int)(tf->asc * s->scale + 0.5f);
    s->desc = s->px - s->asc;
    return 1;
}

static Glyph *glyph(FontSel *s, uint32_t cp) {
    uint32_t h = (cp * 2654435761u) ^ ((uint32_t)s->px << 20) ^ ((uint32_t)s->k << 27);
    for (int i = 0; i < 16; i++) {
        Glyph *g = &g_glyphs[(h + (uint32_t)i) & (GCACHE - 1)];
        if (g->used && g->key == cp && g->size == s->px && g->font == s->k) return g;
        if (!g->used || i == 15) {
            if (g->used) free(g->bmp);
            memset(g, 0, sizeof *g);
            g->used = 1; g->key = cp; g->size = (uint16_t)s->px; g->font = (uint8_t)s->k;
            if (s->k < 0) { g->adv = (int16_t)(s->px / 2); return g; }
            TTF *tf = &g_ttf[s->k];
            int gi = stbtt_FindGlyphIndex(&tf->info, (int)cp);
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&tf->info, gi, &adv, &lsb);
            g->adv = (int16_t)(adv * s->scale + 0.5f);
            int w = 0, hh = 0, xo = 0, yo = 0;
            if (cp > ' ') g->bmp = stbtt_GetGlyphBitmap(&tf->info, s->scale, s->scale, gi, &w, &hh, &xo, &yo);
            g->w = (int16_t)w; g->h = (int16_t)hh; g->xo = (int16_t)xo; g->yo = (int16_t)yo;
            return g;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* text sources: String / byte[] / char[] / StringBuffer / StringBufferGap */

typedef struct Text { Obj *arr; uint32_t gapStart, gapLen; uint32_t size; } Text;

static int text_of(BBVM *vm, ref_t r, Text *tx) {
    Obj *o = vm_obj(vm, r);
    memset(tx, 0, sizeof *tx);
    if (!o) return 0;
    if (o->kind == OK_ARRAY) { tx->arr = o; tx->size = o->len; return 1; }
    if (!o->cls) return 0;
    if (!strcmp(o->cls->full, "java.lang.StringBuffer")) {
        tx->arr = vm_obj(vm, o->d[0]); tx->size = o->d[1];
        return tx->arr != NULL;
    }
    if (strstr(o->cls->full, "StringBufferGap") && o->len >= 4) {
        tx->arr = vm_obj(vm, o->d[0]); tx->gapStart = o->d[1]; tx->gapLen = o->d[2]; tx->size = o->d[3];
        return tx->arr != NULL;
    }
    return 0;
}
static uint32_t text_at(const Text *tx, uint32_t i) {
    if (i >= tx->gapStart) i += tx->gapLen;
    if (!tx->arr || i >= tx->arr->len) return ' ';
    return tx->arr->et == BT_CHAR ? ((const uint16_t *)tx->arr->d)[i] : (uint32_t)((const uint8_t *)tx->arr->d)[i];
}
/* clamp (off, len) to the text */
static void text_range(const Text *tx, int32_t *off, int32_t *len) {
    if (*off < 0) *off = 0;
    if ((uint32_t)*off > tx->size) *off = (int32_t)tx->size;
    if (*len < 0 || (uint32_t)(*off + *len) > tx->size) *len = (int32_t)tx->size - *off;
}

static int text_advance(FontSel *s, const Text *tx, int32_t off, int32_t len) {
    int w = 0;
    for (int32_t i = 0; i < len; i++) { Glyph *g = glyph(s, text_at(tx, (uint32_t)(off + i))); if (g) w += g->adv; }
    return w;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

enum { DS_HMASK = 7, DS_LEFT = 6, DS_RIGHT = 5, DS_HCENTER = 4, DS_VMASK = 56, DS_TOP = 48, DS_BOTTOM = 40, DS_VCENTER = 32, DS_BASELINE = 56, DS_ELLIPSIS = 64 };

static int draw_glyphs(GCtx *c, FontSel *s, const Text *tx, int32_t off, int32_t len, int x, int baseline, int maxw) {
    int pen = 0;
    int a8 = c->alpha;
    for (int32_t i = 0; i < len; i++) {
        uint32_t ch = text_at(tx, (uint32_t)(off + i));
        if (ch < 32) continue;
        Glyph *g = glyph(s, ch);
        if (!g) continue;
        if (maxw >= 0 && pen + g->adv > maxw) break;
        if (g->bmp) {
            int gx = x + pen + g->xo, gy = baseline + g->yo;
            for (int yy = 0; yy < g->h; yy++) {
                int py = gy + yy;
                if (py < c->cy0 || py >= c->cy1) continue;
                const uint8_t *row = g->bmp + yy * g->w;
                for (int xx = 0; xx < g->w; xx++) if (row[xx]) bbg_plot(c, gx + xx, py, c->color, row[xx] * a8 / 255);
            }
        }
        pen += g->adv;
    }
    return pen;
}

/* common body: (text, off, len, x, y, flags, width) with x/y relative */
static int draw_text(BBVM *vm, Thread *t, ref_t gr, const Text *tx, int32_t off, int32_t len, int x, int y, int flags, int width) {
    GCtx c;
    if (!bbg_ctx(vm, gr, &c)) return 0;
    FontSel s;
    if (!font_sel(vm, t, c.g->d[G_FONT], &s)) return 0;
    text_range(tx, &off, &len);
    int adv = text_advance(&s, tx, off, len);
    int h = flags & DS_HMASK, v = flags & DS_VMASK;
    int maxw = width > 0 ? width : -1;
    if (width > 0 && adv < width) {
        if (h == DS_HCENTER) x += (width - adv) / 2;
        else if (h == DS_RIGHT) x += width - adv;
    }
    if (getenv("BBGFXLOG")) {
        char buf[64]; int k = 0;
        for (int32_t i = 0; i < len && k < 60; i++) { unsigned ch = text_at(tx, (uint32_t)(off + i)); buf[k++] = ch < 127 && ch >= 32 ? (char)ch : '?'; }
        buf[k] = 0;
        vm_log(vm, "drawText [p%d bmp %x %dx%d] .%s. at %d,%d flags %x w %d clip %d,%d-%d,%d t %d,%d col %06x font %d px k%d", t->procId, c.g->d[G_BITMAP], c.s.w, c.s.h, buf, x, y, flags, width, c.cx0, c.cy0, c.cx1, c.cy1, c.tx, c.ty, c.color, s.px, s.k);
    }
    int baseline;
    switch (v) {
    case DS_BOTTOM: baseline = y - s.desc; break;
    case DS_VCENTER: baseline = y - s.px / 2 + s.asc; break;
    case DS_BASELINE: baseline = y; break;
    default: baseline = y + s.asc; break;
    }
    return draw_glyphs(&c, &s, tx, off, len, x + c.tx, baseline + c.ty, maxw);
}

/* drawText(X, off, len, x, y, flags, width) */
NAT(g_drawTextX) {
    UNUSED; Text tx;
    if (!text_of(vm, a[1], &tx)) { ret[0] = 0; return NAT_OK; }
    ret[0] = (uint32_t)draw_text(vm, t, a[0], &tx, (int32_t)a[2], (int32_t)a[3], (int)a[4], (int)a[5], (int)a[6], (int)a[7]);
    return NAT_OK;
}
/* drawText(char, x, y, flags, width) */
NAT(g_drawTextC) {
    UNUSED;
    Text tx = { 0 };
    /* build a 1-char text from a temporary char array */
    ref_t arr = vm_new_array(vm, BT_CHAR, NULL, 1, 1);
    Obj *o = vm_obj(vm, arr);
    uint16_t ch = (uint16_t)a[1];
    memcpy(o->d, &ch, 2);
    tx.arr = o; tx.size = 1;
    ret[0] = (uint32_t)draw_text(vm, t, a[0], &tx, 0, 1, (int)a[2], (int)a[3], (int)a[4], (int)a[5]);
    return NAT_OK;
}

static void tm_fill(BBVM *vm, ref_t tmr, int adv, int chars, FontSel *s) {
    Obj *tm = vm_obj(vm, tmr);
    if (!tm || tm->len < 13) return;
    tm->d[0] = (uint32_t)adv; tm->d[1] = 0;
    tm->d[2] = 0; tm->d[3] = (uint32_t)-s->asc; tm->d[4] = (uint32_t)adv; tm->d[5] = (uint32_t)s->desc;
    tm->d[6] = (uint32_t)chars;
    tm->d[7] = (uint32_t)adv; tm->d[8] = 0; tm->d[9] = (uint32_t)chars;
    tm->d[10] = (uint32_t)adv; tm->d[11] = 0; tm->d[12] = 0;
}

/* how many characters fit, honouring DrawTextParam.iMaxAdvance / iMaxCharacters */
static int fit(BBVM *vm, FontSel *s, const Text *tx, int32_t off, int32_t len, ref_t dtpr, int *adv) {
    Obj *p = vm_obj(vm, dtpr);
    int maxAdv = p && p->len > 1 ? (int)p->d[0] : -1, maxCh = p && p->len > 1 ? (int)p->d[1] : -1;
    int track = p && p->len > 2 ? (int)p->d[2] : 0;
    int w = 0, n = 0;
    for (int32_t i = 0; i < len; i++) {
        if (maxCh >= 0 && n >= maxCh) break;
        Glyph *g = glyph(s, text_at(tx, (uint32_t)(off + i)));
        int ga = (g ? g->adv : 0) + track;
        if (maxAdv > 0 && w + ga > maxAdv) break;
        w += ga; n++;
    }
    *adv = w;
    return n;
}

/* drawText(X, off, len, x, y, DrawTextParam, TextMetrics) on Graphics */
static int draw_text_param(BBVM *vm, Thread *t, ref_t gr, ref_t xr, int32_t off, int32_t len, int x, int y, ref_t dtp, ref_t tmr, uint32_t *ret) {
    Text tx;
    GCtx c;
    FontSel s;
    ret[0] = 0;
    if (!text_of(vm, xr, &tx) || !bbg_ctx(vm, gr, &c) || !font_sel(vm, t, c.g->d[G_FONT], &s)) return NAT_OK;
    text_range(&tx, &off, &len);
    {
        Obj *p = vm_obj(vm, dtp);
        if (p && p->len > 7) {
            int so = (int32_t)p->d[5], eo = (int32_t)p->d[6];
            if (so > 0 && so <= len) { off += so; len -= so; eo -= so; }
            if (eo >= 0 && eo < len) len = eo;
        }
    }
    int adv, n = fit(vm, &s, &tx, off, len, dtp, &adv);
    if (getenv("BBGFXLOG")) {
        Obj *p = vm_obj(vm, dtp);
        char buf[64]; int k = 0;
        for (int32_t i = 0; i < n && k < 60; i++) { unsigned ch = text_at(&tx, (uint32_t)(off + i)); buf[k++] = ch < 127 && ch >= 32 ? (char)ch : '?'; if (ch >= 127 || ch < 32) k += snprintf(buf + k, sizeof buf - k > 0 ? sizeof buf - k : 0, "%x", ch); if (k > 60) k = 60; }
        buf[k] = 0;
        vm_log(vm, "drawTextP [p%d bmp %x col %06x a %d] .%s. at %d,%d align %d so %d eo %d clip %d,%d-%d,%d t %d,%d asc %d", t->procId, c.g->d[G_BITMAP], c.color, c.alpha, buf, x, y, p && p->len > 7 ? (int)p->d[7] : -9, p && p->len > 7 ? (int)p->d[5] : -9, p && p->len > 7 ? (int)p->d[6] : -9, c.cx0, c.cy0, c.cx1, c.cy1, c.tx, c.ty, s.asc);
    }
    {
        Obj *p = vm_obj(vm, dtp);
        int al = p && p->len > 7 ? (int)p->d[7] : 0;
        /* DrawTextParam alignment 8 puts y on the baseline; otherwise y is the top */
        draw_glyphs(&c, &s, &tx, off, n, x + c.tx, y + c.ty + (al == 8 ? 0 : s.asc), -1);
    }
    tm_fill(vm, tmr, adv, n, &s);
    ret[0] = (uint32_t)adv;
    return NAT_OK;
}
NAT(g_drawTextP) { UNUSED; return draw_text_param(vm, t, a[0], a[1], (int32_t)a[2], (int32_t)a[3], (int)a[4], (int)a[5], a[6], a[7], ret); }
/* TextGraphics._drawText(Graphics, X, off, len, x, y, DrawTextParam, TextMetrics) */
NAT(tg_drawText) { UNUSED; return draw_text_param(vm, t, a[1], a[2], (int32_t)a[3], (int32_t)a[4], (int)a[5], (int)a[6], a[7], a[8], ret); }

/* ------------------------------------------------------------------ */
/* Font natives                                                        */

NAT(f_setMetrics) {
    UNUSED; FontSel s;
    Obj *fo = vm_obj(vm, a[0]);
    if (!fo || !font_sel(vm, t, a[0], &s)) return NAT_OK;
    fo->d[F_ASCENT] = (uint32_t)s.asc;
    fo->d[F_DESCENT] = (uint32_t)s.desc;
    fo->d[F_LEADING] = 0;
    return NAT_OK;
}
NAT(f_getAdvanceC) {
    UNUSED; FontSel s;
    ret[0] = 0;
    if (!font_sel(vm, t, a[0], &s)) return NAT_OK;
    Glyph *g = glyph(&s, a[1] & 0xffff);
    ret[0] = g ? (uint32_t)g->adv : 0;
    return NAT_OK;
}
NAT(f_getAdvanceX) {   /* _getAdvance(X, off, len) */
    UNUSED; FontSel s; Text tx;
    ret[0] = 0;
    if (!font_sel(vm, t, a[0], &s) || !text_of(vm, a[1], &tx)) return NAT_OK;
    int32_t off = (int32_t)a[2], len = (int32_t)a[3];
    text_range(&tx, &off, &len);
    ret[0] = (uint32_t)text_advance(&s, &tx, off, len);
    return NAT_OK;
}
NAT(f_measureText) {   /* _measureText(X, off, len, DrawTextParam, TextMetrics) */
    UNUSED; FontSel s; Text tx;
    ret[0] = 0;
    if (!font_sel(vm, t, a[0], &s) || !text_of(vm, a[1], &tx)) return NAT_OK;
    int32_t off = (int32_t)a[2], len = (int32_t)a[3];
    text_range(&tx, &off, &len);
    int adv, n = fit(vm, &s, &tx, off, len, a[4], &adv);
    tm_fill(vm, a[5], adv, n, &s);
    ret[0] = (uint32_t)adv;
    return NAT_OK;
}
static void fm_fill(BBVM *vm, Thread *t, ref_t font, ref_t fmr) {
    FontSel s;
    Obj *fm = vm_obj(vm, fmr);
    if (!fm || fm->len < 12 || !font_sel(vm, t, font, &s)) return;
    Glyph *sp = glyph(&s, ' '), *m = glyph(&s, 'M'), *x = glyph(&s, 'x');
    fm->d[0] = (uint32_t)s.px; fm->d[1] = (uint32_t)s.asc; fm->d[2] = (uint32_t)s.asc;
    fm->d[3] = (uint32_t)s.desc; fm->d[4] = (uint32_t)s.desc;
    fm->d[5] = (uint32_t)(s.desc > 1 ? 1 : 0); fm->d[6] = (uint32_t)(s.px >= 24 ? 2 : 1);
    fm->d[7] = 0; fm->d[8] = 0;
    fm->d[9] = sp ? (uint32_t)sp->adv : 4;
    fm->d[10] = m ? (uint32_t)m->adv : (uint32_t)s.px;
    fm->d[11] = x ? (uint32_t)x->adv : (uint32_t)s.px / 2;
}
NAT(f_getMetrics) { UNUSED; fm_fill(vm, t, a[0], a[1]); return NAT_OK; }
NAT(f_getGlyphMetrics) {   /* (char, GlyphMetrics) */
    UNUSED; FontSel s;
    ret[0] = 0;
    Obj *gm = vm_obj(vm, a[2]);
    if (!gm || gm->len < 5 || !font_sel(vm, t, a[0], &s)) return NAT_OK;
    Glyph *g = glyph(&s, a[1] & 0xffff);
    if (!g) return NAT_OK;
    gm->d[0] = (uint32_t)g->xo; gm->d[1] = (uint32_t)-g->yo; gm->d[2] = (uint32_t)g->adv;
    gm->d[3] = (uint32_t)g->w; gm->d[4] = (uint32_t)g->h;
    ret[0] = (uint32_t)g->adv;
    return NAT_OK;
}

/* TextGraphics keeps the font it measures with; find the Font field */
static ref_t tg_font(BBVM *vm, ref_t tgr) {
    Obj *o = vm_obj(vm, tgr);
    if (!o) return 0;
    for (uint32_t i = 0; i < o->len && i < 16; i++) {
        Obj *f = vm_obj(vm, o->d[i]);
        if (f && f->cls && !strcmp(f->cls->full, "net.rim.device.api.ui.Font")) return o->d[i];
        if (f && f->cls && !strcmp(f->cls->full, "net.rim.device.api.ui.Graphics") && f->len > G_FONT) return f->d[G_FONT];
    }
    return 0;
}
NAT(tg_measureText) {   /* _measureText(X, off, len, DrawTextParam, TextMetrics) */
    UNUSED;
    uint32_t b[6] = { tg_font(vm, a[0]), a[1], a[2], a[3], a[4], a[5] };
    return f_measureText(vm, t, b, ret);
}
NAT(tg_getFontMetrics) { UNUSED; fm_fill(vm, t, tg_font(vm, a[0]), a[1]); return NAT_OK; }

/* ------------------------------------------------------------------ */
/* FontManagerImpl                                                     */

NAT(fm_loadFontResource) {   /* (byte[][][] table, index, name, system, ?, ?) -> handle */
    UNUSED;
    fonts_load(vm, t);
    ret[0] = a[1] + 1;
    return NAT_OK;
}
NAT(fm_noop) { UNUSED; return NAT_OK; }
NAT(fm_unload) { UNUSED; ret[0] = 1; return NAT_OK; }

/* ------------------------------------------------------------------ */
/* ArticInterface: the device's native text layout engine.
 *
 * Format() fills the shared Layout with lines (11 ints each: text length,
 * skipped characters, run count, origin x, origin y, bounds l/t/r/b, bidi
 * bytes, flags) and runs (10 ints: start in line, length, format index,
 * extended attributes, advance, x/y offset, ascent, descent, flags); the
 * Java side turns them into Line objects. This version always lays out
 * the whole text again from the first line, wrapping at spaces. */

enum { L_TOTAL = 0, L_BL = 1, L_BT = 2, L_BR = 3, L_BB = 4, L_OBL = 5, L_OBT = 6, L_OBR = 7, L_OBB = 8,
       L_TEXTSTART = 13, L_OLDLINES = 14, L_UNFSTART = 15, L_UNFLEN = 16, L_LINEDATA = 17, L_RUNDATA = 18,
       L_BIDI = 19, L_FLOAT = 20, L_FBIDI = 21, L_FFLOAT = 22 };

static int count_lines(BBVM *vm, ref_t line) {
    Obj *l = vm_obj(vm, line);
    if (!l) return 0;
    for (int g = 0; g < 100000 && vm_obj(vm, l->d[0]); g++) l = vm_obj(vm, l->d[0]);   /* to the first */
    int n = 0;
    for (; l && n < 100000; n++) l = vm_obj(vm, l->d[1]);
    return n;
}

NAT(artic_format) {
    UNUSED;
    /* (Layout, Font, width?, textStart, oldLength, newLength, ?, Z, ?, AttributedString, Line, ...) */
    Obj *lay = vm_obj(vm, a[0]);
    Obj *as = vm_obj(vm, a[9]);
    if (!lay || lay->len < 23 || !as) return NAT_OK;
    FontSel s;
    if (!font_sel(vm, t, a[1], &s)) return NAT_OK;
    int width = (int32_t)a[2];
    if (width <= 0 || width > 100000) width = 0x7fffffff;
    Text tx;
    if (!text_of(vm, as->d[3], &tx)) { memset(&tx, 0, sizeof tx); }
    uint32_t total = as->len > 4 ? as->d[4] : tx.size;
    if (total > tx.size) total = tx.size;
    if (getenv("BBGFXLOG")) vm_log(vm, "Format w=%d start=%d old=%d new=%d a5=%d a6=%d a7=%d len=%u", (int)a[2], (int)a[3], (int)a[4], (int)a[5], (int)a[6], (int)a[7], (int)a[8], total);

    /* break into lines */
    int cap = 16, n = 0;
    int *ls = malloc(sizeof(int) * 3 * (size_t)cap);   /* start, visible length, skipped */
    uint32_t pos = 0;
    do {
        uint32_t i = pos, lastBreak = 0; int w = 0, haveBreak = 0;
        while (i < total) {
            uint32_t c = text_at(&tx, i);
            if (c == '\n' || c == '\r' || c == 0x2029) break;
            Glyph *g = glyph(&s, c);
            int adv = g ? g->adv : 0;
            if (w + adv > width && i > pos) break;
            w += adv;
            if (c == ' ') { lastBreak = i; haveBreak = 1; }
            i++;
        }
        uint32_t end = i, next = i;
        if (i < total) {
            uint32_t c = text_at(&tx, i);
            if (c == '\n' || c == '\r' || c == 0x2029) {
                next = i + 1;
                if (c == '\r' && next < total && text_at(&tx, next) == '\n') next++;
            } else if (haveBreak) { end = lastBreak; next = lastBreak + 1; }
            else next = end;
        }
        if (n == cap) { cap *= 2; ls = realloc(ls, sizeof(int) * 3 * (size_t)cap); }
        ls[3 * n] = (int)pos; ls[3 * n + 1] = (int)(end - pos); ls[3 * n + 2] = (int)(next - end);
        n++;
        pos = next;
    } while (pos < total);
    /* trailing newline starts an empty last line */
    if (total && n && ls[3 * (n - 1)] + ls[3 * (n - 1) + 1] + ls[3 * (n - 1) + 2] == (int)total) {
        uint32_t c = text_at(&tx, total - 1);
        if (c == '\n' || c == '\r' || c == 0x2029) {
            if (n == cap) { cap *= 2; ls = realloc(ls, sizeof(int) * 3 * (size_t)cap); }
            ls[3 * n] = (int)total; ls[3 * n + 1] = 0; ls[3 * n + 2] = 0; n++;
        }
    }

    int oldLines = count_lines(vm, a[10]);
    ref_t ld = vm_new_array(vm, BT_INT, NULL, 1, (uint32_t)(11 * n));
    int nruns = 0;
    for (int k = 0; k < n; k++) if (ls[3 * k + 1] > 0) nruns++;
    ref_t rd = vm_new_array(vm, BT_INT, NULL, 1, (uint32_t)(10 * nruns));
    Obj *L = vm_obj(vm, ld), *R = vm_obj(vm, rd);
    int lineH = s.asc + s.desc, maxW = 0, ri = 0;
    for (int k = 0; k < n; k++) {
        int st = ls[3 * k], vl = ls[3 * k + 1];
        int w = text_advance(&s, &tx, st, vl);
        if (w > maxW) maxW = w;
        uint32_t *d = L->d + 11 * k;
        d[0] = (uint32_t)vl; d[1] = (uint32_t)ls[3 * k + 2]; d[2] = vl > 0 ? 1 : 0;
        d[3] = 0; d[4] = (uint32_t)s.asc;
        d[5] = 0; d[6] = (uint32_t)-s.asc; d[7] = (uint32_t)w; d[8] = (uint32_t)s.desc;
        d[9] = 0;
        /* flag 1: the line starts a paragraph (the first line, or after a line break) */
        int para = k == 0;
        if (k > 0) { int pe = ls[3 * (k - 1)] + ls[3 * (k - 1) + 1]; if (ls[3 * (k - 1) + 2] > 0 && pe < (int)total) { uint32_t c = text_at(&tx, (uint32_t)pe); para = c == '\n' || c == '\r' || c == 0x2029; } }
        d[10] = para ? 1 : 0;
        if (vl > 0) {
            uint32_t *r = R->d + 10 * ri++;
            r[0] = 0; r[1] = (uint32_t)vl; r[2] = 0; r[3] = 0; r[4] = (uint32_t)w;
            r[5] = 0; r[6] = 0; r[7] = (uint32_t)s.asc; r[8] = (uint32_t)s.desc; r[9] = 0;
        }
    }
    free(ls);
    lay = vm_obj(vm, a[0]);
    int bw = width == 0x7fffffff ? maxW : width, bh = lineH * n;
    lay->d[L_TOTAL] = total;
    lay->d[L_BL] = 0; lay->d[L_BT] = 0; lay->d[L_BR] = (uint32_t)bw; lay->d[L_BB] = (uint32_t)bh;
    lay->d[L_OBL] = 0; lay->d[L_OBT] = 0; lay->d[L_OBR] = (uint32_t)bw; lay->d[L_OBB] = (uint32_t)bh;
    lay->d[L_TEXTSTART] = 0; lay->d[L_OLDLINES] = (uint32_t)oldLines;
    lay->d[L_UNFSTART] = total; lay->d[L_UNFLEN] = 0;
    lay->d[L_LINEDATA] = ld; lay->d[L_RUNDATA] = rd;
    for (int k = L_BIDI; k <= L_FFLOAT; k++) if (!vm_obj(vm, lay->d[k])) lay->d[k] = vm_new_array(vm, BT_BYTE, NULL, 1, 16);
    return NAT_OK;
}

/* GetLabelMetrics(String[] labels, Font[] fonts, LabelMetrics out) */
NAT(artic_labelMetrics) {
    UNUSED;
    Obj *fonts = vm_obj(vm, a[1]), *out = vm_obj(vm, a[2]);
    if (!out || out->len < 2) return NAT_OK;
    int asc = 0, desc = 0;
    for (uint32_t i = 0; fonts && i < fonts->len; i++) {
        FontSel s;
        if (font_sel(vm, t, fonts->d[i], &s)) { if (s.asc > asc) asc = s.asc; if (s.desc > desc) desc = s.desc; }
    }
    out->d[0] = (uint32_t)asc; out->d[1] = (uint32_t)desc;
    return NAT_OK;
}
NAT(artic_false) { UNUSED; ret[0] = 0; return NAT_OK; }
NAT(artic_nop) { UNUSED; return NAT_OK; }

const NatEnt kNativesFont[] = {
    { "net.rim.device.internal.ui.ArticInterface.Format", artic_format },
    { "net.rim.device.internal.ui.ArticInterface.GetLabelMetrics", artic_labelMetrics },
    { "net.rim.device.internal.ui.ArticInterface.DocPosToCaret", artic_false },
    { "net.rim.device.internal.ui.ArticInterface.PointToDocPos", artic_nop },
    { "net.rim.device.internal.ui.ArticInterface.DocPosMove", artic_nop },
    { "net.rim.device.api.ui.FontManagerImpl.loadFontResource", fm_loadFontResource },
    { "net.rim.device.api.ui.FontManagerImpl.unloadFontResource", fm_unload },
    { "net.rim.device.api.ui.FontManagerImpl.submit", fm_noop },
    { "net.rim.device.api.ui.FontManagerImpl.setPreferredFallback", fm_noop },
    { "net.rim.device.api.ui.Font.setMetrics", f_setMetrics },
    { "net.rim.device.api.ui.Font.getAdvance(C)", f_getAdvanceC },
    { "net.rim.device.api.ui.Font._getAdvance", f_getAdvanceX },
    { "net.rim.device.api.ui.Font._measureText", f_measureText },
    { "net.rim.device.api.ui.Font.getMetrics", f_getMetrics },
    { "net.rim.device.api.ui.Font.getGlyphMetrics", f_getGlyphMetrics },
    { "net.rim.device.api.ui.TextGraphics._measureText", tg_measureText },
    { "net.rim.device.api.ui.TextGraphics._getFontMetrics", tg_getFontMetrics },
    { "net.rim.device.api.ui.TextGraphics._drawText", tg_drawText },
    { "net.rim.device.api.ui.Graphics.drawText(CIIII)", g_drawTextC },
    { "net.rim.device.api.ui.Graphics.drawText([BIIIIII)", g_drawTextX },
    { "net.rim.device.api.ui.Graphics.drawText([CIIIIII)", g_drawTextX },
    { "net.rim.device.api.ui.Graphics.drawText(Ljava.lang.String;IIIIII)", g_drawTextX },
    { "net.rim.device.api.ui.Graphics.drawText(Ljava.lang.StringBuffer;IIIIII)", g_drawTextX },
    { "net.rim.device.api.ui.Graphics.drawText(Lnet.rim.device.internal.ui.StringBufferGap;IIIIII)", g_drawTextX },
    { "net.rim.device.api.ui.Graphics.drawText(Ljava.lang.String;IIIILnet.rim.device.api.ui.DrawTextParam;Lnet.rim.device.api.ui.TextMetrics;)", g_drawTextP },
    { "net.rim.device.api.ui.Graphics.drawText(Ljava.lang.StringBuffer;IIIILnet.rim.device.api.ui.DrawTextParam;Lnet.rim.device.api.ui.TextMetrics;)", g_drawTextP },
    { "net.rim.device.api.ui.Graphics.drawText(Lnet.rim.device.internal.ui.StringBufferGap;IIIILnet.rim.device.api.ui.DrawTextParam;Lnet.rim.device.api.ui.TextMetrics;)", g_drawTextP },
    { "net.rim.device.api.ui.Graphics.drawText([CIIIILnet.rim.device.api.ui.DrawTextParam;Lnet.rim.device.api.ui.TextMetrics;)", g_drawTextP },
    { NULL, NULL }
};
