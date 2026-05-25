package com.example.sampleui.ui

import android.app.Activity
import org.json.JSONObject
import java.io.File

/**
 * 脚本启动器：串联 "解析 ui.lua → 渲染对话框 → 收集结果 → 调 Go 执行 script.lua" 流程。
 *
 * 渲染交给 OpRenderer，支持嵌套容器、修饰符等更复杂的布局。
 */
class ScriptLauncher(private val activity: Activity) {

    private var goBinaryPath = "/data/local/tmp/autogo-lua"
    private var varsFilePath = "/data/local/tmp/config.json"

    fun setGoBinary(path: String): ScriptLauncher {
        goBinaryPath = path
        return this
    }

    /** 真实模式：解析 ui.lua → 渲染 → 调 Go 二进制执行脚本。 */
    fun launch(
        scriptDir: String,
        scriptName: String = "script.lua",
        uiName: String = "ui.lua",
        fullScreen: Boolean = false,
        onResult: ((success: Boolean, output: String) -> Unit)? = null
    ) {
        val dir = scriptDir.trimEnd('/')
        val scriptFile = "$dir/$scriptName"
        val uiFile = File("$dir/$uiName")

        if (uiFile.exists()) {
            val luaSource = uiFile.readText()
            val parseResult = LuaUIParser.parse(luaSource)
            if (parseResult != null) {
                renderAndCollect(parseResult, fullScreen) { varsJson ->
                    execScript(scriptFile, varsJson, onResult)
                }
                return
            }
        }
        execScript(scriptFile, null, onResult)
    }

    /** 演示模式：直接传入 ui.lua 源码，渲染后回调 JSON（不调用 Go 二进制）。 */
    fun launchDemo(
        uiLuaSource: String,
        fullScreen: Boolean = false,
        onCollected: (varsJson: String) -> Unit
    ) {
        val parseResult = LuaUIParser.parse(uiLuaSource)
        if (parseResult == null) {
            onCollected("{}")
            return
        }
        renderAndCollect(parseResult, fullScreen, onCollected)
    }

    private fun renderAndCollect(
        ui: LuaUIParser.ParseResult,
        fullScreen: Boolean,
        onCollected: (varsJson: String) -> Unit
    ) {
        OpRenderer(activity).show(ui, fullScreen) { result ->
            onCollected(buildVarsJson(ui.ops, result))
        }
    }

    /** 把 OpRenderer 收集到的 result 按变量类型转成 JSON。 */
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
                else -> { /* 其它类型不收 */ }
            }
        }
        return json.toString(2)
    }

    /** 调 autogo-lua 执行脚本。 */
    private fun execScript(
        scriptPath: String,
        varsJson: String?,
        onResult: ((Boolean, String) -> Unit)?
    ) {
        Thread {
            try {
                val cmd = mutableListOf(goBinaryPath, scriptPath)
                if (!varsJson.isNullOrEmpty()) {
                    File(varsFilePath).writeText(varsJson)
                    cmd.add("--vars-file")
                    cmd.add(varsFilePath)
                }
                val process = ProcessBuilder(cmd)
                    .redirectErrorStream(true)
                    .start()
                val output = process.inputStream.bufferedReader().readText()
                val exitCode = process.waitFor()
                activity.runOnUiThread {
                    onResult?.invoke(exitCode == 0, output)
                }
            } catch (e: Exception) {
                activity.runOnUiThread {
                    onResult?.invoke(false, "执行异常: ${e.message}")
                }
            }
        }.start()
    }
}
