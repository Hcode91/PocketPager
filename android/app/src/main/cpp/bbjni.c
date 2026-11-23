/* JNI bridge for the BlackBerry OS family (core/bbos). One OS instance per
 * process: the activity runs in its own process and exits to start over. */
#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <android/log.h>
#include "bbos/bbhost.h"
#include "bbos/bbimport.h"

#define FN(name) Java_com_hcode91_pocketpager_BbNative_##name

static volatile int g_pct;
static char g_what[128];

static void on_progress(void *ctx, const char *what, int pct) {
    (void)ctx;
    snprintf(g_what, sizeof g_what, "%s", what);
    g_pct = pct;
}

static jstring str(JNIEnv *env, const char *s) { return (*env)->NewStringUTF(env, s ? s : ""); }

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

JNIEXPORT jstring JNICALL FN(start)(JNIEnv *env, jclass c, jstring dir) {
    const char *d = (*env)->GetStringUTFChars(env, dir, 0);
    char err[300] = "";
    int rc = bbh_start(d, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, dir, d);
    return rc < 0 ? str(env, err) : NULL;
}

JNIEXPORT jint JNICALL FN(run)(JNIEnv *env, jclass c, jint ms) { return bbh_run(ms); }
JNIEXPORT jboolean JNICALL FN(booted)(JNIEnv *env, jclass c) { return bbh_booted() ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT jstring JNICALL FN(status)(JNIEnv *env, jclass c) {
    char res[320];
    snprintf(res, sizeof res, "%s · %lld M instructions", bbh_status(), bbh_instructions() / 1000000);
    return str(env, res);
}

/* Copy the 480x320 RGB565 screen into an ARGB int array; returns the
 * frame counter so the caller can skip unchanged frames. */
JNIEXPORT jint JNICALL FN(render)(JNIEnv *env, jclass c, jintArray pixels) {
    int w, h, frame;
    const uint16_t *fb = bbh_screen(&w, &h, &frame);
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

JNIEXPORT jint JNICALL FN(launch)(JNIEnv *env, jclass c, jstring module) {
    const char *m = (*env)->GetStringUTFChars(env, module, 0);
    int rc = bbh_launch(m);
    (*env)->ReleaseStringUTFChars(env, module, m);
    return rc;
}

JNIEXPORT jint JNICALL FN(foreground)(JNIEnv *env, jclass c, jstring module) {
    const char *m = (*env)->GetStringUTFChars(env, module, 0);
    int rc = bbh_foreground(m);
    (*env)->ReleaseStringUTFChars(env, module, m);
    return rc;
}

JNIEXPORT jint JNICALL FN(pidOf)(JNIEnv *env, jclass c, jstring module) {
    const char *m = (*env)->GetStringUTFChars(env, module, 0);
    int rc = bbh_pid_of(m);
    (*env)->ReleaseStringUTFChars(env, module, m);
    return rc;
}

JNIEXPORT void JNICALL FN(key)(JNIEnv *env, jclass c, jint ch, jint action, jint status) { bbh_key(ch, action, status); }
JNIEXPORT void JNICALL FN(trackball)(JNIEnv *env, jclass c, jint dx, jint dy) { bbh_trackball(dx, dy); }
JNIEXPORT void JNICALL FN(click)(JNIEnv *env, jclass c, jboolean down) { bbh_click(down); }
