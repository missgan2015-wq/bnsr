package com.example.sampleui.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.Rect
import android.os.Build
import android.view.Gravity
import android.view.View
import android.view.WindowManager

/**
 * 屏幕浮窗高亮：在任意区域绘制带文字的方框。
 * 仿 AnkuLua 的 region:highlight() / setHighlightStyle()。
 *
 * 使用：
 *   HighlightOverlay.show(ctx, Rect(100,200,400,500), "Stop!", 0xFFFF0000.toInt())
 *   HighlightOverlay.hide(ctx)
 */
object HighlightOverlay {

    private var addedView: HighlightView? = null
    private var addedToWm: WindowManager? = null

    fun show(ctx: Context, rect: Rect, text: String?, frameColor: Int) {
        hide(ctx) // 先清掉上一次的

        val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val view = HighlightView(ctx).apply {
            setData(rect, text, frameColor)
        }

        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        } else {
            @Suppress("DEPRECATION")
            WindowManager.LayoutParams.TYPE_PHONE
        }

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            type,
            // 不可获焦 + 透传触摸 + 全屏覆盖
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                    WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
        }

        wm.addView(view, params)
        addedView = view
        addedToWm = wm
    }

    fun hide(ctx: Context) {
        val v = addedView ?: return
        val wm = addedToWm ?: ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        try { wm.removeView(v) } catch (e: Exception) { /* 忽略已经移除的情况 */ }
        addedView = null
        addedToWm = null
    }

    /** 真正绘制的 View */
    private class HighlightView(ctx: Context) : View(ctx) {

        private var rect: Rect = Rect()
        private var text: String? = null

        private val framePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = 6f
        }

        private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            textSize = 48f
            typeface = android.graphics.Typeface.DEFAULT_BOLD
        }

        private val textBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.FILL
        }

        fun setData(r: Rect, t: String?, color: Int) {
            rect = Rect(r)
            text = t
            framePaint.color = color
            textBgPaint.color = (color and 0x00FFFFFF) or 0xCC000000.toInt()
            invalidate()
        }

        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            // 画方框
            canvas.drawRect(rect, framePaint)

            // 画文字（在框上方居中）
            val txt = text ?: return
            val fm = textPaint.fontMetrics
            val textWidth = textPaint.measureText(txt)
            val textHeight = fm.descent - fm.ascent
            val padX = 16f
            val padY = 8f

            val cx = rect.left + (rect.width() / 2f)
            val bgLeft = cx - textWidth / 2f - padX
            val bgRight = cx + textWidth / 2f + padX
            val bgTop = (rect.top - textHeight - padY * 2).coerceAtLeast(0f)
            val bgBottom = bgTop + textHeight + padY * 2

            canvas.drawRect(bgLeft, bgTop, bgRight, bgBottom, textBgPaint)
            canvas.drawText(
                txt,
                cx - textWidth / 2f,
                bgBottom - padY - fm.descent,
                textPaint
            )
        }
    }
}
