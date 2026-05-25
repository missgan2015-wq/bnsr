package com.example.sampleui.ui

import android.app.Activity
import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.support.v7.app.AlertDialog
import android.support.v7.app.AppCompatDialog
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.*
import java.io.IOException
import java.util.Calendar

/**
 * 把 LuaUIParser 解析出的 Op 列表渲染成原生 Android 视图树。
 *
 * 核心数据结构：
 *  - 容器栈：begin* 入栈，end* 出栈，addX 加到栈顶容器
 *  - 上一控件 lastView：修饰符的作用对象
 *  - widgets/widgetOps：控件注册表，用于关闭后收集结果
 *
 * 嵌套支持：
 *   beginGroup/endGroup —— 圆角分组卡片
 *   beginRow/endRow     —— 水平行
 *   beginTabs/endTabs +
 *   beginTab/endTab     —— 标签页（按钮+内容切换）
 */
class OpRenderer(private val activity: Activity) {

    /** 控件注册：varName -> View */
    private val widgets = LinkedHashMap<String, View>()

    /** 控件类型记录：varName -> Op（用于按类型解释 result） */
    private val widgetOps = LinkedHashMap<String, LuaUIParser.Op>()

    /** 关闭后填充的结果 */
    val result = HashMap<String, Any?>()

    /**
     * bindVisible 关联：varName -> 它所跟随的 checkbox 变量名。
     * 渲染完毕后统一应用一次，并给 checkbox 加 listener 实时刷新。
     */
    private val visibilityBindings = mutableMapOf<View, String>()

    fun show(
        ui: LuaUIParser.ParseResult,
        fullScreen: Boolean,
        onCancel: (() -> Unit)? = null,
        onDone: (Map<String, Any?>) -> Unit
    ) {
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            // 左右 padding 设为 0，让 Tabs 等"块状"控件可以撑满对话框宽度。
            // 单个控件需要左右间距时各自加 padding/margin。
            setPadding(0, dp(8), 0, dp(8))
        }

        renderInto(root, ui.ops)
        applyVisibilityBindings()

        // ── 整体三段式布局 ──
        // [标题栏] (固定)
        // [ScrollView] (可滚动，weight=1 占据中间所有剩余空间)
        // [底部分隔线 + footer] (固定)
        //
        // 注意：不主动给容器设 background，让对话框沿用主题里的 dialog 背景
        // （系统会提供圆角 + 阴影 + 默认底色），否则会变成一块大白板。
        val container = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        }

        // 1) 标题栏
        if (ui.title.isNotEmpty()) {
            container.addView(TextView(activity).apply {
                text = ui.title
                textSize = 18f
                setTypeface(typeface, Typeface.BOLD)
                setTextColor(DARK_TEXT_PRIMARY)  // 跟随深色窗口
                val ph = dp(20)
                val pv = dp(16)
                setPadding(ph, pv, ph, pv)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            })
            // 标题下方分隔线
            container.addView(View(activity).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(1)
                )
                setBackgroundColor(DARK_DIVIDER)
            })
        }

        // 2) 内容滚动区（weight=1 占据中间剩余空间）
        val scroll = ScrollView(activity).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
            )
            addView(root, ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ))
        }
        container.addView(scroll)

        // 3) 底部分隔线 + footer（固定在底部，不参与滚动）
        container.addView(View(activity).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1)
            )
            setBackgroundColor(FOOTER_DIVIDER)
        })

        var dialogRef: AppCompatDialog? = null
        // 是否点击了"确定"。默认 false；用 dismiss 兜底处理"取消 / 返回键 / 点对话框外"
        var confirmed = false
        val footer = createFooter(
            onCancel = { dialogRef?.dismiss() },
            onConfirm = {
                collectResults()
                confirmed = true
                dialogRef?.dismiss()
                onDone(result)
            }
        )
        container.addView(footer)

        // 用 AppCompatDialog 替代 AlertDialog，自行控制布局，避免 AlertDialog 把
        // 整个 setView 内容塞进它内部的 ScrollView，导致 footer 跟随滚动。
        // bubble 隐藏由前台 Activity 的 onStart/onStop 引用计数管理，这里不再单独处理。
        val dialog = AppCompatDialog(activity).apply {
            supportRequestWindowFeature(android.view.Window.FEATURE_NO_TITLE)
            setContentView(container)
            setCancelable(true)
            // 任何方式关闭（取消按钮/返回键/点外/dismiss）都会触发，
            // 如果用户没确认就关，转发到 onCancel
            setOnDismissListener {
                if (!confirmed) onCancel?.invoke()
            }
        }
        // 给整个对话框窗口设一个纯色背景（无边框）
        dialog.window?.setBackgroundDrawable(GradientDrawable().apply {
            setColor(DIALOG_BG)
        })
        dialogRef = dialog
        dialog.show()

        // 应用尺寸/位置
        applyDialogWindow(dialog, ui, fullScreen)
    }

    /**
     * 根据 ParseResult 中的尺寸/位置参数调整 dialog window。
     */
    private fun applyDialogWindow(
        dialog: AppCompatDialog,
        ui: LuaUIParser.ParseResult,
        fullScreen: Boolean
    ) {
        val window = dialog.window ?: return
        val dm = activity.resources.displayMetrics

        var w: Int = WindowManager.LayoutParams.WRAP_CONTENT
        var h: Int = WindowManager.LayoutParams.WRAP_CONTENT

        // 尺寸解析约定：
        //   -1 = MATCH_PARENT（全屏铺满）
        //   -2 = WRAP_CONTENT（自适应内容）
        //    0 = 未设置（走 fullScreen 兜底，否则 WRAP_CONTENT）
        //   正数 = dp
        if (ui.widthPercent > 0f) {
            w = (dm.widthPixels * ui.widthPercent).toInt()
        } else if (ui.widthDp != 0) {
            w = when (ui.widthDp) {
                -1 -> WindowManager.LayoutParams.MATCH_PARENT
                -2 -> WindowManager.LayoutParams.WRAP_CONTENT
                else -> dp(ui.widthDp)
            }
        } else if (fullScreen) {
            w = WindowManager.LayoutParams.MATCH_PARENT
        }

        if (ui.heightPercent > 0f) {
            h = (dm.heightPixels * ui.heightPercent).toInt()
        } else if (ui.heightDp != 0) {
            h = when (ui.heightDp) {
                -1 -> WindowManager.LayoutParams.MATCH_PARENT
                -2 -> WindowManager.LayoutParams.WRAP_CONTENT
                else -> dp(ui.heightDp)
            }
        } else if (fullScreen) {
            h = WindowManager.LayoutParams.MATCH_PARENT
        }

        window.setLayout(w, h)

        val attrs = window.attributes
        attrs.gravity = parseGravity(ui.gravity)
        attrs.x = dp(ui.offsetXDp)
        attrs.y = dp(ui.offsetYDp)
        window.attributes = attrs
    }

    /**
     * 解析 Lua 端的位置字符串到 Android Gravity。
     * 支持："center"、"top"、"bottom"、"left"、"right"、
     *       "top-left"、"top-right"、"bottom-left"、"bottom-right"
     * 也支持 "top-center"、"center-left" 等同义写法。
     * 空字符串返回默认 CENTER。
     */
    private fun parseGravity(s: String): Int {
        if (s.isEmpty()) return Gravity.CENTER
        val parts = s.lowercase().split("-", "_", " ").filter { it.isNotEmpty() }
        var g = 0
        var sawVertical = false
        var sawHorizontal = false
        for (p in parts) {
            when (p) {
                "top"    -> { g = g or Gravity.TOP;    sawVertical = true }
                "bottom" -> { g = g or Gravity.BOTTOM; sawVertical = true }
                "left", "start" -> { g = g or Gravity.START; sawHorizontal = true }
                "right", "end"  -> { g = g or Gravity.END;   sawHorizontal = true }
                "center" -> {
                    if (!sawVertical && !sawHorizontal) g = g or Gravity.CENTER
                    else if (!sawVertical) g = g or Gravity.CENTER_VERTICAL
                    else if (!sawHorizontal) g = g or Gravity.CENTER_HORIZONTAL
                }
            }
        }
        // 只指定了一个方向时，另一个方向默认居中
        if (g != 0 && (g and (Gravity.TOP or Gravity.BOTTOM or Gravity.CENTER_VERTICAL)) == 0) {
            g = g or Gravity.CENTER_VERTICAL
        }
        if (g != 0 && (g and (Gravity.START or Gravity.END or Gravity.CENTER_HORIZONTAL)) == 0) {
            g = g or Gravity.CENTER_HORIZONTAL
        }
        return if (g == 0) Gravity.CENTER else g
    }

    /** 创建底部按钮区：[取消] | 竖线 | [确定]，深色背景白色文字 */
    private fun createFooter(onCancel: () -> Unit, onConfirm: () -> Unit): LinearLayout {
        val footer = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(FOOTER_BG)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(56)
            )
        }

        val btnStyle: (Button, () -> Unit) -> Unit = { btn, action ->
            btn.apply {
                isAllCaps = false
                textSize = 18f
                setTextColor(FOOTER_TEXT)
                setBackgroundColor(0x00000000)  // 透明，靠水波纹反馈
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f)
                setOnClickListener { action() }
            }
        }

        val cancelBtn = Button(activity).apply { text = "取消" }
        btnStyle(cancelBtn, onCancel)

        val divider = View(activity).apply {
            layoutParams = LinearLayout.LayoutParams(dp(1), ViewGroup.LayoutParams.MATCH_PARENT).apply {
                topMargin = dp(8); bottomMargin = dp(8)
            }
            setBackgroundColor(FOOTER_DIVIDER)
        }

        val confirmBtn = Button(activity).apply { text = "确定" }
        btnStyle(confirmBtn, onConfirm)

        footer.addView(cancelBtn)
        footer.addView(divider)
        footer.addView(confirmBtn)
        return footer
    }

    /**
     * 把 ops 渲染到 rootContainer。
     * 用一个栈管理容器嵌套，lastView 跟踪刚加的视图给修饰符用。
     *
     * darkContextDepth：跟踪当前是否处于"深色上下文"（在 Tabs 内部）。
     * 渲染 TextView / EditText / CheckBox / Spinner 等控件时，
     * 如果在深色上下文则把文字改为白色，hint 改为浅灰。
     */
    private fun renderInto(rootContainer: LinearLayout, ops: List<LuaUIParser.Op>) {
        val containerStack = ArrayDeque<LinearLayout>().apply { addLast(rootContainer) }
        var lastView: View? = null
        // 整个对话框背景就是深色，所以默认就在深色上下文里
        var darkContextDepth = 1
        val isDark: () -> Boolean = { darkContextDepth > 0 }

        // tabs 状态：每开启一组 tabs 就 push 一份；endTabs 时 pop
        val tabsStack = ArrayDeque<TabsContext>()

        for (op in ops) {
            val parent = containerStack.last()

            when (op) {
                // ── 容器开闭 ──
                is LuaUIParser.Op.GroupBegin -> {
                    val (outer, content) = createGroup(op.title, dark = isDark())
                    parent.addView(outer)
                    containerStack.addLast(content)
                    lastView = outer
                }
                LuaUIParser.Op.GroupEnd -> {
                    if (containerStack.size > 1) containerStack.removeLast()
                }
                LuaUIParser.Op.RowBegin -> {
                    val row = LinearLayout(activity).apply {
                        orientation = LinearLayout.HORIZONTAL
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.WRAP_CONTENT
                        ).apply { topMargin = dp(4); bottomMargin = dp(4) }
                        gravity = Gravity.CENTER_VERTICAL
                    }
                    parent.addView(row)
                    containerStack.addLast(row)
                    lastView = row
                }
                LuaUIParser.Op.RowEnd -> {
                    if (containerStack.size > 1) containerStack.removeLast()
                }

                // ── Tabs ──
                LuaUIParser.Op.TabsBegin -> {
                    val tabs = createTabs()
                    parent.addView(tabs.outer)
                    tabsStack.addLast(tabs)
                    darkContextDepth++
                    lastView = tabs.outer
                }
                LuaUIParser.Op.TabsEnd -> {
                    if (tabsStack.isNotEmpty()) {
                        val tabs = tabsStack.removeLast()
                        // 默认选中第一个 tab
                        if (tabs.tabs.isNotEmpty()) {
                            tabs.selectTab(0)
                        }
                    }
                    if (darkContextDepth > 0) darkContextDepth--
                }
                is LuaUIParser.Op.TabBegin -> {
                    val tabs = tabsStack.lastOrNull() ?: continue
                    val content = tabs.addTab(op.title)
                    // 后续控件都加到 content 里
                    containerStack.addLast(content)
                    lastView = content
                }
                LuaUIParser.Op.TabEnd -> {
                    if (containerStack.size > 1) containerStack.removeLast()
                }

                // ── 控件 ──
                is LuaUIParser.Op.TextView -> {
                    val tv = TextView(activity).apply {
                        text = op.text
                        textSize = 14f
                        setPadding(dp(4), dp(4), dp(4), dp(4))
                        layoutParams = paramsFor(parent)
                        if (isDark()) setTextColor(DARK_TEXT_PRIMARY)
                    }
                    parent.addView(tv)
                    lastView = tv
                }
                LuaUIParser.Op.Separator -> {
                    val v = View(activity).apply {
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT, dp(1)
                        ).apply { topMargin = dp(8); bottomMargin = dp(8) }
                        setBackgroundColor(if (isDark()) DARK_DIVIDER else 0xFFDDDDDD.toInt())
                    }
                    parent.addView(v)
                    lastView = v
                }
                is LuaUIParser.Op.CheckBox -> {
                    val cb = CheckBox(activity).apply {
                        text = op.label
                        isChecked = op.default
                        layoutParams = paramsFor(parent)
                        // 选中态用蓝色
                        buttonTintList = android.content.res.ColorStateList.valueOf(ACCENT_BLUE)
                        if (isDark()) setTextColor(DARK_TEXT_PRIMARY)
                    }
                    parent.addView(cb)
                    register(op.varName, cb, op)
                    lastView = cb
                }
                is LuaUIParser.Op.EditText -> {
                    addInlineLabel(parent, op.label, isDark())
                    val et = EditText(activity).apply {
                        setText(op.default)
                        layoutParams = paramsFor(parent, defaultWeight = 1f)
                        if (isDark()) {
                            setTextColor(DARK_TEXT_PRIMARY)
                            setHintTextColor(DARK_TEXT_HINT)
                        }
                    }
                    parent.addView(et)
                    register(op.varName, et, op)
                    lastView = et
                }
                is LuaUIParser.Op.EditNumber -> {
                    addInlineLabel(parent, op.label, isDark())
                    val et = EditText(activity).apply {
                        setText(formatNumber(op.default))
                        inputType = InputType.TYPE_CLASS_NUMBER or
                                    InputType.TYPE_NUMBER_FLAG_DECIMAL or
                                    InputType.TYPE_NUMBER_FLAG_SIGNED
                        layoutParams = paramsFor(parent, defaultWeight = 1f)
                        if (isDark()) {
                            setTextColor(DARK_TEXT_PRIMARY)
                            setHintTextColor(DARK_TEXT_HINT)
                        }
                    }
                    parent.addView(et)
                    register(op.varName, et, op)
                    lastView = et
                }
                is LuaUIParser.Op.Spinner -> {
                    addInlineLabel(parent, op.label, isDark())
                    val sp = Spinner(activity).apply {
                        adapter = ArrayAdapter(
                            activity,
                            android.R.layout.simple_spinner_dropdown_item,
                            op.options
                        )
                        val idx = op.options.indexOf(op.default).coerceAtLeast(0)
                        if (op.options.isNotEmpty()) setSelection(idx)
                        layoutParams = paramsFor(parent, defaultWeight = 1f)
                        // 深色背景下 spinner 默认箭头看不清，加一点底色让它可见
                        if (isDark()) {
                            backgroundTintList = android.content.res.ColorStateList.valueOf(DARK_TEXT_PRIMARY)
                        }
                    }
                    parent.addView(sp)
                    register(op.varName, sp, op)
                    lastView = sp
                }
                is LuaUIParser.Op.RadioGroup -> {
                    if (op.label.isNotEmpty()) {
                        parent.addView(TextView(activity).apply {
                            text = op.label
                            setPadding(dp(4), dp(4), dp(4), dp(2))
                            if (isDark()) setTextColor(DARK_TEXT_PRIMARY)
                        })
                    }
                    val rg = RadioGroup(activity).apply {
                        orientation = RadioGroup.VERTICAL
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.WRAP_CONTENT
                        )
                    }
                    op.options.forEachIndexed { idx, label ->
                        rg.addView(RadioButton(activity).apply {
                            text = label
                            id = View.generateViewId()
                            tag = idx
                            isChecked = (label == op.default)
                            // 选中态用蓝色
                            buttonTintList = android.content.res.ColorStateList.valueOf(ACCENT_BLUE)
                            if (isDark()) setTextColor(DARK_TEXT_PRIMARY)
                        })
                    }
                    parent.addView(rg)
                    register(op.varName, rg, op)
                    lastView = rg
                }
                is LuaUIParser.Op.SeekBar -> {
                    if (op.label.isNotEmpty()) {
                        parent.addView(TextView(activity).apply {
                            text = op.label
                            setPadding(dp(4), dp(4), dp(4), dp(2))
                            if (isDark()) setTextColor(DARK_TEXT_PRIMARY)
                        })
                    }
                    val row = LinearLayout(activity).apply {
                        orientation = LinearLayout.HORIZONTAL
                        gravity = Gravity.CENTER_VERTICAL
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.WRAP_CONTENT
                        )
                    }
                    val valueView = TextView(activity).apply {
                        text = formatNumber(op.default)
                        setPadding(dp(8), 0, 0, 0)
                        textSize = 14f
                        if (isDark()) setTextColor(DARK_TEXT_PRIMARY)
                    }
                    val range = (op.max - op.min).toInt().coerceAtLeast(1)
                    val sb = SeekBar(activity).apply {
                        max = range
                        progress = (op.default - op.min).toInt().coerceIn(0, range)
                        layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                        tag = op.min
                        setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                            override fun onProgressChanged(s: SeekBar?, p: Int, fromUser: Boolean) {
                                valueView.text = formatNumber(op.min + p)
                            }
                            override fun onStartTrackingTouch(s: SeekBar?) {}
                            override fun onStopTrackingTouch(s: SeekBar?) {}
                        })
                    }
                    row.addView(sb)
                    row.addView(valueView)
                    parent.addView(row)
                    register(op.varName, sb, op)
                    lastView = sb
                }
                is LuaUIParser.Op.Image -> {
                    val iv = ImageView(activity).apply {
                        layoutParams = LinearLayout.LayoutParams(
                            if (op.widthDp > 0) dp(op.widthDp) else ViewGroup.LayoutParams.WRAP_CONTENT,
                            if (op.heightDp > 0) dp(op.heightDp) else ViewGroup.LayoutParams.WRAP_CONTENT
                        ).apply { topMargin = dp(4); bottomMargin = dp(4) }
                        scaleType = ImageView.ScaleType.FIT_CENTER
                    }
                    try {
                        activity.assets.open(op.assetPath).use { ins ->
                            iv.setImageBitmap(android.graphics.BitmapFactory.decodeStream(ins))
                        }
                    } catch (_: IOException) {
                        iv.setBackgroundColor(0xFFEEEEEE.toInt())
                    }
                    parent.addView(iv)
                    lastView = iv
                }

                // ── 新增控件 ──
                is LuaUIParser.Op.ProgressBar -> {
                    if (op.label.isNotEmpty()) {
                        parent.addView(TextView(activity).apply {
                            text = op.label
                            setPadding(dp(4), dp(4), dp(4), dp(2))
                            if (isDark()) setTextColor(DARK_TEXT_PRIMARY)
                        })
                    }
                    val pb = ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal).apply {
                        max = (op.max - op.min).toInt().coerceAtLeast(1)
                        progress = (op.default - op.min).toInt().coerceIn(0, max)
                        // 用 tag 存 min，便于读值
                        tag = op.min
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            dp(8)
                        ).apply { topMargin = dp(4); bottomMargin = dp(4) }
                    }
                    parent.addView(pb)
                    register(op.varName, pb, op)
                    lastView = pb
                }
                is LuaUIParser.Op.DatePicker -> {
                    val btn = createPickerButton(op.label, op.default.ifEmpty { todayString() })
                    btn.setOnClickListener { showDatePickerDialog(btn) }
                    parent.addView(btn)
                    register(op.varName, btn, op)
                    lastView = btn
                }
                is LuaUIParser.Op.TimePicker -> {
                    val btn = createPickerButton(op.label, op.default.ifEmpty { nowTimeString() })
                    btn.setOnClickListener { showTimePickerDialog(btn) }
                    parent.addView(btn)
                    register(op.varName, btn, op)
                    lastView = btn
                }
                is LuaUIParser.Op.ColorButton -> {
                    val btn = createColorButton(op.label, op.default)
                    btn.setOnClickListener { showColorPickerDialog(btn) }
                    parent.addView(btn)
                    register(op.varName, btn, op)
                    lastView = btn
                }

                // ── 修饰符 ──
                is LuaUIParser.Op.SetWeight -> {
                    lastView?.let { applyWeight(it, op.weight) }
                }
                is LuaUIParser.Op.SetHint -> {
                    (lastView as? EditText)?.hint = op.text
                }
                is LuaUIParser.Op.SetMargin -> {
                    lastView?.let { applyMargin(it, op.topDp, op.bottomDp) }
                }
                is LuaUIParser.Op.SetColor -> {
                    val color = parseColor(op.hex) ?: continue
                    when (val v = lastView) {
                        is TextView -> v.setTextColor(color)
                        is ProgressBar -> {
                            // SetColor 应用到 ProgressBar/SeekBar 时改变进度条颜色
                            v.progressTintList = android.content.res.ColorStateList.valueOf(color)
                        }
                        else -> v?.setBackgroundColor(color)
                    }
                }
                is LuaUIParser.Op.SetTextSize -> {
                    (lastView as? TextView)?.textSize = op.sp
                }
                is LuaUIParser.Op.SetEnabled -> {
                    lastView?.isEnabled = op.enabled
                }
                is LuaUIParser.Op.SetVisible -> {
                    lastView?.visibility = if (op.visible) View.VISIBLE else View.GONE
                }
                is LuaUIParser.Op.BindVisible -> {
                    lastView?.let { v -> visibilityBindings[v] = op.checkboxVar }
                }
            }
        }
    }

    // ---------- Tabs ----------

    /**
     * 一组 Tabs 的运行时上下文。
     * outer 是垂直容器（按钮条 + 内容区），tabs 是各 tab 的 (button, contentView) 列表。
     */
    /**
     * 一组 Tabs 的运行时上下文。
     * 每个 tab 对应一个 TabEntry：按钮容器(垂直 LinearLayout) + 标题 + 底部指示线 + 内容区。
     */
    private class TabEntry(
        val buttonContainer: LinearLayout,
        val titleView: TextView,
        val indicator: View,
        val content: LinearLayout
    )

    private inner class TabsContext(
        val outer: LinearLayout,
        val buttonBar: LinearLayout,
        val contentArea: FrameLayout,
        val buttonScroller: HorizontalScrollView,
        val tabs: MutableList<TabEntry> = mutableListOf()
    ) {
        /**
         * 创建一个 tab：返回它的内容容器（外部接着往里塞控件）。
         * 按钮形态：[标题文字] / [底部 3dp 指示线]
         */
        fun addTab(title: String): LinearLayout {
            val ctx = outer.context

            // 第二个及之后的 tab，先在前面插一条短竖线作为分隔
            if (tabs.isNotEmpty()) {
                buttonBar.addView(View(ctx).apply {
                    layoutParams = LinearLayout.LayoutParams(dp(1), ViewGroup.LayoutParams.MATCH_PARENT).apply {
                        topMargin = dp(10)
                        bottomMargin = dp(10)
                    }
                    setBackgroundColor(DARK_DIVIDER)
                })
            }

            val titleView = TextView(ctx).apply {
                text = title
                gravity = Gravity.CENTER
                setTextColor(TAB_TEXT_NORMAL)
                textSize = 14f
                // 左右多留点 padding，让按钮显得有"分量"
                setPadding(dp(16), dp(12), dp(16), dp(8))
            }
            val indicator = View(ctx).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(3)
                )
                setBackgroundColor(0x00000000)  // 默认透明
            }
            val btnContainer = LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                // 选中和未选中背景一致（与下方内容区同色），靠底部蓝色指示线区分状态
                setBackgroundColor(TAB_CONTENT_BG)
                isClickable = true
                isFocusable = true
                // 触摸反馈：在原背景之上叠一层 ripple
                val ta = ctx.obtainStyledAttributes(intArrayOf(android.R.attr.selectableItemBackground))
                foreground = ta.getDrawable(0)
                ta.recycle()
                // wrap_content + 最小宽度，按钮多时可横向滚动
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
                minimumWidth = dp(72)
                addView(titleView)
                addView(indicator)
            }
            val content = LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
                visibility = View.GONE
            }

            buttonBar.addView(btnContainer)
            contentArea.addView(content)
            val idx = tabs.size
            tabs.add(TabEntry(btnContainer, titleView, indicator, content))
            btnContainer.setOnClickListener { selectTab(idx) }
            return content
        }

        fun selectTab(idx: Int) {
            tabs.forEachIndexed { i, t ->
                if (i == idx) {
                    t.content.visibility = View.VISIBLE
                    t.titleView.setTypeface(t.titleView.typeface, Typeface.BOLD)
                    t.titleView.setTextColor(TAB_TEXT_SELECTED)
                    // 蓝色指示线
                    t.indicator.setBackgroundColor(ACCENT_BLUE)
                    // 自动滚动让选中按钮居中可见
                    val btn = t.buttonContainer
                    btn.post {
                        val targetX = btn.left + btn.width / 2 - buttonScroller.width / 2
                        buttonScroller.smoothScrollTo(targetX.coerceAtLeast(0), 0)
                    }
                } else {
                    t.content.visibility = View.GONE
                    t.titleView.setTypeface(
                        Typeface.create(t.titleView.typeface, Typeface.NORMAL),
                        Typeface.NORMAL
                    )
                    t.titleView.setTextColor(TAB_TEXT_NORMAL)
                    t.indicator.setBackgroundColor(0x00000000)
                }
            }
        }
    }

    private fun createTabs(): TabsContext {
        val outer = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }

        // 用 HorizontalScrollView 包裹按钮条，tab 数量超过屏宽时可左右滑动
        val buttonBar = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            // 按钮条底色 = 内容区底色（按钮和内容区融为一体）
            setBackgroundColor(TAB_CONTENT_BG)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        val buttonScroller = HorizontalScrollView(activity).apply {
            isHorizontalScrollBarEnabled = false
            // tab 数量少时沾满宽度便于显示，tab 多时 children 自然撑大、可滚动
            isFillViewport = true
            setBackgroundColor(TAB_CONTENT_BG)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
            addView(buttonBar)
        }

        val content = FrameLayout(activity).apply {
            // 内容区延续深色背景；不加 padding，让子内容撑满整个对话框宽度
            // 子控件如需左右间距，由控件自身的 padding/margin 完成
            setBackgroundColor(TAB_CONTENT_BG)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        outer.addView(buttonScroller)
        outer.addView(content)
        return TabsContext(outer, buttonBar, content, buttonScroller)
    }

    // ---------- 分组卡片 ----------

    /**
     * 创建分组卡片：圆角边框 + 标题 + 内容容器。
     * 返回 (outer, content) 二元组。
     * dark = true 时用浅色文字标题（用在 Tabs 深色背景下）
     *
     * 在深色模式下不再画卡片背景/边框，只用标题区分；浅色模式保持卡片样式。
     */
    private fun createGroup(title: String, dark: Boolean = false): Pair<LinearLayout, LinearLayout> {
        val outer = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            if (!dark) {
                // 浅色：保留卡片样式（背景 + 圆角 + 边框）
                background = GradientDrawable().apply {
                    setColor(0x10000000)
                    cornerRadius = dp(8).toFloat()
                    setStroke(dp(1), 0xFFCCCCCC.toInt())
                }
                val p = dp(8)
                setPadding(p, p, p, p)
            } else {
                // 深色：不画边框/背景，但保留水平 padding 让内部控件不贴对话框边缘
                val ph = dp(16)
                val pv = dp(8)
                setPadding(ph, pv, ph, pv)
            }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(8); bottomMargin = dp(8) }
        }

        if (title.isNotEmpty()) {
            outer.addView(TextView(activity).apply {
                text = title
                textSize = 14f
                setTextColor(if (dark) DARK_TEXT_PRIMARY else Color.parseColor("#444444"))
                setTypeface(typeface, Typeface.BOLD)
                setPadding(0, 0, 0, dp(4))
            })
        }

        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        outer.addView(content)
        return outer to content
    }

    // ---------- DatePicker / TimePicker / ColorButton ----------

    /**
     * 通用 picker 按钮：按钮文本 = "label: value"，点击时由 caller 弹对话框。
     * 用 tag 存当前 value（字符串），便于读值。
     */
    private fun createPickerButton(label: String, value: String): Button {
        return Button(activity).apply {
            text = if (label.isNotEmpty()) "$label: $value" else value
            isAllCaps = false
            tag = value  // 用 tag 存值
            setTag(R_LABEL, label)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(4); bottomMargin = dp(4) }
        }
    }

    private fun showDatePickerDialog(btn: Button) {
        val cal = Calendar.getInstance()
        val current = btn.tag as? String
        if (!current.isNullOrEmpty()) {
            try {
                val parts = current.split("-")
                cal.set(parts[0].toInt(), parts[1].toInt() - 1, parts[2].toInt())
            } catch (_: Exception) { /* 解析失败用今天 */ }
        }
        DatePickerDialog(activity, { _, y, m, d ->
            val value = String.format("%04d-%02d-%02d", y, m + 1, d)
            btn.tag = value
            val label = btn.getTag(R_LABEL) as? String ?: ""
            btn.text = if (label.isNotEmpty()) "$label: $value" else value
        }, cal.get(Calendar.YEAR), cal.get(Calendar.MONTH), cal.get(Calendar.DAY_OF_MONTH)).show()
    }

    private fun showTimePickerDialog(btn: Button) {
        val cal = Calendar.getInstance()
        val current = btn.tag as? String
        if (!current.isNullOrEmpty()) {
            try {
                val parts = current.split(":")
                cal.set(Calendar.HOUR_OF_DAY, parts[0].toInt())
                cal.set(Calendar.MINUTE, parts[1].toInt())
            } catch (_: Exception) { /* 用当前时间 */ }
        }
        TimePickerDialog(activity, { _, h, m ->
            val value = String.format("%02d:%02d", h, m)
            btn.tag = value
            val label = btn.getTag(R_LABEL) as? String ?: ""
            btn.text = if (label.isNotEmpty()) "$label: $value" else value
        }, cal.get(Calendar.HOUR_OF_DAY), cal.get(Calendar.MINUTE), true).show()
    }

    private fun createColorButton(label: String, defaultHex: String): Button {
        val color = parseColor(defaultHex) ?: Color.WHITE
        return Button(activity).apply {
            text = if (label.isNotEmpty()) "$label: $defaultHex" else defaultHex
            isAllCaps = false
            tag = defaultHex
            setTag(R_LABEL, label)
            // 在右侧画一个色块
            val swatch = GradientDrawable().apply {
                setColor(color)
                setStroke(dp(1), 0xFF888888.toInt())
                setSize(dp(20), dp(20))
            }
            setCompoundDrawablesWithIntrinsicBounds(null, null, swatch, null)
            compoundDrawablePadding = dp(8)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(4); bottomMargin = dp(4) }
        }
    }

    /**
     * 极简颜色选择器：弹一个对话框，提供 R/G/B 三个 EditText。
     * 不引入第三方库，够演示用。
     */
    private fun showColorPickerDialog(btn: Button) {
        val current = parseColor(btn.tag as? String ?: "#FFFFFF") ?: Color.WHITE
        val r = Color.red(current)
        val g = Color.green(current)
        val b = Color.blue(current)

        val container = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            val p = dp(16)
            setPadding(p, p, p, p)
        }
        val rEt = EditText(activity).apply { hint = "R (0-255)"; setText(r.toString()); inputType = InputType.TYPE_CLASS_NUMBER }
        val gEt = EditText(activity).apply { hint = "G (0-255)"; setText(g.toString()); inputType = InputType.TYPE_CLASS_NUMBER }
        val bEt = EditText(activity).apply { hint = "B (0-255)"; setText(b.toString()); inputType = InputType.TYPE_CLASS_NUMBER }
        container.addView(rEt); container.addView(gEt); container.addView(bEt)

        AlertDialog.Builder(activity)
            .setTitle("选择颜色 (RGB)")
            .setView(container)
            .setPositiveButton("确定") { _, _ ->
                val nr = rEt.text.toString().toIntOrNull()?.coerceIn(0, 255) ?: r
                val ng = gEt.text.toString().toIntOrNull()?.coerceIn(0, 255) ?: g
                val nb = bEt.text.toString().toIntOrNull()?.coerceIn(0, 255) ?: b
                val hex = String.format("#%02X%02X%02X", nr, ng, nb)
                btn.tag = hex
                val label = btn.getTag(R_LABEL) as? String ?: ""
                btn.text = if (label.isNotEmpty()) "$label: $hex" else hex
                // 更新色块
                val swatch = GradientDrawable().apply {
                    setColor(Color.rgb(nr, ng, nb))
                    setStroke(dp(1), 0xFF888888.toInt())
                    setSize(dp(20), dp(20))
                }
                btn.setCompoundDrawablesWithIntrinsicBounds(null, null, swatch, null)
            }
            .setNegativeButton("取消", null)
            .show()
    }

    // ---------- bindVisible ----------

    /**
     * 渲染完毕后统一应用：扫描 visibilityBindings，给每个目标 view 绑定到对应 checkbox。
     */
    private fun applyVisibilityBindings() {
        for ((view, cbVar) in visibilityBindings) {
            val cb = widgets[cbVar] as? CheckBox ?: continue
            // 初始可见性
            view.visibility = if (cb.isChecked) View.VISIBLE else View.GONE
            // 状态改变时同步
            cb.setOnCheckedChangeListener { _, checked ->
                view.visibility = if (checked) View.VISIBLE else View.GONE
            }
        }
    }

    // ---------- helpers ----------

    /**
     * 根据父布局方向决定要不要内联 label TextView。
     * 水平容器 → label 挂在前面，不抢权重；垂直容器 → label 挂在上面。
     */
    private fun addInlineLabel(parent: LinearLayout, label: String, dark: Boolean = false) {
        if (label.isEmpty()) return
        val tv = TextView(activity).apply {
            text = label
            if (parent.orientation == LinearLayout.HORIZONTAL) {
                setPadding(0, 0, dp(8), 0)
                gravity = Gravity.CENTER_VERTICAL
            } else {
                setPadding(dp(4), dp(4), dp(4), dp(2))
            }
            if (dark) setTextColor(DARK_TEXT_PRIMARY)
        }
        parent.addView(tv)
    }

    private fun paramsFor(parent: LinearLayout, defaultWeight: Float = 0f): LinearLayout.LayoutParams {
        return if (parent.orientation == LinearLayout.HORIZONTAL) {
            LinearLayout.LayoutParams(
                if (defaultWeight > 0) 0 else ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                defaultWeight
            )
        } else {
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
    }

    private fun applyWeight(view: View, weight: Float) {
        val lp = view.layoutParams as? LinearLayout.LayoutParams ?: return
        lp.width = 0
        lp.weight = weight
        view.layoutParams = lp
    }

    private fun applyMargin(view: View, topDp: Int, bottomDp: Int) {
        val lp = view.layoutParams as? ViewGroup.MarginLayoutParams ?: return
        lp.topMargin = dp(topDp)
        lp.bottomMargin = dp(bottomDp)
        view.layoutParams = lp
    }

    private fun parseColor(hex: String): Int? {
        if (hex.isEmpty()) return null
        return try { Color.parseColor(hex) } catch (_: Exception) { null }
    }

    private fun register(name: String, view: View, op: LuaUIParser.Op) {
        if (name.isEmpty()) return
        widgets[name] = view
        widgetOps[name] = op
    }

    private fun collectResults() {
        result.clear()
        for ((name, view) in widgets) {
            val op = widgetOps[name]
            result[name] = readValue(view, op)
        }
    }

    private fun readValue(view: View, op: LuaUIParser.Op?): Any? {
        return when (op) {
            is LuaUIParser.Op.CheckBox -> (view as CheckBox).isChecked
            is LuaUIParser.Op.EditText -> (view as EditText).text.toString()
            is LuaUIParser.Op.EditNumber -> (view as EditText).text.toString().toDoubleOrNull() ?: 0.0
            is LuaUIParser.Op.Spinner -> {
                val sp = view as Spinner
                op.options.getOrElse(sp.selectedItemPosition) { "" }
            }
            is LuaUIParser.Op.RadioGroup -> {
                val rg = view as RadioGroup
                val checkedId = rg.checkedRadioButtonId
                if (checkedId == -1) {
                    ""
                } else {
                    val rb = rg.findViewById<RadioButton>(checkedId)
                    val idx = (rb?.tag as? Int) ?: 0
                    op.options.getOrElse(idx) { "" }
                }
            }
            is LuaUIParser.Op.SeekBar -> {
                val sb = view as SeekBar
                val min = (sb.tag as? Double) ?: 0.0
                min + sb.progress
            }
            is LuaUIParser.Op.ProgressBar -> {
                val pb = view as ProgressBar
                val min = (pb.tag as? Double) ?: 0.0
                min + pb.progress
            }
            is LuaUIParser.Op.DatePicker,
            is LuaUIParser.Op.TimePicker,
            is LuaUIParser.Op.ColorButton -> (view as Button).tag as? String ?: ""
            else -> null
        }
    }

    private fun formatNumber(d: Double): String =
        if (d == d.toLong().toDouble()) d.toLong().toString() else d.toString()

    private fun todayString(): String {
        val c = Calendar.getInstance()
        return String.format("%04d-%02d-%02d",
            c.get(Calendar.YEAR), c.get(Calendar.MONTH) + 1, c.get(Calendar.DAY_OF_MONTH))
    }

    private fun nowTimeString(): String {
        val c = Calendar.getInstance()
        return String.format("%02d:%02d",
            c.get(Calendar.HOUR_OF_DAY), c.get(Calendar.MINUTE))
    }

    private fun dp(v: Int): Int =
        (v * activity.resources.displayMetrics.density).toInt()

    companion object {
        @JvmStatic
        fun create(activity: Activity): OpRenderer = OpRenderer(activity)

        // 用一个固定的 setTag id 存 picker 按钮的 label（避免和 tag 冲突）
        private const val R_LABEL = 0x7F999001

        // CheckBox / RadioButton 选中态颜色（蓝）
        private val ACCENT_BLUE = 0xFF1976D2.toInt()

        // Tabs 深色配色
        private val TAB_BG_DARK          = 0xFF2C2C2C.toInt()  // 未选中按钮 / 顶部按钮条
        private val TAB_BG_DARK_SELECTED = 0xFF424242.toInt()  // 选中按钮（稍亮）
        private val TAB_CONTENT_BG       = 0xFF1E1E1E.toInt()  // 内容区背景（深色）
        private val TAB_TEXT_NORMAL      = 0xFFCCCCCC.toInt()  // 未选中文字
        private val TAB_TEXT_SELECTED    = 0xFFFFFFFF.toInt()  // 选中文字（白）

        // 深色上下文中各类文字 / 分隔线 / 分组卡片的颜色
        private val DARK_TEXT_PRIMARY    = 0xFFFFFFFF.toInt()  // 主文字色（白）
        private val DARK_TEXT_HINT       = 0xFF888888.toInt()  // hint 灰
        private val DARK_DIVIDER         = 0xFF444444.toInt()  // 分隔线
        // 深色模式下分组卡片不上底色（透明），只靠边框区分；
        // 这样卡片背景始终和外层 Tabs/对话框背景一致，不会出现拼色感
        private val DARK_GROUP_BG        = 0x00000000          // 透明

        // 整个对话框的窗口背景（让对话框看起来是一个卡片）
        private val DIALOG_BG            = 0xFF1E1E1E.toInt()  // 深色卡片底
        private val DIALOG_BORDER        = 0xFF444444.toInt()  // 边框（深灰）

        // 底部按钮区配色（深色）
        private val FOOTER_BG            = 0xFF2C2C2C.toInt()  // footer 背景
        private val FOOTER_DIVIDER       = 0xFF444444.toInt()  // footer 顶部分隔线 / 中间竖线
        private val FOOTER_TEXT          = 0xFFFFFFFF.toInt()  // 按钮文字白色
    }
}
