/*
 * App / Device / IME 杂项实现
 *
 * 实现策略：
 *   - 简单功能用 popen / system 调 shell 命令（am / pm / dumpsys / input / cmd）
 *   - 字符串结果通过 char* out_buf 写回，调用方决定怎么 ffi.string 取
 *   - 不引 JNI（避免依赖 Java 端）—— 第一版纯 root shell 即可
 *
 * 注意：popen() 在我们这个进程（uid=0 root）下可以直接执行任何命令，
 * 不需要再用 SampleUI 那一套 IMyShellService binder 转发。
 */

#ifndef EASYLUA_MISC_H
#define EASYLUA_MISC_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Shell 通用工具 ---- */

/**
 * 同步执行 shell 命令并把 stdout 写入 out_buf。
 * 返回写入的字节数（不含 \0），-1 = popen 失败。
 * out_buf 容量不足时截断；buf[len] 总是写 \0。
 */
int Shell_Exec(const char *cmd, char *out_buf, int out_buf_size);

/* ---- App 命名空间 ---- */

int App_CurrentPackage(char *out_buf, int out_buf_size);   /* 长度 / -1 */
int App_CurrentActivity(char *out_buf, int out_buf_size);
int App_Launch(const char *pkg);                             /* 1=ok 0=fail */
int App_IsInstalled(const char *pkg);                        /* 1/0 */
int App_ForceStop(const char *pkg);                          /* 1/0 */
int App_Clear(const char *pkg);                              /* 1/0 */
int App_OpenUrl(const char *url);                            /* 1/0 */

/* ---- Device 命名空间 ---- */

int     Device_IsScreenOn(void);          /* 1/0 */
int     Device_IsScreenUnlock(void);      /* 1/0 */
void    Device_WakeUp(void);
void    Device_Sleep(void);                /* 锁屏，模拟电源键 */
int     Device_GetBattery(void);           /* 0..100，失败 -1 */
int     Device_GetBatteryStatus(void);     /* BatteryManager.BATTERY_STATUS_* */
void    Device_Vibrate(int ms);
int     Device_GetSdkInt(void);            /* getprop ro.build.version.sdk */
int     Device_GetBrand(char *out, int n); /* getprop ro.product.brand */
int     Device_GetModel(char *out, int n); /* getprop ro.product.model */

/* ---- IME 命名空间 ---- */

/**
 * 取剪贴板文字。需要 SampleUI APK 帮忙（剪贴板必须在 Java 端调，
 * shell 的 `cmd clipboard` 在 Android 10+ 已不能直接取数据）。
 * 第一版做法：通过 Bridge_Send* 模拟一个新命令，或者直接 shell `cmd clipboard get-primary`。
 *
 * 这里先用 `cmd clipboard get-primary` 的实验路径（部分 ROM 可用）。
 * 写入字节数 / -1 = 失败。
 */
int IME_GetClipText(char *out, int n);

/** 设置剪贴板：用 `cmd clipboard set-primary "..."`（部分 ROM 不可用，第一版尽力而为） */
int IME_SetClipText(const char *text);

/** 输入文本：`input text "xxx"` 适合 ASCII，不支持中文。 */
int IME_InputText(const char *text);

/** 按键事件：`input keyevent N` */
int IME_KeyAction(int keycode);

#ifdef __cplusplus
}
#endif
#endif
