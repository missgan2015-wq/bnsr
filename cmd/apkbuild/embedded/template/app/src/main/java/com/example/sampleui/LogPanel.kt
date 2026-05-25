package com.example.sampleui

import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.util.Log
import android.widget.ScrollView
import android.widget.TextView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 简单的日志面板：把日志追加到一个 TextView，自动滚到底部。
 * 单例，全 App 共享一份日志，方便不同模块都能写。
 *
 * 没有 attach view 时（比如主页不显示日志），日志仍会输出到 Android Logcat。
 */
object LogPanel {

    private const val LOGCAT_TAG = "easyLua"
    private const val MAX_LINES = 500
    private val timeFmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault())

    private var textView: TextView? = null
    private var scrollView: ScrollView? = null
    private val buffer = ArrayDeque<CharSequence>()

    /**
     * 日志监听器接口：让 DevBridgeServer 等外部模块订阅日志事件，
     * 把屏幕日志同步推给已连接的 VSIX 客户端。
     *
     * 注意：listener 回调发生在调用 i/w/e/d/line 的线程上，listener 自身要做线程安全。
     */
    fun interface Listener {
        /**
         * @param level "INFO" / "WARN" / "ERROR" / "DEBUG" / "RAW"
         * @param tag   日志 tag；line() 进来时为空字符串
         * @param msg   日志消息体；line() 进来时是整行原文
         */
        fun onLog(level: String, tag: String, msg: String)
    }

    private val listeners = java.util.concurrent.CopyOnWriteArrayList<Listener>()

    fun addListener(l: Listener) { listeners.addIfAbsent(l) }
    fun removeListener(l: Listener) { listeners.remove(l) }

    private fun fanout(level: String, tag: String, msg: String) {
        // tag = "script" 是脚本 stdout 的镜像，DevBridge 通过 ProcessListener.onStdoutLine
        // 已经直接转发了 stdout，这里跳过避免 VSIX 收到重复内容。
        if (tag == "script") return
        for (l in listeners) {
            try { l.onLog(level, tag, msg) } catch (_: Throwable) { /* 隔离监听器异常 */ }
        }
    }

    fun attach(tv: TextView, sv: ScrollView) {
        textView = tv
        scrollView = sv
        refresh()
    }

    fun detach() {
        textView = null
        scrollView = null
    }

    fun i(tag: String, msg: String) { Log.i(LOGCAT_TAG, "[$tag] $msg"); append("INFO", 0xFF8BC34A.toInt(), tag, msg); fanout("INFO", tag, msg) }
    fun w(tag: String, msg: String) { Log.w(LOGCAT_TAG, "[$tag] $msg"); append("WARN", 0xFFFFC107.toInt(), tag, msg); fanout("WARN", tag, msg) }
    fun e(tag: String, msg: String) { Log.e(LOGCAT_TAG, "[$tag] $msg"); append("ERROR", 0xFFF44336.toInt(), tag, msg); fanout("ERROR", tag, msg) }
    fun d(tag: String, msg: String) { Log.d(LOGCAT_TAG, "[$tag] $msg"); append("DEBUG", 0xFF9E9E9E.toInt(), tag, msg); fanout("DEBUG", tag, msg) }

    /**
     * 整行原样追加，不加 [time]/[level]/[tag] 前缀。
     * 给"行首已有时间戳和行号"的脚本输出（native print 自己加的格式）专用，
     * 避免外面再叠一层时间戳。
     *
     * color 用于把行首 `[...]` 整体着色（绿/黄/红 = 普通/警告/错误）。
     */
    @Synchronized
    fun line(color: Int, fullLine: String) {
        Log.i(LOGCAT_TAG, fullLine)
        val ss = SpannableString(fullLine)
        // 第一组方括号（[HH:MM:SS.mmm script.lua:line]）整体上色
        val close = fullLine.indexOf(']')
        if (close > 0) {
            ss.setSpan(ForegroundColorSpan(color), 0, close + 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        buffer.addLast(ss)
        while (buffer.size > MAX_LINES) buffer.removeFirst()
        textView?.post { refresh() }
        // 注意：line() 是脚本 stdout 的镜像；DevBridge 端有独立的
        // ScriptRunner.ProcessListener.onStdoutLine 通道转发同一行，
        // 这里不再 fanout 给 Listener，避免 VSIX 端收到重复内容。
    }

    @Synchronized
    private fun append(level: String, color: Int, tag: String, msg: String) {
        val time = timeFmt.format(Date())
        val line = SpannableString("[$time] [$level] [$tag] $msg")
        val start = "[$time] ".length
        val end = start + "[$level]".length
        line.setSpan(ForegroundColorSpan(color), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)

        buffer.addLast(line)
        while (buffer.size > MAX_LINES) buffer.removeFirst()

        textView?.post { refresh() }
    }

    @Synchronized
    fun clear() {
        buffer.clear()
        textView?.post { refresh() }
    }

    private fun refresh() {
        val tv = textView ?: return
        val sv = scrollView
        val sb = StringBuilder()
        for ((idx, line) in buffer.withIndex()) {
            if (idx > 0) sb.append("\n")
            sb.append(line)
        }
        tv.text = sb
        sv?.post { sv.fullScroll(ScrollView.FOCUS_DOWN) }
    }
}
