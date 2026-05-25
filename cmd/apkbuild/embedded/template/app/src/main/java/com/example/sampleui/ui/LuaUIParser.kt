package com.example.sampleui.ui

/**
 * 解析 ui.lua 声明式脚本，转换为渲染指令列表。
 *
 * 支持的语法（每行一个函数调用）：
 *
 *   ── 元信息 ──
 *   dialogTitle("脚本设置")
 *
 *   ── 容器（可嵌套）──
 *   beginGroup("基础设置")           -- 带标题的分组卡片
 *     ...
 *   endGroup()
 *
 *   beginRow()                       -- 水平排列的一行
 *     ...
 *   endRow()
 *
 *   ── 控件 ──
 *   addTextView("欢迎")
 *   addSeparator()
 *   addCheckBox("autoRetry", "自动重试", true)
 *   addEditText("account", "账号", "默认值")
 *   addEditNumber("delay", "延迟(ms)", 1000)
 *   addSpinner("mode", "运行模式", {"快速","普通","慢速"}, "普通")
 *   addRadioGroup("difficulty", "难度", {"简单","中等","困难"}, "中等")
 *   addSeekBar("speed", "速度", 1, 100, 50)         -- 真正的滑块
 *   addImage("scripts/logo.png", 200, 100)          -- assets 图片
 *
 *   ── 修饰符（修饰前一个控件）──
 *   setWeight(1)                     -- 在 beginRow 内分配剩余空间权重
 *   setHint("请输入账号")            -- 输入框 hint
 *   setMargin(8, 8)                  -- 上下边距(dp)
 *
 * 解析输出是一个扁平的 Op 列表（深度优先序），由渲染层用栈处理嵌套。
 */
class LuaUIParser {

    data class ParseResult(
        val title: String,
        val ops: List<Op>,
        // ── 弹窗尺寸/位置（在 dialogXxx 顶层函数中设置）──
        val widthDp: Int = 0,        // 0 = 自适应；-1 = MATCH_PARENT；-2 = WRAP_CONTENT；正数 = dp
        val heightDp: Int = 0,
        val widthPercent: Float = 0f,  // 0 = 不使用；>0 = 按屏幕宽度百分比
        val heightPercent: Float = 0f,
        val gravity: String = "",      // "center"/"top"/"bottom"/"left"/"right"/"top-left" 等
        val offsetXDp: Int = 0,
        val offsetYDp: Int = 0
    )

    /**
     * 渲染操作指令。控件、容器开闭、修饰符全部统一为 Op。
     * 渲染层维护一个容器栈：begin* 入栈，end* 出栈，addX 加到栈顶容器。
     */
    sealed class Op {
        // ── 容器 ──
        data class GroupBegin(val title: String) : Op()
        object GroupEnd : Op()
        object RowBegin : Op()
        object RowEnd : Op()
        object TabsBegin : Op()
        object TabsEnd : Op()
        data class TabBegin(val title: String) : Op()
        object TabEnd : Op()

        // ── 控件 ──
        data class TextView(val text: String) : Op()
        object Separator : Op()
        data class CheckBox(val varName: String, val label: String, val default: Boolean) : Op()
        data class EditText(val varName: String, val label: String, val default: String) : Op()
        data class EditNumber(val varName: String, val label: String, val default: Double) : Op()
        data class Spinner(val varName: String, val label: String, val options: List<String>, val default: String) : Op()
        data class RadioGroup(val varName: String, val label: String, val options: List<String>, val default: String) : Op()
        data class SeekBar(val varName: String, val label: String, val min: Double, val max: Double, val default: Double) : Op()
        data class Image(val assetPath: String, val widthDp: Int, val heightDp: Int) : Op()
        data class ProgressBar(val varName: String, val label: String, val min: Double, val max: Double, val default: Double) : Op()
        data class DatePicker(val varName: String, val label: String, val default: String) : Op()  // default = "yyyy-MM-dd"
        data class TimePicker(val varName: String, val label: String, val default: String) : Op()  // default = "HH:mm"
        data class ColorButton(val varName: String, val label: String, val default: String) : Op() // default = "#RRGGBB"

        // ── 修饰符（修饰最近添加的 widget 或刚开启的容器）──
        data class SetWeight(val weight: Float) : Op()
        data class SetHint(val text: String) : Op()
        data class SetMargin(val topDp: Int, val bottomDp: Int) : Op()
        data class SetColor(val hex: String) : Op()
        data class SetTextSize(val sp: Float) : Op()
        data class SetEnabled(val enabled: Boolean) : Op()
        data class SetVisible(val visible: Boolean) : Op()
        data class BindVisible(val checkboxVar: String) : Op()  // 跟随某个 checkbox 状态

        /** 控件类型的简单判别（修饰符 / 容器开闭不是 widget） */
        val isWidget: Boolean
            get() = when (this) {
                is TextView, is CheckBox, is EditText, is EditNumber,
                is Spinner, is RadioGroup, is SeekBar, is Image,
                is ProgressBar, is DatePicker, is TimePicker, is ColorButton -> true
                else -> false
            }

        /** 持有变量名的控件统一接口（用于结果收集） */
        fun boundVarName(): String? = when (this) {
            is CheckBox -> varName
            is EditText -> varName
            is EditNumber -> varName
            is Spinner -> varName
            is RadioGroup -> varName
            is SeekBar -> varName
            is ProgressBar -> varName
            is DatePicker -> varName
            is TimePicker -> varName
            is ColorButton -> varName
            else -> null
        }
    }

    companion object {
        @JvmStatic
        fun parse(luaSource: String): ParseResult? {
            var title = "设置"
            var widthDp = 0
            var heightDp = 0
            var widthPct = 0f
            var heightPct = 0f
            var gravity = ""
            var offsetX = 0
            var offsetY = 0
            val ops = mutableListOf<Op>()

            for (rawLine in luaSource.lines()) {
                val line = rawLine.trim()
                if (line.isEmpty() || line.startsWith("--")) continue

                val funcName = line.substringBefore('(').trim()
                val args = extractArgs(line)

                when (funcName) {
                    "dialogTitle" -> {
                        title = args.getOrNull(0)?.asString() ?: title
                    }
                    "dialogSize" -> {
                        widthDp = args.getOrNull(0)?.asDouble()?.toInt() ?: 0
                        heightDp = args.getOrNull(1)?.asDouble()?.toInt() ?: 0
                    }
                    "dialogSizePercent" -> {
                        widthPct = args.getOrNull(0)?.asDouble()?.toFloat() ?: 0f
                        heightPct = args.getOrNull(1)?.asDouble()?.toFloat() ?: 0f
                    }
                    "dialogPosition" -> {
                        gravity = args.getOrNull(0)?.asString() ?: ""
                    }
                    "dialogOffset" -> {
                        offsetX = args.getOrNull(0)?.asDouble()?.toInt() ?: 0
                        offsetY = args.getOrNull(1)?.asDouble()?.toInt() ?: 0
                    }

                    // 容器
                    "beginGroup" -> ops.add(Op.GroupBegin(args.getOrNull(0)?.asString() ?: ""))
                    "endGroup"   -> ops.add(Op.GroupEnd)
                    "beginRow"   -> ops.add(Op.RowBegin)
                    "endRow"     -> ops.add(Op.RowEnd)
                    "beginTabs"  -> ops.add(Op.TabsBegin)
                    "endTabs"    -> ops.add(Op.TabsEnd)
                    "beginTab"   -> ops.add(Op.TabBegin(args.getOrNull(0)?.asString() ?: ""))
                    "endTab"     -> ops.add(Op.TabEnd)

                    // 控件
                    "addTextView" -> ops.add(Op.TextView(args.getOrNull(0)?.asString() ?: ""))
                    "addSeparator" -> ops.add(Op.Separator)
                    "addCheckBox" -> ops.add(Op.CheckBox(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        default = args.getOrNull(2)?.asBool() ?: false
                    ))
                    "addEditText" -> ops.add(Op.EditText(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        default = args.getOrNull(2)?.asString() ?: ""
                    ))
                    "addEditNumber" -> ops.add(Op.EditNumber(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        default = args.getOrNull(2)?.asDouble() ?: 0.0
                    ))
                    "addSpinner" -> ops.add(Op.Spinner(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        options = args.getOrNull(2)?.asList() ?: emptyList(),
                        default = args.getOrNull(3)?.asString() ?: ""
                    ))
                    "addRadioGroup" -> ops.add(Op.RadioGroup(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        options = args.getOrNull(2)?.asList() ?: emptyList(),
                        default = args.getOrNull(3)?.asString() ?: ""
                    ))
                    // 兼容老版本：addSlider 等价于 addSeekBar
                    "addSlider", "addSeekBar" -> ops.add(Op.SeekBar(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        min = args.getOrNull(2)?.asDouble() ?: 0.0,
                        max = args.getOrNull(3)?.asDouble() ?: 100.0,
                        default = args.getOrNull(4)?.asDouble() ?: 0.0
                    ))
                    "addImage" -> ops.add(Op.Image(
                        assetPath = args.getOrNull(0)?.asString() ?: "",
                        widthDp = args.getOrNull(1)?.asDouble()?.toInt() ?: 0,
                        heightDp = args.getOrNull(2)?.asDouble()?.toInt() ?: 0
                    ))
                    "addProgressBar" -> ops.add(Op.ProgressBar(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        min = args.getOrNull(2)?.asDouble() ?: 0.0,
                        max = args.getOrNull(3)?.asDouble() ?: 100.0,
                        default = args.getOrNull(4)?.asDouble() ?: 0.0
                    ))
                    "addDatePicker" -> ops.add(Op.DatePicker(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        default = args.getOrNull(2)?.asString() ?: ""
                    ))
                    "addTimePicker" -> ops.add(Op.TimePicker(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        default = args.getOrNull(2)?.asString() ?: ""
                    ))
                    "addColorButton" -> ops.add(Op.ColorButton(
                        varName = args.getOrNull(0)?.asString() ?: "",
                        label = args.getOrNull(1)?.asString() ?: "",
                        default = args.getOrNull(2)?.asString() ?: "#FFFFFF"
                    ))

                    // 修饰符
                    "setWeight" -> ops.add(Op.SetWeight(args.getOrNull(0)?.asDouble()?.toFloat() ?: 1f))
                    "setHint"   -> ops.add(Op.SetHint(args.getOrNull(0)?.asString() ?: ""))
                    "setMargin" -> ops.add(Op.SetMargin(
                        topDp = args.getOrNull(0)?.asDouble()?.toInt() ?: 0,
                        bottomDp = args.getOrNull(1)?.asDouble()?.toInt() ?: 0
                    ))
                    "setColor"     -> ops.add(Op.SetColor(args.getOrNull(0)?.asString() ?: "#000000"))
                    "setTextSize"  -> ops.add(Op.SetTextSize(args.getOrNull(0)?.asDouble()?.toFloat() ?: 14f))
                    "setEnabled"   -> ops.add(Op.SetEnabled(args.getOrNull(0)?.asBool() ?: true))
                    "setVisible"   -> ops.add(Op.SetVisible(args.getOrNull(0)?.asBool() ?: true))
                    "bindVisible"  -> ops.add(Op.BindVisible(args.getOrNull(0)?.asString() ?: ""))
                }
            }

            return if (ops.isEmpty()) null else ParseResult(
                title, ops,
                widthDp, heightDp, widthPct, heightPct,
                gravity, offsetX, offsetY
            )
        }

        // ========== 参数提取 ==========

        private fun extractArgs(line: String): List<LuaArg> {
            val start = line.indexOf('(')
            val end = line.lastIndexOf(')')
            if (start < 0 || end <= start) return emptyList()
            val inner = line.substring(start + 1, end).trim()
            if (inner.isEmpty()) return emptyList()
            return splitArgs(inner).map { parseLuaArg(it.trim()) }
        }

        /**
         * 按逗号分割，但跳过字符串内和 {} 内的逗号。
         */
        private fun splitArgs(s: String): List<String> {
            val result = mutableListOf<String>()
            val current = StringBuilder()
            var inString = false
            var stringChar = ' '
            var braceDepth = 0

            for (ch in s) {
                when {
                    inString -> {
                        current.append(ch)
                        if (ch == stringChar) inString = false
                    }
                    ch == '"' || ch == '\'' -> {
                        inString = true
                        stringChar = ch
                        current.append(ch)
                    }
                    ch == '{' -> { braceDepth++; current.append(ch) }
                    ch == '}' -> { braceDepth--; current.append(ch) }
                    ch == ',' && braceDepth == 0 -> {
                        result.add(current.toString()); current.clear()
                    }
                    else -> current.append(ch)
                }
            }
            if (current.isNotEmpty()) result.add(current.toString())
            return result
        }

        private fun parseLuaArg(s: String): LuaArg {
            val trimmed = s.trim()
            return when {
                (trimmed.startsWith("\"") && trimmed.endsWith("\"")) ||
                (trimmed.startsWith("'") && trimmed.endsWith("'")) ->
                    LuaArg.Str(trimmed.substring(1, trimmed.length - 1))
                trimmed == "true" -> LuaArg.Bool(true)
                trimmed == "false" -> LuaArg.Bool(false)
                trimmed == "nil" -> LuaArg.Nil
                trimmed.startsWith("{") && trimmed.endsWith("}") -> {
                    val inner = trimmed.substring(1, trimmed.length - 1)
                    val items = splitArgs(inner).map { parseLuaArg(it.trim()) }
                    LuaArg.Table(items.mapNotNull { it.asString() })
                }
                else -> trimmed.toDoubleOrNull()?.let { LuaArg.Num(it) } ?: LuaArg.Str(trimmed)
            }
        }
    }

    sealed class LuaArg {
        data class Str(val value: String) : LuaArg()
        data class Num(val value: Double) : LuaArg()
        data class Bool(val value: Boolean) : LuaArg()
        data class Table(val items: List<String>) : LuaArg()
        object Nil : LuaArg()

        fun asString(): String? = when (this) {
            is Str -> value
            is Num -> if (value == value.toLong().toDouble()) value.toLong().toString() else value.toString()
            is Bool -> value.toString()
            else -> null
        }

        fun asDouble(): Double? = when (this) {
            is Num -> value
            is Str -> value.toDoubleOrNull()
            else -> null
        }

        fun asBool(): Boolean? = when (this) {
            is Bool -> value
            is Str -> value == "true"
            else -> null
        }

        fun asList(): List<String>? = when (this) {
            is Table -> items
            else -> null
        }
    }
}
