package com.easylua.support;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

/**
 * MIUI 专属反射 hook，用于绕开 root 进程下的崩溃。
 *
 * 已知坑：
 *   1. MIUI 在 ViewRootImpl.&lt;init&gt;() 里调
 *      ForceDarkHelper.registAppDarkModeObserver(...)，
 *      在 MiuiBinderProxy.callOneWayTransact 内部 obj.getClass() 抛 NPE，
 *      因为 root 进程没有 IIntelMiuiHelper service binder。
 *
 *   2. MIUI 的 TypefaceUtils.replaceTypeface 在 TextView 构造里调
 *      Settings.System.getInt → ContentResolver → RemoteException。
 *
 * 解决思路：在 bootstrap 完成后第一次创建 View 之前，把 MIUI 内部的单例 / 静态
 * 字段替换成 null 或 noop 代理，让那些 register 调用提前 return。
 *
 * 注意：所有 hook 失败都不能阻止启动，只能 best-effort，因为不同版本字段名差异大。
 */
public final class MiuiHooks {

    private static volatile boolean applied;

    private MiuiHooks() {}

    /**
     * 在创建任何 View / WindowManager.addView 之前调用一次。
     * 多次调用是幂等的。
     */
    public static synchronized void apply() {
        if (applied) return;
        applied = true;

        // 只在 MIUI 上跑（其它 ROM 没这些类）
        if (!isMiui()) {
            System.out.println("[MiuiHooks] not MIUI, skip");
            return;
        }

        nullOutForceDarkHelper();
        disableTypefaceVarFont();
    }

    private static boolean isMiui() {
        try {
            Class.forName("miui.os.Build");
            return true;
        } catch (Throwable ignored) {
            return false;
        }
    }

    /* =========================================================
     * 1) 让 ForceDarkHelper.getInstance().registAppDarkModeObserver 变 noop
     *
     * 思路：把 sInstance 字段（或 ForceDarkHelper 内部所有 IInterface 字段）
     * 设为 null。registAppDarkModeObserver 里如果有 null check 会跳过；
     * 没有 null check 的版本退化为 NPE 但被 Overlay 的 try/catch 接住。
     *
     * 更稳的办法：把整个 register 方法替换成空实现 — Java 不能改字节码，
     * 退而求其次通过反射 nullify 内部 binder 字段。
     * ========================================================= */
    private static void nullOutForceDarkHelper() {
        try {
            Class<?> cls = Class.forName("android.view.ForceDarkHelper");
            // 1) 找单例：通常叫 sInstance / mInstance
            Object instance = null;
            for (Field f : cls.getDeclaredFields()) {
                if (Modifier.isStatic(f.getModifiers()) && cls.isAssignableFrom(f.getType())) {
                    f.setAccessible(true);
                    Object v = f.get(null);
                    if (v != null) { instance = v; break; }
                }
            }
            // 调一次 getInstance() 强制触发懒加载
            if (instance == null) {
                try {
                    Method get = cls.getDeclaredMethod("getInstance");
                    get.setAccessible(true);
                    instance = get.invoke(null);
                } catch (Throwable ignored) {}
            }

            int patched = 0;
            if (instance != null) {
                // 2) 把单例里所有非基本类型字段都置 null，
                //    这样 registAppDarkModeObserver 内部访问 mIIntelMiuiHelper 等会 NPE 但不会调 binder
                for (Field f : cls.getDeclaredFields()) {
                    if (Modifier.isStatic(f.getModifiers())) continue;
                    if (f.getType().isPrimitive()) continue;
                    f.setAccessible(true);
                    try {
                        f.set(instance, null);
                        patched++;
                    } catch (Throwable ignored) {}
                }
            }
            System.out.println("[MiuiHooks] ForceDarkHelper instance="
                    + (instance != null) + " nulled=" + patched);
        } catch (ClassNotFoundException e) {
            System.out.println("[MiuiHooks] no ForceDarkHelper, skip");
        } catch (Throwable t) {
            System.out.println("[MiuiHooks] ForceDarkHelper hook failed: " + t);
        }
    }

    /* =========================================================
     * 2) 让 MIUI TypefaceUtils.getVarFont 跳过 ContentResolver 查询。
     *
     * 核心代码（来自 MIUI Android 10 framework.jar 反编译）：
     *   int scale = Holder.sFontScale &lt; 0
     *               ? loadFontScaleSetting(context)
     *               : Holder.sFontScale;
     *
     * 只要 Holder.sFontScale >= 0，就不会走 Settings.System.getInt → ContentResolver →
     * SecurityException 那条路。我们把它直接置 50（默认值）即可。
     *
     * 类名：miui.util.TypefaceUtils$Holder，字段：sFontScale (static int)
     * ========================================================= */
    private static void disableTypefaceVarFont() {
        try {
            Class<?> holder = Class.forName("miui.util.TypefaceUtils$Holder");
            Field f = holder.getDeclaredField("sFontScale");
            f.setAccessible(true);
            f.setInt(null, 50);
            System.out.println("[MiuiHooks] TypefaceUtils$Holder.sFontScale = 50");
        } catch (ClassNotFoundException ignored) {
            // 非 MIUI 或字段名不同
        } catch (NoSuchFieldException ignored) {
            System.out.println("[MiuiHooks] TypefaceUtils$Holder has no sFontScale field");
        } catch (Throwable t) {
            System.out.println("[MiuiHooks] TypefaceUtils hook failed: " + t);
        }
    }
}
