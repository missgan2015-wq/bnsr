package com.example.sampleui

import android.util.Log
import java.io.BufferedReader
import java.io.DataOutputStream
import java.io.InputStreamReader
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * 常驻 root shell：整个 App 进程只 fork 一次 `su`，后续命令通过 stdin 推送、stdout 读取。
 *
 * 解决的问题：
 *   - 之前每次 `Runtime.exec(arrayOf("su","-c", cmd))` 都开一个新 su 进程，
 *     部分 Magisk/KernelSU 设置（"每次询问"）下会反复弹授权对话框。
 *   - 单例常驻后，整个进程生命周期只授权一次。
 *
 * 用法：
 *   val result = RootShell.exec("ls /data/local/tmp")        // 阻塞返回 stdout
 *   val rc = RootShell.run("rm -f /tmp/x; mkdir /tmp/x")     // 阻塞返回退出码
 *   RootShell.isRootGranted()                                // 同步检查是否有 root
 *
 * 注意：
 *   - 命令里不能包含我们用来定界的 marker 字符串（_RS_END_xxx_），实际不会冲突。
 *   - 如果 su 没装/被拒绝，所有调用立即返回 -1 / "" 而不是阻塞。
 */
object RootShell {

    private const val TAG = "RootShell"
    private const val DEFAULT_TIMEOUT_MS = 30_000L

    @Volatile
    private var process: Process? = null

    @Volatile
    private var stdin: DataOutputStream? = null

    /** stdout 行队列（不区分命令边界，由 marker 切分） */
    private val stdoutLines = LinkedBlockingQueue<String>()

    /** 标记 su 是否可用（已尝试启动且 fork 成功） */
    @Volatile
    private var available: Boolean = false

    /** 是否真的拿到了 root（uid=0） */
    @Volatile
    private var rooted: Boolean = false

    /** 用于结果分隔的递增序号 */
    @Volatile
    private var seq: Long = 0

    /**
     * 启动常驻 su shell。如果已启动则不重复启动。
     * 第一次调用通常会触发 Magisk/KernelSU 的授权对话框（同步等用户确认）。
     *
     * @return true 表示拿到了 root；false 表示用户拒绝或机器没装 su。
     */
    @Synchronized
    fun start(): Boolean {
        if (available && rooted) return true
        if (available && !rooted) return false

        try {
            val p = Runtime.getRuntime().exec("su")
            stdin = DataOutputStream(p.outputStream)

            // 单独线程读 stdout / stderr 到一个共享队列
            // stderr 合并到 stdout：Process 不像 ProcessBuilder.redirectErrorStream，需要手动起线程读两边
            Thread({
                try {
                    BufferedReader(InputStreamReader(p.inputStream)).useLines { seq ->
                        seq.forEach { stdoutLines.put(it) }
                    }
                } catch (_: Exception) { /* 进程退出 */ }
            }, "RootShell-stdout").apply { isDaemon = true; start() }

            Thread({
                try {
                    BufferedReader(InputStreamReader(p.errorStream)).useLines { seq ->
                        seq.forEach {
                            // 记录到日志，但不混进 stdout 队列（否则会污染 marker 解析）
                            Log.w(TAG, "[stderr] $it")
                        }
                    }
                } catch (_: Exception) {}
            }, "RootShell-stderr").apply { isDaemon = true; start() }

            process = p
            available = true

            // 验证是否真有 root：发 `id` 命令看 uid=0
            val out = exec("id", DEFAULT_TIMEOUT_MS)
            rooted = out.contains("uid=0")
            if (!rooted) {
                LogPanel.w(TAG, "su 已 fork 但 uid != 0：$out")
                shutdown()
            } else {
                LogPanel.i(TAG, "Root shell 启动成功，已获 root 权限")
            }
            return rooted
        } catch (e: Exception) {
            LogPanel.w(TAG, "su 不可用：${e.message}")
            shutdown()
            return false
        }
    }

    /** 同步阻塞地执行 cmd，返回合并后的 stdout 文本（不含尾部换行）。 */
    fun exec(cmd: String, timeoutMs: Long = DEFAULT_TIMEOUT_MS): String {
        if (!available) return ""
        val (out, _) = execWithRc(cmd, timeoutMs)
        return out
    }

    /** 同步执行 cmd，返回退出码（-1 表示出错/超时）。 */
    fun run(cmd: String, timeoutMs: Long = DEFAULT_TIMEOUT_MS): Int {
        if (!available) return -1
        val (_, rc) = execWithRc(cmd, timeoutMs)
        return rc
    }

    /**
     * 执行 cmd，返回 (stdout, exit code)。
     *
     * 实现：往常驻 shell 写：
     *   { <cmd> ; } 2>&1
     *   echo __RS_END_<seq>__:$?
     * 然后从 stdout 队列里读直到看到 marker 行，把前面的攒成 stdout、marker 上的数字当 rc。
     */
    private fun execWithRc(cmd: String, timeoutMs: Long): Pair<String, Int> {
        val out = stdin ?: return "" to -1
        val n = synchronized(this) { ++seq }
        val marker = "__RS_END_${n}__"

        try {
            // 命令体合并 stderr 到 stdout，方便单管道捕获
            out.writeBytes("{ $cmd; } 2>&1\n")
            out.writeBytes("echo $marker:\$?\n")
            out.flush()
        } catch (e: Exception) {
            LogPanel.e(TAG, "写命令失败：${e.message}")
            shutdown()
            return "" to -1
        }

        val deadline = System.currentTimeMillis() + timeoutMs
        val sb = StringBuilder()
        var rc = -1
        while (true) {
            val left = deadline - System.currentTimeMillis()
            if (left <= 0) {
                LogPanel.w(TAG, "命令超时：$cmd")
                return sb.toString().trimEnd('\n') to -1
            }
            val line = stdoutLines.poll(left, TimeUnit.MILLISECONDS) ?: continue

            if (line.startsWith(marker)) {
                rc = line.substringAfter(":").trim().toIntOrNull() ?: -1
                break
            }
            sb.append(line).append('\n')
        }
        return sb.toString().trimEnd('\n') to rc
    }

    /** 是否拿到了 root（start() 已成功且 uid=0）。 */
    fun isRootGranted(): Boolean = rooted

    /** 关闭常驻 shell（一般不需要主动调，进程退出时自然释放）。 */
    @Synchronized
    fun shutdown() {
        try { stdin?.writeBytes("exit\n"); stdin?.flush() } catch (_: Exception) {}
        try { stdin?.close() } catch (_: Exception) {}
        try { process?.destroy() } catch (_: Exception) {}
        stdin = null
        process = null
        available = false
        rooted = false
        stdoutLines.clear()
    }
}
