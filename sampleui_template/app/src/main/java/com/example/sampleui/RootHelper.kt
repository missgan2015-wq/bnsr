package com.example.sampleui

/**
 * Root 权限检查与请求。
 *
 * 实现委托给 [RootShell]：整个 App 进程只 fork 一次 `su`，
 * 后续命令复用同一个常驻 shell，避免 Magisk/KernelSU 反复弹授权框。
 */
object RootHelper {

    /** 是否已获得 root 权限（同步检查，可能阻塞，建议异步调用）。 */
    fun isRootGranted(): Boolean {
        // 已经启动过且 rooted → 直接返回；否则触发 start
        return RootShell.isRootGranted() || RootShell.start()
    }

    /**
     * 请求 root：同 isRootGranted —— 第一次调用 start() 就会触发 Magisk/KernelSU 授权对话框。
     * 返回是否成功授权。
     */
    fun requestRoot(): Boolean = RootShell.start()
}
