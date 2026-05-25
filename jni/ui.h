/*
 * UI 命名空间 —— easyLua 在 root 进程内的两条 UI 路径
 *
 * 1) Highlight：JNI 反向调 com.easylua.EasyLuaJni.uiHighlight*
 *    -> Overlay (Canvas + Paint 自绘) 通过 WindowManager.addView 出图
 *    类型 TYPE_APPLICATION_OVERLAY (2038)，root + processName="android"
 *    + sFontScale hook 后在 MIUI 等定制 ROM 上能直接 attach。
 *
 * 2) Toast：直接在 native 调 `am broadcast` 触发 SampleUI APK 的
 *    ToastReceiver。原因是 MIUI WindowManagerService.addWindow 对
 *    TYPE_TOAST 强校验 caller package + 走 NotificationManagerService
 *    的 windowToken，root 进程拿不到合法 token，会 SecurityException。
 *    把这条路 offload 给一个普通 APK 解决，APK 不需要在前台。
 */

#ifndef EASYLUA_UI_H
#define EASYLUA_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jni.h>

/* 由 easylua.c 在 nativeMain 入口处保存，供 Highlight 反向调 Java 时使用 */
void Ui_SetJvm(JavaVM *jvm);

/* Toast：通过 am broadcast 让 SampleUI APK 弹 Toast。
 *   x / y 系统 Toast 不支持精确坐标，目前忽略，保留参数兼容 Lua 端签名。
 *   dur_ms < 3500 用 SHORT，>= 3500 用 LONG（Android 系统 Toast 只有这两档）。
 *   返回 0 = ok，<0 = 失败（SampleUI 未安装时 am broadcast 仍会成功，但 toast 不会出现）。
 */
int Ui_Toast(const char *msg, int x, int y, int dur_ms);

/* Highlight 矩形（SikuliX 风格）：
 *   - 空心矩形描边
 *   - 可选 label 在矩形上方
 *   - color_argb：0xAARRGGBB；0 表示默认红色 0xFFFF0000
 *   - dur_ms <= 0 表示常驻直到 Ui_HighlightOff
 */
int Ui_Highlight(int x, int y, int w, int h,
                 int color_argb, int dur_ms, const char *label);

/* 立即清掉所有 highlight（不影响 toast） */
int Ui_HighlightOff(void);

#ifdef __cplusplus
}
#endif
#endif
