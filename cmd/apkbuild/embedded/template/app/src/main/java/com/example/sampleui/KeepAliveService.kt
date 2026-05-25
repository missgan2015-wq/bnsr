package com.example.sampleui

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.support.v4.app.NotificationCompat

/**
 * 前台服务：通过持续通知占据一个"前台"位置，让系统不会轻易杀掉本进程。
 *
 * 这是 Android 8+ 推荐的进程保活方案：
 *   - 必须有一个持续可见的通知（用户能感知到 App 在跑）
 *   - 系统在内存紧张时也不会主动杀前台服务进程
 *
 * 用法：
 *   KeepAliveService.start(ctx, "脚本运行中")
 *   KeepAliveService.stop(ctx)
 *
 * 注意：这只能挡住"系统正常回收"，挡不住：
 *   - 用户从最近任务列表手动滑掉
 *   - OEM 激进省电策略（小米/华为/OPPO 等需要用户在系统设置中允许后台运行）
 *   - 电池优化（需要用户加入白名单）
 */
class KeepAliveService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val title = intent?.getStringExtra(EXTRA_TITLE) ?: "easyLua 运行中"
        val text = intent?.getStringExtra(EXTRA_TEXT) ?: "点击返回主程序"

        ensureChannel(this)
        startForeground(NOTIFICATION_ID, buildNotification(title, text))

        // START_STICKY: 进程被系统杀掉后会尝试重启服务（intent=null）
        return START_STICKY
    }

    private fun buildNotification(title: String, text: String): Notification {
        // 点击通知打开主页
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        } else {
            PendingIntent.FLAG_UPDATE_CURRENT
        }
        val pi = PendingIntent.getActivity(this, 0, openIntent, flags)

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(title)
            .setContentText(text)
            .setContentIntent(pi)
            .setOngoing(true)              // 不可被滑动清除
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setShowWhen(false)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "keepalive"
        private const val NOTIFICATION_ID = 1001
        private const val EXTRA_TITLE = "title"
        private const val EXTRA_TEXT = "text"

        /** 启动前台服务；如已运行则只更新通知文案 */
        fun start(ctx: Context, title: String = "easyLua 运行中", text: String = "点击返回主程序") {
            val intent = Intent(ctx, KeepAliveService::class.java).apply {
                putExtra(EXTRA_TITLE, title)
                putExtra(EXTRA_TEXT, text)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(intent)
            } else {
                ctx.startService(intent)
            }
        }

        /** 停止前台服务（通知消失） */
        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, KeepAliveService::class.java))
        }

        /** 创建 Android 8+ 必须的通知渠道 */
        private fun ensureChannel(ctx: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            if (nm.getNotificationChannel(CHANNEL_ID) != null) return
            val channel = NotificationChannel(
                CHANNEL_ID,
                "进程保活",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "保持脚本运行时不被系统杀掉"
                setShowBadge(false)
                enableLights(false)
                enableVibration(false)
            }
            nm.createNotificationChannel(channel)
        }
    }
}
