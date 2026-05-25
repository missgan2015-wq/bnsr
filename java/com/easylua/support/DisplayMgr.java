package com.easylua.support;

import java.lang.reflect.Method;

/**
 * 通过反射调用 DisplayManagerGlobal.getDisplayInfo(int)。
 *
 * 这是 scrcpy 用的核心 hidden API 反射技巧的最简版本：
 *   1. android.hardware.display.DisplayManagerGlobal#getInstance() -- @hide static
 *   2. 实例 .getDisplayInfo(int displayId)                          -- @hide
 *   3. 返回 android.view.DisplayInfo                                -- @hide
 *   4. 用反射读 logicalWidth / logicalHeight / rotation / dpi /
 *      layerStack / flags / uniqueId 字段
 *
 * 普通 APK 进程在 API 28+ 上能拿到 logicalWidth/Height，但 layerStack /
 * uniqueId 这些"敏感字段"在 hidden API 限制下读不到（拿不到值或抛异常）。
 *
 * 在 app_process（uid=2000 shell）或 root 进程下，所有字段都能读到，
 * 这正是 SurfaceControl-based 截屏所需的入口。
 */
public final class DisplayMgr {

    private DisplayMgr() {}

    /** 缓存反射结果，避免每次调用都重新 lookup */
    private static Object dmgInstance;
    private static Method getDisplayInfoMethod;

    private static synchronized void ensureInit() throws Exception {
        if (getDisplayInfoMethod != null) return;
        Class<?> clz = Class.forName("android.hardware.display.DisplayManagerGlobal");
        Method getInstance = clz.getDeclaredMethod("getInstance");
        dmgInstance = getInstance.invoke(null);
        getDisplayInfoMethod = clz.getMethod("getDisplayInfo", int.class);
    }

    /**
     * 拿指定 displayId 的显示信息。
     * 调用失败（hidden API 拒绝 / display 不存在）返回 null。
     */
    public static SizeInfo getDisplayInfo(int displayId) {
        try {
            ensureInit();
            Object di = getDisplayInfoMethod.invoke(dmgInstance, displayId);
            if (di == null) return null;

            Class<?> dc = di.getClass();
            int w = dc.getDeclaredField("logicalWidth").getInt(di);
            int h = dc.getDeclaredField("logicalHeight").getInt(di);
            int rotation = dc.getDeclaredField("rotation").getInt(di);
            int dpi = dc.getDeclaredField("logicalDensityDpi").getInt(di);
            int layerStack = readIntFieldOrZero(dc, di, "layerStack");
            int flags = readIntFieldOrZero(dc, di, "flags");
            String uniqueId = readStringFieldOrNull(dc, di, "uniqueId");
            return new SizeInfo(displayId, w, h, rotation, dpi, layerStack, flags, uniqueId);
        } catch (Throwable t) {
            t.printStackTrace(System.out);
            return null;
        }
    }

    private static int readIntFieldOrZero(Class<?> c, Object obj, String name) {
        try {
            return c.getDeclaredField(name).getInt(obj);
        } catch (Throwable t) {
            return 0;
        }
    }

    private static String readStringFieldOrNull(Class<?> c, Object obj, String name) {
        try {
            return (String) c.getDeclaredField(name).get(obj);
        } catch (Throwable t) {
            return null;
        }
    }
}
