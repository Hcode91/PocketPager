/* JNI bridge for the BlackBerry OS family.
 *
 * The OS core (libbbcore.so: VM, natives, front-end API) is loaded here at
 * a fixed address, so a save state made in one run of the app points at
 * the same code and variables in the next (see core/bbos/bbsnap.c). Its
 * memory regions are at fixed addresses too (bbarena.c). This library
 * itself holds only the bridge and the installer importer.
 *
 * One OS per process: the activity runs in its own process and exits to
 * start over. */
#include <jni.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <android/dlext.h>
#include <android/log.h>
#include "bbos/bbimport.h"

#define FN(name) Java_com_hcode91_pocketpager_BbNative_##name
#define TAG "bbos"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#if UINTPTR_MAX > 0xffffffffu
#define CORE_ADDR ((uintptr_t)0x4f00000000ull)
#else
#define CORE_ADDR ((uintptr_t)0)   /* 32-bit: no fixed place; saves stay within one run */
#endif
#define CORE_SPACE ((size_t)64 << 20)

/* ---- the core's API (bbhost.h), resolved after loading it ---- */
static struct {
    int (*start)(const char *dir, char *err, size_t errn);
    int (*run)(int maxMs);
    int (*booted)(void);
    long long (*instructions)(void);
    const char *(*status)(void);
    const uint16_t *(*screen)(int *w, int *h, int *frame);
    int (*launch)(const char *module);
    int (*pid_of)(const char *module);
    int (*foreground)(const char *module);
    void (*key)(int ch, int action, int status);
    void (*trackball)(int dx, int dy);
    void (*click)(int down);
    int (*save)(const char *path, char *err, size_t errn);
    int (*load)(const char *path, char *err, size_t errn);
} core;
static int g_coreState;          /* 0 not loaded, 1 loaded at its address, 2 loaded elsewhere, -1 failed */
static char g_coreErr[200];

static int load_core(void) {
    if (g_coreState) return g_coreState > 0;
    void *h = NULL;
    void *space = !CORE_ADDR ? MAP_FAILED : mmap((void *)CORE_ADDR, CORE_SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (space != MAP_FAILED && (uintptr_t)space == CORE_ADDR) {
        android_dlextinfo ext;
        memset(&ext, 0, sizeof ext);
        ext.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
        ext.reserved_addr = space;
        ext.reserved_size = CORE_SPACE;
        h = android_dlopen_ext("libbbcore.so", RTLD_NOW | RTLD_LOCAL, &ext);
        if (!h) __android_log_print(ANDROID_LOG_WARN, TAG, "fixed-address load failed: %s", dlerror());
    } else if (space != MAP_FAILED) munmap(space, CORE_SPACE);
    g_coreState = 1;
    if (!h) {
        /* still playable; save states then only reload within this run */
        h = dlopen("libbbcore.so", RTLD_NOW | RTLD_LOCAL);
        g_coreState = 2;
    }
    if (!h) { snprintf(g_coreErr, sizeof g_coreErr, "cannot load the OS core: %s", dlerror()); g_coreState = -1; return 0; }
#define SYM(field, name) *(void **)&core.field = dlsym(h, name)
    SYM(start, "bbh_start"); SYM(run, "bbh_run"); SYM(booted, "bbh_booted");
    SYM(instructions, "bbh_instructions"); SYM(status, "bbh_status"); SYM(screen, "bbh_screen");
    SYM(launch, "bbh_launch"); SYM(pid_of, "bbh_pid_of"); SYM(foreground, "bbh_foreground");
    SYM(key, "bbh_key"); SYM(trackball, "bbh_trackball"); SYM(click, "bbh_click");
    SYM(save, "bbh_save"); SYM(load, "bbh_load");
#undef SYM
    if (!core.start || !core.run || !core.save || !core.load) {
        snprintf(g_coreErr, sizeof g_coreErr, "the OS core is incomplete");
        g_coreState = -1;
        return 0;
    }
    return 1;
}

static volatile int g_pct;
static char g_what[128];

static void on_progress(void *ctx, const char *what, int pct) {
    (void)ctx;
    snprintf(g_what, sizeof g_what, "%s", what);
    g_pct = pct;
}

static jstring str(JNIEnv *env, const char *s) { return (*env)->NewStringUTF(env, s ? s : ""); }

/* ---- importing (runs here, outside the core) ---- */

JNIEXPORT jstring JNICALL FN(importInstaller)(JNIEnv *env, jclass c, jstring exe, jstring out, jstring tmp) {
    const char *e = (*env)->GetStringUTFChars(env, exe, 0);
    const char *o = (*env)->GetStringUTFChars(env, out, 0);
    const char *t = (*env)->GetStringUTFChars(env, tmp, 0);
    char err[300] = "";
    g_pct = 0; g_what[0] = 0;
    int n = bbimport_installer(e, o, t, on_progress, NULL, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, exe, e);
    (*env)->ReleaseStringUTFChars(env, out, o);
    (*env)->ReleaseStringUTFChars(env, tmp, t);
    char res[340];
    if (n < 0) snprintf(res, sizeof res, "error:%s", err); else snprintf(res, sizeof res, "ok:%d", n);
    return str(env, res);
}

JNIEXPORT jstring JNICALL FN(importProgress)(JNIEnv *env, jclass c) {
    char res[160];
    snprintf(res, sizeof res, "%s (%d%%)", g_what, g_pct);
    return str(env, res);
}

JNIEXPORT jstring JNICALL FN(importModules)(JNIEnv *env, jclass c, jstring path, jstring out) {
    const char *p = (*env)->GetStringUTFChars(env, path, 0);
    const char *o = (*env)->GetStringUTFChars(env, out, 0);
    char err[300] = "", names[2048] = "";
    int n = bbimport_modules(p, o, names, sizeof names, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, path, p);
    (*env)->ReleaseStringUTFChars(env, out, o);
    char res[2400];
    /* "ok:count:app1,app2" or "error:message" */
    if (n < 0) snprintf(res, sizeof res, "error:%s", err); else snprintf(res, sizeof res, "ok:%d:%s", n, names);
    return str(env, res);
}

/* ---- the OS ---- */

JNIEXPORT jstring JNICALL FN(start)(JNIEnv *env, jclass c, jstring dir) {
    if (!load_core()) return str(env, g_coreErr);
    const char *d = (*env)->GetStringUTFChars(env, dir, 0);
    char err[300] = "";
    int rc = core.start(d, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, dir, d);
    return rc < 0 ? str(env, err) : NULL;
}

JNIEXPORT jint JNICALL FN(run)(JNIEnv *env, jclass c, jint ms) { return load_core() ? core.run(ms) : -1; }
JNIEXPORT jboolean JNICALL FN(booted)(JNIEnv *env, jclass c) { return load_core() && core.booted() ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT jstring JNICALL FN(status)(JNIEnv *env, jclass c) {
    if (!load_core()) return str(env, g_coreErr);
    char res[320];
    snprintf(res, sizeof res, "%s · %lld M instructions", core.status(), core.instructions() / 1000000);
    return str(env, res);
}

/* Copy the 480x320 RGB565 screen into an ARGB int array; returns the
 * frame counter so the caller can skip unchanged frames. */
JNIEXPORT jint JNICALL FN(render)(JNIEnv *env, jclass c, jintArray pixels) {
    if (!load_core()) return -1;
    int w, h, frame;
    const uint16_t *fb = core.screen(&w, &h, &frame);
    jsize n = (*env)->GetArrayLength(env, pixels);
    if (n < w * h) return -1;
    jint *px = (*env)->GetIntArrayElements(env, pixels, 0);
    for (int i = 0; i < w * h; i++) {
        uint16_t v = fb[i];
        uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        px[i] = (jint)(0xff000000u | (r << 19 | (r >> 2) << 16) | (g << 10 | (g >> 4) << 8) | (b << 3 | b >> 2));
    }
    (*env)->ReleaseIntArrayElements(env, pixels, px, 0);
    return frame;
}

static jint with_module(JNIEnv *env, jstring module, int (*fn)(const char *)) {
    if (!fn) return -1;
    const char *m = (*env)->GetStringUTFChars(env, module, 0);
    int rc = fn(m);
    (*env)->ReleaseStringUTFChars(env, module, m);
    return rc;
}
JNIEXPORT jint JNICALL FN(launch)(JNIEnv *env, jclass c, jstring module) { return load_core() ? with_module(env, module, core.launch) : -1; }
JNIEXPORT jint JNICALL FN(foreground)(JNIEnv *env, jclass c, jstring module) { return load_core() ? with_module(env, module, core.foreground) : -1; }
JNIEXPORT jint JNICALL FN(pidOf)(JNIEnv *env, jclass c, jstring module) { return load_core() ? with_module(env, module, core.pid_of) : -1; }

JNIEXPORT void JNICALL FN(key)(JNIEnv *env, jclass c, jint ch, jint action, jint status) { if (load_core()) core.key(ch, action, status); }
JNIEXPORT void JNICALL FN(trackball)(JNIEnv *env, jclass c, jint dx, jint dy) { if (load_core()) core.trackball(dx, dy); }
JNIEXPORT void JNICALL FN(click)(JNIEnv *env, jclass c, jboolean down) { if (load_core()) core.click(down); }

/* ---- save states: null on success, else a message ---- */

static jstring snap(JNIEnv *env, jstring path, int loading) {
    if (!load_core()) return str(env, g_coreErr);
    if (loading && g_coreState == 2)
        return str(env, "this run of the app couldn't place the emulator at its usual address, so it can't load saves; close the app fully and try again");
    const char *p = (*env)->GetStringUTFChars(env, path, 0);
    char err[300] = "";
    int rc = loading ? core.load(p, err, sizeof err) : core.save(p, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, path, p);
    return rc < 0 ? str(env, err) : NULL;
}
JNIEXPORT jstring JNICALL FN(saveState)(JNIEnv *env, jclass c, jstring path) { return snap(env, path, 0); }
JNIEXPORT jstring JNICALL FN(loadState)(JNIEnv *env, jclass c, jstring path) { return snap(env, path, 1); }
