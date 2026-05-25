package com.example.sampleui.ui

import android.app.Activity
import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.*
import android.support.v7.app.AlertDialog

/**
 * AnkuLua 风格的链式对话框 DSL。
 *
 * 设计要点：
 *  - 像 AnkuLua 一样，每个 add* 方法直接把控件追加到当前根布局
 *  - newRow() 切换到新的水平 LinearLayout，模拟 AnkuLua 的"行布局"
 *  - show() / showFullScreen() 关闭后通过 result(key) 读取每个控件的值
 *  - 与 AnkuLua 不同：值不污染全局，全部装在 DialogBuilder 实例的 result map 里
 */
class DialogBuilder(private val context: Context) {

    // 顶层垂直容器
    private val rootLayout: LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        val pad = dp(16)
        setPadding(pad, pad, pad, pad)
    }

    // 当前正在追加控件的"行"。null 表示直接挂到 root
    private var currentRow: LinearLayout? = null

    // 控件注册表：key -> 控件，关闭时统一读值
    private val widgets = LinkedHashMap<String, View>()

    // 关闭后填充的结果
    val result = HashMap<String, Any?>()

    // ---------- 布局控制 ----------

    /** 开启一个新行。后续 add* 都会加到这一行，直到下一次 newRow() */
    fun newRow(): DialogBuilder {
        currentRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(4); bottomMargin = dp(4) }
        }
        rootLayout.addView(currentRow)
        return this
    }

    /** 添加一条分割线 */
    fun addSeparator(): DialogBuilder {
        val v = View(context).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1)
            ).apply { topMargin = dp(8); bottomMargin = dp(8) }
            setBackgroundColor(0xFFDDDDDD.toInt())
        }
        rootLayout.addView(v)
        currentRow = null
        return this
    }

    // ---------- 控件 ----------

    fun addTextView(text: String): DialogBuilder {
        val tv = TextView(context).apply {
            this.text = text
            textSize = 14f
            gravity = Gravity.CENTER_VERTICAL
            val pad = dp(4)
            setPadding(pad, pad, pad, pad)
        }
        attach(tv)
        return this
    }

    /** 文本输入框，关闭后 result[key] 是 String */
    fun addEditText(key: String, default: String = ""): DialogBuilder {
        val et = EditText(context).apply {
            setText(default)
            layoutParams = rowParam(weight = 1f)
        }
        widgets[key] = et
        attach(et)
        return this
    }

    /** 数字输入框，关闭后 result[key] 是 Double（用 Number 也行，看脚本作者方便） */
    fun addEditNumber(key: String, default: Number = 0): DialogBuilder {
        val et = EditText(context).apply {
            setText(default.toString())
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or
                    InputType.TYPE_NUMBER_FLAG_SIGNED
            layoutParams = rowParam(weight = 1f)
        }
        widgets[key] = et
        attach(et)
        return this
    }

    /** 复选框，结果是 Boolean */
    fun addCheckBox(key: String, label: String, default: Boolean = false): DialogBuilder {
        val cb = CheckBox(context).apply {
            text = label
            isChecked = default
            layoutParams = rowParam(weight = 1f)
        }
        widgets[key] = cb
        attach(cb)
        return this
    }

    /** 单选组：先 newRow → addRadioGroup → 多个 addRadioButton */
    fun addRadioGroup(key: String, defaultValue: Int = -1): DialogBuilder {
        val rg = RadioGroup(context).apply {
            orientation = RadioGroup.VERTICAL
            tag = defaultValue
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        widgets[key] = rg
        // RadioGroup 是垂直组合控件，独立成块挂到 root，不放进当前 row
        currentRow = null
        rootLayout.addView(rg)
        lastRadioGroup = rg
        return this
    }

    private var lastRadioGroup: RadioGroup? = null

    fun addRadioButton(label: String, value: Int): DialogBuilder {
        val group = lastRadioGroup ?: throw IllegalStateException(
            "调用 addRadioButton 前必须先 addRadioGroup"
        )
        val rb = RadioButton(context).apply {
            text = label
            id = View.generateViewId()
            tag = value
        }
        group.addView(rb)
        // 命中默认值则选中
        if ((group.tag as? Int) == value) rb.isChecked = true
        return this
    }

    /** 下拉选择，items 是显示文本，result[key] 是被选中项的索引（Int） */
    fun addSpinnerIndex(key: String, items: List<String>, defaultIndex: Int = 0): DialogBuilder {
        val sp = Spinner(context).apply {
            adapter = ArrayAdapter(
                context,
                android.R.layout.simple_spinner_dropdown_item,
                items
            )
            setSelection(defaultIndex.coerceIn(0, items.size - 1))
            layoutParams = rowParam(weight = 1f)
        }
        widgets[key] = sp
        attach(sp)
        return this
    }

    // ---------- 显示 ----------

    /** 弹窗显示期间不需要单独藏脚本悬浮按钮——
     *  前台 Activity 的 onStart/onStop 已经通过 ControlBubble.pushHide/popHide
     *  维护引用计数，弹普通 dialog 时 bubble 自然不可见。 */

    /** 普通对话框 */
    fun show(title: String, onClosed: ((Map<String, Any?>) -> Unit)? = null) {
        AlertDialog.Builder(context)
            .setTitle(title)
            .setView(wrapInScroll(rootLayout))
            .setPositiveButton("确定") { _, _ ->
                collectResults()
                onClosed?.invoke(result)
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 全屏对话框：仿 AnkuLua 的 dialogShowFullScreen */
    fun showFullScreen(title: String, onClosed: ((Map<String, Any?>) -> Unit)? = null) {
        val dialog = AlertDialog.Builder(context)
            .setTitle(title)
            .setView(wrapInScroll(rootLayout))
            .setPositiveButton("确定") { _, _ ->
                collectResults()
                onClosed?.invoke(result)
            }
            .setNegativeButton("取消", null)
            .create()
        dialog.show()
        dialog.window?.setLayout(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT
        )
    }

    // ---------- 内部 ----------

    private fun attach(v: View) {
        val parent = currentRow ?: rootLayout
        parent.addView(v)
    }

    private fun wrapInScroll(content: View): ScrollView {
        return ScrollView(context).apply {
            addView(
                content,
                ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            )
        }
    }

    private fun rowParam(weight: Float = 0f): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(
            if (weight > 0) 0 else ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            weight
        )

    private fun collectResults() {
        result.clear()
        for ((key, view) in widgets) {
            result[key] = when (view) {
                is EditText -> {
                    if (view.inputType and InputType.TYPE_CLASS_NUMBER != 0) {
                        view.text.toString().toDoubleOrNull() ?: 0.0
                    } else {
                        view.text.toString()
                    }
                }
                is CheckBox -> view.isChecked
                is RadioGroup -> {
                    val checkedId = view.checkedRadioButtonId
                    if (checkedId == -1) null
                    else view.findViewById<RadioButton>(checkedId)?.tag as? Int
                }
                is Spinner -> view.selectedItemPosition
                else -> null
            }
        }
    }

    private fun dp(v: Int): Int =
        (v * context.resources.displayMetrics.density).toInt()

    companion object {
        /** 入口函数，类似 AnkuLua 的 dialogInit() */
        @JvmStatic
        fun create(activity: Activity): DialogBuilder = DialogBuilder(activity)
    }
}
