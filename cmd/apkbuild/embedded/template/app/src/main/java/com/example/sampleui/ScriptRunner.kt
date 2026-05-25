package com.example.sampleui

import android.app.Activity
import com.example.sampleui.ui.ControlBubble
import com.example.sampleui.ui.EasyLuaSecret
import com.example.sampleui.ui.LuaUIParser
import com.example.sampleui.ui.OpRenderer
import com.example.sampleui.ui.UiLuaWriter
import org.json.JSONObject
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader

/**
 * 脚本运行器：单实例，同一时刻只允许一个脚本运行。
 *
 * 部署目录（root 释放到 /data/local/tmp/easyLua/）：
 *     ├── libeasylua.so       easyLua C 引擎（含 LuaJIT），由 app_process 加载
 *     ├── easylua/
 *     │   └── easylua.dex     Java 桥（com.easylua.EasyLuaMain 入口）
 *     ├── easyLua.tag         版本标签（size+mtime）增量部署用
 *     └── scripts/            从 APK assets/scripts/ 同步
 *         （含 script.lua 主脚本 + 可选 ui.lua 参数收集 UI）
 *
 * 启动方式：
 *     CLASSPATH=easylua.dex app_process /system/bin com.easylua.EasyLuaMain \
 *         --lua libeasylua.so script.lua [--vars-file config.json]
 *
 * 流程：
 *   1. init() 把 jniLibs/libeasylua.so + APK assets 通过 root 释放到 DEPLOY_DIR。
 *      用 size+mtime 当 tag，避免每次启动重复部署。
 *   2. start() 用 su -c 启动 app_process（root 保证 SurfaceFlinger / uinput 可用）。
 *   3. 如果有 ui.lua 则弹配置对话框，收集到 JSON 后启动。
 *   4. 每次运行新建一个日志文件 (LogFile)，把 stdout 实时写入。
 *   5. 显示控制悬浮球，点停止即杀进程。
 */
object ScriptRunner {

    private const val TAG = "ScriptRunner"

    /** jniLibs 中嵌入的引擎库名（Android 系统约定 lib*.so 才会被解压到 nativeLibraryDir）*/
    private const val EASYLUA_LIB_NAME = "libeasylua.so"

    /**
     * 部署子目录名。
     *
     * 由 apkbuild 模板渲染替换；保留 "easyLua" 时与 SampleUI 仓库原版一致。
     * 两种使用方式：
     *   - VSIX F5 推 → /data/local/tmp/easyLua/（明文脚本）
     *   - APK 自释放 → /data/local/tmp/easyLua/（启动时强制覆盖）
     *
     * "强制覆盖"保证用户启动 APK 时跑的永远是 APK 内自带脚本，不会被
     * VSIX 推送的明文版本污染；反之 VSIX 工作流不受 APK 安装影响。
     *
     * 占位符没被替换（=SampleUI 仓库自己编译，没走 apkbuild）时回退到 "easyLua"。
     */
    private const val DEPLOY_DIR_NAME = "{{DEPLOY_DIR_NAME}}"

    /** 嵌入资源/可执行文件的部署目录（root 才能读写 /data/local/tmp，但所有 root 进程都能访问） */
    private val DEPLOY_DIR: String = run {
        val name = if (DEPLOY_DIR_NAME.startsWith("{{")) "easyLua" else DEPLOY_DIR_NAME
        "/data/local/tmp/$name"
    }

    /**
     * 运行时基础设施目录：所有"非用户脚本"的资源放这里，方便用户区分。
     *   - libeasylua.so          ← native 引擎
     *   - easylua.dex            ← Java 桥
     *   - _bootstrap.lua         ← 启动引导（设 package.path + dofile 用户入口）
     *
     * 与 scripts/ 平级摆放：
     *   /data/local/tmp/easyLua/
     *   ├── _runtime/            ← 这里
     *   ├── _runtime.tag
     *   └── scripts/             ← 用户脚本
     */
    private val RUNTIME_DIR: String = "$DEPLOY_DIR/_runtime"

    /** native 引擎库部署路径（保留 .so 后缀让 app_process 内 System.load 能加载） */
    private val EASYLUA_SO_PATH: String = "$RUNTIME_DIR/libeasylua.so"

    /** Java 桥 dex 部署路径（被 app_process 通过 -Djava.class.path 加载） */
    private val EASYLUA_DEX_PATH: String = "$RUNTIME_DIR/easylua.dex"

    /** Java 入口类（com.easylua.EasyLuaMain.main） */
    private const val EASYLUA_MAIN_CLASS = "com.easylua.EasyLuaMain"

    /** 部署版本标签文件（用于增量更新；与 _runtime/ 平级方便手动 cat 查看） */
    private val DEPLOY_TAG_PATH: String = "$DEPLOY_DIR/_runtime.tag"

    /**
     * 把 APK 内嵌入的 easyLua 引擎 + assets 资源通过 root 一次性释放到 DEPLOY_DIR。
     * 在 MainActivity onCreate 调用一次。
     *
     * 部署内容：
     *   - libeasylua.so  ← jniLibs（C 引擎，含 LuaJIT）
     *   - easylua.dex    ← assets/easylua/（Java 桥接层）
     *   - scripts/ 下的 .lua  ← assets/scripts/（用户脚本）
     */
    fun init(ctx: android.content.Context) {
        appContext = ctx.applicationContext
        val nativeDir = ctx.applicationInfo.nativeLibraryDir

        // jniLibs 中的 easyLua C 引擎
        val src = java.io.File("$nativeDir/$EASYLUA_LIB_NAME")
        if (!src.exists()) {
            LogPanel.e(TAG, "找不到 easyLua 引擎：${src.absolutePath}")
            return
        }

        // assets：scripts/ + easylua/easylua.dex
        // 在主线程把 assets 先复制到 App 私有 cacheDir，root 阶段再 cat 进 DEPLOY_DIR
        // （su 子进程读不到 APK 内的 assets，但能读 App cacheDir）
        val stagedAssets = stageAssetsToCache(ctx, "scripts") +
                stageAssetsToCache(ctx, "easylua")

        // 用 size+mtime+assets 总数 拼一个 tag，资源没变就跳过部署
        val assetsTotalSize = stagedAssets.values.sumOf { it.length() }
        val tag = "${src.length()}_${src.lastModified()}" +
                "_a${stagedAssets.size}_${assetsTotalSize}"

        Thread {
            try {
                deployAllWithRoot(src, stagedAssets, tag)
            } catch (e: Exception) {
                LogPanel.e(TAG, "释放资源失败：${e.message}")
            }
        }.start()
    }

    /**
     * 把 APK assets 下指定子目录递归复制到 App 的 cacheDir，方便后续 root 阶段直接 cat 写到 DEPLOY_DIR。
     *
     * @return Map<相对于 DEPLOY_DIR 的目标路径, 缓存好的源文件>
     *
     * 例：assetsRoot="scripts" → 返回 {"scripts/script.lua" -> File(.../cache/_assets/scripts/script.lua), ...}
     */
    private fun stageAssetsToCache(
        ctx: android.content.Context,
        assetsRoot: String
    ): Map<String, java.io.File> {
        val result = mutableMapOf<String, java.io.File>()
        val cacheRoot = java.io.File(ctx.cacheDir, "_assets")

        fun recurse(assetsPath: String) {
            val children = try {
                ctx.assets.list(assetsPath) ?: emptyArray()
            } catch (_: Exception) { emptyArray() }

            if (children.isEmpty()) {
                // 叶子：当作文件处理
                try {
                    ctx.assets.open(assetsPath).use { input ->
                        val cached = java.io.File(cacheRoot, assetsPath)
                        cached.parentFile?.mkdirs()
                        java.io.FileOutputStream(cached).use { out ->
                            input.copyTo(out)
                        }
                        result[assetsPath] = cached
                    }
                } catch (_: Exception) { /* 不是文件就忽略 */ }
                return
            }
            // 有子项 → 当目录处理（继续递归）
            for (name in children) {
                val sub = if (assetsPath.isEmpty()) name else "$assetsPath/$name"
                recurse(sub)
            }
        }

        recurse(assetsRoot)
        return result
    }

    /**
     * 把 APK assets 的相对路径映射到设备端部署目录里的相对路径。
     *
     * 规则：
     *   easylua/easylua.dex   → _runtime/easylua.dex   （内部基础设施）
     *   easylua/<其他>        → _runtime/<其他>        （未来 dex 拆分时不破坏）
     *   scripts/<...>         → scripts/<...>          （用户脚本，原样保留）
     *   其它                  → 原样保留
     *
     * 返回 null 表示忽略该 asset（当前没有需要忽略的，预留扩展位）。
     */
    private fun remapAssetTarget(assetRelPath: String): String? {
        return when {
            assetRelPath == "easylua/easylua.dex" -> "_runtime/easylua.dex"
            assetRelPath.startsWith("easylua/") -> "_runtime/" + assetRelPath.removePrefix("easylua/")
            else -> assetRelPath
        }
    }

    /**
     * 通过 root 部署：libeasylua.so + assets 写到 DEPLOY_DIR（tag 没变就跳过）。
     *
     * 设备端目标路径：
     *   /data/local/tmp/easyLua/
     *   ├── _runtime/
     *   │   ├── libeasylua.so       ← jniLibs/<abi>/libeasylua.so
     *   │   └── easylua.dex         ← APK assets/easylua/easylua.dex
     *   ├── _runtime.tag            ← 部署版本标签
     *   └── scripts/                ← APK assets/scripts/<...> 原样保留目录结构
     *       ├── main.luac
     *       ├── ui/ui.lua
     *       └── ...
     *
     * 这样用户/开发者一眼就能区分 "_runtime/ = 系统的别动" 和 "scripts/ = 我的代码"。
     */
    private fun deployAllWithRoot(
        src: java.io.File,
        stagedAssets: Map<String, java.io.File>,
        tag: String
    ) {
        if (!RootHelper.isRootGranted()) {
            LogPanel.w(TAG, "未授权 root，跳过资源部署；请在主页申请 ROOT 后再启动脚本")
            return
        }

        // 注意：APK 自释放模式下不再用 deploy.tag 命中跳过——每次启动 APK 都强制
        // 重新部署 assets，确保运行的永远是 APK 内自带的脚本（用户主动 push 改动
        // 仍需要 VSIX F5 走 /data/local/tmp/easyLua/scripts/ 主目录，与本 APK 隔离）。

        LogPanel.i(TAG, "正在通过 root 部署资源到 $DEPLOY_DIR")

        // 部署脚本（顺序）：
        //   1. DEPLOY_DIR 是文件 → 先 rm（兼容老版本）
        //   2. 清掉老约定残留：根目录的 libeasylua.so / easyLua.tag / easylua/ 子目录
        //   3. mkdir -p DEPLOY_DIR/_runtime
        //   4. 写 libeasylua.so 到 _runtime/（755 让 dlopen 通过）
        //   5. 逐个 assets：mkdir 父目录 → cat → chmod 644（路径按下面的映射重写）
        //   6. 写 _runtime.tag + chown -R shell:shell（方便 adb push 替换脚本）
        val sb = StringBuilder()
        sb.append("([ -f $DEPLOY_DIR ] && rm -f $DEPLOY_DIR; true) && ")
        // 清老约定路径（早期版本把 .so / dex 直接放在 DEPLOY_DIR 下；现在都搬进 _runtime/）
        sb.append("rm -f /data/local/tmp/easyLua.tag $DEPLOY_DIR/easyLua.tag && ")
        sb.append("rm -f $DEPLOY_DIR/libeasylua.so $DEPLOY_DIR/easyLua && ")
        sb.append("rm -rf $DEPLOY_DIR/easylua && ")
        // 清老版本的 _bootstrap.lua（当前版本用 native 端 set_package_path,不再需要这个文件）
        sb.append("rm -f $DEPLOY_DIR/_bootstrap.lua $RUNTIME_DIR/_bootstrap.lua && ")
        // 建主目录 + 运行时子目录
        sb.append("mkdir -p $DEPLOY_DIR && chmod 755 $DEPLOY_DIR && ")
        sb.append("mkdir -p $RUNTIME_DIR && chmod 755 $RUNTIME_DIR && ")
        // libeasylua.so → _runtime/libeasylua.so
        sb.append("cat '${src.absolutePath}' > $EASYLUA_SO_PATH && chmod 755 $EASYLUA_SO_PATH")

        // assets 路径重映射 + 保留相对目录结构
        for ((relPath, cached) in stagedAssets) {
            val dstRel = remapAssetTarget(relPath) ?: continue
            val dst = "$DEPLOY_DIR/$dstRel"
            val parent = dst.substringBeforeLast('/', "$DEPLOY_DIR")
            val srcEsc = cached.absolutePath.replace("'", "'\\''")
            val dstEsc = dst.replace("'", "'\\''")
            val parentEsc = parent.replace("'", "'\\''")
            sb.append(" && mkdir -p '$parentEsc'")
            sb.append(" && cat '$srcEsc' > '$dstEsc' && chmod 644 '$dstEsc'")
        }
        sb.append(" && echo $tag > $DEPLOY_TAG_PATH")
        // chown 让 adb push 可直接覆盖；脚本进程是 root 不在乎权限
        sb.append(" && chown -R shell:shell $DEPLOY_DIR")

        // 走常驻 shell：avoid 重复授权弹窗；现在资源很小，30 秒超时即可
        val rc = RootShell.run(sb.toString(), timeoutMs = 30 * 1000)
        if (rc == 0) {
            val totalKb = (src.length() + stagedAssets.values.sumOf { it.length() }) / 1024
            LogPanel.i(TAG, "资源部署成功：libeasylua.so + ${stagedAssets.size} 个 assets（合计约 ${totalKb}KB）")
        } else {
            LogPanel.e(TAG, "资源部署失败（退出码 $rc）")
        }
    }

    /** 脚本根目录：直接指向部署目录（main.lua 风格）。
     *  历史路径 $DEPLOY_DIR/scripts 仍能跑（VSIX 把文件放哪儿就在哪儿）。 */
    var scriptDir = DEPLOY_DIR

    /**
     * 主脚本路径。所有用户脚本统一在 $DEPLOY_DIR/scripts/ 下；候选优先级（.luac 优先于 .lua）：
     *   1. scripts/main.luac        ← apkbuild + obfus 默认
     *   2. scripts/main.lua         ← VSIX F5 推明文脚本
     *   3. scripts/script.luac      ← 老约定（兼容 SampleUI 自带 demo）
     *   4. scripts/script.lua
     *
     * 兜底：返回 scripts/main.lua（让"找不到脚本"对话框给出最常见的引导路径）。
     */
    val scriptPath: String
        get() {
            val candidates = listOf(
                "$DEPLOY_DIR/scripts/main.luac",
                "$DEPLOY_DIR/scripts/main.lua",
                "$DEPLOY_DIR/scripts/script.luac",
                "$DEPLOY_DIR/scripts/script.lua",
            )
            for (p in candidates) {
                if (File(p).exists()) return p
            }
            return "$DEPLOY_DIR/scripts/main.lua"
        }

    /** UI 配置脚本路径（默认 ui.lua；兼容 ui/ui.lua）。
     *
     *  与 scriptPath 不同：ui.lua 必须保持源码格式（SampleUI 的 LuaUIParser 是文本 DSL
     *  解析器，读不了 .luac 字节码）。所以 .lua 优先于 .luac，apkbuild 流水线也会把
     *  ui.lua 走资源加密通道写到 APK 内（落盘 ui.lua.enc，运行时由 EasyLuaSecret 解密）。
     *
     *  候选优先级：
     *    1) ui.lua / ui/ui.lua          ← 明文，VSIX F5 推送 / 开发期回写后的状态
     *    2) ui.lua.enc / ui/ui.lua.enc  ← APK 内嵌的密文（apkbuild 默认产物）
     *    3) ui.luac / ui/ui.luac        ← 历史路径，已不再使用，留作兼容兜底
     */
    val uiPath: String
        get() {
            val candidates = listOf(
                "$DEPLOY_DIR/scripts/ui.lua",
                "$DEPLOY_DIR/scripts/ui/ui.lua",
                "$DEPLOY_DIR/scripts/ui.lua.enc",
                "$DEPLOY_DIR/scripts/ui/ui.lua.enc",
                "$DEPLOY_DIR/scripts/ui.luac",
                "$DEPLOY_DIR/scripts/ui/ui.luac",
            )
            for (p in candidates) {
                if (File(p).exists()) return p
            }
            return candidates.first()  // 给后续 File(uiPath).exists() 用，命中后失败也无副作用
        }

    /** UI 结果 JSON 临时文件（也写到部署目录里，避免依赖外部存储权限） */
    private val varsFilePath get() = "$DEPLOY_DIR/config.json"

    /** App Context（init 时保存）：让 headless 启动路径不再依赖 Activity */
    @Volatile
    private var appContext: android.content.Context? = null

    /** 当前运行的 Go 进程 */
    @Volatile
    private var process: Process? = null

    /** 当前运行的日志写入器 */
    @Volatile
    private var logWriter: LogFile.Writer? = null

    /** 是否有脚本在运行 */
    val isRunning: Boolean get() = process != null

    /** 进程退出后回调（让 MainActivity 刷新 UI 状态）。 */
    @Volatile
    var onProcessExitCallback: (() -> Unit)? = null

    /** 进程启动后回调（让 MainActivity 切到后台等动作）。 */
    @Volatile
    var onProcessStartedCallback: (() -> Unit)? = null

    /**
     * 进程生命周期 + stdout 行级监听器。
     *
     * 给 DevBridgeServer 等外部模块订阅脚本运行状态用：
     *   - 把 stdout 行（已剥掉 `[easylua]` 内部诊断 + `[[CMD:...]]` 协议行）实时推给 VSIX
     *   - 进程启动/退出时各推一次事件，方便 VSIX 在调试视图上同步状态
     *
     * 与 onProcessStartedCallback / onProcessExitCallback 并存：那两个是 Activity 专用、
     * 单一回调；这里是支持多订阅者、可携带退出码的细粒度事件流。
     */
    interface ProcessListener {
        /** 进程已 fork 成功（已经能拿到 Process 对象）。 */
        fun onStarted() {}
        /** 一行已被显示在日志面板的输出（不含被吞掉的 `[easylua]` / `[[CMD:...]]` 行）。 */
        fun onStdoutLine(line: String) {}
        /** 进程已退出；exitCode = -1 表示因异常未拿到退出码。 */
        fun onExited(exitCode: Int) {}
    }

    private val processListeners = java.util.concurrent.CopyOnWriteArrayList<ProcessListener>()

    fun addProcessListener(l: ProcessListener) { processListeners.addIfAbsent(l) }
    fun removeProcessListener(l: ProcessListener) { processListeners.remove(l) }

    private inline fun fanoutProcess(action: (ProcessListener) -> Unit) {
        for (l in processListeners) {
            try { action(l) } catch (_: Throwable) { /* 隔离监听器异常 */ }
        }
    }

    /**
     * 启动脚本（固定路径）。
     * 如果当前已有脚本在跑，会先停止再启动。
     *
     * 如果有 ui.lua 则用透明 DialogActivity 弹出配置对话框（不会显示主界面），
     * 用户填完确定后再 fork Go 进程。
     */
    fun start(activity: Activity) {
        if (isRunning) {
            LogPanel.w(TAG, "已有脚本在运行，先停止")
            stop()
        }

        if (!File(scriptPath).exists()) {
            LogPanel.e(TAG, "找不到主脚本：$scriptPath")
            // 启动失败 → 悬浮按钮回到停止态（先前已被点击切到运行态）
            activity.runOnUiThread { ControlBubble.setRunning(false) }
            return
        }

        val ui = File(uiPath)
        if (ui.exists()) {
            LogPanel.i(TAG, "发现 ui.lua，启动 DialogActivity 弹配置对话框")
            // 用透明 Activity 弹对话框，不会显示主界面。
            // 注意：从 Activity 上下文启动时 *不要* 加 FLAG_ACTIVITY_NEW_TASK，
            // 否则会因为 DialogActivity 不在 recents 而被桌面化 ROM 显示成第二个任务标签。
            // standard 模式 + 同 task 启动既能保证它出现在前台、又不会多标签。
            val intent = android.content.Intent(activity, DialogActivity::class.java).apply {
                putExtra(DialogActivity.EXTRA_UI_PATH, uiPath)
            }
            activity.startActivity(intent)
            return
        }

        LogPanel.i(TAG, "无 ui.lua，直接启动脚本")
        launchGoProcess(activity, null, scriptPath, headless = false)
    }

    /**
     * DialogActivity 收集到结果后回调本方法，把值回写到 ui.lua 自身（保留注释 / 排版），
     * 然后真正启动脚本进程。脚本端 runtime.lua 启动时会自动 dofile(ui.lua) → Config 表，
     * 因此不再需要中间的 vars 文件。
     *
     * 文件格式自适应：uiPath 可能是 ui.lua（明文）或 ui.lua.enc（apkbuild 默认产物）。
     * 读时按格式解，回写时按相同格式重新加密 / 写明文，保持落盘形态一致。
     */
    fun onDialogResult(activity: Activity, ops: List<LuaUIParser.Op>, result: Map<String, Any?>) {
        // 回写 ui.lua（命中 root 文件，需要走 RootShell；先在 cacheDir 落盘再 cat 过去）
        try {
            val uiFilePath = uiPath
            val isEnc = uiFilePath.endsWith(".enc")
            val src = if (isEnc) {
                EasyLuaSecret.decryptToString(File(uiFilePath).readBytes())
            } else {
                File(uiFilePath).readText()
            }
            val rewritten = UiLuaWriter.rewrite(src, result)
            if (rewritten != null) {
                val cached = File(activity.cacheDir, "ui.lua")
                if (isEnc) {
                    cached.writeBytes(EasyLuaSecret.encryptString(rewritten))
                } else {
                    cached.writeText(rewritten)
                }
                val esc = uiFilePath.replace("'", "'\\''")
                val rc = RootShell.run(
                    "cat '${cached.absolutePath}' > '$esc' && chmod 644 '$esc'"
                )
                if (rc == 0) {
                    LogPanel.i(TAG, "已把对话框值回写到 $uiFilePath")
                } else {
                    LogPanel.w(TAG, "回写 ui.lua 失败 rc=$rc，脚本仍会用旧 default 启动")
                }
            }
        } catch (e: Exception) {
            LogPanel.w(TAG, "回写 ui.lua 异常：${e.message}")
        }

        launchGoProcess(activity, null, scriptPath, headless = false)
    }

    /**
     * 不依赖 Activity 的启动入口（给 DevBridgeServer / VSIX 用，也给手机端"按悬浮球启动"复用）。
     *
     * 与 start(Activity) 的差异：
     *   - 不依赖 Activity：调用方持有 ApplicationContext 即可
     *   - 进程退出后不刷新 MainActivity UI（onProcessExitCallback 不会被调）
     *
     * @param scriptOverride 临时覆盖默认 scriptPath；null 走默认路径
     * @param varsJson      历史保留：可选 vars 文件内容；现在 ui.lua 走"原地回写 + Config 表"路径，
     *                      传 null 即可。
     * @param useDialog     true 时如果发现 ui.lua 就拉起 DialogActivity 弹窗；用户填好后再 fork
     *                      脚本进程。默认 true，与手机 ▶ 按钮的体验一致。
     *                      如果想纯 headless（CI / 自动化测试），明确传 false。
     * @param keepAlive     true = 启动时拉起 [KeepAliveService]，让 SampleUI 进程不被系统回收。
     *                      手机端"用悬浮球启动"必须 true（用户随时切桌面）；VSIX 联调路径默认
     *                      false（USB 一直连着，没必要在通知栏占位置）。
     */
    fun startHeadless(
        scriptOverride: String? = null,
        varsJson: String? = null,
        useDialog: Boolean = true,
        keepAlive: Boolean = false
    ) {
        val ctx = appContext ?: run {
            LogPanel.e(TAG, "ScriptRunner 未初始化：调用 init() 后再 startHeadless()")
            return
        }
        if (isRunning) {
            LogPanel.w(TAG, "已有脚本在运行，先停止")
            stop()
        }
        val script = scriptOverride ?: scriptPath
        if (!File(script).exists()) {
            LogPanel.e(TAG, "找不到主脚本：$script")
            return
        }

        // 有 ui.lua 且开启弹窗：拉起透明 DialogActivity（NEW_TASK 让它能从非前台 Service 拉起）
        if (useDialog) {
            val ui = File(uiPath)
            if (ui.exists()) {
                LogPanel.i(TAG, "headless + ui.lua：启动 DialogActivity 弹配置对话框")
                pendingHeadlessScript = script
                pendingHeadlessKeepAlive = keepAlive
                val intent = android.content.Intent(ctx, DialogActivity::class.java).apply {
                    putExtra(DialogActivity.EXTRA_UI_PATH, uiPath)
                    putExtra(DialogActivity.EXTRA_HEADLESS, true)
                    addFlags(
                        android.content.Intent.FLAG_ACTIVITY_NEW_TASK or
                        android.content.Intent.FLAG_ACTIVITY_CLEAR_TOP
                    )
                }
                ctx.startActivity(intent)
                return
            }
        }

        LogPanel.i(TAG, "headless 启动：$script")
        launchGoProcess(ctx, varsJson, script, headless = true, keepAlive = keepAlive)
    }

    /**
     * VSIX 路径下 DialogActivity 完成时回调本方法。
     * 与 onDialogResult 行为一致（回写 ui.lua → fork），区别是走 headless = true。
     */
    fun onHeadlessDialogResult(activity: Activity, result: Map<String, Any?>) {
        // 回写 ui.lua（自适应明文 / 密文格式）
        try {
            val uiFilePath = uiPath
            val isEnc = uiFilePath.endsWith(".enc")
            val src = if (isEnc) {
                EasyLuaSecret.decryptToString(File(uiFilePath).readBytes())
            } else {
                File(uiFilePath).readText()
            }
            val rewritten = UiLuaWriter.rewrite(src, result)
            if (rewritten != null) {
                val cached = File(activity.cacheDir, "ui.lua")
                if (isEnc) {
                    cached.writeBytes(EasyLuaSecret.encryptString(rewritten))
                } else {
                    cached.writeText(rewritten)
                }
                val esc = uiFilePath.replace("'", "'\\''")
                val rc = RootShell.run(
                    "cat '${cached.absolutePath}' > '$esc' && chmod 644 '$esc'"
                )
                if (rc == 0) LogPanel.i(TAG, "已把对话框值回写到 $uiFilePath")
                else         LogPanel.w(TAG, "回写 ui.lua 失败 rc=$rc")
            }
        } catch (e: Exception) {
            LogPanel.w(TAG, "回写 ui.lua 异常：${e.message}")
        }

        val script = pendingHeadlessScript ?: scriptPath
        val keepAlive = pendingHeadlessKeepAlive
        pendingHeadlessScript = null
        pendingHeadlessKeepAlive = false
        val ctx = appContext ?: activity.applicationContext
        launchGoProcess(ctx, null, script, headless = true, keepAlive = keepAlive)
    }

    /** VSIX headless 路径下，DialogActivity 暂存的脚本路径 */
    @Volatile
    private var pendingHeadlessScript: String? = null

    /** headless dialog 路径上"是否需要保活通知"的暂存（手机悬浮球路径 = true，VSIX 路径 = false） */
    @Volatile
    private var pendingHeadlessKeepAlive: Boolean = false

    /**
     * DialogActivity 在 VSIX headless 路径上被取消时调用。
     *
     * 这条路径上脚本进程还没 fork（dialog 是在 fork 之前弹的），所以 launchGoProcess
     * 内的 onExited fanout 永远不会被触发，VSIX 端的 session 状态会一直停在 `running`，
     * 既不能再点运行也不能点停止。
     *
     * 这里手动派发一个等价的 onExited(0) 事件给所有 ProcessListener（DevBridge 会
     * 转成 EVT_SCRIPT_EXITED 推给 VSIX），让 VSIX 状态机回到 `connected` 空闲态。
     *
     * exitCode = 0 表示"用户主动取消"，不算异常退出；与脚本正常 return 0 同义。
     */
    fun onHeadlessDialogCancelled() {
        if (pendingHeadlessScript == null) return  // 不是 VSIX headless 路径，无需处理
        pendingHeadlessScript = null
        pendingHeadlessKeepAlive = false
        LogPanel.i(TAG, "VSIX headless 路径：用户取消配置对话框，通知 VSIX 回到空闲态")
        fanoutProcess { it.onExited(0) }
    }

    /** 停止当前脚本并隐藏控制球。 */
    fun stop() {
        // 1) 如果配置对话框还在前台（脚本进程还没 fork），把它关掉
        DialogActivity.dismissIfShowing()

        // 1b) headless 路径：dialog 还没确认就被外部 stop（VSIX 点停止 / 重新运行）。
        //     这里兜底通知一次，避免依赖 dialog onDismissListener 在 Activity finish
        //     时是否会触发——某些机型上 finish() 直接销毁 window，listener 不会跑。
        //     onHeadlessDialogCancelled 内部对 pendingHeadlessScript 做了空检查，
        //     与 DialogActivity.onCancel 的调用幂等，重复调用只会派发一次退出事件。
        onHeadlessDialogCancelled()

        // 2) 进程已 fork：杀进程
        val p = process
        if (p != null) {
            LogPanel.i(TAG, "停止脚本进程")
            try {
                // 用常驻 root shell 杀进程，避免再次触发授权弹窗
                RootShell.run("pkill -f $EASYLUA_MAIN_CLASS 2>/dev/null")
                // 同时把 su wrapper 杀掉
                p.destroy()
            } catch (e: Exception) {
                LogPanel.e(TAG, "停止失败：${e.message}")
            }
            process = null
            return
        }

        // 3) 进程还未启动也没对话框残留：把悬浮球切回 ▶ 停止态，避免视觉残留
        try {
            ControlBubble.setRunning(false)
        } catch (_: Exception) {}
    }

    /**
     * 启动 Go 子进程，stdout/stderr 同时写入屏幕日志和文件日志。
     *
     * @param ctx        任意 Context；UI 操作走主线程 Handler，不再依赖 Activity
     * @param varsJson   ui.lua 收集到的参数，null 表示不写
     * @param script     要跑的脚本绝对路径（VSIX 可指定临时脚本）
     * @param headless   true = 不刷主页 UI 回调（Activity 生命周期相关）；
     *                   悬浮球状态在两条路径都需要刷新，独立处理
     * @param keepAlive  true = 启动 [KeepAliveService] 让 SampleUI 进程在用户切桌面后
     *                   仍不被系统回收。手机端走 ▶ 时必填 true；VSIX 联调路径默认 false。
     */
    private fun launchGoProcess(
        ctx: android.content.Context,
        varsJson: String?,
        script: String,
        headless: Boolean,
        keepAlive: Boolean = !headless
    ) {
        val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())
        val onMain: (() -> Unit) -> Unit = { action ->
            if (headless) { /* headless 模式不刷 UI */ }
            else mainHandler.post { action() }
        }

        // 检查二进制是否已部署
        val deployedBin = File(EASYLUA_SO_PATH)
        if (!deployedBin.exists()) {
            LogPanel.e(TAG, "easyLua 引擎未部署：$EASYLUA_SO_PATH")
            LogPanel.i(TAG, "请确认已授权 ROOT；授权后重启 App 自动部署")
            onMain { ControlBubble.setRunning(false) }
            fanoutProcess { it.onExited(-1) }
            return
        }
        if (!RootHelper.isRootGranted()) {
            LogPanel.e(TAG, "未授权 root，无法启动脚本进程")
            onMain { ControlBubble.setRunning(false) }
            fanoutProcess { it.onExited(-1) }
            return
        }

        // 准备 UI 参数文件（写到 DEPLOY_DIR 内）：先在 cacheDir 落盘，再用常驻 root shell
        // cat 过去；避免每次启动都开新 su 进程触发授权
        if (!varsJson.isNullOrEmpty()) {
            try {
                val cached = File(ctx.cacheDir, "config.json")
                cached.writeText(varsJson)
                val cmd = "cat '${cached.absolutePath}' > '$varsFilePath' && chmod 644 '$varsFilePath'"
                val rc = RootShell.run(cmd)
                if (rc != 0) {
                    LogPanel.e(TAG, "写参数文件失败（root 写入返回 $rc）")
                    onMain { ControlBubble.setRunning(false) }
                    fanoutProcess { it.onExited(-1) }
                    return
                }
                LogPanel.i(TAG, "UI 参数已写入：$varsFilePath")
            } catch (e: Exception) {
                LogPanel.e(TAG, "写参数文件失败：${e.message}")
                onMain { ControlBubble.setRunning(false) }
                fanoutProcess { it.onExited(-1) }
                return
            }
        }

        // 启动前的准备：清掉残留的 app_process 子进程
        // chmod 主要是给 .so 加权限（dlopen 需要可读）
        RootShell.run("chmod 755 $EASYLUA_SO_PATH 2>/dev/null; pkill -f $EASYLUA_MAIN_CLASS 2>/dev/null; sleep 0.25")

        // 启动命令：
        //   CLASSPATH=easylua.dex app_process /system/bin com.easylua.EasyLuaMain --lua libeasylua.so script.lua [vars]
        //
        // app_process 加载 dex 后调 EasyLuaMain.main(args)，args 里：
        //   args[0] = "--lua"
        //   args[1] = libeasylua.so 绝对路径
        //   args[2] = 用户脚本（main.luac / main.lua / 任意 .lua）绝对路径
        //   args[3..] = 附加参数（vars 文件）
        // 启动命令：
        //   cd /data/local/tmp/easyLua && \
        //     CLASSPATH=_runtime/easylua.dex \
        //     app_process /system/bin com.easylua.EasyLuaMain \
        //       --lua _runtime/libeasylua.so scripts/main.luac [vars]
        //
        // package.path 由 native 端 (libeasylua.so / run_script) 自动设好——
        // 它从 script_path 派生 deploy_dir（找 /scripts/ 之前那一层），
        // 再设置 scripts/?.luac, scripts/libs/?.lua 等候选。
        // 因此用户脚本可以直接 require("libs.foo") 命中 scripts/libs/foo.lua。
        // 早期版本走 _bootstrap.lua 文件中转,现在已废弃。
        val varsArg = if (!varsJson.isNullOrEmpty()) " '--vars-file' '$varsFilePath'" else ""
        val scriptEsc = script.replace("'", "'\\''")
        val shellCmd = "cd '$DEPLOY_DIR' && " +
                "CLASSPATH='$EASYLUA_DEX_PATH' " +
                "exec app_process /system/bin '$EASYLUA_MAIN_CLASS' " +
                "'--lua' '$EASYLUA_SO_PATH' '$scriptEsc'$varsArg"
        val cmd = arrayOf("su", "-c", shellCmd)

        // 创建本次运行的日志文件
        val log = LogFile.create()
        logWriter = log
        val timeFmt = java.text.SimpleDateFormat("HH:mm:ss.SSS", java.util.Locale.getDefault())
        fun stamp() = "[${timeFmt.format(java.util.Date())}]"
        
        LogPanel.i(TAG, "启动：$shellCmd")
        LogPanel.i(TAG, "日志文件：${log.file.absolutePath}")

        // 启动前台保活服务，让进程在后台不被系统轻易杀掉。
        // 仅在调用方明确要求时启用：
        //   - 手机端 ▶ 路径（用户随时可能切桌面）→ keepAlive = true
        //   - VSIX 联调路径（USB 一直连着、VSIX 自带运行指示器）→ keepAlive = false
        if (keepAlive) {
            KeepAliveService.start(ctx, "脚本运行中", "点击返回 easyLua")
        }

        // 切换悬浮球到运行态。
        // 无论谁触发的运行（手机 ▶ / VSIX）都应该刷悬浮球：headless 路径下 ControlBubble
        // 仍然是 SYSTEM_ALERT_WINDOW 全局浮窗，主界面 finish 后它依然在屏，状态切换有意义。
        // onMain 在 headless 下是 no-op，必须直接 post。
        mainHandler.post { ControlBubble.setRunning(true) }

        // fork 进程
        Thread {
            try {
                val p = ProcessBuilder(*cmd)
                    .redirectErrorStream(true)
                    .start()
                process = p

                // 通知主线程进程已启动
                onProcessStartedCallback?.invoke()
                onProcessStartedCallback = null
                fanoutProcess { it.onStarted() }

                BufferedReader(InputStreamReader(p.inputStream)).use { reader ->
                    var line: String?
                    // 续行状态：上一条 stdout 行带了 native 端的 [HH:MM:SS.mmm] 前缀，
                    // 它后面紧跟的不带前缀行视为同一次 print 输出的多行内容
                    // （比如 cjson.encode 出来的 pretty JSON）。续行不补 stamp、不加 tag，
                    // 沿用上一行的颜色，避免每行都被打上"script  ..."标签和重复时间戳。
                    var lastStampedColor: Int? = null
                    while (reader.readLine().also { line = it } != null) {
                        val l = line ?: continue

                        // 启动 banner / 内部诊断行：
                        //   [easylua]      boot/hello/stream started/script exit/bye 等普通诊断
                        //   [easylua-c]    C 引擎层日志，含 lua 错误（"script error" / "script load error"）
                        //   [easylua-perf] 性能采样
                        //
                        // 错误类（含 "error"/"FATAL"/"exit code = " 非零）必须送到日志面板，
                        // 否则脚本静默 exitCode=1 用户找不到原因。其他诊断只写日志文件。
                        if (l.startsWith("[easylua]") ||
                            l.startsWith("[easylua-c]") ||
                            l.startsWith("[easylua-perf]")) {
                            log.write(l)
                            val isError = l.contains("error", ignoreCase = true) ||
                                          l.contains("FATAL", ignoreCase = true)
                            if (isError) {
                                LogPanel.e("script", l)
                                fanoutProcess { it.onStdoutLine(l) }
                            }
                            lastStampedColor = null
                            continue
                        }

                        // 识别脚本通过 stdout 发出的命令：
                        //   [[CMD:name]]{...json...}
                        if (l.startsWith("[[CMD:")) {
                            val end = l.indexOf("]]")
                            if (end > 0) {
                                val name = l.substring(6, end)
                                val payload = l.substring(end + 2)
                                dispatchScriptCommand(ctx, name, payload)
                                continue  // 命令行不写日志、不显示
                            }
                        }

                        // native 端 print 自带 [HH:MM:SS.mmm script.lua:line] 前缀。
                        // 检测到行首是 "[数字数字:" 就视为已带时间戳，原样写入和显示。
                        val alreadyStamped = l.length >= 4 &&
                                l[0] == '[' &&
                                l[1].isDigit() && l[2].isDigit() &&
                                l[3] == ':'

                        if (alreadyStamped) {
                            log.write(l)
                            // 按内容关键字着色（错误红 / 警告黄 / 普通绿）
                            val color: Int = when {
                                l.contains("error", true) || l.contains("[ERROR]") -> 0xFFF44336.toInt()
                                l.contains("warn",  true) || l.contains("[WARN]")  -> 0xFFFFC107.toInt()
                                else -> 0xFF8BC34A.toInt()
                            }
                            LogPanel.line(color, l)
                            lastStampedColor = color
                        } else if (lastStampedColor != null) {
                            // 续行：上一行带 stamp，这行没有，视为同一次 print 调用输出的
                            // 多行内容。原样写入日志文件 + 用上一行的颜色显示，不补 stamp、不加 tag。
                            log.write(l)
                            LogPanel.line(lastStampedColor!!, l)
                        } else {
                            // 兜底：没有前缀的旧式输出，仍走 LogPanel 加时间戳
                            log.write("${stamp()} $l")
                            when {
                                l.startsWith("[ERROR]") || l.contains("error", true) ->
                                    LogPanel.e("script", l)
                                l.startsWith("[WARN]") || l.contains("warn", true) ->
                                    LogPanel.w("script", l)
                                else -> LogPanel.i("script", l)
                            }
                        }

                        // 推给 ProcessListener（已剥掉内部诊断行 / 协议行）
                        fanoutProcess { it.onStdoutLine(l) }
                    }
                }

                val code = p.waitFor()
                log.write("")
                log.write("${stamp()} === 脚本退出，退出码: $code ===")
                if (code == 0) {
                    LogPanel.i(TAG, "脚本正常结束")
                } else {
                    LogPanel.w(TAG, "脚本退出码：$code")
                }
                fanoutProcess { it.onExited(code) }
            } catch (e: Exception) {
                log.write("${stamp()} [FATAL] 执行异常: ${e.message}")
                LogPanel.e(TAG, "执行异常：${e.message}")
                fanoutProcess { it.onExited(-1) }
            } finally {
                log.close()
                logWriter = null
                process = null
                // 关闭前台保活服务（仅在 keepAlive 路径上启过）。
                // 不依赖 headless 标志：headless 与 keepAlive 现在解耦——前者表示
                // "不刷主界面 UI"、后者表示"是否需要常驻通知防回收"。
                if (keepAlive) {
                    KeepAliveService.stop(ctx)
                }
                // 进程退出，悬浮球切回 ▶ 停止态。
                // 与启动时一致：headless 路径也要切（onMain 在 headless 下是 no-op，
                // 必须直接 post 到主线程）。
                mainHandler.post {
                    ControlBubble.setRunning(false)
                    if (!headless) onProcessExitCallback?.invoke()
                }
            }
        }.start()
    }

    /** 把 OpRenderer 收集到的 result 转为 JSON。 */
    private fun buildVarsJson(
        ops: List<LuaUIParser.Op>,
        result: Map<String, Any?>
    ): String {
        val json = JSONObject()
        for (op in ops) {
            val name = op.boundVarName() ?: continue
            val v = result[name]
            when (op) {
                is LuaUIParser.Op.CheckBox -> json.put(name, v as? Boolean ?: false)
                is LuaUIParser.Op.EditText -> json.put(name, v as? String ?: "")
                is LuaUIParser.Op.EditNumber -> json.put(name, v as? Double ?: 0.0)
                is LuaUIParser.Op.Spinner -> json.put(name, v as? String ?: "")
                is LuaUIParser.Op.RadioGroup -> json.put(name, v as? String ?: "")
                is LuaUIParser.Op.SeekBar -> json.put(name, v as? Double ?: 0.0)
                is LuaUIParser.Op.ProgressBar -> json.put(name, v as? Double ?: 0.0)
                is LuaUIParser.Op.DatePicker -> json.put(name, v as? String ?: "")
                is LuaUIParser.Op.TimePicker -> json.put(name, v as? String ?: "")
                is LuaUIParser.Op.ColorButton -> json.put(name, v as? String ?: "")
                else -> {}
            }
        }
        return json.toString(2)
    }

    /**
     * 处理脚本通过 stdout 协议下发的命令。
     *
     * 协议：单行 `[[CMD:name]]{json_payload}`
     *   - bubble.pos:     {"x": 0.5, "y": 0.5}                按屏幕百分比定位悬浮按钮
     *   - highlight.show: {"x":100,"y":200,"w":300,"h":400,"text":"...","color":"#FF0000"}
     *   - highlight.hide: {}
     *   - toast:          {"msg": "...", "dur": 2000}         通过 Android Toast 弹消息
     *
     * 未识别的 name 仅打印一条警告日志，不报错。
     */
    private fun dispatchScriptCommand(ctx: android.content.Context, name: String, payload: String) {
        val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())
        try {
            val json = if (payload.isNotBlank()) JSONObject(payload) else JSONObject()
            when (name) {
                "bubble.pos" -> {
                    val x = json.optDouble("x", 0.0).toFloat()
                    val y = json.optDouble("y", 0.0).toFloat()
                    mainHandler.post {
                        ControlBubble.setPositionPercent(x, y)
                    }
                    LogPanel.i(TAG, "[CMD] bubble.pos x=$x y=$y")
                }
                "highlight.show" -> {
                    val x = json.optInt("x", 0)
                    val y = json.optInt("y", 0)
                    val w = json.optInt("w", 0)
                    val h = json.optInt("h", 0)
                    val text = json.optString("text", "")
                    val colorStr = json.optString("color", "#FF0000")
                    val colorInt = parseHexColor(colorStr) ?: 0xFFFF0000.toInt()
                    mainHandler.post {
                        com.example.sampleui.ui.HighlightOverlay.show(
                            ctx,
                            android.graphics.Rect(x, y, x + w, y + h),
                            if (text.isEmpty()) null else text,
                            colorInt
                        )
                    }
                    LogPanel.i(TAG, "[CMD] highlight.show ($x,$y,${w}x${h}) text=\"$text\" color=$colorStr")
                }
                "highlight.hide" -> {
                    mainHandler.post {
                        com.example.sampleui.ui.HighlightOverlay.hide(ctx)
                    }
                    LogPanel.i(TAG, "[CMD] highlight.hide")
                }
                "toast" -> {
                    val msg = json.optString("msg", "")
                    val dur = json.optInt("dur", 2000)
                    mainHandler.post {
                        val len = if (dur >= 3500) android.widget.Toast.LENGTH_LONG
                                   else android.widget.Toast.LENGTH_SHORT
                        android.widget.Toast.makeText(ctx.applicationContext, msg, len).show()
                    }
                }
                else -> LogPanel.w(TAG, "未识别脚本命令：$name")
            }
        } catch (e: Exception) {
            LogPanel.w(TAG, "解析命令 [$name] 失败：${e.message}（payload=$payload）")
        }
    }

    /**
     * 解析 #RGB / #RRGGBB / #AARRGGBB / 0xAARRGGBB 等十六进制颜色串到 Android ARGB int。
     * 解析失败返回 null 由调用方兜底。
     */
    private fun parseHexColor(s: String): Int? {
        if (s.isBlank()) return null
        val raw = s.trim().removePrefix("#").removePrefix("0x").removePrefix("0X")
        return try {
            when (raw.length) {
                3 -> {
                    // RGB → RRGGBB
                    val r = raw[0].toString().repeat(2)
                    val g = raw[1].toString().repeat(2)
                    val b = raw[2].toString().repeat(2)
                    (0xFF000000.toInt()) or ("$r$g$b").toLong(16).toInt()
                }
                6 -> (0xFF000000.toInt()) or raw.toLong(16).toInt()
                8 -> raw.toLong(16).toInt()
                else -> null
            }
        } catch (_: Exception) { null }
    }
}
