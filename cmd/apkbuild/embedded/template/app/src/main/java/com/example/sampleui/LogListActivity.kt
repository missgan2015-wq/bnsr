package com.example.sampleui

import android.content.Intent
import android.os.Bundle
import android.support.v7.app.AlertDialog
import android.support.v7.app.AppCompatActivity
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import com.example.sampleui.ui.ControlBubble
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 日志列表页：列出 LogFile 目录下的所有 .log 文件，按时间倒序。
 * 点击进入详情页，长按删除单条。
 */
class LogListActivity : AppCompatActivity() {

    private lateinit var listView: ListView
    private lateinit var tvEmpty: TextView
    private var adapter: LogAdapter? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_log_list)

        listView = findViewById(R.id.lvLogs)
        tvEmpty = findViewById(R.id.tvEmpty)

        findViewById<TextView>(R.id.btnBack).setOnClickListener { finish() }
        findViewById<TextView>(R.id.btnClearAll).setOnClickListener { confirmClearAll() }

        listView.setOnItemClickListener { _, _, position, _ ->
            val file = adapter?.getItem(position) as? File ?: return@setOnItemClickListener
            startActivity(Intent(this, LogDetailActivity::class.java).apply {
                putExtra(EXTRA_PATH, file.absolutePath)
            })
        }
        listView.setOnItemLongClickListener { _, _, position, _ ->
            val file = adapter?.getItem(position) as? File ?: return@setOnItemLongClickListener true
            confirmDelete(file)
            true
        }
    }

    override fun onResume() {
        super.onResume()
        reload()
    }

    override fun onStart() {
        super.onStart()
        ControlBubble.pushHide()
    }

    override fun onStop() {
        super.onStop()
        ControlBubble.popHide()
    }

    private fun reload() {
        val files = LogFile.listAll()
        adapter = LogAdapter(files)
        listView.adapter = adapter

        if (files.isEmpty()) {
            listView.visibility = View.GONE
            tvEmpty.visibility = View.VISIBLE
        } else {
            listView.visibility = View.VISIBLE
            tvEmpty.visibility = View.GONE
        }
    }

    private fun confirmDelete(file: File) {
        AlertDialog.Builder(this)
            .setTitle("删除日志")
            .setMessage("删除 ${file.name}？")
            .setPositiveButton("删除") { _, _ ->
                LogFile.delete(file)
                reload()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun confirmClearAll() {
        val files = LogFile.listAll()
        if (files.isEmpty()) return
        AlertDialog.Builder(this)
            .setTitle("清空全部日志")
            .setMessage("将删除 ${files.size} 个日志文件，无法恢复")
            .setPositiveButton("清空") { _, _ ->
                files.forEach { LogFile.delete(it) }
                reload()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 列表 adapter：每行显示文件时间 + 大小 */
    private inner class LogAdapter(val files: List<File>) : BaseAdapter() {
        private val timeFmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault())

        override fun getCount(): Int = files.size
        override fun getItem(position: Int): Any = files[position]
        override fun getItemId(position: Int): Long = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
            val ctx = this@LogListActivity
            val density = ctx.resources.displayMetrics.density
            fun dp(v: Int) = (v * density).toInt()

            val row = (convertView as? LinearLayout) ?: LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(20), dp(12), dp(20), dp(12))
                addView(TextView(ctx).apply { textSize = 15f; setTextColor(0xFF222222.toInt()) })
                addView(TextView(ctx).apply { textSize = 12f; setTextColor(0xFF888888.toInt()) })
            }
            val tvName = row.getChildAt(0) as TextView
            val tvSize = row.getChildAt(1) as TextView

            val f = files[position]
            tvName.text = timeFmt.format(Date(f.lastModified()))
            tvSize.text = "${formatSize(f.length())}    ${f.name}"
            return row
        }

        private fun formatSize(bytes: Long): String = when {
            bytes < 1024 -> "${bytes}B"
            bytes < 1024 * 1024 -> "${bytes / 1024}KB"
            else -> "${bytes / 1024 / 1024}MB"
        }
    }

    companion object {
        const val EXTRA_PATH = "path"
    }
}
