package com.easylua.support;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;

import java.util.ArrayList;
import java.util.List;

/**
 * 屏幕浮窗：Highlight 自绘（Canvas + Paint）。
 *
 * Toast 不在这里实现 —— 走 am broadcast 给 SampleUI APK 的 ToastReceiver，
 * 详见 jni/ui.c::Ui_Toast。在 root 进程里直接 Toast.makeText 会被 MIUI WMS
 * 用 caller package 校验拦截，那条路在 root 下走不通。
 *
 * Highlight 设计参考 SikuliX：
 *   - 空心矩形 + 可选标签
 *   - 多个矩形可同时存在
 *   - 可选时长，<= 0 表示常驻直到 highlightOff()
 *
 * 显示通过 WindowManager.addView，type 选 TYPE_APPLICATION_OVERLAY (SDK 26+)
 * 或 TYPE_SYSTEM_ALERT (旧)。我们以 root (uid=0) 启动，两者都能加。
 *
 * 单例 view（OverlayView），所有 highlight 项目挂在它的 list 里，每帧 onDraw
 * 重画。
 *
 * 线程安全：
 *   - WindowManager 操作必须在有 Looper 的线程上做。我们用专用 HandlerThread。
 *   - Lua 端调来的 highlight 都 post 到这个 handler。
 */
public final class Overlay {

    private Overlay() {}

    /* =========================================================
     * 单例 view + 自己的 handler 线程
     * ========================================================= */

    private static volatile OverlayView rootView;
    private static volatile WindowManager attachedWm;
    private static volatile Handler uiHandler;

    private static synchronized Handler handler() {
        if (uiHandler == null) {
            HandlerThread t = new HandlerThread("easylua-overlay");
            t.setDaemon(true);
            t.start();
            uiHandler = new Handler(t.getLooper());
        }
        return uiHandler;
    }

    /** 懒创建并 attach 到 WindowManager。必须在 handler 线程调。 */
    private static OverlayView ensureView() {
        if (rootView != null) return rootView;
        Context ctx = SystemContext.get();
        // MIUI 兜底：禁用 ViewRootImpl 里的 ForceDarkHelper 等 hook，避免 addView 时 NPE
        MiuiHooks.apply();
        WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        if (wm == null) return null;

        OverlayView v = new OverlayView(ctx);
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                pickWindowType(),
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.START;
        if (Build.VERSION.SDK_INT >= 28) lp.layoutInDisplayCutoutMode = 3;

        try {
            wm.addView(v, lp);
        } catch (Throwable t) {
            System.out.println("[Overlay] addView FAILED: " + t);
            t.printStackTrace(System.out);
            return null;
        }
        rootView = v;
        attachedWm = wm;
        System.out.println("[Overlay] root view attached, type=" + lp.type);
        return v;
    }

    private static int pickWindowType() {
        return Build.VERSION.SDK_INT >= 26 ? 2038 /* TYPE_APPLICATION_OVERLAY */
                                            : 2003 /* TYPE_SYSTEM_ALERT */;
    }

    /* =========================================================
     * Highlight：空心矩形 + 可选文字标签
     * ========================================================= */

    /**
     * 显示一个 highlight。
     *
     * @param x       矩形左上 x
     * @param y       矩形左上 y
     * @param w       宽
     * @param h       高
     * @param colorArgb  ARGB 颜色，<= 0 表示用红色 0xFFFF0000
     * @param durMs   显示时长 ms，<=0 表示常驻直到 highlightOff()
     * @param label   矩形上方的标签文字，null 或空串不画
     */
    public static void highlight(final int x, final int y, final int w, final int h,
                                 final int colorArgb, final int durMs, final String label) {
        handler().post(new Runnable() {
            @Override public void run() {
                OverlayView v = ensureView();
                if (v == null) return;
                int color = (colorArgb == 0) ? 0xFFFF0000 : colorArgb;
                final HighlightItem item = new HighlightItem(x, y, w, h, color, label);
                v.addHighlight(item);
                if (durMs > 0) {
                    handler().postDelayed(new Runnable() {
                        @Override public void run() {
                            if (rootView != null) rootView.removeHighlight(item);
                        }
                    }, durMs);
                }
            }
        });
    }

    /** 立即清掉全部 highlight */
    public static void highlightOff() {
        handler().post(new Runnable() {
            @Override public void run() {
                if (rootView != null) rootView.clearHighlights();
            }
        });
    }

    /* =========================================================
     * 数据 + View
     * ========================================================= */

    private static final class HighlightItem {
        final int x, y, w, h;
        final int color;
        final String label;
        HighlightItem(int x, int y, int w, int h, int color, String label) {
            this.x = x; this.y = y; this.w = w; this.h = h;
            this.color = color; this.label = label;
        }
    }

    /**
     * 透明全屏 View，所有 highlight 都在这一个 View 上画。
     * 避免每个 highlight 创建独立 view → addView 调用 + Surface 资源开销大。
     */
    private static final class OverlayView extends View {

        private final List<HighlightItem> highlights = new ArrayList<>();

        // 矩形描边
        private final Paint framePaint;
        // 文字
        private final Paint labelPaint;
        // 文字背景
        private final Paint labelBgPaint;

        // 缓存 RectF / Rect，避免每帧分配
        private final Rect tmpFrame = new Rect();
        private final RectF tmpRoundRect = new RectF();

        OverlayView(Context ctx) {
            super(ctx);
            float density = ctx.getResources().getDisplayMetrics().density;

            framePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
            framePaint.setStyle(Paint.Style.STROKE);
            framePaint.setStrokeWidth(3f * density);

            labelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
            labelPaint.setColor(Color.WHITE);
            labelPaint.setTextSize(14f * density);
            // 用 DEFAULT_BOLD 触发不到 MIUI var font hook（DEFAULT 倒可能）
            try {
                labelPaint.setTypeface(Typeface.DEFAULT_BOLD);
            } catch (Throwable ignored) {}

            labelBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
            labelBgPaint.setStyle(Paint.Style.FILL);
        }

        synchronized void addHighlight(HighlightItem h) { highlights.add(h); invalidate(); }

        synchronized void removeHighlight(HighlightItem h) {
            highlights.remove(h);
            invalidate();
        }

        synchronized void clearHighlights() {
            highlights.clear();
            invalidate();
        }

        @Override
        protected synchronized void onDraw(Canvas canvas) {
            for (int i = 0; i < highlights.size(); i++) {
                HighlightItem h = highlights.get(i);
                framePaint.setColor(h.color);
                tmpFrame.set(h.x, h.y, h.x + h.w, h.y + h.h);
                canvas.drawRect(tmpFrame, framePaint);
                if (h.label != null && !h.label.isEmpty()) {
                    drawLabelAbove(canvas, tmpFrame, h.label, h.color);
                }
            }
        }

        private void drawLabelAbove(Canvas canvas, Rect frame, String text, int frameColor) {
            float density = getResources().getDisplayMetrics().density;
            float padX = 8f * density;
            float padY = 4f * density;

            float textW = labelPaint.measureText(text);
            Paint.FontMetrics fm = labelPaint.getFontMetrics();
            float textH = fm.descent - fm.ascent;

            float cx = frame.left + frame.width() / 2f;
            float bgW = textW + padX * 2;
            float bgH = textH + padY * 2;

            float bgLeft = cx - bgW / 2f;
            float bgRight = cx + bgW / 2f;
            float bgTop = Math.max(0, frame.top - bgH);
            float bgBottom = bgTop + bgH;

            // 文字背景：用 frame 颜色但不透明度提高
            labelBgPaint.setColor((frameColor & 0x00FFFFFF) | 0xCC000000);
            tmpRoundRect.set(bgLeft, bgTop, bgRight, bgBottom);
            canvas.drawRoundRect(tmpRoundRect, 4f * density, 4f * density, labelBgPaint);

            canvas.drawText(text, cx - textW / 2f, bgBottom - padY - fm.descent, labelPaint);
        }
    }
}
