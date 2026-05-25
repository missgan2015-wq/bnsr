package com.easylua.support;

import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Surface;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * 一次性截屏（同步阻塞）。
 *
 * 使用 SurfaceFlinger 虚拟 Display + ImageReader 路径：
 *   1) 反射调 SurfaceControl.createDisplay 拿一个 displayToken
 *   2) ImageReader.newInstance(w, h, RGBA_8888, 2) 创建帧消费者
 *   3) 在 transaction 内：setDisplaySurface / setDisplayProjection / setDisplayLayerStack
 *      ←——— 这是把"显示器 layerStack 的画面"投到"我们 Surface"的关键三步
 *   4) 等 onImageAvailable 回调触发，acquireLatestImage 取一帧
 *   5) 复制 ByteBuffer 到 Bitmap（处理 rowStride 对齐）
 *   6) 销毁 display + reader
 *
 * 注意：本类**仅 Stage B 验证用**，做完一帧就拆掉所有资源。
 * 流式连续截屏（视频流）会在 Stage C 用单独的常驻 ScreenStream 实现。
 */
public final class Screenshot {

    private Screenshot() {}

    /**
     * 截一帧 0 号 Display，返回 Bitmap（ARGB_8888）。失败抛异常。
     *
     * @param timeoutMs 等帧超时（毫秒）
     */
    public static Bitmap grab(int displayId, long timeoutMs) throws Exception {
        SizeInfo info = DisplayMgr.getDisplayInfo(displayId);
        if (info == null) throw new IllegalStateException("getDisplayInfo failed");
        return grab(info, timeoutMs);
    }

    public static Bitmap grab(SizeInfo info, long timeoutMs) throws Exception {
        // ScreenShot.recreateVirtualDisplay 把 ImageReader 创建放到 main 线程，
        // 我们这里也保持一致（部分设备 setOnImageAvailableListener 必须有 Looper）
        ensureMainLooper();

        int w = info.width;
        int h = info.height;
        if (w <= 0 || h <= 0) throw new IllegalStateException("bad size: " + w + "x" + h);

        // ImageReader 工作线程（回调在它的 Looper 上跑）
        HandlerThread readerThread = new HandlerThread("easylua-reader");
        readerThread.start();
        Handler readerHandler = new Handler(readerThread.getLooper());

        // 这里关键：format 用 PixelFormat.RGBA_8888（数值 = 1），
        // 与原版 ScreenShot.recreateVirtualDisplay 的 ImageReader.newInstance(i, i2, 1, 2) 一致
        ImageReader reader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2);

        AtomicReference<Image> firstImage = new AtomicReference<>(null);
        final CountDownLatch latch = new CountDownLatch(1);
        reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
            @Override
            public void onImageAvailable(ImageReader r) {
                // 只取第一帧（之后的帧让它继续填 buffer 池但我们不消费 -- 不会泄漏，因为 reader 即将被 close）
                Image img = r.acquireLatestImage();
                if (img != null && firstImage.compareAndSet(null, img)) {
                    latch.countDown();
                } else if (img != null) {
                    img.close();
                }
            }
        }, readerHandler);

        IBinder display = null;
        try {
            display = SurfaceCtrl.createDisplay("easylua-cap", false);
            if (display == null) throw new IllegalStateException("createDisplay returned null");

            // 把 reader 的 Surface 接到我们这个虚拟 Display 上
            // layerStack 必须用真实 Display 的 layerStack（否则收到的是黑屏）
            Surface surface = reader.getSurface();
            SurfaceCtrl.bindSurface(display, surface, w, h, info.layerStack);

            // 等帧
            long deadline = SystemClock.elapsedRealtime() + timeoutMs;
            if (!latch.await(timeoutMs, TimeUnit.MILLISECONDS)) {
                throw new IllegalStateException("等待截屏超时（" + timeoutMs + "ms）");
            }
            long elapsed = SystemClock.elapsedRealtime() - (deadline - timeoutMs);
            System.out.println("[easylua] frame received in " + elapsed + " ms");

            Image img = firstImage.get();
            try {
                return imageToBitmap(img);
            } finally {
                img.close();
            }
        } finally {
            // 拆卸：先 destroy display 让 SurfaceFlinger 不再喂帧，再 close reader
            if (display != null) SurfaceCtrl.destroyDisplay(display);
            try { reader.close(); } catch (Throwable ignored) {}
            readerThread.quitSafely();
        }
    }

    /**
     * 把一个 RGBA_8888 的 Image 转成 ARGB_8888 的 Bitmap。
     *
     * 处理 rowPadding：ImageReader 给的 ByteBuffer 每行可能比 width*4 多
     * (rowStride - pixelStride*width) 个填充字节，必须先转成"宽度=width+padding/pixelStride"
     * 的 Bitmap，再裁回 width。
     */
    private static Bitmap imageToBitmap(Image img) {
        int w = img.getWidth();
        int h = img.getHeight();
        Image.Plane[] planes = img.getPlanes();
        ByteBuffer buf = planes[0].getBuffer();
        int pixelStride = planes[0].getPixelStride();
        int rowStride = planes[0].getRowStride();
        int rowPadding = rowStride - pixelStride * w;

        Bitmap padded = Bitmap.createBitmap(w + rowPadding / pixelStride, h, Bitmap.Config.ARGB_8888);
        padded.copyPixelsFromBuffer(buf);
        if (rowPadding == 0) return padded;
        Bitmap result = Bitmap.createBitmap(padded, 0, 0, w, h);
        padded.recycle();
        return result;
    }

    private static void ensureMainLooper() {
        if (Looper.getMainLooper() != null) return;
        // 本进程从 app_process 启动时 main looper 没自动 prepare，需要手动一次
        Looper.prepareMainLooper();
    }

    /**
     * 把 Bitmap 保存为 PNG 文件。失败时抛异常。
     */
    public static void saveAsPng(Bitmap bmp, String path) throws Exception {
        File f = new File(path);
        File p = f.getParentFile();
        if (p != null && !p.exists()) p.mkdirs();
        try (FileOutputStream fos = new FileOutputStream(f)) {
            bmp.compress(Bitmap.CompressFormat.PNG, 100, fos);
        }
    }
}
