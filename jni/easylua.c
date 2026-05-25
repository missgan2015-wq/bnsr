/*
 * easyLua native 引擎
 *
 * 命名规则（与 Lua 端 PascalCase 风格对齐）：
 *   - C 端 export：EasyLua_<Module>_<Func>，Module 与 Lua 命名空间一一对应
 *   - 例：Lua 的 Screen.Pixel(x, y) → C 端 EasyLua_Screen_Pixel(x, y)
 *
 * 设计原则（FFI 友好）：
 *   - 所有 export 函数只用基础数值类型 + 原始指针
 *   - 不返回 string / table，需要字符串时由调用方传入 char* buffer
 *   - 函数签名稳定无副作用，让 LuaJIT JIT trace 能内联
 */

#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

/* AHardwareBuffer：API 26+，带 fromHardwareBuffer：API 29+ */
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "touch.h"
#include "ui.h"
#include "images.h"
#include "misc.h"
#include "net.h"
#include "runtime_lua.h"
#include "lfs.h"   /* LuaFileSystem 1.9.0：嵌入式编译，免 require */

#define EXPORT __attribute__((visibility("default")))

/* ========================================================================== */
/*  错误日志宏                                                                */
/*                                                                            */
/*  所有 EXPORT 接口的参数校验失败、内部异常都通过 EL_ERR 打印，统一前缀      */
/*  "[easylua-c] error:"。该前缀会被 ScriptRunner.kt 端识别并：                */
/*    1) 写入设备本地日志文件 /sdcard/AutoGo/logs/<时间戳>.log                */
/*    2) 写入 LogPanel 屏幕日志（红色脚本 e 级）                              */
/*    3) 通过 ProcessListener.onStdoutLine 转发给 VSIX → 脚本输出 tab          */
/*                                                                            */
/*  使用：                                                                    */
/*    EL_ERR("FindColor: color must not be null");                            */
/*    EL_ERR("CmpColor: x=%d y=%d 越界 (w=%d h=%d)", x, y, W, H);             */
/*                                                                            */
/*  设计：                                                                    */
/*    - fprintf(stderr) 在 app_process 进程里和 stdout 合并到同一管道；       */
/*    - 这是热路径里的兜底，调用方一般会立即返回错误码，所以代价无所谓；     */
/*    - 不引入 android/log.h，避免跨 NDK 头依赖；用户用 `adb logcat` 抓时     */
/*      app_process 的 stderr 仍然会被 logwrapper 转一份到 system 日志。      */
/* ========================================================================== */
#define EL_ERR(fmt, ...) do { \
    fprintf(stderr, "[easylua-c] error: " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while (0)

/* 前向声明：resolve_region 要用，避免文件顺序依赖 */
EXPORT int EasyLua_Screen_Width(void);
EXPORT int EasyLua_Screen_Height(void);

/* 把 x2/y2 == 0 解析成屏幕边界。
 * Lua 端 API 约定（runtime.lua 注释里写的）："x2 / y2 = 0 表示用屏幕边界"，
 * 用户写 FindColor(0,0,0,0,...) / CaptureScreen() 时全屏作用。
 * 校验不能直接 reject (x2 <= x1)，得先把 0 替换掉。
 *
 * 副作用：必须等首帧才能拿到屏幕宽高（_Width/_Height 内部 condvar 会等）。
 *        屏幕 still 0 → 视频流挂了，让上层报错。
 */
static inline void resolve_region(int *x1, int *y1, int *x2, int *y2)
{
    int w = EasyLua_Screen_Width();
    int h = EasyLua_Screen_Height();
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 <= 0 || *x2 > w) *x2 = w;
    if (*y2 <= 0 || *y2 > h) *y2 = h;
}

/* ========================================================================== */
/*  全局帧缓存（紧凑 RGBA8888，width*4 stride，共享给 images.c）                */
/*                                                                            */
/*  Stage H：来自 java ImageReader plane 的 ByteBuffer (经一次 memcpy)         */
/*  Stage I：来自 NDK AHardwareBuffer_lock 的 dma-buf 直读 (经一次 memcpy)     */
/*                                                                            */
/*  保留紧凑布局是为了让 Lua 端的 Screen.Pixel(x, y) / Images.FindColor 都能   */
/*  按 (y * w + x) * 4 计算偏移，无需关心 row stride 对齐。                    */
/* ========================================================================== */

static pthread_mutex_t g_frame_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_frame_cv;   /* 在 frame_cv_init() 里以 MONOTONIC 时钟初始化 */
static pthread_once_t  g_frame_cv_once = PTHREAD_ONCE_INIT;
static uint8_t        *g_frame_data = NULL;
static int             g_frame_w = 0;
static int             g_frame_h = 0;
static int             g_frame_cap = 0;
static uint64_t        g_frame_seq = 0;

static void frame_cv_init(void)
{
    /* 用 CLOCK_MONOTONIC，避免系统时间被回调时 timedwait 永远阻塞。
     * bionic 自 API 21 起支持 pthread_condattr_setclock。 */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_frame_cv, &attr);
    pthread_condattr_destroy(&attr);
}

static void frame_store(int w, int h, const uint8_t *buf, int row_stride)
{
    pthread_once(&g_frame_cv_once, frame_cv_init);
    int packed = w * h * 4;
    pthread_mutex_lock(&g_frame_mu);
    if (packed > g_frame_cap) {
        free(g_frame_data);
        g_frame_data = (uint8_t *)malloc(packed);
        g_frame_cap = packed;
    }
    g_frame_w = w;
    g_frame_h = h;
    if (row_stride == w * 4) {
        memcpy(g_frame_data, buf, packed);
    } else {
        for (int y = 0; y < h; y++) {
            memcpy(g_frame_data + y * w * 4, buf + y * row_stride, w * 4);
        }
    }
    g_frame_seq++;
    pthread_cond_broadcast(&g_frame_cv);   /* 唤醒所有等首帧的读屏 API */
    pthread_mutex_unlock(&g_frame_mu);
}

/* 给 images.c 用的同步访问接口。
 *
 * 行为：
 *   - 当前已有帧（g_frame_data != NULL）：立即持锁返回，零延迟。
 *   - 视频流还没第一帧：condvar 阻塞最多 3 秒等首帧；超时返回 NULL。
 *
 * 所有读屏 API（Pixel / Find* / Cmp* 等）都走这条路径，所以脚本不需要
 * 在开头显式 WaitFrame，"启动后第一次找色"会自动 block 到首帧 ready。 */
/* 内部：持锁等首帧到达，最多 timeout_ms 毫秒。返回 0 = 已有帧，<0 = 超时。
 * 调用方必须已经持有 g_frame_mu。timeout_ms <= 0 表示用默认 3000。 */
static int frame_wait_first_locked(int timeout_ms)
{
    if (g_frame_data != NULL) return 0;
    if (timeout_ms <= 0) timeout_ms = 3000;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec  += 1;
        ts.tv_nsec -= 1000000000L;
    }
    while (g_frame_data == NULL) {
        int rc = pthread_cond_timedwait(&g_frame_cv, &g_frame_mu, &ts);
        if (rc == ETIMEDOUT) return -1;
    }
    return 0;
}

/* 给 images.c 用的同步访问接口。
 *
 * 行为：
 *   - 视频流首帧未到时，condvar 阻塞最多 3 秒；超时返回 NULL（视频流挂了）。
 *   - 已有帧后立即持锁返回，无延迟。
 *
 * 所有读屏 API（Pixel / Find* / Cmp* 等）都走这条路径，所以脚本不需要
 * 在开头显式等帧——首次调用会自动 block 到首帧 ready。 */
EXPORT const uint8_t *EasyLua_FrameDataLocked(int *out_w, int *out_h)
{
    pthread_once(&g_frame_cv_once, frame_cv_init);
    pthread_mutex_lock(&g_frame_mu);
    frame_wait_first_locked(0);   /* 首帧没到则等最多 3 秒 */

    if (out_w) *out_w = g_frame_w;
    if (out_h) *out_h = g_frame_h;
    /* 调用方必须紧接着 EasyLua_FrameUnlock。指针未拷贝，零开销。 */
    return g_frame_data;
}

EXPORT void EasyLua_FrameUnlock(void)
{
    pthread_mutex_unlock(&g_frame_mu);
}

/* ========================================================================== */
/*  Screen 命名空间                                                           */
/* ========================================================================== */

EXPORT int EasyLua_Screen_Width(void)
{
    pthread_once(&g_frame_cv_once, frame_cv_init);
    pthread_mutex_lock(&g_frame_mu);
    frame_wait_first_locked(0);
    int v = g_frame_w;
    pthread_mutex_unlock(&g_frame_mu);
    return v;
}

EXPORT int EasyLua_Screen_Height(void)
{
    pthread_once(&g_frame_cv_once, frame_cv_init);
    pthread_mutex_lock(&g_frame_mu);
    frame_wait_first_locked(0);
    int v = g_frame_h;
    pthread_mutex_unlock(&g_frame_mu);
    return v;
}

EXPORT uint64_t EasyLua_Screen_Seq(void)
{
    pthread_mutex_lock(&g_frame_mu);
    uint64_t v = g_frame_seq;
    pthread_mutex_unlock(&g_frame_mu);
    return v;
}

/* 给 Lua FFI 直读用，配合 Screen_Width/Height/Seq 使用。
 * 警告：返回指针在下一帧到达时可能被 free/realloc，调用方在一帧周期内用完。 */
EXPORT const uint8_t *EasyLua_Screen_Data(void)
{
    pthread_mutex_lock(&g_frame_mu);
    const uint8_t *p = g_frame_data;
    pthread_mutex_unlock(&g_frame_mu);
    return p;
}

/* 取 0xRRGGBB 单像素颜色，越界返回 0 */
EXPORT int EasyLua_Screen_Pixel(int x, int y)
{
    return Images_Pixel(x, y);
}

/* ========================================================================== */
/*  Images 命名空间（找色 / 比色）                                            */
/* ========================================================================== */

EXPORT int EasyLua_Images_CmpColor(int x, int y, const char *color, float sim)
{
    if (!color || !*color) { EL_ERR("CmpColor: color must not be null/empty"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("CmpColor: sim=%g out of [0,1]", sim); return -1; }
    return Images_CmpColor(x, y, color, sim);
}

EXPORT int EasyLua_Images_FindColor(int x1, int y1, int x2, int y2,
                                    const char *color, float sim, int dir,
                                    int *out_x, int *out_y)
{
    if (!color || !*color) { EL_ERR("FindColor: color must not be null/empty"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindColor: sim=%g out of [0,1]", sim); return -1; }
    if (!out_x || !out_y)  { EL_ERR("FindColor: out_x/out_y must not be null"); return -1; }
    resolve_region(&x1, &y1, &x2, &y2);
    if (x2 <= x1 || y2 <= y1) {
        EL_ERR("FindColor: 区域非法 (%d,%d)-(%d,%d)（屏幕未就绪？）", x1, y1, x2, y2);
        return -1;
    }
    return Images_FindColor(x1, y1, x2, y2, color, sim, dir, out_x, out_y);
}

EXPORT int EasyLua_Images_GetColorCountInRegion(int x1, int y1, int x2, int y2,
                                                const char *color, float sim)
{
    if (!color || !*color) { EL_ERR("GetColorCountInRegion: color must not be null/empty"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("GetColorCountInRegion: sim=%g out of [0,1]", sim); return -1; }
    resolve_region(&x1, &y1, &x2, &y2);
    if (x2 <= x1 || y2 <= y1) {
        EL_ERR("GetColorCountInRegion: 区域非法 (%d,%d)-(%d,%d)", x1, y1, x2, y2);
        return -1;
    }
    return Images_GetColorCountInRegion(x1, y1, x2, y2, color, sim);
}

EXPORT int EasyLua_Images_DetectsMultiColors(const char *colors, float sim)
{
    if (!colors || !*colors) { EL_ERR("DetectsMultiColors: colors must not be null/empty"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("DetectsMultiColors: sim=%g out of [0,1]", sim); return -1; }
    return Images_DetectsMultiColors(colors, sim);
}

EXPORT int EasyLua_Images_FindMultiColors(int x1, int y1, int x2, int y2,
                                          const char *colors, float sim, int dir,
                                          int *out_x, int *out_y)
{
    if (!colors || !*colors) { EL_ERR("FindMultiColors: colors must not be null/empty"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindMultiColors: sim=%g out of [0,1]", sim); return -1; }
    if (!out_x || !out_y)  { EL_ERR("FindMultiColors: out_x/out_y must not be null"); return -1; }
    resolve_region(&x1, &y1, &x2, &y2);
    if (x2 <= x1 || y2 <= y1) {
        EL_ERR("FindMultiColors: 区域非法 (%d,%d)-(%d,%d)", x1, y1, x2, y2);
        return -1;
    }
    return Images_FindMultiColors(x1, y1, x2, y2, colors, sim, dir, out_x, out_y);
}

EXPORT int EasyLua_Images_FindMultiColorsAll(int x1, int y1, int x2, int y2,
                                             const char *colors, float sim, int dir,
                                             int *out_xy, int max_n)
{
    if (!colors || !*colors) { EL_ERR("FindMultiColorsAll: colors must not be null/empty"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindMultiColorsAll: sim=%g out of [0,1]", sim); return -1; }
    if (!out_xy)              { EL_ERR("FindMultiColorsAll: out_xy must not be null"); return -1; }
    if (max_n <= 0)           { EL_ERR("FindMultiColorsAll: max_n=%d must be > 0", max_n); return -1; }
    resolve_region(&x1, &y1, &x2, &y2);
    if (x2 <= x1 || y2 <= y1) {
        EL_ERR("FindMultiColorsAll: 区域非法 (%d,%d)-(%d,%d)", x1, y1, x2, y2);
        return -1;
    }
    return Images_FindMultiColorsAll(x1, y1, x2, y2, colors, sim, dir, out_xy, max_n);
}

/* ========================================================================== */
/*  Images 命名空间（找图 FindPic / FindPicAll）                              */
/*                                                                            */
/*  与 FindColor / FindMultiColors 系列的差别：                               */
/*    - 模板生命周期独立：先 LoadTemplate 一次拿到 EasyLuaTemplate*，         */
/*      再用同一模板多次 FindPic / FindPicAll；Lua 端通过 cdata + __gc        */
/*      自动释放。                                                            */
/*    - 透明像素（alpha < 128）与四角 color key 的剔除在 Images_LoadTemplate  */
/*      内部一次完成，wrapper 不再重复校验图像内容。                          */
/*    - 区域 normalize_rect 与"扫描区装不下模板"的判定均在 Images_FindPic /  */
/*      Images_FindPicAll 内部处理（含等首帧）；wrapper 仅做指针 / sim 范围 / */
/*      max_n 等参数级 sanity 校验，避免双层语义割裂。                        */
/* ========================================================================== */

EXPORT EasyLuaTemplate *EasyLua_Images_LoadTemplate(const uint8_t *bytes, int len,
                                                    const char *path_hint)
{
    if (!bytes || len <= 0) {
        EL_ERR("LoadTemplate: bytes/len 非法 (bytes=%p, len=%d)",
               (const void *)bytes, len);
        return NULL;
    }
    /* path_hint 允许为 NULL（仅用于错误日志），底层会自行处理 */
    return Images_LoadTemplate(bytes, len, path_hint);
}

EXPORT void EasyLua_Images_FreeTemplate(EasyLuaTemplate *t)
{
    /* 底层对 t == NULL 已是 no-op；保留显式转发以便后续替换实现 */
    Images_FreeTemplate(t);
}

EXPORT int EasyLua_Images_TemplateW(const EasyLuaTemplate *t)
{
    if (!t) { EL_ERR("TemplateW: template must not be null"); return 0; }
    return Images_TemplateW(t);
}

EXPORT int EasyLua_Images_TemplateH(const EasyLuaTemplate *t)
{
    if (!t) { EL_ERR("TemplateH: template must not be null"); return 0; }
    return Images_TemplateH(t);
}

EXPORT int EasyLua_Images_TemplateValidPx(const EasyLuaTemplate *t)
{
    if (!t) { EL_ERR("TemplateValidPx: template must not be null"); return 0; }
    return Images_TemplateValidPx(t);
}

EXPORT int EasyLua_Images_FindPic(int x1, int y1, int x2, int y2,
                                  const EasyLuaTemplate *t, float sim, int dir,
                                  int *out_x, int *out_y)
{
    if (!t)                       { EL_ERR("FindPic: template must not be null"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindPic: sim=%g out of [0,1]", sim); return -1; }
    if (!out_x || !out_y)         { EL_ERR("FindPic: out_x/out_y must not be null"); return -1; }
    /* 注意：dir 范围、区域 normalize、模板装不下判定均由 Images_FindPic 内部完成 */
    return Images_FindPic(x1, y1, x2, y2, t, sim, dir, out_x, out_y);
}

EXPORT int EasyLua_Images_FindPicAll(int x1, int y1, int x2, int y2,
                                     const EasyLuaTemplate *t, float sim,
                                     int *out_xy, int max_n)
{
    if (!t)                       { EL_ERR("FindPicAll: template must not be null"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindPicAll: sim=%g out of [0,1]", sim); return -1; }
    if (!out_xy)                  { EL_ERR("FindPicAll: out_xy must not be null"); return -1; }
    if (max_n <= 0)               { EL_ERR("FindPicAll: max_n=%d must be > 0", max_n); return -1; }
    /* 区域 normalize 与模板装不下判定均由 Images_FindPicAll 内部完成 */
    return Images_FindPicAll(x1, y1, x2, y2, t, sim, out_xy, max_n);
}

/* 色差模式：FindPic / FindPicAll 的大漠经典签名变体。
 * 单像素判定走 |Δr|<=dr && |Δg|<=dg && |Δb|<=db；命中规则按通过比例 sim。 */
EXPORT int EasyLua_Images_FindPicDelta(int x1, int y1, int x2, int y2,
                                       const EasyLuaTemplate *t,
                                       int dr, int dg, int db,
                                       float sim, int dir,
                                       int *out_x, int *out_y)
{
    if (!t)                       { EL_ERR("FindPicDelta: template must not be null"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindPicDelta: sim=%g out of [0,1]", sim); return -1; }
    if (!out_x || !out_y)         { EL_ERR("FindPicDelta: out_x/out_y must not be null"); return -1; }
    return Images_FindPicDelta(x1, y1, x2, y2, t, dr, dg, db, sim, dir, out_x, out_y);
}

EXPORT int EasyLua_Images_FindPicAllDelta(int x1, int y1, int x2, int y2,
                                          const EasyLuaTemplate *t,
                                          int dr, int dg, int db,
                                          float sim,
                                          int *out_xy, int max_n)
{
    if (!t)                       { EL_ERR("FindPicAllDelta: template must not be null"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindPicAllDelta: sim=%g out of [0,1]", sim); return -1; }
    if (!out_xy)                  { EL_ERR("FindPicAllDelta: out_xy must not be null"); return -1; }
    if (max_n <= 0)               { EL_ERR("FindPicAllDelta: max_n=%d must be > 0", max_n); return -1; }
    return Images_FindPicAllDelta(x1, y1, x2, y2, t, dr, dg, db, sim, out_xy, max_n);
}

/* 多模板找图（"A.png|B.png" 格式 ⇒ Lua 层先把每个文件名 LoadTemplate 后
 * 把 cdata 数组传进来；C 内单次扫描，命中任一即返回）。 */
EXPORT int EasyLua_Images_FindPicMulti(int x1, int y1, int x2, int y2,
                                       const EasyLuaTemplate * const *tpls,
                                       int n_tpls,
                                       float sim, int dir,
                                       int *out_x, int *out_y, int *out_idx)
{
    if (!tpls || n_tpls <= 0)     { EL_ERR("FindPicMulti: tpls/n_tpls invalid"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindPicMulti: sim=%g out of [0,1]", sim); return -1; }
    if (!out_x || !out_y)         { EL_ERR("FindPicMulti: out_x/out_y must not be null"); return -1; }
    return Images_FindPicMulti(x1, y1, x2, y2, tpls, n_tpls, sim, dir, out_x, out_y, out_idx);
}

EXPORT int EasyLua_Images_FindPicAllMulti(int x1, int y1, int x2, int y2,
                                          const EasyLuaTemplate * const *tpls,
                                          int n_tpls,
                                          float sim,
                                          int *out_xy, int *out_idxs, int max_n)
{
    if (!tpls || n_tpls <= 0)     { EL_ERR("FindPicAllMulti: tpls/n_tpls invalid"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindPicAllMulti: sim=%g out of [0,1]", sim); return -1; }
    if (!out_xy || !out_idxs)     { EL_ERR("FindPicAllMulti: out arrays must not be null"); return -1; }
    if (max_n <= 0)               { EL_ERR("FindPicAllMulti: max_n=%d must be > 0", max_n); return -1; }
    return Images_FindPicAllMulti(x1, y1, x2, y2, tpls, n_tpls, sim, out_xy, out_idxs, max_n);
}

EXPORT int EasyLua_Images_FindPicMultiDelta(int x1, int y1, int x2, int y2,
                                            const EasyLuaTemplate * const *tpls,
                                            int n_tpls,
                                            int dr, int dg, int db,
                                            float sim, int dir,
                                            int *out_x, int *out_y, int *out_idx)
{
    if (!tpls || n_tpls <= 0)     { EL_ERR("FindPicMultiDelta: tpls/n_tpls invalid"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindPicMultiDelta: sim=%g out of [0,1]", sim); return -1; }
    if (!out_x || !out_y)         { EL_ERR("FindPicMultiDelta: out_x/out_y must not be null"); return -1; }
    return Images_FindPicMultiDelta(x1, y1, x2, y2, tpls, n_tpls,
                                    dr, dg, db, sim, dir, out_x, out_y, out_idx);
}

EXPORT int EasyLua_Images_FindPicAllMultiDelta(int x1, int y1, int x2, int y2,
                                               const EasyLuaTemplate * const *tpls,
                                               int n_tpls,
                                               int dr, int dg, int db,
                                               float sim,
                                               int *out_xy, int *out_idxs, int max_n)
{
    if (!tpls || n_tpls <= 0)     { EL_ERR("FindPicAllMultiDelta: tpls/n_tpls invalid"); return -1; }
    if (sim < 0.0f || sim > 1.0f) { EL_ERR("FindPicAllMultiDelta: sim=%g out of [0,1]", sim); return -1; }
    if (!out_xy || !out_idxs)     { EL_ERR("FindPicAllMultiDelta: out arrays must not be null"); return -1; }
    if (max_n <= 0)               { EL_ERR("FindPicAllMultiDelta: max_n=%d must be > 0", max_n); return -1; }
    return Images_FindPicAllMultiDelta(x1, y1, x2, y2, tpls, n_tpls,
                                       dr, dg, db, sim, out_xy, out_idxs, max_n);
}

/* Screen.CaptureScreen 系列：返回独立帧快照。Lua 端持有 cdata，__gc 时调 CaptureFree */
EXPORT EasyLuaSnapshot *EasyLua_Screen_Capture(int x1, int y1, int x2, int y2)
{
    resolve_region(&x1, &y1, &x2, &y2);
    if (x2 <= x1 || y2 <= y1) {
        EL_ERR("Screen_Capture: 区域非法 (%d,%d)-(%d,%d)（屏幕未就绪？）", x1, y1, x2, y2);
        return NULL;
    }
    return Images_Capture(x1, y1, x2, y2);
}

EXPORT void EasyLua_Screen_CaptureFree(EasyLuaSnapshot *s)
{
    if (!s) return;
    Images_CaptureFree(s);
}

EXPORT int EasyLua_Screen_SnapshotPixel(const EasyLuaSnapshot *s, int x, int y)
{
    if (!s) { EL_ERR("Screen_SnapshotPixel: snapshot must not be null"); return -1; }
    return Images_SnapshotPixel(s, x, y);
}

EXPORT int EasyLua_Screen_SnapshotSave(const EasyLuaSnapshot *s, const char *path,
                                       const char *type, int quality)
{
    if (!s)                 { EL_ERR("Screen_SnapshotSave: snapshot must not be null"); return -1; }
    if (!path || !*path)    { EL_ERR("Screen_SnapshotSave: path must not be null/empty"); return -1; }
    /* type 允许 null/空：Images_SnapshotSave 内部会按 path 后缀推断（.png/.jpg/.jpeg/.bmp） */
    /* quality 允许 0：Images_SnapshotSave 内部会取默认 90；负数仍然报错 */
    if (quality < 0 || quality > 100) { EL_ERR("Screen_SnapshotSave: quality=%d out of [0,100]", quality); return -1; }
    return Images_SnapshotSave(s, path, type ? type : "", quality);
}

/* ========================================================================== */
/*  Motion 命名空间                                                           */
/* ========================================================================== */

EXPORT int EasyLua_Motion_Init(int max_x, int max_y)
{
    if (max_x <= 0 || max_y <= 0) {
        EL_ERR("Motion_Init: max_x=%d max_y=%d must be > 0", max_x, max_y);
        return -1;
    }
    return touch_init(max_x, max_y);
}

EXPORT int EasyLua_Motion_Down(int x, int y, int slot, int orientation)
{
    if (slot < 0 || slot > 9) { EL_ERR("Motion_Down: slot=%d out of [0,9]", slot); return -1; }
    return touch_down(x, y, slot, orientation);
}

EXPORT int EasyLua_Motion_Move(int x, int y, int slot, int orientation)
{
    if (slot < 0 || slot > 9) { EL_ERR("Motion_Move: slot=%d out of [0,9]", slot); return -1; }
    return touch_move(x, y, slot, orientation);
}

EXPORT int EasyLua_Motion_Up(int slot)
{
    if (slot < 0 || slot > 9) { EL_ERR("Motion_Up: slot=%d out of [0,9]", slot); return -1; }
    return touch_up(slot);
}

EXPORT int EasyLua_Motion_Swipe(int x1, int y1, int x2, int y2,
                                int duration_ms, int slot, int orientation)
{
    if (slot < 0 || slot > 9)   { EL_ERR("Motion_Swipe: slot=%d out of [0,9]", slot); return -1; }
    if (duration_ms < 0)        { EL_ERR("Motion_Swipe: duration_ms=%d must be >= 0", duration_ms); return -1; }
    return touch_swipe(x1, y1, x2, y2, duration_ms, slot, orientation);
}

EXPORT int EasyLua_Motion_SwipeBezier(int x1, int y1, int x2, int y2,
                                      int duration_ms, int slot, int orientation)
{
    if (slot < 0 || slot > 9)   { EL_ERR("Motion_SwipeBezier: slot=%d out of [0,9]", slot); return -1; }
    if (duration_ms < 0)        { EL_ERR("Motion_SwipeBezier: duration_ms=%d must be >= 0", duration_ms); return -1; }
    return touch_swipe_bezier(x1, y1, x2, y2, duration_ms, slot, orientation);
}

EXPORT void EasyLua_Motion_Cleanup(void)
{
    touch_cleanup();
}

/* ========================================================================== */
/*  Ui 命名空间（自绘 Toast + Highlight，绕开 TextView，MIUI 也能跑）         */
/* ========================================================================== */

EXPORT int EasyLua_Ui_Toast(const char *msg, int x, int y, int dur_ms)
{
    if (!msg || !*msg)  { EL_ERR("Ui_Toast: msg must not be null/empty"); return -1; }
    /* x/y = -1 表示自动居中/居底（runtime.lua 默认值），不视为错误 */
    /* dur_ms <= 0 表示常驻或瞬间消失，由 Ui_Toast 内部决定语义 */
    return Ui_Toast(msg, x, y, dur_ms);
}

EXPORT int EasyLua_Ui_Highlight(int x, int y, int w, int h,
                                int color_argb, int dur_ms, const char *label)
{
    if (w <= 0 || h <= 0) { EL_ERR("Ui_Highlight: w=%d h=%d must be > 0", w, h); return -1; }
    /* dur_ms <= 0 表示常驻（runtime.lua 文档明说），不视为错误 */
    /* label 允许 null（不显示文字） */
    return Ui_Highlight(x, y, w, h, color_argb, dur_ms, label);
}

EXPORT int EasyLua_Ui_HighlightOff(void)
{
    return Ui_HighlightOff();
}

/* ========================================================================== */
/*  Utils 命名空间                                                            */
/* ========================================================================== */

EXPORT void EasyLua_Utils_Sleep(int ms)
{
    if (ms > 0) usleep((useconds_t)ms * 1000);
}

EXPORT int64_t EasyLua_Utils_NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

EXPORT int64_t EasyLua_Utils_NowUs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

EXPORT int EasyLua_Utils_Log(const char *msg)
{
    if (!msg) { EL_ERR("Utils_Log: msg must not be null"); return -1; }
    fprintf(stdout, "[lua] %s\n", msg);
    fflush(stdout);
    return 0;
}

/* ========================================================================== */
/*  App / Device / IME 命名空间（misc.c 实现）                                */
/* ========================================================================== */

/* 防御小工具：判 buf/n 与 string 参数；返回 -1 表示已打印错误并应当返回 */
#define CHECK_BUF(fn, buf, n) do { \
    if (!(buf)) { EL_ERR(fn ": buf must not be null"); return -1; } \
    if ((n) <= 0) { EL_ERR(fn ": n=%d must be > 0", (n)); return -1; } \
} while (0)
#define CHECK_STR(fn, name, s) do { \
    if (!(s) || !*(s)) { EL_ERR(fn ": " name " must not be null/empty"); return -1; } \
} while (0)

EXPORT int  EasyLua_App_CurrentPackage(char *buf, int n)  { CHECK_BUF("App_CurrentPackage", buf, n); return App_CurrentPackage(buf, n); }
EXPORT int  EasyLua_App_CurrentActivity(char *buf, int n) { CHECK_BUF("App_CurrentActivity", buf, n); return App_CurrentActivity(buf, n); }
EXPORT int  EasyLua_App_Launch(const char *pkg)           { CHECK_STR("App_Launch", "pkg", pkg); return App_Launch(pkg); }
EXPORT int  EasyLua_App_IsInstalled(const char *pkg)      { CHECK_STR("App_IsInstalled", "pkg", pkg); return App_IsInstalled(pkg); }
EXPORT int  EasyLua_App_ForceStop(const char *pkg)        { CHECK_STR("App_ForceStop", "pkg", pkg); return App_ForceStop(pkg); }
EXPORT int  EasyLua_App_Clear(const char *pkg)            { CHECK_STR("App_Clear", "pkg", pkg); return App_Clear(pkg); }
EXPORT int  EasyLua_App_OpenUrl(const char *url)          { CHECK_STR("App_OpenUrl", "url", url); return App_OpenUrl(url); }

EXPORT int  EasyLua_Device_IsScreenOn(void)        { return Device_IsScreenOn(); }
EXPORT int  EasyLua_Device_IsScreenUnlock(void)    { return Device_IsScreenUnlock(); }
EXPORT void EasyLua_Device_WakeUp(void)            { Device_WakeUp(); }
EXPORT void EasyLua_Device_Sleep(void)             { Device_Sleep(); }
EXPORT int  EasyLua_Device_GetBattery(void)        { return Device_GetBattery(); }
EXPORT int  EasyLua_Device_GetBatteryStatus(void)  { return Device_GetBatteryStatus(); }
EXPORT void EasyLua_Device_Vibrate(int ms)         {
    if (ms < 0) { EL_ERR("Device_Vibrate: ms=%d must be >= 0", ms); return; }
    Device_Vibrate(ms);
}
EXPORT int  EasyLua_Device_GetSdkInt(void)         { return Device_GetSdkInt(); }
EXPORT int  EasyLua_Device_GetBrand(char *b, int n) { CHECK_BUF("Device_GetBrand", b, n); return Device_GetBrand(b, n); }
EXPORT int  EasyLua_Device_GetModel(char *b, int n) { CHECK_BUF("Device_GetModel", b, n); return Device_GetModel(b, n); }

EXPORT int  EasyLua_IME_GetClipText(char *b, int n) { CHECK_BUF("IME_GetClipText", b, n); return IME_GetClipText(b, n); }
EXPORT int  EasyLua_IME_SetClipText(const char *t)  { CHECK_STR("IME_SetClipText", "text", t); return IME_SetClipText(t); }
EXPORT int  EasyLua_IME_InputText(const char *t)    { CHECK_STR("IME_InputText", "text", t); return IME_InputText(t); }
EXPORT int  EasyLua_IME_KeyAction(int keycode)      {
    /* keycode 0 是 Lua 端调用方默认值（runtime.lua: KeyAction(keycode or 0)），
     * Android KeyEvent.KEYCODE_UNKNOWN = 0；让底层去判断而不是这里截断 */
    return IME_KeyAction(keycode);
}

EXPORT int  EasyLua_Shell_Exec(const char *cmd, char *buf, int n) {
    CHECK_STR("Shell_Exec", "cmd", cmd);
    CHECK_BUF("Shell_Exec", buf, n);
    return Shell_Exec(cmd, buf, n);
}

/* ========================================================================== */
/*  Net 命名空间（net.c 实现）                                                */
/*                                                                            */
/*  fd 由 native 分配，Lua 端用 Tcp:Close() / GC 释放，重复关闭安全。         */
/* ========================================================================== */

EXPORT int EasyLua_Net_TcpConnect(const char *host, int port, int timeout_ms)
{
    CHECK_STR("Net_TcpConnect", "host", host);
    if (port <= 0 || port > 65535) {
        EL_ERR("Net_TcpConnect: port=%d out of (0,65535]", port);
        return -1;
    }
    return Net_TcpConnect(host, port, timeout_ms);
}

EXPORT int EasyLua_Net_TcpSend(int fd, const char *buf, int len, int timeout_ms)
{
    if (fd < 0) { EL_ERR("Net_TcpSend: fd=%d invalid", fd); return -1; }
    if (!buf)   { EL_ERR("Net_TcpSend: buf must not be null"); return -1; }
    if (len < 0){ EL_ERR("Net_TcpSend: len=%d must be >= 0", len); return -1; }
    return Net_TcpSend(fd, buf, len, timeout_ms);
}

EXPORT int EasyLua_Net_TcpRecv(int fd, char *buf, int cap, int timeout_ms)
{
    if (fd < 0)  { EL_ERR("Net_TcpRecv: fd=%d invalid", fd); return -1; }
    CHECK_BUF("Net_TcpRecv", buf, cap);
    return Net_TcpRecv(fd, buf, cap, timeout_ms);
}

EXPORT int EasyLua_Net_TcpSetNoDelay(int fd, int on)
{
    if (fd < 0) { EL_ERR("Net_TcpSetNoDelay: fd=%d invalid", fd); return -1; }
    return Net_TcpSetNoDelay(fd, on);
}

EXPORT int EasyLua_Net_UdpOpen(void)
{
    return Net_UdpOpen();
}

EXPORT int EasyLua_Net_UdpSendTo(int fd, const char *host, int port,
                                 const char *buf, int len)
{
    if (fd < 0) { EL_ERR("Net_UdpSendTo: fd=%d invalid", fd); return -1; }
    CHECK_STR("Net_UdpSendTo", "host", host);
    if (port <= 0 || port > 65535) {
        EL_ERR("Net_UdpSendTo: port=%d out of (0,65535]", port);
        return -1;
    }
    if (!buf)   { EL_ERR("Net_UdpSendTo: buf must not be null"); return -1; }
    if (len < 0){ EL_ERR("Net_UdpSendTo: len=%d must be >= 0", len); return -1; }
    return Net_UdpSendTo(fd, host, port, buf, len);
}

EXPORT int EasyLua_Net_UdpRecvFrom(int fd, char *buf, int cap, int timeout_ms,
                                   char *out_host, int out_n, int *out_port)
{
    if (fd < 0) { EL_ERR("Net_UdpRecvFrom: fd=%d invalid", fd); return -1; }
    CHECK_BUF("Net_UdpRecvFrom", buf, cap);
    /* out_host / out_port 允许 NULL（Lua 端不关心源地址） */
    return Net_UdpRecvFrom(fd, buf, cap, timeout_ms, out_host, out_n, out_port);
}

EXPORT void EasyLua_Net_Close(int fd)
{
    Net_Close(fd);
}

EXPORT int EasyLua_Net_SetTimeout(int fd, int recv_ms, int send_ms)
{
    if (fd < 0) { EL_ERR("Net_SetTimeout: fd=%d invalid", fd); return -1; }
    return Net_SetTimeout(fd, recv_ms, send_ms);
}

EXPORT int EasyLua_Net_DnsResolve(const char *host, char *out, int n)
{
    CHECK_STR("Net_DnsResolve", "host", host);
    CHECK_BUF("Net_DnsResolve", out, n);
    return Net_DnsResolve(host, out, n);
}

EXPORT int EasyLua_Net_LocalIp(char *out, int n)
{
    CHECK_BUF("Net_LocalIp", out, n);
    return Net_LocalIp(out, n);
}

EXPORT int EasyLua_Net_HttpRequest(const char *method, const char *url,
                                   const char *headers,
                                   const char *body, int body_len,
                                   char *out_buf, int out_cap,
                                   int *out_status, int timeout_ms)
{
    /* method 允许 NULL（默认 GET）；headers/body 也允许 NULL */
    CHECK_STR("Net_HttpRequest", "url", url);
    CHECK_BUF("Net_HttpRequest", out_buf, out_cap);
    if (body && body_len < 0) {
        EL_ERR("Net_HttpRequest: body_len=%d must be >= 0", body_len);
        return -1;
    }
    return Net_HttpRequest(method, url, headers, body, body_len,
                           out_buf, out_cap, out_status, timeout_ms);
}

/* ========================================================================== */
/*  LuaJIT 引导                                                               */
/* ========================================================================== */

/* print 重定向：输出格式 [HH:MM:SS.mmm script.lua:42]  ...
 *   - 时间戳取 wallclock，精确到毫秒
 *   - 行号取调用 print 的脚本行（lua_getstack/getinfo level=1）
 * 仍然把 number/string 直接写出去，其它类型转 typename + ptr。
 */
static int lua_print_via_stdout(lua_State *L)
{
    /* 时间戳 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    int ms = (int)(ts.tv_nsec / 1000000);

    /* 行号 + 简短源文件名（去路径，留 basename）。
     * 跳到第一个不是 runtime.lua 的栈帧——runtime 里有 print wrapper（cjson 等增强），
     * 单层 getstack(1) 拿到的会是 wrapper 自己。*/
    const char *src = "?";
    int line = -1;
    lua_Debug ar;
    for (int level = 1; level < 8; level++) {
        if (!lua_getstack(L, level, &ar)) break;
        if (!lua_getinfo(L, "Sl", &ar)) break;
        const char *s = ar.source ? ar.source : "";
        if (s[0] == '@' || s[0] == '=') s++;
        const char *slash = strrchr(s, '/');
        const char *bs    = strrchr(s, '\\');
        if (bs && bs > slash) slash = bs;
        const char *base = slash ? slash + 1 : s;
        if (strcmp(base, "runtime.lua") == 0) continue;  /* 跳过 runtime 内部包装层 */
        line = ar.currentline;
        src  = base;
        break;
    }
    /* 找不到就回退到原来的 level=1 行为 */
    if (line < 0 && lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
        line = ar.currentline;
        if (ar.source && ar.source[0]) {
            const char *s = ar.source;
            if (s[0] == '@' || s[0] == '=') s++;
            const char *slash = strrchr(s, '/');
            const char *bs    = strrchr(s, '\\');
            if (bs && bs > slash) slash = bs;
            src = slash ? slash + 1 : s;
        }
    }

    fprintf(stdout, "[%02d:%02d:%02d.%03d %s:%d] ",
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms,
            src, line);

    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) fputc('\t', stdout);
        int t = lua_type(L, i);
        switch (t) {
        case LUA_TNIL:
            fputs("nil", stdout); break;
        case LUA_TBOOLEAN:
            fputs(lua_toboolean(L, i) ? "true" : "false", stdout); break;
        case LUA_TNUMBER:
        case LUA_TSTRING: {
            size_t len = 0;
            const char *s = lua_tolstring(L, i, &len);
            fwrite(s, 1, len, stdout);
            break;
        }
        default:
            fprintf(stdout, "%s: %p", lua_typename(L, t), lua_topointer(L, i));
            break;
        }
    }
    fputc('\n', stdout);
    fflush(stdout);
    return 0;
}

static int load_runtime(lua_State *L)
{
    if (luaL_loadbuffer(L, (const char *)runtime_lua_blob,
                        runtime_lua_blob_len, "=runtime.lua") != 0) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "[easylua-c] runtime load error: %s\n",
                err ? err : "(unknown)");
        return -1;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "[easylua-c] runtime exec error: %s\n",
                err ? err : "(unknown)");
        return -1;
    }
    return 0;
}

/* run_script 把脚本结果分类成三种 exitcode：
 *   0 主线程正常结束
 *   1 lua 错误（未捕获）
 *   2 主动 exitScript()
 *
 * 同时在结束时调用 runtime.lua 注册的 __easylua_dispatch_stop(isError, exitcode)，
 * 把 (error, exitcode) 透传给用户的 setStopCallback 回调。 */

/* 把 (status, exitcode_out) 拆开返回；调用方根据 exitcode 决定打印还是静默 */
/**
 * 计算用户脚本所在的 deploy 目录。
 *
 * 入参示例:
 *   /data/local/tmp/easyLua/scripts/main.luac
 *      ↓
 *   返回 "/data/local/tmp/easyLua"
 *
 * 规则:从 script_path 向上找包含 "scripts/" 的层,取它的父目录。
 * 找不到时返回 NULL,让调用方退化到不设 package.path 的默认行为。
 *
 * 输出指针由 caller 释放(strdup)。
 */
static char *derive_deploy_dir(const char *script_path)
{
    if (!script_path) return NULL;
    /* 找最后一个 "/scripts/" 子串 */
    const char *p = script_path;
    const char *last = NULL;
    const char *needle = "/scripts/";
    size_t needle_len = 9;
    for (const char *cur = strstr(p, needle); cur; cur = strstr(cur + 1, needle)) {
        last = cur;
    }
    if (!last) return NULL;
    /* deploy_dir = script_path[0 .. last] (包含 / 之前部分) */
    size_t len = (size_t)(last - script_path);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, script_path, len);
    out[len] = '\0';
    return out;
}

/**
 * 在 lua state 上设置 package.path / package.cpath,让 require 能找到
 * deploy_dir/scripts/ 下的用户脚本和模块。
 *
 * 这一步替代了之前的 _bootstrap.lua 文件——相同行为,但不需要落盘任何 lua 引导脚本。
 *
 * 设置完后,require("libs.foo") 会按以下优先级搜索:
 *   <deploy_dir>/scripts/?.luac
 *   <deploy_dir>/scripts/?.lua
 *   <deploy_dir>/scripts/?/init.luac
 *   <deploy_dir>/scripts/?/init.lua
 *   <deploy_dir>/scripts/libs/?.luac
 *   <deploy_dir>/scripts/libs/?.lua
 *   ...(等等)
 */
static void set_package_path(lua_State *L, const char *deploy_dir)
{
    if (!deploy_dir) return;

    /* 拼 package.path */
    char path_buf[2048];
    snprintf(path_buf, sizeof(path_buf),
        "%s/scripts/?.luac;%s/scripts/?.lua;"
        "%s/scripts/?/init.luac;%s/scripts/?/init.lua;"
        "%s/scripts/libs/?.luac;%s/scripts/libs/?.lua;"
        "%s/scripts/libs/?/init.luac;%s/scripts/libs/?/init.lua;"
        "%s/scripts/ui/?.luac;%s/scripts/ui/?.lua",
        deploy_dir, deploy_dir, deploy_dir, deploy_dir,
        deploy_dir, deploy_dir, deploy_dir, deploy_dir,
        deploy_dir, deploy_dir);

    /* 拼 package.cpath:让用户在 scripts/lib?.so 放 native 模块时能 require */
    char cpath_buf[512];
    snprintf(cpath_buf, sizeof(cpath_buf),
        "%s/scripts/?.so;%s/scripts/lib?.so",
        deploy_dir, deploy_dir);

    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushstring(L, path_buf);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, cpath_buf);
    lua_setfield(L, -2, "cpath");
    lua_pop(L, 1);

    /* 暴露给脚本调试用,与之前的 _easyLua_bootstrap 等价 */
    lua_newtable(L);
    lua_pushstring(L, path_buf);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, cpath_buf);
    lua_setfield(L, -2, "cpath");
    lua_pushstring(L, deploy_dir);
    lua_setfield(L, -2, "deployDir");
    lua_setglobal(L, "_easyLua_bootstrap");
}

static int run_script(const char *script_path)
{
    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "[easylua-c] luaL_newstate failed\n");
        return 1;
    }
    luaL_openlibs(L);

    /* 在 require 任何东西之前设好 package.path,替代 _bootstrap.lua */
    char *deploy_dir = derive_deploy_dir(script_path);
    if (deploy_dir) {
        set_package_path(L, deploy_dir);
        free(deploy_dir);
    }

    lua_pushcfunction(L, lua_print_via_stdout);
    lua_setglobal(L, "print");

    /* 内置 lfs（LuaFileSystem 1.9.0）：免 require，脚本里直接 lfs.xxx 即可。
     * 同时写 package.loaded["lfs"]，让遗留的 require("lfs") 也能拿到同一张表。
     * 这里手动展开（这个 LuaJIT backport 未提供 luaL_requiref）。 */
    lua_pushcfunction(L, luaopen_lfs);
    lua_call(L, 0, 1);                 /* lfs 表入栈 */
    lua_pushvalue(L, -1);
    lua_setglobal(L, "lfs");           /* 全局 lfs */
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "loaded");
        if (lua_istable(L, -1)) {
            lua_pushvalue(L, -3);
            lua_setfield(L, -2, "lfs");
        }
        lua_pop(L, 1);                 /* pop package.loaded */
    }
    lua_pop(L, 2);                     /* pop package, lfs 表 */

    if (load_runtime(L) != 0) {
        lua_close(L);
        return 1;
    }

    int exitcode = 0;          /* 0 正常 / 1 错误 / 2 exitScript */
    int is_error = 0;          /* 给 dispatch_stop 用 */

    /* 加载脚本文件，编译失败按 exitcode = 1 处理 */
    if (luaL_loadfile(L, script_path) != 0) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "[easylua-c] script load error: %s\n",
                err ? err : "(unknown)");
        lua_pop(L, 1);
        is_error = 1;
        exitcode = 1;
    } else {
        /* 用 lua_pcall 执行已加载的 chunk。需要识别 exit token 区分主动退出。 */
        if (lua_pcall(L, 0, 0, 0) != 0) {
            /* 出错。先看栈顶是不是 exit token。 */
            int top = lua_gettop(L);
            lua_getglobal(L, "__easylua_exit_token");
            int is_exit = lua_rawequal(L, top, top + 1);
            lua_pop(L, 1);   /* pop token */

            if (is_exit) {
                /* 主动 exitScript()：不打印错误，exitcode = 2 */
                lua_pop(L, 1);
                exitcode = 2;
            } else {
                /* 真实运行时错误 */
                const char *err = lua_tostring(L, -1);
                fprintf(stderr, "[easylua-c] script error: %s\n",
                        err ? err : "(unknown)");
                lua_pop(L, 1);
                is_error = 1;
                exitcode = 1;
            }
        }
    }

    /* 调 runtime.lua 的 stop dispatcher：__easylua_dispatch_stop(isError, exitcode) */
    lua_getglobal(L, "__easylua_dispatch_stop");
    if (lua_isfunction(L, -1)) {
        lua_pushboolean(L, is_error);
        lua_pushinteger(L, exitcode);
        if (lua_pcall(L, 2, 0, 0) != 0) {
            /* dispatcher 自身错了——不应当影响最终 exitcode，只打日志 */
            const char *err = lua_tostring(L, -1);
            fprintf(stderr, "[easylua-c] stop callback error: %s\n",
                    err ? err : "(unknown)");
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    lua_close(L);
    return exitcode;
}

/* ========================================================================== */
/*  JNI 接口                                                                  */
/* ========================================================================== */

JNIEXPORT jint JNICALL
Java_com_easylua_EasyLuaJni_nativeMain(JNIEnv *env, jclass clz, jstring jPath)
{
    /* 保存 JavaVM 指针，供 Ui_* 反向调 Java */
    JavaVM *jvm = NULL;
    if ((*env)->GetJavaVM(env, &jvm) == JNI_OK) {
        Ui_SetJvm(jvm);
    }

    const char *path = (*env)->GetStringUTFChars(env, jPath, NULL);
    int rc = run_script(path);
    (*env)->ReleaseStringUTFChars(env, jPath, path);
    return rc;
}

/* Stage H/scrcpy 兜底路径：从 java ImageReader plane 拷过来。
 * 当 HardwareBuffer 路径不可用（SDK<29 或 ROM 不支持 CPU_READ usage）时使用。 */
JNIEXPORT void JNICALL
Java_com_easylua_EasyLuaJni_nativeOnFrame(JNIEnv *env, jclass clz,
                                          jint w, jint h, jobject buf,
                                          jint rowStride)
{
    void *addr = (*env)->GetDirectBufferAddress(env, buf);
    if (!addr) return;
    frame_store((int)w, (int)h, (const uint8_t *)addr, (int)rowStride);
}

/* Stage I 首选路径：HardwareBuffer 直读 dma-buf（API 29+）。
 *
 * 把 java HardwareBuffer 转成 NDK AHardwareBuffer，lock 拿 CPU 地址，
 * memcpy 到紧凑缓冲，立即 unlock + release。整个调用同步完成，
 * 调用方可以立即 close java 端 hb 引用。
 *
 * 相比 nativeOnFrame：
 *   - 省掉 java ImageReader plane 拼装（getBuffer 内部要做 plane 检查 + ByteBuffer 包装）
 *   - 省掉 GetDirectBufferAddress 的 JNI 调用
 *   - dma-buf 直读，不经过 java heap
 *
 * 返回：
 *    0  成功
 *   -1  jhb 为空 / fromHardwareBuffer 失败
 *   -2  AHardwareBuffer_lock 失败（部分 ROM 不支持 CPU_READ）—— 调用方应降级
 */
JNIEXPORT jint JNICALL
Java_com_easylua_EasyLuaJni_nativeOnFrameHB(JNIEnv *env, jclass clz,
                                            jobject jhb, jint w, jint h)
{
    if (!jhb) return -1;
    AHardwareBuffer *hb = AHardwareBuffer_fromHardwareBuffer(env, jhb);
    if (!hb) return -1;

    /* 持有一份 hb 引用，确保我们 unlock 之前 java 端 hb.close() 不会回收 */
    AHardwareBuffer_acquire(hb);

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(hb, &desc);
    /* desc.stride 是像素数（不是字节数），RGBA8888 一像素 4 字节 */
    int row_stride_bytes = (int)desc.stride * 4;

    /* 性能采样：环境变量 EASYLUA_PERF=1 启用，每 60 帧打一次 */
    static int s_perf_enabled = -1;
    if (s_perf_enabled < 0) {
        const char *e = getenv("EASYLUA_PERF");
        s_perf_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    static int s_sample_counter = 0;
    int do_sample = s_perf_enabled && ((s_sample_counter++ % 60) == 0);

    void *addr = NULL;
    struct timespec t0, t1, t2;
    if (do_sample) clock_gettime(CLOCK_MONOTONIC, &t0);

    int rc = AHardwareBuffer_lock(hb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                                  -1 /*fence*/, NULL /*rect*/, &addr);

    if (do_sample) clock_gettime(CLOCK_MONOTONIC, &t1);

    jint result;
    if (rc == 0 && addr) {
        frame_store((int)w, (int)h, (const uint8_t *)addr, row_stride_bytes);
        if (do_sample) clock_gettime(CLOCK_MONOTONIC, &t2);
        AHardwareBuffer_unlock(hb, NULL);
        result = 0;

        if (do_sample) {
            long lock_us = (t1.tv_sec - t0.tv_sec) * 1000000L
                         + (t1.tv_nsec - t0.tv_nsec) / 1000L;
            long copy_us = (t2.tv_sec - t1.tv_sec) * 1000000L
                         + (t2.tv_nsec - t1.tv_nsec) / 1000L;
            fprintf(stdout, "[easylua-perf] frame %dx%d stride=%d  "
                            "lock=%ldus  read+memcpy=%ldus\n",
                    (int)w, (int)h, row_stride_bytes, lock_us, copy_us);
            fflush(stdout);
        }
    } else {
        fprintf(stderr, "[easylua] AHardwareBuffer_lock failed rc=%d\n", rc);
        result = -2;
    }

    AHardwareBuffer_release(hb);
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_easylua_EasyLuaJni_nativeHello(JNIEnv *env, jclass clz)
{
    return (*env)->NewStringUTF(env, "hello from libeasylua.so");
}
