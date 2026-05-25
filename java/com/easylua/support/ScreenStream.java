package com.easylua.support;

import android.graphics.PixelFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.view.Surface;

import com.easylua.EasyLuaJni;

import java.nio.ByteBuffer;

/**
 * 屏幕视频流：通过 SurfaceFlinger 虚拟 Display + ImageReader 持续吃帧。
 *
 * 双路径策略（按"先 NDK 不行才 scrcpy"原则）：
 *
 *   A) HardwareBuffer 路径（首选）
 *      ImageReader.newInstance(format, maxImages, usage=GPU_SAMPLED_IMAGE|CPU_READ_OFTEN)
 *      Image.getHardwareBuffer() -> native AHardwareBuffer_lock 直读 dma-buf
 *      要求 SDK 29+，且 ROM 支持 RGBA8888 + CPU_READ usage 组合
 *
 *   B) scrcpy 路径（兜底）
 *      ImageReader.newInstance(format, maxImages)（不指定 usage）
 *      Image.getPlanes()[0].buffer -> native memcpy
 *      所有 SDK 28+ 设备保底可用
 *
 * 切换时机：
 *   - start() 默认尝试 A
 *   - 创建 reader 失败 -> 立即切 B
 *   - 连续 FALLBACK_THRESHOLD 帧 lock 失败 -> 重启 reader 切 B
 *
 * 旋转 / 分辨率变化：
 *   - watchdog 线程每秒 poll DisplayMgr.getDisplayInfo
 *   - width/height/rotation 任一改变 -> rebuild reader + display（同一线程，避免锁竞争）
 *   - 跨 rebuild 期间 native 帧缓存保持上一帧，不会出现 "Screen.Width=0"
 *
 * 简化点：
 *   - 暂时不做 SM-F926/F916 的 5s 自动重建
 *   - 只支持 SDK 28+ 的 SurfaceControl.createDisplay 路径
 */
public final class ScreenStream {

    /** API 29+ 的 HardwareBuffer.USAGE_CPU_READ_OFTEN */
    private static final long USAGE_CPU_READ_OFTEN = 0x6L;
    /** ImageReader.newInstance(format, usage) 用的 usage 标志组合 */
    private static final long IMAGE_USAGE_HB = USAGE_CPU_READ_OFTEN
            | HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE;

    /** 连续这么多帧 HB 路径失败就 fallback */
    private static final int FALLBACK_THRESHOLD = 3;

    /** display info 轮询周期 */
    private static final long POLL_INTERVAL_MS = 1000L;

    /** 视频流模式 */
    private enum Mode { HARDWARE_BUFFER, BYTE_BUFFER }

    private final int displayId;
    private SizeInfo info;
    private ImageReader reader;
    private IBinder display;
    private HandlerThread thread;
    private Handler handler;
    private volatile boolean running;
    private volatile Mode mode;
    private int hbFailStreak;

    public ScreenStream(int displayId) {
        this.displayId = displayId;
    }

    public synchronized void start() throws Exception {
        if (running) return;

        info = DisplayMgr.getDisplayInfo(displayId);
        if (info == null) throw new IllegalStateException("getDisplayInfo failed");

        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();

        thread = new HandlerThread("easylua-stream");
        thread.start();
        handler = new Handler(thread.getLooper());

        attachReaderAndDisplay();

        running = true;
        System.out.println("[easylua] stream started: " + info.width + "x" + info.height +
                "  rotation=" + info.rotation +
                "  layerStack=" + info.layerStack +
                "  mode=" + mode);

        // 启动旋转 / 分辨率 watchdog
        handler.postDelayed(new Runnable() {
            @Override public void run() { pollDisplayChange(); }
        }, POLL_INTERVAL_MS);
    }

    /** 在 stream handler 线程上创建 reader 并绑定到 display */
    private void attachReaderAndDisplay() throws Exception {
        // 先尝试 HardwareBuffer 路径
        if (Build.VERSION.SDK_INT >= 29 && tryStartHardwareBuffer()) {
            mode = Mode.HARDWARE_BUFFER;
        } else {
            startByteBuffer();
            mode = Mode.BYTE_BUFFER;
        }

        if (display == null) {
            display = SurfaceCtrl.createDisplay("easylua-stream", false);
            if (display == null) throw new IllegalStateException("createDisplay returned null");
        }

        Surface surface = reader.getSurface();
        SurfaceCtrl.bindSurface(display, surface, info.width, info.height, info.layerStack);
    }

    /**
     * 尝试创建 HardwareBuffer 后端的 ImageReader。
     * @return true 成功，false 不支持需要 fallback
     */
    private boolean tryStartHardwareBuffer() {
        try {
            reader = ImageReader.newInstance(info.width, info.height,
                    PixelFormat.RGBA_8888, /*maxImages*/ 2, IMAGE_USAGE_HB);
            reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
                @Override
                public void onImageAvailable(ImageReader r) {
                    onFrameHardwareBuffer(r);
                }
            }, handler);
            return true;
        } catch (Throwable t) {
            System.out.println("[easylua] HardwareBuffer reader unavailable, fallback to ByteBuffer: " + t);
            if (reader != null) {
                try { reader.close(); } catch (Throwable ignored) {}
                reader = null;
            }
            return false;
        }
    }

    private void startByteBuffer() {
        reader = ImageReader.newInstance(info.width, info.height,
                PixelFormat.RGBA_8888, /*maxImages*/ 2);
        reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
            @Override
            public void onImageAvailable(ImageReader r) {
                onFrameLegacy(r);
            }
        }, handler);
    }

    /** Stage I 路径：Image -> HardwareBuffer -> native AHardwareBuffer_lock 直读 dma-buf */
    private void onFrameHardwareBuffer(ImageReader r) {
        Image img = r.acquireLatestImage();
        if (img == null) return;
        boolean success = false;
        try {
            HardwareBuffer hb = img.getHardwareBuffer();
            if (hb != null) {
                int rc = EasyLuaJni.nativeOnFrameHB(hb, img.getWidth(), img.getHeight());
                hb.close();
                success = (rc == 0);
            }
        } catch (Throwable t) {
            System.out.println("[easylua] HardwareBuffer frame error: " + t);
        } finally {
            img.close();
        }

        if (!success) {
            hbFailStreak++;
            if (hbFailStreak >= FALLBACK_THRESHOLD) {
                System.out.println("[easylua] HardwareBuffer 连续 " + FALLBACK_THRESHOLD
                        + " 帧失败，切换到 ByteBuffer 模式");
                fallbackToByteBuffer();
            }
        } else {
            hbFailStreak = 0;
        }
    }

    /** Stage H scrcpy 路径：拷贝 plane 到 ByteBuffer，立即 close */
    private void onFrameLegacy(ImageReader r) {
        Image img = r.acquireLatestImage();
        if (img == null) return;
        try {
            Image.Plane[] planes = img.getPlanes();
            if (planes.length == 0) return;
            ByteBuffer buf = planes[0].getBuffer();
            int rowStride = planes[0].getRowStride();
            EasyLuaJni.nativeOnFrame(img.getWidth(), img.getHeight(), buf, rowStride);
        } finally {
            img.close();
        }
    }

    /** 运行时降级：把 reader 重建为 ByteBuffer 模式，display 不重建 */
    private synchronized void fallbackToByteBuffer() {
        if (!running || mode == Mode.BYTE_BUFFER) return;
        try {
            ImageReader oldReader = reader;
            reader = null;
            if (oldReader != null) oldReader.close();

            startByteBuffer();
            mode = Mode.BYTE_BUFFER;
            hbFailStreak = 0;

            Surface surface = reader.getSurface();
            SurfaceCtrl.bindSurface(display, surface, info.width, info.height, info.layerStack);
        } catch (Throwable t) {
            System.out.println("[easylua] fallbackToByteBuffer failed: " + t);
        }
    }

    /**
     * 每秒检查 display 信息是否变化（width/height/rotation）。
     * 变化就重建 reader（display 也重建以重新获取正确 layerStack）。
     */
    private void pollDisplayChange() {
        if (!running) return;
        try {
            SizeInfo cur = DisplayMgr.getDisplayInfo(displayId);
            if (cur != null && infoChanged(info, cur)) {
                System.out.println("[easylua] display 变化: "
                        + info.width + "x" + info.height + " r=" + info.rotation
                        + "  ->  "
                        + cur.width + "x" + cur.height + " r=" + cur.rotation);
                rebuildOnDisplayChange(cur);
            }
        } catch (Throwable t) {
            System.out.println("[easylua] pollDisplayChange err: " + t);
        }
        if (running) handler.postDelayed(new Runnable() {
            @Override public void run() { pollDisplayChange(); }
        }, POLL_INTERVAL_MS);
    }

    private static boolean infoChanged(SizeInfo a, SizeInfo b) {
        if (a == null || b == null) return false;
        return a.width != b.width
                || a.height != b.height
                || a.rotation != b.rotation
                || a.layerStack != b.layerStack;
    }

    /** 旋转 / 分辨率改变后的重建：display + reader 都拆掉重新创建 */
    private synchronized void rebuildOnDisplayChange(SizeInfo cur) {
        if (!running) return;

        info = cur;

        // 1) 拆 reader（display 我们重建，因为 layerStack/projection 都需要重新设置）
        if (reader != null) {
            try { reader.close(); } catch (Throwable ignored) {}
            reader = null;
        }
        if (display != null) {
            try { SurfaceCtrl.destroyDisplay(display); } catch (Throwable ignored) {}
            display = null;
        }

        // 2) 重建
        try {
            attachReaderAndDisplay();
            hbFailStreak = 0;
            System.out.println("[easylua] stream rebuilt: " + info.width + "x" + info.height +
                    "  rotation=" + info.rotation +
                    "  mode=" + mode);
        } catch (Throwable t) {
            System.out.println("[easylua] rebuild failed: " + t);
            t.printStackTrace(System.out);
        }
    }

    public synchronized void stop() {
        if (!running) return;
        running = false;
        if (display != null) {
            SurfaceCtrl.destroyDisplay(display);
            display = null;
        }
        if (reader != null) {
            try { reader.close(); } catch (Throwable ignored) {}
            reader = null;
        }
        if (thread != null) {
            thread.quitSafely();
            thread = null;
            handler = null;
        }
        System.out.println("[easylua] stream stopped");
    }
}
