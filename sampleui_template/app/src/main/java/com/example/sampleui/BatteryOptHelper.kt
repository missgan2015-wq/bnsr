package com.example.sampleui

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings

/**
 * 电池优化白名单帮助类。
 *
 * Android 6+ 引入 Doze 模式，会限制后台 CPU/网络。把 App 加入电池优化白名单后，
 * 系统不会对它应用激进的省电限制，悬浮球和后台进程更稳定。
 */
object BatteryOptHelper {

    /** 当前是否已被加入白名单（不再受电池优化）。 */
    fun isIgnoringBatteryOptimizations(ctx: Context): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return true
        val pm = ctx.getSystemService(Context.POWER_SERVICE) as PowerManager
        return pm.isIgnoringBatteryOptimizations(ctx.packageName)
    }

    /**
     * 直接弹系统对话框请求加入白名单（推荐）。
     * 需要在 Manifest 声明 REQUEST_IGNORE_BATTERY_OPTIMIZATIONS 权限。
     */
    @SuppressLint("BatteryLife")
    fun requestIgnoreBatteryOptimizations(activity: Activity) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return
        if (isIgnoringBatteryOptimizations(activity)) return
        try {
            val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                data = Uri.parse("package:${activity.packageName}")
            }
            activity.startActivity(intent)
        } catch (_: Exception) {
            // 部分 OEM 屏蔽了直弹对话框，降级到设置页让用户手动找
            openBatteryOptimizationSettings(activity)
        }
    }

    /** 打开电池优化设置页（让用户在列表里手动选 App）。 */
    fun openBatteryOptimizationSettings(ctx: Context) {
        try {
            ctx.startActivity(
                Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS).apply {
                    flags = Intent.FLAG_ACTIVITY_NEW_TASK
                }
            )
        } catch (_: Exception) {
            // 再降级到 App 详情页
            ctx.startActivity(
                Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
                    data = Uri.parse("package:${ctx.packageName}")
                    flags = Intent.FLAG_ACTIVITY_NEW_TASK
                }
            )
        }
    }
}
