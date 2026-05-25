package com.example.sampleui.ui

/**
 * ui.lua 回写器：把对话框收集到的值原地写回 widget 调用的 default 参数，
 * 保留注释、空行、缩进、字符串引号风格等所有非默认值的细节。
 *
 * 仅识别 widget 调用，按签名表确定 default 参数在第几个位置：
 *   addCheckBox(name, label, default)               default = 3
 *   addEditText(name, label, default)               default = 3
 *   addEditNumber(name, label, default)             default = 3
 *   addSpinner(name, label, options, default)       default = 4
 *   addRadioGroup(name, label, options, default)    default = 4
 *   addSeekBar(name, label, min, max, default)      default = 5  (alias addSlider)
 *   addProgressBar(name, label, min, max, default)  default = 5
 *   addDatePicker(name, label, default)             default = 3
 *   addTimePicker(name, label, default)             default = 3
 *   addColorButton(name, label, default)            default = 3
 *
 * 容器 / 装饰 / 修饰符行（dialogTitle / beginGroup / setHint 等）原样保留。
 *
 * 行级处理策略：
 *   1) 找到 '(' 与最后一个 ')'，把行切成 [前缀][参数列表][后缀+尾部注释]
 *   2) 切分参数（跳过字符串和 {} 内的逗号，与 LuaUIParser.splitArgs 一致）
 *   3) 第 1 个参数视为 varName，命中 result map 才替换
 *   4) 替换第 defaultIdx 个参数：保留它的前后空白，仅替换字面量内容
 *   5) 重新拼接，其它行原样不动
 */
object UiLuaWriter {

    private val WIDGET_DEFAULT_AT = mapOf(
        "addCheckBox" to 3,
        "addEditText" to 3,
        "addEditNumber" to 3,
        "addSpinner" to 4,
        "addRadioGroup" to 4,
        "addSeekBar" to 5,
        "addSlider" to 5,
        "addProgressBar" to 5,
        "addDatePicker" to 3,
        "addTimePicker" to 3,
        "addColorButton" to 3,
    )

    /**
     * 用 [result] 中的值替换 [src] 里能命中的 default 参数。
     *
     * @return 修改后的完整文本；若没有任何行被改动则返回 null（调用方可跳过写盘）
     */
    fun rewrite(src: String, result: Map<String, Any?>): String? {
        // 保留原始换行符（CRLF / LF）。lines() 不带换行符，这里手动用正则切分
        // 简单起见用 LF 切，再保留尾部 LF；CRLF 的 \r 会作为行尾的一部分被保留
        val lines = src.split("\n")
        var changed = false
        val out = lines.map { line ->
            val rewritten = transformLine(line, result)
            if (rewritten != null && rewritten != line) {
                changed = true
                rewritten
            } else line
        }
        return if (changed) out.joinToString("\n") else null
    }

    private fun transformLine(line: String, result: Map<String, Any?>): String? {
        // 去掉行尾 \r（如果是 CRLF）以便处理，最后再补回去
        val cr = line.endsWith("\r")
        val core = if (cr) line.dropLast(1) else line

        val trimmed = core.trim()
        if (trimmed.isEmpty() || trimmed.startsWith("--")) return null

        val parenStart = core.indexOf('(')
        if (parenStart < 0) return null
        val funcName = core.substring(0, parenStart).trim()
        val defaultIdx = WIDGET_DEFAULT_AT[funcName] ?: return null

        // 找出对应这个 '(' 的 ')'：跳过字符串和嵌套 {}/()
        val parenEnd = matchingClose(core, parenStart) ?: return null

        val prefix = core.substring(0, parenStart + 1) // 含 '('
        val inner = core.substring(parenStart + 1, parenEnd)
        val suffix = core.substring(parenEnd) // 从 ')' 开始（含尾部行内注释）

        val args = splitArgs(inner)
        if (args.size < defaultIdx) return null

        val varNameRaw = args[0].trim()
        val varName = unquote(varNameRaw) ?: return null
        if (!result.containsKey(varName)) return null

        val funcNameTrim = funcName
        val newLiteral = formatLuaValue(funcNameTrim, result[varName])

        // 替换第 defaultIdx 个参数，保留前后空白
        val origArg = args[defaultIdx - 1]
        args[defaultIdx - 1] = replacePreservingSpaces(origArg, newLiteral)

        val rebuilt = prefix + args.joinToString(",") + suffix
        return if (cr) "$rebuilt\r" else rebuilt
    }

    /** 找到与 [openIdx] 处 '(' 配对的 ')' 索引；找不到返回 null */
    private fun matchingClose(s: String, openIdx: Int): Int? {
        var depth = 0
        var i = openIdx
        var inString = false
        var stringChar = ' '
        var braceDepth = 0
        while (i < s.length) {
            val c = s[i]
            when {
                inString -> if (c == stringChar) inString = false
                c == '"' || c == '\'' -> { inString = true; stringChar = c }
                c == '{' -> braceDepth++
                c == '}' -> braceDepth--
                braceDepth == 0 && c == '(' -> depth++
                braceDepth == 0 && c == ')' -> {
                    depth--
                    if (depth == 0) return i
                }
                // 遇到行内注释 '--' 则视为参数列表结束（lua 行内注释）
                braceDepth == 0 && c == '-' && i + 1 < s.length && s[i + 1] == '-' -> return null
            }
            i++
        }
        return null
    }

    /** 与 LuaUIParser.splitArgs 等价：按逗号分割，跳过字符串内和 {} 内的逗号 */
    private fun splitArgs(s: String): MutableList<String> {
        val result = mutableListOf<String>()
        val cur = StringBuilder()
        var inString = false
        var stringChar = ' '
        var braceDepth = 0
        for (ch in s) {
            when {
                inString -> {
                    cur.append(ch)
                    if (ch == stringChar) inString = false
                }
                ch == '"' || ch == '\'' -> {
                    inString = true; stringChar = ch
                    cur.append(ch)
                }
                ch == '{' -> { braceDepth++; cur.append(ch) }
                ch == '}' -> { braceDepth--; cur.append(ch) }
                ch == ',' && braceDepth == 0 -> {
                    result.add(cur.toString()); cur.clear()
                }
                else -> cur.append(ch)
            }
        }
        if (cur.isNotEmpty()) result.add(cur.toString())
        return result
    }

    private fun unquote(s: String): String? {
        val t = s.trim()
        if (t.length < 2) return null
        if ((t.startsWith("\"") && t.endsWith("\"")) ||
            (t.startsWith("'") && t.endsWith("'"))) {
            return t.substring(1, t.length - 1)
        }
        return null
    }

    /** 把 lua 值格式化为 lua 字面量。CheckBox 的 boolean 用 true/false；数值原样；其它用 "..." */
    private fun formatLuaValue(funcName: String, v: Any?): String {
        return when (funcName) {
            "addCheckBox" -> {
                val b = when (v) {
                    is Boolean -> v
                    is Number -> v.toDouble() != 0.0
                    is String -> v == "true" || v == "1"
                    else -> false
                }
                if (b) "true" else "false"
            }
            "addEditNumber", "addSeekBar", "addSlider", "addProgressBar" -> {
                val n = when (v) {
                    is Number -> v.toDouble()
                    is String -> v.toDoubleOrNull() ?: 0.0
                    else -> 0.0
                }
                // 整数就显示整数，避免 50 -> 50.0 这种丑话
                if (n == n.toLong().toDouble()) n.toLong().toString() else n.toString()
            }
            else -> {
                // string 类，转义 \ 和 "
                val s = (v ?: "").toString()
                "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
            }
        }
    }

    /** 用 [newLiteral] 替换 [original] 字符串中"实际内容"的部分，保留前后空白 */
    private fun replacePreservingSpaces(original: String, newLiteral: String): String {
        val leading = original.takeWhile { it.isWhitespace() }
        val trailing = original.takeLastWhile { it.isWhitespace() }
        return leading + newLiteral + trailing
    }
}
