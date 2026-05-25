package com.example.sampleui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.support.v4.app.ActivityCompat
import android.support.v4.content.ContextCompat
import android.support.v7.app.AlertDialog
import android.support.v7.app.AppCompatActivity
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import com.example.sampleui.control.DevBridgeServer
import com.example.sampleui.ui.ControlBubble
import java.io.File

/**
 * easyLua 主页：脚本启动壳。
 *
 * 简洁列表式布局：
 *   查看脚本运行日志             ›
 *   开启ROOT(激活)模式服务       ☐
 *   开启悬浮窗权限                ☐
 *   [           启动脚本         ]
 */
class MainActivity : AppCompatActivity() {

    private lateinit var tvStatus: TextView
    private lateinit var cbRoot: CheckBox
    private lateinit var cbOverlay: CheckBox
    private lateinit var btnStart: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        cbRoot = findViewById(R.id.cbRoot)
        cbOverlay = findViewById(R.id.cbOverlay)
        btnStart = findViewById(R.id.btnStart)

        // 行点击：日志 → 进入列表页
        findViewById<LinearLayout>(R.id.cardLogs).setOnClickListener {
            startActivity(Intent(this, LogListActivity::class.java))
        }
        // 行点击：ROOT → 检查/申请
        findViewById<LinearLayout>(R.id.cardRoot).setOnClickListener {
            requestRootInBackground()
        }
        // 行点击：悬浮窗 → 跳转设置
        findViewById<LinearLayout>(R.id.cardOverlay).setOnClickListener {
            requestOverlayPermission()
        }
        // 启动按钮
        btnStart.setOnClickListener { onStartClicked() }

        // 注册脚本退出回调
        ScriptRunner.onProcessExitCallback = {
            runOnUiThread { updateRunStatus() }
        }

        // 解析 Go 二进制路径（从 jniLibs 拿，不再固定 /data/local/tmp）
        ScriptRunner.init(this)

        // 启动 VSIX 开发桥服务（VSIX 通过 adb forward tcp:8790 接入，远程同步脚本/运行/看日志）
        ensureDevBridgeServer()

        // 申请存储权限（第一次启动）
        requestStoragePermissionIfNeeded()
        // 引导加入电池优化白名单（不强制）
        promptBatteryOptimizationOnce()
        // scriptDir 现在位于 /data/local/tmp/easyLua/scripts，需要 root 才能 mkdir，
        // 由 ScriptRunner.init() 通过 root 一次性创建并部署 assets，这里不再尝试。
        LogFile.ensureDir()

        refreshAll()
    }

    override fun onResume() {
        super.onResume()
        refreshAll()
    }

    override fun onStart() {
        super.onStart()
        // 用 push/pop 引用计数避免 Activity 切换瞬间 bubble 闪烁。
        // 系统保证："新 Activity onStart" 早于 "旧 Activity onStop"，
        // 所以过渡期间 hideStack > 0，bubble 始终隐藏。
        ControlBubble.pushHide()
    }

    override fun onStop() {
        super.onStop()
        ControlBubble.popHide()
    }

    override fun onDestroy() {
        super.onDestroy()
        LogPanel.detach()
    }

    private fun refreshAll() {
        updateRunStatus()
        cbOverlay.isChecked = hasOverlayPermission()
        // 如果常驻 RootShell 里已经记录拿到过 root，就直接显示已勾选，不再重发 `su id`
        // 触发授权弹窗。只有未授权时才异步尝试一次。
        if (RootShell.isRootGranted()) {
            cbRoot.isChecked = true
        } else {
            Thread {
                val ok = RootHelper.isRootGranted()
                runOnUiThread { cbRoot.isChecked = ok }
            }.start()
        }
    }

    // ---------- 启动脚本 ----------

    private fun onStartClicked() {
        if (!File(ScriptRunner.scriptPath).exists()) {
            AlertDialog.Builder(this)
                .setTitle("找不到脚本")
                .setMessage("请先把主脚本放到：\n${ScriptRunner.scriptPath}")
                .setPositiveButton("好", null)
                .show()
            return
        }
        if (!hasOverlayPermission()) {
            Toast.makeText(this, "需要悬浮窗权限以显示控制按钮", Toast.LENGTH_SHORT).show()
            requestOverlayPermission()
            return
        }

        // 显示悬浮球（默认停止态，▶ 图标），不立即启动脚本。
        // 关键：用 ApplicationContext，让悬浮球的生命周期跟 Application 绑定，
        // 而不是当前 MainActivity——这样下面 finish() 主界面后悬浮球依然在屏。
        //
        // onPlay 走 startHeadless(keepAlive=true)：
        //   - 不依赖 Activity（避免拉起隐形 task 把主界面带回前台）
        //   - keepAlive=true 启 KeepAliveService 防止 SampleUI 进程被系统回收
        ControlBubble.show(
            ctx = applicationContext,
            onPlay = { ScriptRunner.startHeadless(keepAlive = true) },
            onPause = { ScriptRunner.stop() }
        )
        // 每次"启动脚本"都把悬浮球复位到左下角，避免上次拖动/脚本调用
        // setPositionPercent 残留的位置
        ControlBubble.resetToDefaultPosition()

        // 直接 finish 主界面（不要 moveTaskToBack）。
        //
        // 原因：DialogActivity 现在用 standard launchMode + 默认 affinity 启动。
        // 如果 MainActivity 还在 task 栈底，弹 dialog 时整个 task 被前台化，
        // 用户会先看到 MainActivity 闪一下再被透明 dialog 盖住。
        // finish 后 task 里没有可见 Activity，dialog 启动只显示自己。
        //
        // 用户想回主页：点桌面图标重启即可。DevBridgeServer + ControlBubble 都跟
        // Application 进程绑定，不会因为这次 finish 而中断。
        finish()
    }

    // ---------- ROOT ----------

    private fun requestRootInBackground() {
        if (cbRoot.isChecked) {
            // 已授权时点击只是再确认一次
            Toast.makeText(this, "ROOT 已授权", Toast.LENGTH_SHORT).show()
            return
        }
        Toast.makeText(this, "请在弹出的授权窗中允许", Toast.LENGTH_SHORT).show()
        Thread {
            val ok = RootHelper.requestRoot()
            runOnUiThread { cbRoot.isChecked = ok }
        }.start()
    }

    // ---------- 悬浮窗权限 ----------

    private fun hasOverlayPermission(): Boolean {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.M ||
                Settings.canDrawOverlays(this)
    }

    private fun requestOverlayPermission() {
        if (hasOverlayPermission()) {
            cbOverlay.isChecked = true
            return
        }
        AlertDialog.Builder(this)
            .setTitle("需要悬浮窗权限")
            .setMessage("脚本运行时显示控制悬浮球，方便随时停止。\n请在下一页授权。")
            .setPositiveButton("去授权") { _, _ ->
                val intent = Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:$packageName")
                )
                startActivityForResult(intent, REQ_OVERLAY)
            }
            .setNegativeButton("跳过", null)
            .show()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_OVERLAY) {
            cbOverlay.isChecked = hasOverlayPermission()
        }
    }

    // ---------- 电池优化白名单 ----------

    /** 首次启动引导一次，不强制；用户拒绝后不会再弹（基于 SharedPreferences）。 */
    private fun promptBatteryOptimizationOnce() {
        if (BatteryOptHelper.isIgnoringBatteryOptimizations(this)) return
        val prefs = getSharedPreferences("sampleui", MODE_PRIVATE)
        if (prefs.getBoolean("batt_opt_prompted", false)) return
        prefs.edit().putBoolean("batt_opt_prompted", true).apply()

        AlertDialog.Builder(this)
            .setTitle("建议加入电池优化白名单")
            .setMessage(
                "Android 会在后台限制 App 的运行，可能导致脚本被中断。\n" +
                        "把 easyLua 加入白名单后，系统不会再对它省电限制。"
            )
            .setPositiveButton("去设置") { _, _ ->
                BatteryOptHelper.requestIgnoreBatteryOptimizations(this)
            }
            .setNegativeButton("以后再说", null)
            .show()
    }

    // ---------- 存储权限 ----------

    private fun requestStoragePermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return
        val perms = arrayOf(
            Manifest.permission.READ_EXTERNAL_STORAGE,
            Manifest.permission.WRITE_EXTERNAL_STORAGE
        )
        val missing = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), REQ_STORAGE)
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_STORAGE) {
            // scriptDir 由 root 部署时创建，这里只确保日志目录可写
            LogFile.ensureDir()
        }
    }

    // ---------- 状态显示 ----------

    private fun updateRunStatus() {
        val running = ScriptRunner.isRunning
        tvStatus.text = if (running) "运行中" else "未运行"
        tvStatus.setTextColor(
            if (running) 0xFF4CAF50.toInt() else 0xFF888888.toInt()
        )
        btnStart.isEnabled = !running
        btnStart.text = if (running) "运行中（用悬浮球停止）" else "启动脚本"
    }

    companion object {
        private const val TAG = "MainActivity"
        private const val REQ_OVERLAY = 1001
        private const val REQ_STORAGE = 1002

        /** 全 App 共用的 VSIX 开发桥服务（PC 端 VSCode 扩展接入），只在第一次启动时创建。 */
        @Volatile
        private var devBridgeServer: DevBridgeServer? = null

        @Synchronized
        private fun ensureDevBridgeServer(appContext: android.content.Context) {
            if (devBridgeServer != null) return
            val srv = DevBridgeServer(appContext)
            srv.start()
            devBridgeServer = srv
        }
    }

    private fun ensureDevBridgeServer() {
        ensureDevBridgeServer(applicationContext)
    }
}
