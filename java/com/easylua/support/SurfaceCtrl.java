package com.easylua.support;

import android.graphics.Rect;
import android.os.IBinder;
import android.view.Surface;

import java.lang.reflect.Method;

/**
 * 通过反射调 android.view.SurfaceControl 的 hidden API。
 *
 * 这套调用是用 SurfaceFlinger 创建虚拟 Display 把屏幕画面投影到指定 Surface，
 * 再通过 ImageReader 拿帧的核心。
 *
 * 关键 hidden API（API 28~33 都存在；API 34+ 部分签名变更，本类未覆盖）：
 *   - createDisplay(String name, boolean secure) -> IBinder
 *   - destroyDisplay(IBinder token)
 *   - openTransaction() / closeTransaction()
 *   - setDisplaySurface(IBinder, Surface)
 *   - setDisplayProjection(IBinder, int orientation, Rect layerStackRect, Rect displayRect)
 *   - setDisplayLayerStack(IBinder, int layerStack)
 *
 * 普通 APK 进程会被 hidden API 限制拦截；以 app_process / root 启动的进程
 * 默认豁免（uid=2000 shell 等）。
 */
public final class SurfaceCtrl {

    private SurfaceCtrl() {}

    private static final Class<?> CLS;
    static {
        try {
            CLS = Class.forName("android.view.SurfaceControl");
        } catch (ClassNotFoundException e) {
            throw new AssertionError("SurfaceControl 反射加载失败：" + e);
        }
    }

    /** 缓存反射方法句柄，避免每次调用都 lookup */
    private static Method M_createDisplay;
    private static Method M_destroyDisplay;
    private static Method M_openTransaction;
    private static Method M_closeTransaction;
    private static Method M_setDisplaySurface;
    private static Method M_setDisplayProjection;
    private static Method M_setDisplayLayerStack;

    public static IBinder createDisplay(String name, boolean secure) throws Exception {
        if (M_createDisplay == null) {
            M_createDisplay = CLS.getMethod("createDisplay", String.class, boolean.class);
        }
        return (IBinder) M_createDisplay.invoke(null, name, secure);
    }

    public static void destroyDisplay(IBinder token) {
        try {
            if (M_destroyDisplay == null) {
                M_destroyDisplay = CLS.getMethod("destroyDisplay", IBinder.class);
            }
            M_destroyDisplay.invoke(null, token);
        } catch (Throwable t) {
            // 销毁是 best-effort，吞掉异常避免清理流程被打断
            t.printStackTrace(System.out);
        }
    }

    public static void openTransaction() throws Exception {
        if (M_openTransaction == null) {
            M_openTransaction = CLS.getMethod("openTransaction");
        }
        M_openTransaction.invoke(null);
    }

    public static void closeTransaction() throws Exception {
        if (M_closeTransaction == null) {
            M_closeTransaction = CLS.getMethod("closeTransaction");
        }
        M_closeTransaction.invoke(null);
    }

    public static void setDisplaySurface(IBinder token, Surface surface) throws Exception {
        if (M_setDisplaySurface == null) {
            M_setDisplaySurface = CLS.getMethod("setDisplaySurface", IBinder.class, Surface.class);
        }
        M_setDisplaySurface.invoke(null, token, surface);
    }

    public static void setDisplayProjection(IBinder token, int orientation,
                                            Rect layerStackRect, Rect displayRect) throws Exception {
        if (M_setDisplayProjection == null) {
            M_setDisplayProjection = CLS.getMethod("setDisplayProjection",
                    IBinder.class, int.class, Rect.class, Rect.class);
        }
        M_setDisplayProjection.invoke(null, token, orientation, layerStackRect, displayRect);
    }

    public static void setDisplayLayerStack(IBinder token, int layerStack) throws Exception {
        if (M_setDisplayLayerStack == null) {
            M_setDisplayLayerStack = CLS.getMethod("setDisplayLayerStack",
                    IBinder.class, int.class);
        }
        M_setDisplayLayerStack.invoke(null, token, layerStack);
    }

    /**
     * 用 transaction 包裹的"接 Surface 到 Display"组合操作。
     * 设置 surface + projection + layerStack 必须在同一 transaction 内才生效。
     */
    public static void bindSurface(IBinder token, Surface surface,
                                   int width, int height, int layerStack) throws Exception {
        Rect rect = new Rect(0, 0, width, height);
        openTransaction();
        try {
            setDisplaySurface(token, surface);
            setDisplayProjection(token, 0, rect, rect);
            setDisplayLayerStack(token, layerStack);
        } finally {
            closeTransaction();
        }
    }
}
