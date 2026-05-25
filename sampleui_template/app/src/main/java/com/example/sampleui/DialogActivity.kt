package com.example.sampleui

import android.os.Bundle
import android.support.v7.app.AppCompatActivity
import com.example.sampleui.ui.ControlBubble
import com.example.sampleui.ui.EasyLuaSecret
import com.example.sampleui.ui.LuaUIParser
import com.example.sampleui.ui.OpRenderer

/**
 * 透明 Activity：仅用来承载 ui.lua 配置对话框。
 *
 * 主 Activity 已经被 moveTaskToBack 收到桌面后，单独的 Dialog 没法弹（需要前台 Activity context）。
 * 我们用一个透明主题的 Activity 临时拉起：用户只看到对话框浮在桌面之上，
 * 看不到 SampleUI 主界面。用户点确定/取消后此 Activity finish，回到桌面。
 *
 * 由 ScriptRunner.start 启动；启动时把 ui.lua 路径作为 extra 传入。
 */
class DialogActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        currentInstance = this

        // 兜底强制状态栏 / 导航栏完全透明：部分 ROM 即使在 styles.xml 里设了
        // android:windowIsTranslucent=true 仍会在 Activity 顶部叠一条不透明深色
        // （来自系统默认 statusBarColor）。这里在运行期再设一次，覆盖 ROM 默认。
        try {
            val window = window
            // SDK 21+ 才有 setStatusBarColor / setNavigationBarColor
            window.statusBarColor = android.graphics.Color.TRANSPARENT
            window.navigationBarColor = android.graphics.Color.TRANSPARENT
            // 让 layout 延伸到状态栏底下，避免被状态栏挤压
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            )
        } catch (_: Throwable) { /* 老 SDK 兜底 */ }

        val uiPath = intent.getStringExtra(EXTRA_UI_PATH)
        if (uiPath.isNullOrEmpty()) {
            ControlBubble.setRunning(false)
            finish()
            return
        }
        val source = try {
            // 方案 B1：ui.lua 在 APK 内已加密为 ui.lua.enc。
            // 优先按密文路径读 → EasyLuaSecret 解密；找不到再退回明文 ui.lua（VSIX 推送等场景）。
            if (uiPath.endsWith(".enc")) {
                EasyLuaSecret.decryptToString(java.io.File(uiPath).readBytes())
            } else {
                java.io.File(uiPath).readText()
            }
        } catch (e: Exception) {
            LogPanel.e(TAG, "读取 ui.lua 失败：${e.message}")
            ControlBubble.setRunning(false)
            finish()
            return
        }
        val parsed = LuaUIParser.parse(source)
        if (parsed == null) {
            LogPanel.w(TAG, "ui.lua 解析为空")
            ControlBubble.setRunning(false)
            finish()
            return
        }

        OpRenderer(this).show(
            ui = parsed,
            fullScreen = false,
            onCancel = {
                LogPanel.i(TAG, "用户取消了配置对话框")
                // 用户取消 → 脚本未真正启动，悬浮按钮回到 ▶ 停止态
                ControlBubble.setRunning(false)
                // VSIX headless 路径还需要通知一下 ScriptRunner，让它把"挂起的脚本"
                // 清空并向 ProcessListener 派发一个等价的 onExited(0)，否则 VSIX 端
                // session 会一直停在 running 状态。
                if (intent.getBooleanExtra(EXTRA_HEADLESS, false)) {
                    ScriptRunner.onHeadlessDialogCancelled()
                }
                finish()
            },
            onDone = { result ->
                // 把结果通过单例传回给 ScriptRunner，再 finish 自身
                if (intent.getBooleanExtra(EXTRA_HEADLESS, false)) {
                    ScriptRunner.onHeadlessDialogResult(this, result)
                } else {
                    ScriptRunner.onDialogResult(this, parsed.ops, result)
                }
                finish()
            }
        )
    }

    override fun onStart() {
        super.onStart()
        ControlBubble.pushHide()
    }

    override fun onPause() {
        super.onPause()
        // 关闭转场动画，避免对话框消失时短暂闪一下空白页
        overridePendingTransition(0, 0)
    }

    override fun onStop() {
        super.onStop()
        ControlBubble.popHide()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (currentInstance === this) currentInstance = null
    }

    companion object {
        private const val TAG = "DialogActivity"
        const val EXTRA_UI_PATH = "ui_path"

        /** 弹窗是否给 VSIX headless 路径用：true 时确认后回调 onHeadlessDialogResult */
        const val EXTRA_HEADLESS = "headless"

        /**
         * 当前正在显示的 DialogActivity 实例引用。
         *
         * 用途：脚本进程还没 fork 时（用户刚点 ▶ 弹出配置对话框、还没确认），
         * 用户再点 ⏸ 取消，需要把对话框一起关掉。由 ScriptRunner.stop() 触发
         * [dismissIfShowing] 来 finish 这个 Activity。
         */
        @Volatile
        private var currentInstance: DialogActivity? = null

        /** 如果当前有 DialogActivity 在显示，就把它 finish 掉（用户取消语义）。 */
        fun dismissIfShowing() {
            val a = currentInstance ?: return
            if (!a.isFinishing) {
                a.runOnUiThread {
                    // 视为"用户取消"：悬浮球回到 ▶ 停止态
                    ControlBubble.setRunning(false)
                    a.finish()
                }
            }
        }
    }
}
