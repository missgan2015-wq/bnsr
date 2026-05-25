package com.example.sampleui

import android.os.Bundle
import android.support.v7.app.AlertDialog
import android.support.v7.app.AppCompatActivity
import android.widget.TextView
import com.example.sampleui.ui.ControlBubble
import java.io.File

/**
 * 日志详情页：显示单个日志文件的内容。
 */
class LogDetailActivity : AppCompatActivity() {

    private var logFile: File? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_log_detail)

        val path = intent.getStringExtra(LogListActivity.EXTRA_PATH)
        if (path.isNullOrEmpty()) {
            finish()
            return
        }
        val f = File(path)
        if (!f.exists()) {
            finish()
            return
        }
        logFile = f

        findViewById<TextView>(R.id.tvTitle).text = f.name
        findViewById<TextView>(R.id.btnBack).setOnClickListener { finish() }
        findViewById<TextView>(R.id.btnDelete).setOnClickListener { confirmDelete() }
        findViewById<TextView>(R.id.tvContent).text = LogFile.read(f)
    }

    override fun onStart() {
        super.onStart()
        ControlBubble.pushHide()
    }

    override fun onStop() {
        super.onStop()
        ControlBubble.popHide()
    }

    private fun confirmDelete() {
        val f = logFile ?: return
        AlertDialog.Builder(this)
            .setTitle("删除日志")
            .setMessage("删除 ${f.name}？")
            .setPositiveButton("删除") { _, _ ->
                LogFile.delete(f)
                finish()
            }
            .setNegativeButton("取消", null)
            .show()
    }
}
