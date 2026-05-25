package com.example.sampleui.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Color
import android.graphics.PixelFormat
import android.os.Build
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.animation.DecelerateInterpolator
import android.widget.FrameLayout
import android.widget.ImageView
import kotlin.math.abs

/**
 * 脚本控制悬浮窗：灰色半透明矩形 + 中间播放/暂停图标。
 *
 * 行为：
 *   - 默认在左下角
 *   - 可拖动；松手时自动吸附到最近的左/右边（带动画）
 *   - 点击切换 ▶/⏸ 状态（启动/停止脚本）
 */
object ControlBubble {

    // 视觉常量
    private const val BUBBLE_DP = 42   // 悬浮球尺寸（dp）
    private const val ICON_DP   = 32   // 中间图标尺寸（dp）
    private const val EDGE_DP   = 0    // 吸附时距屏幕边缘的间距（dp，0 = 完全贴边）
    private const val SNAP_MS   = 220L // 吸附动画时长
    private const val CORNER_DP = 2    // 矩形背景圆角（dp）

    // 状态色（约 90% 不透明）
    //   停止态 = 深灰 #404040
    //   运行态 = 绿色 #4CAF50（Material Green 500，符合"运行中"语义）
    private const val COLOR_IDLE_ARGB    = 0xE6404040.toInt()
    private const val COLOR_RUNNING_ARGB = 0xE64CAF50.toInt()

    private var rootView: FrameLayout? = null
    private var addedToWm: WindowManager? = null
    private var params: WindowManager.LayoutParams? = null
    private var iconView: ImageView? = null
    /** root 的背景 drawable（持有引用方便切换运行/停止态颜色） */
    private var bgDrawable: android.graphics.drawable.GradientDrawable? = null
    private var snapAnimator: ValueAnimator? = null

    /** 缓存的底部导航栏高度（像素），由 show() 从 Activity 取一次后保存 */
    private var bottomBarHeight: Int = 0

    /** 缓存的顶部状态栏高度（像素），由 show() 从 Activity 取一次后保存 */
    private var topBarHeight: Int = 0

    private var isRunning = false
    private var onPlayCb: (() -> Unit)? = null
    private var onPauseCb: (() -> Unit)? = null

    fun show(
        ctx: Context,
        onPlay: () -> Unit,
        onPause: () -> Unit
    ) {
        if (rootView != null) return  // 已显示
        onPlayCb = onPlay
        onPauseCb = onPause

        // 从 Activity 取一次真实底部安全区高度（overlay 自己的 window 拿不到 insets）
        bottomBarHeight = computeBottomBarHeight(ctx)
        topBarHeight = computeTopBarHeight(ctx)

        val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val density = ctx.resources.displayMetrics.density
        fun dp(v: Int): Int = (v * density).toInt()

        // 状态化矩形容器（带 2dp 圆角，约 90% 不透明）
        // 默认停止态深灰；setRunning(true) 时切到绿色
        val bg = android.graphics.drawable.GradientDrawable().apply {
            shape = android.graphics.drawable.GradientDrawable.RECTANGLE
            setColor(COLOR_IDLE_ARGB)
            cornerRadius = CORNER_DP * density
        }
        val root = FrameLayout(ctx).apply {
            background = bg
            layoutParams = ViewGroup.LayoutParams(dp(BUBBLE_DP), dp(BUBBLE_DP))
        }

        // 中间图标
        val icon = ImageView(ctx).apply {
            setImageDrawable(makePlayIcon(ctx))
            layoutParams = FrameLayout.LayoutParams(
                dp(ICON_DP), dp(ICON_DP), Gravity.CENTER
            )
        }
        root.addView(icon)

        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        } else {
            @Suppress("DEPRECATION")
            WindowManager.LayoutParams.TYPE_PHONE
        }

        val lp = WindowManager.LayoutParams(
            // 固定宽高，让 WM 真正按 BUBBLE_DP 渲染（WRAP_CONTENT 会被内部 view 测量出更小尺寸）
            dp(BUBBLE_DP),
            dp(BUBBLE_DP),
            type,
            // FLAG_NOT_TOUCH_MODAL：bubble 矩形外的触摸不被截获，
            //   让桌面 / 其它 App 在 bubble 之外区域照常响应。
            // FLAG_NOT_FOCUSABLE：不抢输入焦点。
            // FLAG_LAYOUT_IN_SCREEN / FLAG_LAYOUT_NO_LIMITS：可以铺到状态栏 / 导航栏区域。
            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS or
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            // 默认左下角：贴左 + 完全贴底，只留导航栏/手势条高度。
            // 用真实屏幕尺寸（含 status / nav bar），与 lp.x/lp.y 坐标系一致。
            val (_, screenH) = getRealScreenSize(wm)
            x = dp(EDGE_DP)
            y = screenH - dp(BUBBLE_DP) - bottomBarHeight
        }

        // 让系统识别为标准可点击窗口（部分 ROM 在自管事件下会丢点击）
        // 关键：root 必须 isClickable + 注册 OnClickListener，否则一些机型/Android 版本
        // 在悬浮窗上不会真正派发点击
        root.isClickable = true
        root.isFocusable = false  // 不抢输入焦点
        root.setOnClickListener {
            // 点击后立即切换到目标状态（运行/停止），无需等回调返回
            // 之前是先触发回调再被外部 setRunning，导致用户感知"延迟"
            if (isRunning) {
                setRunning(false)
                onPauseCb?.invoke()
            } else {
                setRunning(true)
                onPlayCb?.invoke()
            }
        }

        // 拖动 + 点击：根据位移区分；松手时吸附
        // onTouch 不消费 DOWN/UP（返回 false），让系统的 click detection 工作；
        // 只在判定为拖动后接管事件流，并跳过 click。
        root.setOnTouchListener(object : View.OnTouchListener {
            private var initialX = 0
            private var initialY = 0
            private var touchStartX = 0f
            private var touchStartY = 0f
            private var moved = false
            private val touchSlop = dp(6)

            override fun onTouch(v: View, event: MotionEvent): Boolean {
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> {
                        snapAnimator?.cancel()
                        initialX = lp.x
                        initialY = lp.y
                        touchStartX = event.rawX
                        touchStartY = event.rawY
                        moved = false
                        // 不消费 → 系统会继续派发，最终 ACTION_UP 时若没标记 cancel 会触发 onClick
                        return false
                    }
                    MotionEvent.ACTION_MOVE -> {
                        val dx = event.rawX - touchStartX
                        val dy = event.rawY - touchStartY
                        if (!moved && (abs(dx) > touchSlop || abs(dy) > touchSlop)) {
                            moved = true
                        }
                        if (moved) {
                            // MOVE 跟手指自由移动，不在这里 clamp。
                            // 松手时由 snapToEdge 统一做边界校正 + 吸附动画，
                            // 这样体验丝滑、不会"硬碰边界"卡顿。
                            lp.x = initialX + dx.toInt()
                            lp.y = initialY + dy.toInt()
                            try { wm.updateViewLayout(rootView, lp) } catch (_: Exception) {}
                            return true  // 消费 MOVE，避免父级再处理
                        }
                        return false
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        if (moved) {
                            // 已判定为拖动：吸附 + 消费事件（防止触发 onClick）
                            snapToEdge(ctx, wm, lp)
                            return true
                        }
                        // 没拖动 → 不消费 → 系统会触发 setOnClickListener
                        return false
                    }
                }
                return false
            }
        })

        wm.addView(root, lp)

        rootView = root
        addedToWm = wm
        params = lp
        iconView = icon
        bgDrawable = bg
        isRunning = false
    }

    /**
     * 根据当前位置，把悬浮球用动画吸附到最近的屏幕边（左 / 右）。
     * 同时把 y 限制在屏幕可见范围内。
     */
    private fun snapToEdge(ctx: Context, wm: WindowManager, lp: WindowManager.LayoutParams) {
        val density = ctx.resources.displayMetrics.density
        fun dp(v: Int): Int = (v * density).toInt()

        val (screenW, screenH) = getRealScreenSize(wm)

        val bubbleSize = dp(BUBBLE_DP)
        val edge = dp(EDGE_DP)

        // 决定目标 X：把球的中心点跟屏幕中线比较
        val centerX = lp.x + bubbleSize / 2
        val targetX = if (centerX < screenW / 2) {
            edge                                  // 吸附到左
        } else {
            screenW - bubbleSize - edge           // 吸附到右
        }

        // Y 限制在屏幕内：顶部留状态栏高度，底部留导航栏高度
        val minY = topBarHeight
        val maxY = screenH - bubbleSize - bottomBarHeight
        val targetY = lp.y.coerceIn(minY, maxY)

        android.util.Log.d(
            "ControlBubble",
            "snap: lp=(${lp.x},${lp.y}) screen=${screenW}x${screenH} -> target=($targetX,$targetY)"
        )

        // 动画从当前位置平滑过渡到目标
        val startX = lp.x
        val startY = lp.y
        snapAnimator?.cancel()
        snapAnimator = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = SNAP_MS
            interpolator = DecelerateInterpolator()
            addUpdateListener { animator ->
                val t = animator.animatedValue as Float
                lp.x = (startX + (targetX - startX) * t).toInt()
                lp.y = (startY + (targetY - startY) * t).toInt()
                try { wm.updateViewLayout(rootView, lp) } catch (_: Exception) {}
            }
            start()
        }
    }

    /**
     * 切换运行/停止态：图标 + 背景色一起变。
     *   true  → 显示暂停 ⏸ + 绿色背景（脚本运行中）
     *   false → 显示播放 ▶ + 深灰背景（脚本未运行）
     */
    fun setRunning(running: Boolean) {
        val ctx = rootView?.context ?: return
        val icon = iconView ?: return
        isRunning = running
        icon.setImageDrawable(
            if (running) makePauseIcon(ctx) else makePlayIcon(ctx)
        )
        bgDrawable?.setColor(
            if (running) COLOR_RUNNING_ARGB else COLOR_IDLE_ARGB
        )
    }

    fun hide(ctx: Context) {
        snapAnimator?.cancel()
        snapAnimator = null
        val v = rootView ?: return
        val wm = addedToWm ?: ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        try { wm.removeView(v) } catch (_: Exception) {}
        rootView = null
        addedToWm = null
        params = null
        iconView = null
        bgDrawable = null
        isRunning = false
        onPlayCb = null
        onPauseCb = null
    }

    fun isShowing(): Boolean = rootView != null

    /**
     * 把悬浮按钮位置复位到默认左下角。
     *
     * 与 `bubble.SetPositionPercent(0, 1)` 行为完全一致：
     *   - 中心点对齐到屏幕左下角 (0, screenH)
     *   - 再 clamp 回屏幕可见区域：x=0、y=screenH - bubble - bottomBarHeight
     *
     * 这样 Lua 端 `bubble.SetPositionPercent(0, 1)` 与 App 端的"默认位置"
     * 保持一致，用户可预期。
     */
    fun resetToDefaultPosition() {
        if (rootView == null) return
        setPositionPercent(0f, 1f)
    }

    /**
     * 切换悬浮按钮的可见性（不销毁，仅隐藏 view），状态/位置/运行态都会保留。
     *
     * 用途：MainActivity 在前台时把悬浮按钮藏起来，避免挡住主页 UI；
     * 主程序进入后台时再 setVisible(true) 让用户能在桌面/其它 App 上看到它。
     *
     * 若悬浮按钮根本没 show 过，本方法是 no-op。
     */
    fun setVisible(visible: Boolean) {
        applyVisibility(externalForceHidden = !visible, hideStackDelta = 0)
    }

    // ---- "前台 Activity 在显示时藏 bubble" 的引用计数 ----
    //
    // 多个 Activity 可能在过渡瞬间同时被认为"前台"：
    //   旧 Activity onStop 之前，新 Activity 已经 onStart。
    // 直接在 onResume/onPause 调 setVisible(true/false) 会让 bubble 在切换时
    // 闪一下。改成 push/pop 引用计数，只要 stack > 0 就藏，过渡期间不闪。
    private var hideStack: Int = 0
    private var externalForceHidden: Boolean = false

    /**
     * Activity 进入前台时调（onStart）。多次调用会累加，必须配对调用 popHide。
     * 计数 > 0 时 bubble 不可见。
     */
    fun pushHide() {
        applyVisibility(externalForceHidden, +1)
    }

    /**
     * Activity 退出前台时调（onStop）。
     */
    fun popHide() {
        applyVisibility(externalForceHidden, -1)
    }

    /** 同时维护两种隐藏来源：外部 setVisible(false) 与 push/pop 引用计数。 */
    private fun applyVisibility(externalForceHidden: Boolean, hideStackDelta: Int) {
        this.externalForceHidden = externalForceHidden
        hideStack = (hideStack + hideStackDelta).coerceAtLeast(0)
        val v = rootView ?: return
        val shouldHide = externalForceHidden || hideStack > 0
        v.visibility = if (shouldHide) View.GONE else View.VISIBLE
    }

    /**
     * 按屏幕百分比定位悬浮按钮（左上角 = (0, 0)，右下角 = (1, 1)）。
     *
     * 计算约定：把"悬浮按钮的中心"对齐到 `(xp * screenW, yp * screenH)`，
     * 然后再用 [coerceIn] 把按钮整体框死在屏幕可见区域里（避免越界）。
     *
     * 行为细节：
     *   - 取消正在播放的吸附动画（避免参数被覆盖）
     *   - 若悬浮窗未显示则直接 no-op
     *   - 直接按 lp.x/lp.y 跳转，不做插值动画（脚本侧通常希望立刻生效）
     *
     * @param xp 横向百分比，范围 [0.0, 1.0]，0=左 1=右
     * @param yp 纵向百分比，范围 [0.0, 1.0]，0=上 1=下
     */
    fun setPositionPercent(xp: Float, yp: Float) {
        val root = rootView ?: return
        val wm = addedToWm ?: return
        val lp = params ?: return

        snapAnimator?.cancel()

        val ctx = root.context
        val density = ctx.resources.displayMetrics.density

        val (screenW, screenH) = getRealScreenSize(wm)

        val bubbleSize = (BUBBLE_DP * density).toInt()
        val xClamped = xp.coerceIn(0f, 1f)
        val yClamped = yp.coerceIn(0f, 1f)

        // 中心对齐：lp.x/y 是左上角坐标，所以减去半个尺寸
        val targetX = (screenW * xClamped - bubbleSize / 2f).toInt()
        val targetY = (screenH * yClamped - bubbleSize / 2f).toInt()

        // 在屏幕可见区域内：左右不出界，上下分别留出状态栏 / 导航栏高度
        lp.x = targetX.coerceIn(0, screenW - bubbleSize)
        lp.y = targetY.coerceIn(topBarHeight, screenH - bubbleSize - bottomBarHeight)

        try { wm.updateViewLayout(root, lp) } catch (_: Exception) {}
    }

    /**
     * 获取屏幕底部"不应放置悬浮控件"的高度（像素）。
     *
     * 来源优先级：
     *  1. Activity 的 decorView.rootWindowInsets（最准，能在手势导航下拿到真实手势条高度）
     *  2. systemBars/mandatorySystemGestures/tappable（API 30+，多个 inset 取最大）
     *  3. navigation_bar_height 资源（旧 API）
     *  4. 兜底 56dp
     *
     * 取最大值后再加 16dp 视觉间距。
     */
    private fun computeBottomBarHeight(ctx: Context): Int {
        val density = ctx.resources.displayMetrics.density
        val extraPadding = (32 * density).toInt()

        var fromDecorView = 0
        var systemBars = 0
        var mandatoryGestures = 0
        var tappable = 0
        var resourceValue = 0

        // 1) 从 Activity 的 decorView 拿（最可靠）
        val activity = findActivity(ctx)
        if (activity != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            try {
                val insets = activity.window.decorView.rootWindowInsets
                if (insets != null) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        // 取多种 inset 中最大的：systemBars / mandatoryGestures / tappable
                        val sb = insets.getInsets(android.view.WindowInsets.Type.systemBars()).bottom
                        val mg = insets.getInsets(android.view.WindowInsets.Type.mandatorySystemGestures()).bottom
                        val tp = insets.getInsets(android.view.WindowInsets.Type.tappableElement()).bottom
                        fromDecorView = maxOf(sb, mg, tp)
                    } else {
                        @Suppress("DEPRECATION")
                        fromDecorView = insets.systemWindowInsetBottom
                    }
                }
            } catch (_: Exception) { /* 降级 */ }
        }

        // 2) 从 maximumWindowMetrics 兜底（注意：在 overlay window 上下文经常返回 0）
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
                val insets = wm.maximumWindowMetrics.windowInsets
                systemBars = insets.getInsets(android.view.WindowInsets.Type.systemBars()).bottom
                mandatoryGestures = insets.getInsets(android.view.WindowInsets.Type.mandatorySystemGestures()).bottom
                tappable = insets.getInsets(android.view.WindowInsets.Type.tappableElement()).bottom
            } catch (_: Exception) { /* 降级 */ }
        }

        // 3) 资源
        val res = ctx.resources
        val resId = res.getIdentifier("navigation_bar_height", "dimen", "android")
        if (resId > 0) resourceValue = res.getDimensionPixelSize(resId)

        // 取所有来源最大值，再加视觉间距；最后用 56dp 兜底
        val maxRaw = maxOf(fromDecorView, systemBars, mandatoryGestures, tappable, resourceValue)
        val minDp = (56 * density).toInt()
        val finalHeight = maxOf(maxRaw, minDp) + extraPadding

        android.util.Log.d(
            "ControlBubble",
            "navInsets: decorView=$fromDecorView systemBars=$systemBars " +
                    "mandatoryGestures=$mandatoryGestures tappable=$tappable " +
                    "resource=$resourceValue → max=$maxRaw +extra=$extraPadding = $finalHeight"
        )
        return finalHeight
    }

    /**
     * 获取屏幕顶部"不应放置悬浮控件"的高度（像素）。
     *
     * 来源优先级（与 [computeBottomBarHeight] 对称）：
     *  1. Activity 的 decorView.rootWindowInsets 顶部（systemBars）
     *  2. status_bar_height 资源（旧 API 兜底）
     *  3. 兜底 24dp
     *
     * 返回值再加 8dp 视觉间距，让 bubble 不会顶到状态栏紧贴。
     */
    private fun computeTopBarHeight(ctx: Context): Int {
        val density = ctx.resources.displayMetrics.density
        val extraPadding = (8 * density).toInt()

        var fromDecorView = 0
        var resourceValue = 0

        val activity = findActivity(ctx)
        if (activity != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            try {
                val insets = activity.window.decorView.rootWindowInsets
                if (insets != null) {
                    fromDecorView = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        insets.getInsets(android.view.WindowInsets.Type.systemBars()).top
                    } else {
                        @Suppress("DEPRECATION")
                        insets.systemWindowInsetTop
                    }
                }
            } catch (_: Exception) { /* 降级 */ }
        }

        val res = ctx.resources
        val resId = res.getIdentifier("status_bar_height", "dimen", "android")
        if (resId > 0) resourceValue = res.getDimensionPixelSize(resId)

        val raw = maxOf(fromDecorView, resourceValue, (24 * density).toInt())
        val finalHeight = raw + extraPadding

        android.util.Log.d(
            "ControlBubble",
            "topInsets: decorView=$fromDecorView resource=$resourceValue " +
                    "→ raw=$raw +extra=$extraPadding = $finalHeight"
        )
        return finalHeight
    }

    /** 从 Context 链找 Activity（ContextWrapper 可能是 ContextThemeWrapper / Service） */
    private fun findActivity(ctx: Context): android.app.Activity? {
        var c: Context? = ctx
        while (c is android.content.ContextWrapper) {
            if (c is android.app.Activity) return c
            c = c.baseContext
        }
        return null
    }

    /**
     * 取真实屏幕物理尺寸（含状态栏、导航栏），与 bubble window 的 lp.x/lp.y 坐标系一致。
     *
     * 注意 DisplayMetrics.heightPixels 在 MIUI / 部分 ROM 上返回的是
     * Activity 内容区高度（不含状态栏），跟 lp.y 坐标系不一致，会让
     * coerceIn 的边界值偏小，导致 bubble 拖到屏幕底部时看起来"卡住"。
     * 这里统一从 WindowManager 拿真实尺寸。
     */
    private fun getRealScreenSize(wm: WindowManager): Pair<Int, Int> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bounds = wm.maximumWindowMetrics.bounds
            bounds.width() to bounds.height()
        } else {
            val pt = android.graphics.Point()
            @Suppress("DEPRECATION")
            wm.defaultDisplay.getRealSize(pt)
            pt.x to pt.y
        }
    }

    // ---------- 图标构造（不依赖图片资源，纯代码绘制）----------

    /** 播放图标 ▶（白色实心三角形） */
    private fun makePlayIcon(ctx: Context): android.graphics.drawable.Drawable {
        val density = ctx.resources.displayMetrics.density
        val size = (ICON_DP * density).toInt()
        val bmp = android.graphics.Bitmap.createBitmap(size, size, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = android.graphics.Paint.Style.FILL
        }
        // 三角形（视觉居中：稍微往右偏一点点抵消"右尖"的视错觉）
        val path = android.graphics.Path().apply {
            moveTo(size * 0.32f, size * 0.18f)
            lineTo(size * 0.82f, size * 0.50f)
            lineTo(size * 0.32f, size * 0.82f)
            close()
        }
        canvas.drawPath(path, paint)
        return android.graphics.drawable.BitmapDrawable(ctx.resources, bmp)
    }

    /** 暂停图标 ⏸（两条白色竖条） */
    private fun makePauseIcon(ctx: Context): android.graphics.drawable.Drawable {
        val density = ctx.resources.displayMetrics.density
        val size = (ICON_DP * density).toInt()
        val bmp = android.graphics.Bitmap.createBitmap(size, size, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bmp)
        val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = android.graphics.Paint.Style.FILL
        }
        val barW = size * 0.18f
        val gap = size * 0.16f
        val cx = size * 0.5f
        val top = size * 0.20f
        val bottom = size * 0.80f
        canvas.drawRect(cx - gap / 2 - barW, top, cx - gap / 2, bottom, paint)
        canvas.drawRect(cx + gap / 2, top, cx + gap / 2 + barW, bottom, paint)
        return android.graphics.drawable.BitmapDrawable(ctx.resources, bmp)
    }
}
