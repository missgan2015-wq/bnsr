package com.easylua.support;

import android.content.Context;
import android.os.Looper;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * 在 root 进程内伪造一个可用的 Context。
 *
 * 关键问题：app_process 启动后没有正常的 Application Context，
 * 直接 ContextWrapper(null) 会让 getSystemService 返回 null。
 *
 * scrcpy 项目的解决思路（这里采用同样的）：
 *   1. 反射创建一个 ActivityThread 实例
 *   2. 把它装进 sCurrentActivityThread 静态字段
 *   3. 标记 mSystemThread = true（系统线程）
 *   4. 调 ActivityThread.getSystemContext() 拿到一个真实 Context
 *
 * 这一段代码每个 Android 版本字段名都没变（28~35 都验证过），
 * 是 root 工具最稳定的入口。
 */
public final class SystemContext {

    private static volatile boolean inited;
    private static volatile Context cachedContext;

    private SystemContext() {}

    /** 取 Context；首次调用会跑 bootstrap 创建 ActivityThread */
    public static synchronized Context get() {
        if (!inited) {
            try {
                bootstrap();
            } catch (Throwable t) {
                System.err.println("[SystemContext] bootstrap failed: " + t);
                t.printStackTrace(System.err);
            }
            inited = true;
        }
        return cachedContext;
    }

    private static void bootstrap() throws Exception {
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();

        Class<?> atClass = Class.forName("android.app.ActivityThread");

        // 1. new ActivityThread()
        Constructor<?> ctor = atClass.getDeclaredConstructor();
        ctor.setAccessible(true);
        Object at = ctor.newInstance();

        // 2. ActivityThread.sCurrentActivityThread = at
        Field cur = atClass.getDeclaredField("sCurrentActivityThread");
        cur.setAccessible(true);
        cur.set(null, at);

        // 3. at.mSystemThread = true（让一些 system_server 检查通过）
        try {
            Field sys = atClass.getDeclaredField("mSystemThread");
            sys.setAccessible(true);
            sys.setBoolean(at, true);
        } catch (NoSuchFieldException ignored) {
            // 部分 ROM 可能改名了，跳过
        }

        // 4. 给 mBoundApplication 装一个伪 ApplicationInfo，避免 MIUI / OEM 启动时
        //    在 LoadedApk 里访问 mBoundApplication 时 NPE
        try {
            Class<?> bindClass = Class.forName("android.app.ActivityThread$AppBindData");
            Constructor<?> bindCtor = bindClass.getDeclaredConstructor();
            bindCtor.setAccessible(true);
            Object bind = bindCtor.newInstance();
            android.content.pm.ApplicationInfo appInfo = new android.content.pm.ApplicationInfo();
            // UID 0 (root) 在系统 AppOpsService 里只接受 packageName="android"，
            // 用其它名字会在 WindowManagerService.addWindow 抛
            // SecurityException: "Package <X> not in UID 0"
            appInfo.packageName = "android";
            // MIUI ForceDarkHelper.registAppDarkModeObserver 会读 processName 传给 binder，
            // 不能为 null，否则 MiuiBinderProxy.callTransact 在 param.getClass() 处 NPE
            appInfo.processName = "android";
            Field bindAppInfo = bindClass.getDeclaredField("appInfo");
            bindAppInfo.setAccessible(true);
            bindAppInfo.set(bind, appInfo);
            Field mBound = atClass.getDeclaredField("mBoundApplication");
            mBound.setAccessible(true);
            mBound.set(at, bind);
        } catch (Throwable t) {
            System.out.println("[SystemContext] fillAppInfo skipped: " + t);
        }

        // 5. 拿系统 Context
        //    MIUI 内部 ThemeCompatibilityLoader 在 <clinit> 里读
        //    /data/system/theme_config/theme_compatibility.xml，未安装第三方主题时
        //    一定 ENOENT，并且它直接 printStackTrace 到 stderr。这只是 fallback 路径，
        //    不影响功能；这里临时把 stderr 切到 NUL，避免每次启动都 dump 一段红字。
        java.io.PrintStream origErr = System.err;
        System.setErr(new java.io.PrintStream(new java.io.OutputStream() {
            @Override public void write(int b) {}
        }));
        Method getSysCtx = atClass.getDeclaredMethod("getSystemContext");
        getSysCtx.setAccessible(true);
        Context ctx;
        try {
            ctx = (Context) getSysCtx.invoke(at);
        } finally {
            System.setErr(origErr);
        }

        // 6. 修补 ctx.getApplicationInfo()，让 processName / dataDir 等关键字段非 null
        //    MIUI 的 ForceDarkHelper.registAppDarkModeObserver 会把 processName 传给
        //    MiuiBinderProxy.callOneWayTransact，null 会让 param.getClass() NPE
        try {
            android.content.pm.ApplicationInfo info = ctx.getApplicationInfo();
            if (info != null) {
                if (info.processName == null) info.processName = "android";
                if (info.packageName == null) info.packageName = "android";
                if (info.dataDir == null)    info.dataDir    = "/data/local/tmp/easylua";
            }
        } catch (Throwable t) {
            System.out.println("[SystemContext] patch appInfo skipped: " + t);
        }

        cachedContext = ctx;
    }
}
