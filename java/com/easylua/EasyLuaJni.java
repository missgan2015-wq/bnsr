package com.easylua;

import java.nio.ByteBuffer;

/**
 * easyLua native 桥接类。
 *
 * Java <-> C 之间的所有 JNI 接口集中在这里：
 *   - nativeMain(scriptPath)       Java → C：交出控制权，让 LuaJIT 跑脚本
 *   - nativeOnFrame(w,h,buf,row)   Java → C：把屏幕帧推到 C 端缓存（每收到一帧调一次）
 *   - nativeHello()                Java → C：sanity check，验证 .so 加载成功
 *
 * 加载约定：libeasylua.so 在 app_process 里需要用 System.load(绝对路径) 加载，
 * 部署在 /data/local/tmp/easylua/libeasylua.so。
 */
public final class EasyLuaJni {

    private EasyLuaJni() {}

    /** 把 .so 加载进来；调一次即可，多次调用是安全的（System.load 内部去重） */
    public static void loadFrom(String soPath) {
        System.load(soPath);
    }

    /** sanity check：返回 "hello from libeasylua.so" */
    public static native String nativeHello();

    /**
     * 启动 LuaJIT 跑脚本，阻塞直到脚本结束。
     * 返回值：0 = 成功，非 0 = 错误码。
     */
    public static native int nativeMain(String scriptPath);

    /**
     * Java 端把一帧 RGBA8888 推给 native 缓存（Stage H 路径，用 ByteBuffer copy）。
     * Stage I 之后保留这条路径作为 SDK < 29 的 fallback，但不再被 ScreenStream 使用。
     *
     * @param width      实际像素宽（不含 padding）
     * @param height     实际像素高
     * @param buffer     Direct ByteBuffer，指向 ImageReader plane 的内存
     * @param rowStride  每行字节数（含 padding，可能 != width*4）
     */
    public static native void nativeOnFrame(int width, int height,
                                            ByteBuffer buffer, int rowStride);

    /**
     * Stage I：Java 端把一帧的 HardwareBuffer 推给 native，零拷贝路径。
     *
     * native 端会调 AHardwareBuffer_fromHardwareBuffer + AHardwareBuffer_lock 拿到
     * dma-buf 的 CPU mapped 地址，memcpy 到紧凑帧缓存后立即 unlock。
     *
     * @param hb        android.hardware.HardwareBuffer，必须是 RGBA_8888，API 29+
     * @param width     实际像素宽
     * @param height    实际像素高
     * @return 0 成功；&lt; 0 失败（调用方应在连续失败时降级到 nativeOnFrame）
     */
    public static native int nativeOnFrameHB(android.hardware.HardwareBuffer hb,
                                             int width, int height);

    /* ---- 给 native (jni/ui.c) 反向调的 UI helper ----
     *
     * Highlight：自绘 Canvas，走本进程内 Overlay
     * Toast：    在 native 端用 am broadcast，**不**经过这里 */

    /** 显示 highlight 矩形（SikuliX 风格）
     *  @param colorArgb  ARGB 颜色，0 = 默认红色
     *  @param durMs      显示时长 ms，<= 0 = 常驻
     *  @param label      矩形上方文字标签，可为 null/空 */
    public static void uiHighlight(int x, int y, int w, int h,
                                   int colorArgb, int durMs, String label) {
        com.easylua.support.Overlay.highlight(x, y, w, h, colorArgb, durMs, label);
    }

    /** 立即清掉所有 highlight */
    public static void uiHighlightOff() {
        com.easylua.support.Overlay.highlightOff();
    }
}
