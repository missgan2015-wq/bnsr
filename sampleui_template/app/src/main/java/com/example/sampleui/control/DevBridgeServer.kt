package com.example.sampleui.control

import android.content.Context
import android.util.Log
import com.example.sampleui.LogPanel
import com.example.sampleui.RootShell
import com.example.sampleui.ScriptRunner
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.util.concurrent.CopyOnWriteArrayList

/**
 * VSIX 开发桥服务：让 PC 端 VSCode 扩展通过 `adb forward tcp:8790 tcp:8790`
 * 远程同步脚本、运行/停止脚本、订阅设备日志。
 *
 * ## 帧协议（大端字节序）
 *
 * 请求：
 *   ┌──────────────┬──────────────┬──────────────┬──────────┐
 *   │ messageId(4) │ bodyLength(4)│ command(4)   │ payload  │
 *   └──────────────┴──────────────┴──────────────┴──────────┘
 *   bodyLength = command(4) + payload.length
 *
 * 响应：
 *   ┌──────────────┬──────────────┬──────────────┬──────────┐
 *   │ messageId(4) │ bodyLength(4)│ status(4)    │ payload  │
 *   └──────────────┴──────────────┴──────────────┴──────────┘
 *   bodyLength = status(4) + payload.length
 *
 * 服务端主动推送事件帧（不属于任何请求的响应）：
 *   messageId = 0
 *   status    = STATUS_EVENT (5)
 *   payload   = [eventCode:4][event payload...]
 *
 * ## payload 编码原语
 *   string : [len:4][utf8 bytes]
 *   int    : 4 bytes 大端
 *   bytes  : [len:4][raw bytes]
 *
 * ## 命令一览（DEV_*）
 *   100 PING            req: 空                          resp: "pong"
 *   101 SYNC_FILE       req: [string:relPath][bytes:content]   resp: 空
 *                       relPath 相对 ScriptRunner.scriptDir，例 "scripts/script.lua"
 *   102 SYNC_DELETE     req: [string:relPath]            resp: 空
 *   103 RUN_SCRIPT      req: [string:scriptRelPath][int:debug] resp: 空
 *                       scriptRelPath 留空走 ScriptRunner 默认主脚本
 *                       debug = 1 预留给后续断点调试，当前忽略
 *   104 STOP_SCRIPT     req: 空                          resp: 空
 *   105 LIST_INFO       req: 空                          resp: [string:json]
 *
 * ## 事件码（EVT_*）
 *   200 LOG             [string:level][string:tag][string:msg]
 *   201 SCRIPT_STARTED  空
 *   202 SCRIPT_STDOUT   [string:line]
 *   203 SCRIPT_EXITED   [int:exitCode]
 */
class DevBridgeServer(
    private val appContext: Context,
    private val port: Int = DEFAULT_PORT
) {

    @Volatile private var serverSocket: ServerSocket? = null
    @Volatile private var running: Boolean = false
    private var acceptThread: Thread? = null

    /** 当前所有已连接客户端，事件需要广播给它们。 */
    private val clients = CopyOnWriteArrayList<ClientHandle>()

    // 共享给所有客户端的日志监听器：把屏幕日志同步成 EVT_LOG 帧广播
    private val logListener = LogPanel.Listener { level, tag, msg ->
        broadcastEvent(EVT_LOG) { buf ->
            putString(buf, level)
            putString(buf, tag)
            putString(buf, msg)
        }
    }

    // 进程生命周期监听器：把 stdout 行 + start/exit 事件广播
    private val processListener = object : ScriptRunner.ProcessListener {
        override fun onStarted() {
            broadcastEvent(EVT_SCRIPT_STARTED) { /* no payload */ }
        }
        override fun onStdoutLine(line: String) {
            broadcastEvent(EVT_SCRIPT_STDOUT) { buf -> putString(buf, line) }
        }
        override fun onExited(exitCode: Int) {
            broadcastEvent(EVT_SCRIPT_EXITED) { buf -> buf.putInt(exitCode) }
        }
    }

    @Synchronized
    fun start() {
        if (running) return
        running = true
        LogPanel.addListener(logListener)
        ScriptRunner.addProcessListener(processListener)
        acceptThread = Thread({ runAccept() }, "DevBridge-Accept").apply {
            isDaemon = true
            start()
        }
    }

    @Synchronized
    fun stop() {
        running = false
        LogPanel.removeListener(logListener)
        ScriptRunner.removeProcessListener(processListener)
        try { serverSocket?.close() } catch (_: Exception) {}
        serverSocket = null
        acceptThread?.interrupt()
        acceptThread = null
        // 关掉所有客户端
        for (c in clients) c.close()
        clients.clear()
    }

    fun isRunning(): Boolean = running

    private fun runAccept() {
        try {
            ServerSocket().use { socket ->
                socket.reuseAddress = true
                socket.bind(InetSocketAddress("127.0.0.1", port))
                serverSocket = socket
                LogPanel.i(TAG, "DevBridge 已监听 127.0.0.1:$port（VSIX 通过 adb forward 接入）")
                while (running) {
                    val client = try {
                        socket.accept()
                    } catch (e: Exception) {
                        if (!running) return
                        Log.w(TAG, "accept 失败：${e.message}")
                        continue
                    }
                    val handle = ClientHandle(client)
                    clients.add(handle)
                    Thread({
                        try { handle.runLoop() }
                        finally {
                            clients.remove(handle)
                            handle.close()
                        }
                    }, "DevBridge-Client-${client.port}").apply {
                        isDaemon = true
                        start()
                    }
                }
            }
        } catch (e: IOException) {
            if (running) LogPanel.e(TAG, "DevBridge 监听异常停止：${e.message}")
        } finally {
            running = false
            serverSocket = null
        }
    }

    /** 广播事件到所有客户端。任意客户端写入失败会被踢掉。 */
    private fun broadcastEvent(eventCode: Int, fillPayload: (ByteBuffer) -> Unit) {
        if (clients.isEmpty()) return
        // 事件 payload = [eventCode:4][...]; 这里先用一个动态 buffer 攒
        val tmp = ByteBuffer.allocate(MAX_EVENT_PAYLOAD).order(ByteOrder.BIG_ENDIAN)
        tmp.putInt(eventCode)
        try {
            fillPayload(tmp)
        } catch (e: Exception) {
            Log.w(TAG, "事件 payload 序列化异常：${e.message}")
            return
        }
        tmp.flip()
        val payload = ByteArray(tmp.remaining())
        tmp.get(payload)

        val dead = mutableListOf<ClientHandle>()
        for (c in clients) {
            if (!c.tryWrite(0, STATUS_EVENT, payload)) dead.add(c)
        }
        for (c in dead) {
            clients.remove(c)
            c.close()
        }
    }

    /** 单个客户端连接的处理器。 */
    private inner class ClientHandle(private val socket: Socket) {
        private val input = DataInputStream(socket.getInputStream())
        private val output = DataOutputStream(socket.getOutputStream())
        private val writeLock = Any()

        fun runLoop() {
            val remote = "${socket.inetAddress?.hostAddress ?: "?"}:${socket.port}"
            LogPanel.i(TAG, "DevBridge 收到连接：$remote")
            try {
                while (running && !socket.isClosed) {
                    val messageId = try { input.readInt() } catch (_: java.io.EOFException) { return }
                    val bodyLength = input.readInt()
                    if (bodyLength < COMMAND_BYTES || bodyLength > MAX_BODY_BYTES) {
                        Log.w(TAG, "非法 bodyLength=$bodyLength，断开 $remote")
                        return
                    }
                    val body = ByteArray(bodyLength)
                    input.readFully(body)

                    val buf = ByteBuffer.wrap(body).order(ByteOrder.BIG_ENDIAN)
                    val command = buf.getInt()
                    val payload = ByteArray(buf.remaining())
                    buf.get(payload)

                    val (status, respPayload) = try {
                        handleCommand(command, payload)
                    } catch (t: Throwable) {
                        Log.w(TAG, "命令处理异常 cmd=$command：${t.message}")
                        STATUS_INTERNAL_ERROR to stringPayload(t.message ?: "internal error")
                    }
                    if (!tryWrite(messageId, status, respPayload)) return
                }
            } catch (e: IOException) {
                if (running) Log.w(TAG, "客户端 $remote 异常断开：${e.message}")
            }
        }

        /** 写一帧响应/事件。返回 false 表示写失败（客户端已断开）。 */
        fun tryWrite(messageId: Int, status: Int, payload: ByteArray): Boolean {
            return try {
                synchronized(writeLock) {
                    output.writeInt(messageId)
                    output.writeInt(STATUS_BYTES + payload.size)
                    output.writeInt(status)
                    output.write(payload)
                    output.flush()
                }
                true
            } catch (_: IOException) { false }
        }

        fun close() {
            try { socket.close() } catch (_: Exception) {}
        }
    }

    /** 命令分发：返回 (status, payload)。 */
    private fun handleCommand(command: Int, payload: ByteArray): Pair<Int, ByteArray> {
        val buf = ByteBuffer.wrap(payload).order(ByteOrder.BIG_ENDIAN)
        return when (command) {
            DEV_PING -> STATUS_OK to stringPayload("pong")
            DEV_SYNC_FILE -> handleSyncFile(buf)
            DEV_SYNC_DELETE -> handleSyncDelete(buf)
            DEV_RUN_SCRIPT -> handleRunScript(buf)
            DEV_STOP_SCRIPT -> handleStopScript()
            DEV_LIST_INFO -> handleListInfo()
            else -> {
                LogPanel.w(TAG, "未知命令：$command（payload ${payload.size} 字节）")
                STATUS_INVALID_COMMAND to stringPayload("unknown command $command")
            }
        }
    }

    // ---- 命令实现 ----

    private fun handleSyncFile(buf: ByteBuffer): Pair<Int, ByteArray> {
        val rel = readString(buf)
        val bytes = readBytes(buf)
        val safe = sanitizeRelPath(rel) ?: return STATUS_INVALID_REQUEST to
                stringPayload("非法相对路径：$rel")

        // 先写到 App 私有 cacheDir，再用常驻 root shell cat 到部署目录
        // （deploy 目录在 /data/local/tmp，普通进程无写权限）
        val cached = File(appContext.cacheDir, "_devbridge/$safe")
        try {
            cached.parentFile?.mkdirs()
            cached.writeBytes(bytes)
        } catch (e: Exception) {
            return STATUS_INTERNAL_ERROR to stringPayload("缓存落盘失败：${e.message}")
        }

        // 目标位置：DEPLOY_DIR/scripts/<safe>。
        // VSIX 推送的所有文件都进 scripts/ 子目录，与 APK 自释放路径完全对齐。
        val dst = "$DEPLOY_DIR/scripts/$safe"
        val parent = dst.substringBeforeLast('/', "$DEPLOY_DIR/scripts")
        val srcEsc = cached.absolutePath.replace("'", "'\\''")
        val dstEsc = dst.replace("'", "'\\''")
        val parentEsc = parent.replace("'", "'\\''")
        val cmd = "mkdir -p '$parentEsc' && cat '$srcEsc' > '$dstEsc' && chmod 644 '$dstEsc'"
        val rc = RootShell.run(cmd)
        return if (rc == 0) {
            LogPanel.i(TAG, "已同步文件：scripts/$safe (${bytes.size} 字节)")
            STATUS_OK to ByteArray(0)
        } else {
            STATUS_INTERNAL_ERROR to stringPayload("root 写入失败 rc=$rc")
        }
    }

    private fun handleSyncDelete(buf: ByteBuffer): Pair<Int, ByteArray> {
        val rel = readString(buf)
        val safe = sanitizeRelPath(rel) ?: return STATUS_INVALID_REQUEST to
                stringPayload("非法相对路径：$rel")
        val dst = "$DEPLOY_DIR/scripts/$safe"
        val dstEsc = dst.replace("'", "'\\''")
        val rc = RootShell.run("rm -rf '$dstEsc'")
        return if (rc == 0) {
            LogPanel.i(TAG, "已删除文件：scripts/$safe")
            STATUS_OK to ByteArray(0)
        } else {
            STATUS_INTERNAL_ERROR to stringPayload("rm 失败 rc=$rc")
        }
    }

    private fun handleRunScript(buf: ByteBuffer): Pair<Int, ByteArray> {
        val rel = readString(buf)
        val debug = if (buf.remaining() >= 4) buf.getInt() else 0

        val scriptOverride = if (rel.isBlank()) null else {
            val safe = sanitizeRelPath(rel) ?: return STATUS_INVALID_REQUEST to
                    stringPayload("非法相对路径：$rel")
            // VSIX 端把"工作区相对路径"传过来，与 sync 时的目标路径对齐：scripts/ 子目录
            "$DEPLOY_DIR/scripts/$safe"
        }
        if (debug != 0) {
            LogPanel.w(TAG, "RUN_SCRIPT 收到 debug=$debug，但断点调试尚未实现，按普通运行处理")
        }
        ScriptRunner.startHeadless(scriptOverride = scriptOverride, varsJson = null)
        return STATUS_OK to ByteArray(0)
    }

    private fun handleStopScript(): Pair<Int, ByteArray> {
        ScriptRunner.stop()
        return STATUS_OK to ByteArray(0)
    }

    private fun handleListInfo(): Pair<Int, ByteArray> {
        // 先返回最小字段，VSIX 端用于校验设备已就绪
        val json = org.json.JSONObject()
        json.put("ok", true)
        json.put("scriptDir", ScriptRunner.scriptDir)
        json.put("scriptPath", ScriptRunner.scriptPath)
        json.put("isRunning", ScriptRunner.isRunning)
        json.put("rooted", RootShell.isRootGranted())
        return STATUS_OK to stringPayload(json.toString())
    }

    /**
     * 把客户端送来的相对路径规范化，过滤掉 ".." / 绝对路径等危险用法。
     * 返回 null 表示路径非法。
     */
    private fun sanitizeRelPath(rel: String): String? {
        if (rel.isBlank()) return null
        if (rel.startsWith("/") || rel.startsWith("\\")) return null
        // 统一分隔符
        val unified = rel.replace('\\', '/').trim('/')
        if (unified.isEmpty()) return null
        // 不允许包含 .. 段
        for (seg in unified.split('/')) {
            if (seg == "." || seg == "..") return null
        }
        return unified
    }

    companion object {
        private const val TAG = "DevBridge"
        const val DEFAULT_PORT = 8790

        // status code
        const val STATUS_OK = 0
        const val STATUS_INVALID_COMMAND = 1
        const val STATUS_INVALID_REQUEST = 2
        const val STATUS_INTERNAL_ERROR = 3
        const val STATUS_EVENT = 5

        // 命令码
        const val DEV_PING = 100
        const val DEV_SYNC_FILE = 101
        const val DEV_SYNC_DELETE = 102
        const val DEV_RUN_SCRIPT = 103
        const val DEV_STOP_SCRIPT = 104
        const val DEV_LIST_INFO = 105

        // 事件码
        const val EVT_LOG = 200
        const val EVT_SCRIPT_STARTED = 201
        const val EVT_SCRIPT_STDOUT = 202
        const val EVT_SCRIPT_EXITED = 203

        private const val COMMAND_BYTES = 4
        private const val STATUS_BYTES = 4
        private const val MAX_BODY_BYTES = 32 * 1024 * 1024  // 32MB：允许同步较大脚本
        private const val MAX_EVENT_PAYLOAD = 64 * 1024      // 单条事件 payload 上限

        // ScriptRunner.DEPLOY_DIR 是 private，这里硬编码同步常量
        private const val DEPLOY_DIR = "/data/local/tmp/easyLua"

        // ---- 编码原语 ----

        fun readString(buffer: ByteBuffer): String {
            val len = buffer.getInt()
            if (len < 0 || len > buffer.remaining()) {
                throw IOException("invalid string length: $len")
            }
            val bytes = ByteArray(len)
            buffer.get(bytes)
            return String(bytes, StandardCharsets.UTF_8)
        }

        fun readBytes(buffer: ByteBuffer): ByteArray {
            val len = buffer.getInt()
            if (len < 0 || len > buffer.remaining()) {
                throw IOException("invalid bytes length: $len")
            }
            val out = ByteArray(len)
            buffer.get(out)
            return out
        }

        fun putString(buffer: ByteBuffer, s: String) {
            val bytes = s.toByteArray(StandardCharsets.UTF_8)
            buffer.putInt(bytes.size)
            buffer.put(bytes)
        }

        fun stringPayload(value: String): ByteArray {
            val bytes = value.toByteArray(StandardCharsets.UTF_8)
            val out = ByteBuffer.allocate(4 + bytes.size).order(ByteOrder.BIG_ENDIAN)
            out.putInt(bytes.size)
            out.put(bytes)
            return out.array()
        }
    }
}
