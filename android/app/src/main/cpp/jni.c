/*
 * PocketPager JNI bridge.
 * All calls except nativeDrainLog are made from the emulator thread.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <android/log.h>
#include "rim/board.h"

#define LOG_LINES 256
#define LOG_LEN 160

static char g_lines[LOG_LINES][LOG_LEN];
static int g_head, g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_sink(const char *line)
{
	__android_log_print(ANDROID_LOG_INFO, "PocketPager", "%s", line);
	pthread_mutex_lock(&g_lock);
	strncpy(g_lines[g_head], line, LOG_LEN - 1);
	g_lines[g_head][LOG_LEN - 1] = 0;
	g_head = (g_head + 1) % LOG_LINES;
	if (g_count < LOG_LINES) g_count++;
	pthread_mutex_unlock(&g_lock);
}

JNIEXPORT jlong JNICALL
Java_com_hcode91_pocketpager_Native_create(JNIEnv *env, jclass cls, jbyteArray image, jobjectArray errOut)
{
	(void)cls;
	pp_set_log(log_sink);
	jsize len = (*env)->GetArrayLength(env, image);
	jbyte *data = (*env)->GetByteArrayElements(env, image, NULL);
	char err[128] = "";
	PPBoard *b = pp_board_new((const uint8_t *)data, (size_t)len, err, sizeof err);
	(*env)->ReleaseByteArrayElements(env, image, data, JNI_ABORT);
	if (!b && errOut && (*env)->GetArrayLength(env, errOut) > 0)
		(*env)->SetObjectArrayElement(env, errOut, 0, (*env)->NewStringUTF(env, err));
	return (jlong)(intptr_t)b;
}

JNIEXPORT void JNICALL
Java_com_hcode91_pocketpager_Native_destroy(JNIEnv *env, jclass cls, jlong h)
{
	(void)env; (void)cls;
	pp_board_free((PPBoard *)(intptr_t)h);
}

JNIEXPORT void JNICALL
Java_com_hcode91_pocketpager_Native_reset(JNIEnv *env, jclass cls, jlong h)
{
	(void)env; (void)cls;
	pp_board_reset((PPBoard *)(intptr_t)h);
}

JNIEXPORT void JNICALL
Java_com_hcode91_pocketpager_Native_run(JNIEnv *env, jclass cls, jlong h, jint us)
{
	(void)env; (void)cls;
	pp_board_run((PPBoard *)(intptr_t)h, (uint32_t)us);
}

JNIEXPORT jint JNICALL
Java_com_hcode91_pocketpager_Native_model(JNIEnv *env, jclass cls, jlong h)
{
	(void)env; (void)cls;
	return pp_board_model((PPBoard *)(intptr_t)h);
}

JNIEXPORT jint JNICALL
Java_com_hcode91_pocketpager_Native_lcdWidth(JNIEnv *env, jclass cls, jlong h)
{
	(void)env; (void)cls;
	return pp_lcd_width((PPBoard *)(intptr_t)h);
}

JNIEXPORT jint JNICALL
Java_com_hcode91_pocketpager_Native_lcdHeight(JNIEnv *env, jclass cls, jlong h)
{
	(void)env; (void)cls;
	return pp_lcd_height((PPBoard *)(intptr_t)h);
}

JNIEXPORT jboolean JNICALL
Java_com_hcode91_pocketpager_Native_render(JNIEnv *env, jclass cls, jlong h, jintArray pixels)
{
	(void)cls;
	jint *px = (*env)->GetIntArrayElements(env, pixels, NULL);
	int changed = pp_lcd_render((PPBoard *)(intptr_t)h, (uint32_t *)px);
	(*env)->ReleaseIntArrayElements(env, pixels, px, 0);
	return changed ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_hcode91_pocketpager_Native_key(JNIEnv *env, jclass cls, jlong h, jint key, jboolean down)
{
	(void)env; (void)cls;
	pp_key((PPBoard *)(intptr_t)h, key, down);
}

JNIEXPORT jstring JNICALL
Java_com_hcode91_pocketpager_Native_status(JNIEnv *env, jclass cls, jlong h)
{
	(void)cls;
	char buf[256];
	pp_board_status((PPBoard *)(intptr_t)h, buf, sizeof buf);
	return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jboolean JNICALL
Java_com_hcode91_pocketpager_Native_stopped(JNIEnv *env, jclass cls, jlong h)
{
	(void)env; (void)cls;
	return pp_board_stopped((PPBoard *)(intptr_t)h) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL
Java_com_hcode91_pocketpager_Native_getFlash(JNIEnv *env, jclass cls, jlong h)
{
	(void)cls;
	jbyteArray arr = (*env)->NewByteArray(env, PP_FLASH_IMAGE_SIZE);
	if (!arr) return NULL;
	jbyte *p = (*env)->GetByteArrayElements(env, arr, NULL);
	pp_board_get_flash((PPBoard *)(intptr_t)h, (uint8_t *)p);
	(*env)->ReleaseByteArrayElements(env, arr, p, 0);
	return arr;
}

JNIEXPORT jboolean JNICALL
Java_com_hcode91_pocketpager_Native_setFlash(JNIEnv *env, jclass cls, jlong h, jbyteArray data)
{
	(void)cls;
	jsize len = (*env)->GetArrayLength(env, data);
	jbyte *p = (*env)->GetByteArrayElements(env, data, NULL);
	int ok = pp_board_set_flash((PPBoard *)(intptr_t)h, (const uint8_t *)p, (size_t)len);
	(*env)->ReleaseByteArrayElements(env, data, p, JNI_ABORT);
	if (ok) pp_board_reset((PPBoard *)(intptr_t)h);
	return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_hcode91_pocketpager_Native_flashDirty(JNIEnv *env, jclass cls, jlong h)
{
	(void)env; (void)cls;
	return pp_board_flash_dirty((PPBoard *)(intptr_t)h) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_hcode91_pocketpager_Native_factory(JNIEnv *env, jclass cls, jlong h)
{
	(void)env; (void)cls;
	pp_board_factory((PPBoard *)(intptr_t)h);
	pp_board_reset((PPBoard *)(intptr_t)h);
}

/* Returns new log lines since the last call, joined by newlines. */
JNIEXPORT jstring JNICALL
Java_com_hcode91_pocketpager_Native_drainLog(JNIEnv *env, jclass cls)
{
	(void)cls;
	static char out[LOG_LINES * LOG_LEN];
	size_t n = 0;
	pthread_mutex_lock(&g_lock);
	int start = (g_head - g_count + LOG_LINES) % LOG_LINES;
	for (int i = 0; i < g_count; i++) {
		const char *l = g_lines[(start + i) % LOG_LINES];
		size_t ll = strlen(l);
		memcpy(out + n, l, ll);
		n += ll;
		out[n++] = '\n';
	}
	g_count = 0;
	pthread_mutex_unlock(&g_lock);
	out[n] = 0;
	return (*env)->NewStringUTF(env, out);
}
