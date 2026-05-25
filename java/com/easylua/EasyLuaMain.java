package com.easylua;

import android.graphics.Bitmap;

import com.easylua.support.DisplayMgr;
import com.easylua.support.ScreenStream;
import com.easylua.support.Screenshot;
import com.easylua.support.SizeInfo;

/**
 * easyLua 应用层入口。
 *
 * 由 app_process 启动：
 *   app_process -Djava.class.path=easylua.dex / com.easylua.EasyLuaMain [args]
 *
 * 子命令：
 *   info                        打印 display 信息（Stage A）
 *   --screenshot PATH           截一帧到 PATH（Stage B）
 *   --lua SO_PATH SCRIPT_PATH   加载 .so 起 LuaJIT 跑脚本，并启视频流（Stage C+D）
 */
public final class EasyLuaMain {

    private EasyLuaMain() {}

    public static void main(String[] args) {
        // 启动 banner 改成单行，避免每次脚本都刷一屏
        System.out.println(String.format("[easylua] boot  sdk=%d  uid=%d  pid=%d",
                android.os.Build.VERSION.SDK_INT,
                android.os.Process.myUid(),
                android.os.Process.myPid()));

        String mode = args.length > 0 ? args[0] : "info";
        try {
            if ("--screenshot".equals(mode)) {
                if (args.length < 2) {
                    System.err.println("usage: easyLua --screenshot <path>");
                    System.exit(2);
                }
                runScreenshot(args[1]);
            } else if ("--lua".equals(mode)) {
                if (args.length < 3) {
                    System.err.println("usage: easyLua --lua <so> <script.lua>");
                    System.exit(2);
                }
                runLua(args[1], args[2]);
            } else {
                runInfo();
            }
        } catch (Throwable t) {
            System.err.println("[easylua] FATAL: " + t);
            t.printStackTrace(System.err);
            System.exit(1);
        }

        System.out.println("[easylua] bye");
        System.out.flush();
    }

    /** Stage A：打印 display 信息 */
    private static void runInfo() {
        SizeInfo info = DisplayMgr.getDisplayInfo(0);
        if (info == null) {
            System.out.println("[easylua] display info = null");
            return;
        }
        System.out.println("[easylua] display 0:");
        System.out.println("[easylua]   size = " + info.width + " x " + info.height);
        System.out.println("[easylua]   dpi = " + info.dpi);
        System.out.println("[easylua]   rotation = " + info.rotation);
        System.out.println("[easylua]   layerStack = " + info.layerStack);
        System.out.println("[easylua]   uniqueId = " + info.uniqueId);
    }

    /** Stage B：单帧截图存 PNG */
    private static void runScreenshot(String path) throws Exception {
        SizeInfo info = DisplayMgr.getDisplayInfo(0);
        if (info == null) throw new IllegalStateException("无法获取 display 0 信息");
        System.out.println("[easylua] capturing " + info.width + "x" + info.height +
                "  layerStack=" + info.layerStack);

        long t0 = System.currentTimeMillis();
        Bitmap bmp = Screenshot.grab(info, 3000L);
        long t1 = System.currentTimeMillis();
        System.out.println("[easylua] grabbed " + bmp.getWidth() + "x" + bmp.getHeight() +
                " in " + (t1 - t0) + " ms");

        Screenshot.saveAsPng(bmp, path);
        long t2 = System.currentTimeMillis();
        bmp.recycle();
        System.out.println("[easylua] saved -> " + path + "  (encode " + (t2 - t1) + " ms)");
    }

    /**
     * Stage C+D / H：
     *   1) 让 SystemContext bootstrap，让 WindowManager 可用
     *   2) 加载 libeasylua.so
     *   3) 起视频流
     *   4) 同步调 nativeMain 跑脚本（OverlayUi 自己有 HandlerThread 处理 UI）
     */
    private static void runLua(final String soPath, final String scriptPath) throws Exception {
        // SystemContext 提前 bootstrap
        try {
            com.easylua.support.SystemContext.get();
        } catch (Throwable t) {
            System.err.println("[easylua] SystemContext warn: " + t);
        }

        EasyLuaJni.loadFrom(soPath);
        System.out.println("[easylua] " + EasyLuaJni.nativeHello());

        ScreenStream stream = new ScreenStream(0);
        stream.start();
        try {
            int rc = EasyLuaJni.nativeMain(scriptPath);
            System.out.println("[easylua] script exit code = " + rc);
        } finally {
            stream.stop();
        }
    }
}
