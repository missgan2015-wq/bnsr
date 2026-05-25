/*
 * App / Device / IME 实现：纯 shell 命令路径，不依赖 JNI
 *
 * 性能注意：popen 每次会 fork+exec /system/bin/sh，开销 ~5-15ms。
 * 高频脚本调用要谨慎（不像 motion / images 是 native 直跑）。
 */

#include "misc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- shell 通用 ---- */

int Shell_Exec(const char *cmd, char *out_buf, int out_buf_size)
{
    if (out_buf && out_buf_size > 0) out_buf[0] = 0;
    if (!cmd) return -1;

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    int total = 0;
    if (out_buf && out_buf_size > 1) {
        int cap = out_buf_size - 1;
        while (total < cap) {
            int n = (int)fread(out_buf + total, 1, (size_t)(cap - total), fp);
            if (n <= 0) break;
            total += n;
        }
        out_buf[total] = 0;
    } else {
        /* 调用方不要输出，把 stdout 读完丢弃 */
        char tmp[256];
        while (fread(tmp, 1, sizeof(tmp), fp) > 0) {}
    }

    pclose(fp);
    /* 去掉末尾换行 */
    while (total > 0 && (out_buf[total - 1] == '\n' || out_buf[total - 1] == '\r')) {
        out_buf[--total] = 0;
    }
    return total;
}

/* 把命令的整行输出写到 out，返回长度 */
static int run_one(const char *cmd, char *out, int n)
{
    return Shell_Exec(cmd, out, n);
}

/* "包含某个子串" 简化判断 */
static int output_contains(const char *cmd, const char *needle)
{
    char buf[1024];
    if (Shell_Exec(cmd, buf, sizeof(buf)) < 0) return 0;
    return strstr(buf, needle) != NULL ? 1 : 0;
}

/* ---- App ---- */

/* 从 dumpsys window 输出里抽 mCurrentFocus 的 pkg/activity */
static void parse_focus(char *focus_line, char *pkg, int pkg_n,
                        char *act, int act_n)
{
    /* 形如：mCurrentFocus=Window{abc xxx pkg/com.x.MainActivity} */
    if (pkg && pkg_n > 0) pkg[0] = 0;
    if (act && act_n > 0) act[0] = 0;

    char *p = strstr(focus_line, "Window{");
    if (!p) return;
    /* 跳过 Window{<hash> u<id> 后到第一个空格 */
    p = strchr(p, ' ');
    if (!p) return;
    p++;
    /* 现在 p 指向 pkg/activity} 或 mAcc 等。skip 后再次找空格直到 pkg/act */
    while (*p == ' ') p++;
    /* 有些 ROM 在 pkg 前还有 "u0 " 之类前缀，再找一次空格 */
    char *next = strchr(p, ' ');
    char *slash = strchr(p, '/');
    if (next && slash && next < slash) {
        p = next + 1;
    }
    slash = strchr(p, '/');
    if (!slash) return;
    /* pkg = [p, slash) */
    int pkg_len = (int)(slash - p);
    if (pkg && pkg_n > 0) {
        int copy = pkg_len < pkg_n - 1 ? pkg_len : pkg_n - 1;
        memcpy(pkg, p, (size_t)copy);
        pkg[copy] = 0;
    }
    /* activity = [slash+1, '}') */
    char *brace = strchr(slash + 1, '}');
    if (!brace) {
        /* 也可能是空白结束 */
        brace = strchr(slash + 1, ' ');
    }
    if (!brace) brace = slash + strlen(slash);
    int act_len = (int)(brace - slash - 1);
    if (act && act_n > 0) {
        int copy = act_len < act_n - 1 ? act_len : act_n - 1;
        memcpy(act, slash + 1, (size_t)copy);
        act[copy] = 0;
    }
}

int App_CurrentPackage(char *out_buf, int out_buf_size)
{
    char raw[2048];
    int n = run_one("dumpsys window | grep -E 'mCurrentFocus|mFocusedApp' | head -1",
                    raw, sizeof(raw));
    if (n <= 0) return 0;
    char pkg[256], act[256];
    parse_focus(raw, pkg, sizeof(pkg), act, sizeof(act));
    int len = (int)strlen(pkg);
    if (out_buf && out_buf_size > 0) {
        int copy = len < out_buf_size - 1 ? len : out_buf_size - 1;
        memcpy(out_buf, pkg, (size_t)copy);
        out_buf[copy] = 0;
    }
    return len;
}

int App_CurrentActivity(char *out_buf, int out_buf_size)
{
    char raw[2048];
    int n = run_one("dumpsys window | grep -E 'mCurrentFocus|mFocusedApp' | head -1",
                    raw, sizeof(raw));
    if (n <= 0) return 0;
    char pkg[256], act[256];
    parse_focus(raw, pkg, sizeof(pkg), act, sizeof(act));
    int len = (int)strlen(act);
    if (out_buf && out_buf_size > 0) {
        int copy = len < out_buf_size - 1 ? len : out_buf_size - 1;
        memcpy(out_buf, act, (size_t)copy);
        out_buf[copy] = 0;
    }
    return len;
}

int App_Launch(const char *pkg)
{
    if (!pkg || !*pkg) return 0;
    char cmd[512];
    /* 用 monkey 启动是最稳的方式（不用解 main activity 名字） */
    snprintf(cmd, sizeof(cmd),
             "monkey -p '%s' -c android.intent.category.LAUNCHER 1 2>&1",
             pkg);
    return output_contains(cmd, "Events injected");
}

int App_IsInstalled(const char *pkg)
{
    if (!pkg || !*pkg) return 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pm list packages '%s'", pkg);
    char buf[1024];
    if (Shell_Exec(cmd, buf, sizeof(buf)) < 0) return 0;
    /* 输出形如 "package:com.example.x"，要严格匹配防止 com.example.xy 误判 */
    char target[300];
    snprintf(target, sizeof(target), "package:%s", pkg);
    char *line = buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        if (strcmp(line, target) == 0) return 1;
        if (!eol) break;
        line = eol + 1;
    }
    return 0;
}

int App_ForceStop(const char *pkg)
{
    if (!pkg || !*pkg) return 0;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "am force-stop '%s'", pkg);
    Shell_Exec(cmd, NULL, 0);
    return 1;
}

int App_Clear(const char *pkg)
{
    if (!pkg || !*pkg) return 0;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pm clear '%s' 2>&1", pkg);
    return output_contains(cmd, "Success");
}

int App_OpenUrl(const char *url)
{
    if (!url || !*url) return 0;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "am start -a android.intent.action.VIEW -d '%s' 2>&1", url);
    return output_contains(cmd, "Starting");
}

/* ---- Device ---- */

int Device_IsScreenOn(void)
{
    /* dumpsys power 输出 mWakefulness=Awake / Asleep / Dreaming / Dozing */
    return output_contains("dumpsys power | grep -E 'mWakefulness=' | head -1",
                           "Awake");
}

int Device_IsScreenUnlock(void)
{
    /* mShowingLockscreen=false / true */
    return output_contains(
        "dumpsys window policy | grep -E 'mShowingLockscreen' | head -1",
        "mShowingLockscreen=false");
}

void Device_WakeUp(void)  { Shell_Exec("input keyevent KEYCODE_WAKEUP", NULL, 0); }
void Device_Sleep(void)   { Shell_Exec("input keyevent KEYCODE_SLEEP",  NULL, 0); }

int Device_GetBattery(void)
{
    char buf[256];
    if (Shell_Exec("dumpsys battery | grep -E '^  level' | head -1",
                   buf, sizeof(buf)) <= 0) return -1;
    char *p = strchr(buf, ':');
    if (!p) return -1;
    return atoi(p + 1);
}

int Device_GetBatteryStatus(void)
{
    char buf[256];
    if (Shell_Exec("dumpsys battery | grep -E '^  status' | head -1",
                   buf, sizeof(buf)) <= 0) return -1;
    char *p = strchr(buf, ':');
    if (!p) return -1;
    return atoi(p + 1);
}

void Device_Vibrate(int ms)
{
    /* 第一版用 cmd vibrator vibrate（API 28+）；老版 ROM 不可用就 noop */
    if (ms <= 0) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "cmd vibrator vibrate %d 2>/dev/null", ms);
    Shell_Exec(cmd, NULL, 0);
}

int Device_GetSdkInt(void)
{
    char buf[64];
    if (Shell_Exec("getprop ro.build.version.sdk", buf, sizeof(buf)) <= 0) return 0;
    return atoi(buf);
}

static int read_prop(const char *key, char *out, int n)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "getprop %s", key);
    return Shell_Exec(cmd, out, n);
}

int Device_GetBrand(char *out, int n) { return read_prop("ro.product.brand", out, n); }
int Device_GetModel(char *out, int n) { return read_prop("ro.product.model", out, n); }

/* ---- IME ---- */

int IME_GetClipText(char *out, int n)
{
    /* `cmd clipboard get-primary` 在 Android 11+ 大多数 ROM 限制读取，
     * 这里只做一次尝试；失败就让 Lua 端去走 Bridge 让 APK 取（待补）。 */
    return Shell_Exec("cmd clipboard get-primary 2>/dev/null", out, n);
}

int IME_SetClipText(const char *text)
{
    if (!text) return 0;
    /* set-primary 用 stdin 接收文本，shell 转义麻烦；用 sed 风格转义。
     * 简单处理：禁止单引号；只支持普通 ASCII / 中文 utf-8（不含单引号） */
    if (strchr(text, '\'')) return 0;  /* 第一版不允许带单引号 */
    char cmd[2048];
    int len = snprintf(cmd, sizeof(cmd),
                       "echo -n '%s' | cmd clipboard set-primary 2>/dev/null",
                       text);
    if (len <= 0 || len >= (int)sizeof(cmd)) return 0;
    Shell_Exec(cmd, NULL, 0);
    return 1;
}

int IME_InputText(const char *text)
{
    if (!text || !*text) return 1;
    if (strchr(text, '\'')) return 0;
    char cmd[2048];
    int len = snprintf(cmd, sizeof(cmd), "input text '%s'", text);
    if (len <= 0 || len >= (int)sizeof(cmd)) return 0;
    Shell_Exec(cmd, NULL, 0);
    return 1;
}

int IME_KeyAction(int keycode)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "input keyevent %d", keycode);
    Shell_Exec(cmd, NULL, 0);
    return 1;
}
