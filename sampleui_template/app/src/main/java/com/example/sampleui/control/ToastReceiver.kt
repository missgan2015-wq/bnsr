package com.example.sampleui.control

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.Toast

/**
 * 接收 easyLua root 进程通过 `am broadcast` 发来的 Toast 请求。
 *
 * root 进程触发命令：
 *   am broadcast -n com.example.sampleui/.control.ToastReceiver \
 *       -a com.easylua.TOAST \
 *       --es msg "Hello" \
 *       --ei dur 0
 *
 * extras：
 *   msg : String   要显示的文本（必填）
 *   dur : Int      0 = SHORT(2s)，1 = LONG(3.5s)
 *
 * APK 不需要在前台。Manifest 里声明 exported=true，root 通过 -n 显式指定组件，
 * AMS 会把 broadcast 投递到本进程；APK 进程未启动时，AMS 会启动它。
 *
 * 设计原则：
 *   - 单向无返回，root 不等结果，不需要 socket
 *   - 显式 component（-n）触发，不依赖隐式 action 路由
 *   - exported=true 但只接受显式调用（隐式调用不会路由进来）
 */
class ToastReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val msg = intent.getStringExtra(EXTRA_MSG).orEmpty()
        val dur = intent.getIntExtra(EXTRA_DUR, 0)
        if (msg.isEmpty()) {
            Log.w(TAG, "ignore empty msg")
            return
        }
        // BroadcastReceiver 默认在主线程，但 Toast.show 必须有 Looper，主线程肯定有；
        // 加一层 main handler post 兜底（防止 onReceive 被偶发调到非主线程）。
        Handler(Looper.getMainLooper()).post {
            try {
                val length = if (dur == 1) Toast.LENGTH_LONG else Toast.LENGTH_SHORT
                Toast.makeText(context.applicationContext, msg, length).show()
            } catch (t: Throwable) {
                Log.e(TAG, "toast failed", t)
            }
        }
    }

    companion object {
        const val ACTION = "com.easylua.TOAST"
        const val EXTRA_MSG = "msg"
        const val EXTRA_DUR = "dur"
        private const val TAG = "EasyLuaToast"
    }
}
