/*
 * UI 反向调 Java（com.easylua.EasyLuaJni 的 ui* 静态方法）。
 *
 * Highlight 走 JNI -> Overlay (Canvas 自绘)。
 * Toast 改走 am broadcast -> SampleUI APK 内的 ToastReceiver（方案 B）。
 *   原因：root 进程的 uid=0 在 MIUI WindowManagerService.addWindow 时
 *   被 doesAddToastWindowRequireToken 拒绝，无法直接走原生 Toast。
 *   故由 APK 进程接收广播后 Toast.makeText().show()，规避所有限制。
 */

#include "ui.h"
#include "misc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JavaVM    *g_jvm = NULL;
static jclass     g_cls_jni = NULL;
static jmethodID  g_mid_highlight = NULL;       /* uiHighlight(int,int,int,int,int,int,String) */
static jmethodID  g_mid_highlight_off = NULL;   /* uiHighlightOff() */

static pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;
static int             g_init_done = 0;

void Ui_SetJvm(JavaVM *jvm) { g_jvm = jvm; }

static int attach_env(JNIEnv **env)
{
    if (!g_jvm) return 0;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)env, JNI_VERSION_1_6) == JNI_OK) return 0;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, env, NULL) == JNI_OK) return 1;
    *env = NULL;
    return 0;
}

static void detach_env(int need) { if (need && g_jvm) (*g_jvm)->DetachCurrentThread(g_jvm); }

static int ensure_init(JNIEnv *env)
{
    if (g_init_done) return g_init_done > 0;
    pthread_mutex_lock(&g_init_mu);
    if (g_init_done) { pthread_mutex_unlock(&g_init_mu); return g_init_done > 0; }

    jclass local = (*env)->FindClass(env, "com/easylua/EasyLuaJni");
    if (!local) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        g_init_done = -1;
        pthread_mutex_unlock(&g_init_mu);
        return 0;
    }
    g_cls_jni = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);

    g_mid_highlight = (*env)->GetStaticMethodID(env, g_cls_jni,
        "uiHighlight", "(IIIIIILjava/lang/String;)V");
    g_mid_highlight_off = (*env)->GetStaticMethodID(env, g_cls_jni,
        "uiHighlightOff", "()V");

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }

    int ok = (g_mid_highlight && g_mid_highlight_off);
    g_init_done = ok ? 1 : -1;
    pthread_mutex_unlock(&g_init_mu);
    return ok;
}

static jstring s2j(JNIEnv *env, const char *s)
{
    return s ? (*env)->NewStringUTF(env, s) : NULL;
}

/* ---- API ---- */

/* SampleUI 的 ToastReceiver 包名/组件 */
#define TOAST_RECEIVER_COMPONENT  "com.example.sampleui/.control.ToastReceiver"
#define TOAST_RECEIVER_ACTION     "com.easylua.TOAST"

/* shell 字符串转义：把 ' 替换成 '\'' （单引号包裹内容里碰到单引号的标准做法） */
static int sh_escape(const char *src, char *dst, int dst_size)
{
    int wi = 0;
    for (int i = 0; src && src[i] != 0 && wi + 5 < dst_size; i++) {
        char c = src[i];
        if (c == '\'') {
            /* '...'\\''...' */
            if (wi + 4 >= dst_size) break;
            dst[wi++] = '\'';
            dst[wi++] = '\\';
            dst[wi++] = '\'';
            dst[wi++] = '\'';
        } else {
            dst[wi++] = c;
        }
    }
    if (wi >= dst_size) wi = dst_size - 1;
    dst[wi] = 0;
    return wi;
}

int Ui_Toast(const char *msg, int x, int y, int dur_ms)
{
    /* 方案 B：直接用 am broadcast 触发 SampleUI 的 ToastReceiver。
     * 不依赖 JNI 反向调，也不需要 APK 在前台。
     *
     * dur_ms < 3500 -> dur=0 (SHORT)，>= 3500 -> dur=1 (LONG)
     * x / y 系统 Toast 不支持精确坐标，直接忽略（兼容 Lua 端签名）。
     */
    (void)x; (void)y;
    if (!msg) msg = "";

    char esc[2048];
    sh_escape(msg, esc, sizeof(esc));

    int dur_flag = (dur_ms >= 3500) ? 1 : 0;
    char cmd[3072];
    int n = snprintf(cmd, sizeof(cmd),
        "am broadcast -n %s -a %s --es msg '%s' --ei dur %d >/dev/null 2>&1",
        TOAST_RECEIVER_COMPONENT, TOAST_RECEIVER_ACTION, esc, dur_flag);
    if (n <= 0 || n >= (int)sizeof(cmd)) return -1;

    char buf[256];
    int len = Shell_Exec(cmd, buf, sizeof(buf));
    return len >= 0 ? 0 : -2;
}

int Ui_Highlight(int x, int y, int w, int h,
                 int color_argb, int dur_ms, const char *label)
{
    JNIEnv *env;
    int detach = attach_env(&env);
    if (!env) return -1;

    int rc = -1;
    if (ensure_init(env)) {
        jstring jl = s2j(env, label);
        (*env)->CallStaticVoidMethod(env, g_cls_jni, g_mid_highlight,
                                     x, y, w, h, color_argb, dur_ms, jl);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            rc = -2;
        } else {
            rc = 0;
        }
        if (jl) (*env)->DeleteLocalRef(env, jl);
    }
    detach_env(detach);
    return rc;
}

int Ui_HighlightOff(void)
{
    JNIEnv *env;
    int detach = attach_env(&env);
    if (!env) return -1;

    int rc = -1;
    if (ensure_init(env)) {
        (*env)->CallStaticVoidMethod(env, g_cls_jni, g_mid_highlight_off);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            rc = -2;
        } else {
            rc = 0;
        }
    }
    detach_env(detach);
    return rc;
}
