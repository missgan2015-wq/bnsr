package com.example.sampleui

import java.io.File
import java.io.FileWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 日志文件管理：每次脚本运行写一个独立的 .log 文件，文件名包含时间戳。
 *
 * 文件结构：
 *   /sdcard/AutoGo/logs/
 *     ├── 2026-05-20_08-30-15.log
 *     ├── 2026-05-20_09-12-44.log
 *     └── ...
 *
 * 用法：
 *   val log = LogFile.create()            // 开始新日志
 *   log.write("[INFO] xxx")               // 追加一行
 *   log.close()                           // 收尾
 *
 *   LogFile.listAll()                     // 列出全部日志
 *   LogFile.read(file)                    // 读出文件内容
 *   LogFile.delete(file)                  // 删除某条
 */
object LogFile {

    /** 日志根目录 */
    var logDir = "/sdcard/AutoGo/logs"

    private val nameFmt = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.getDefault())

    fun ensureDir() {
        val d = File(logDir)
        if (!d.exists()) d.mkdirs()
    }

    /** 创建一个新的日志写入器（脚本启动时调用）。 */
    fun create(): Writer {
        ensureDir()
        val name = "${nameFmt.format(Date())}.log"
        val f = File(logDir, name)
        return Writer(f)
    }

    /** 列出全部日志文件（按时间倒序：最新的在前面）。 */
    fun listAll(): List<File> {
        val dir = File(logDir)
        if (!dir.exists() || !dir.isDirectory) return emptyList()
        return dir.listFiles { f -> f.isFile && f.name.endsWith(".log") }
            ?.sortedByDescending { it.lastModified() }
            ?: emptyList()
    }

    fun read(f: File): String = if (f.exists()) f.readText() else ""
    fun delete(f: File): Boolean = f.exists() && f.delete()

    /** 单个日志文件的写入器，append + 自动 flush。 */
    class Writer(val file: File) {
        private val w: FileWriter? = try {
            file.parentFile?.mkdirs()
            FileWriter(file, true)
        } catch (_: Exception) { null }

        fun write(line: String) {
            try {
                w?.appendLine(line)
                w?.flush()
            } catch (_: Exception) { /* ignore */ }
        }

        fun close() {
            try { w?.close() } catch (_: Exception) {}
        }
    }
}
