# easyLua - AnkuLua 风格 UI 演示

用 **Kotlin + Support Library v7（AppCompat 28.0.0）** 实现的最小 UI 演示，复刻 AnkuLua 的两大 UI 能力：

1. **链式表单对话框 DSL**：`DialogBuilder.create(activity).addTextView(...).addEditText(...).show("标题")`
2. **屏幕浮窗高亮**：`HighlightOverlay.show(ctx, rect, "Stop!", 0xFFFF0000.toInt())`

## 工程结构

```
easyLua/
├── settings.gradle / build.gradle / gradle.properties
├── app/
│   ├── build.gradle
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── res/
│       │   ├── layout/activity_main.xml      （主界面：5 个测试按钮）
│       │   ├── values/styles.xml             （Theme.AppCompat 主题）
│       │   ├── values/colors.xml
│       │   └── values/strings.xml
│       └── java/com/example/sampleui/
│           ├── MainActivity.kt               （演示入口）
│           └── ui/
│               ├── DialogBuilder.kt          （AnkuLua 风格 dialogInit/addXxx 实现）
│               └── HighlightOverlay.kt       （浮窗高亮实现）
```

## 关键技术点

### 1. 锁定 Support v7 不走 androidx

`gradle.properties`：
```
android.useAndroidX=false
android.enableJetifier=false
```
依赖：
```
implementation 'com.android.support:appcompat-v7:28.0.0'
implementation 'com.android.support:design:28.0.0'
```
注意 `compileSdkVersion 28` —— Support Library 28 是最后一版，要配套 SDK 28 才能正常引到。Android Studio 高版本 (Giraffe+) 默认会推 androidx，但本工程已强制关闭。

### 2. DialogBuilder 的设计

仿 AnkuLua 但比它更干净：

| AnkuLua | 本项目 |
|---|---|
| `addEditText("name", "x")` 关闭后 `name` 变全局变量 | 关闭后传 `Map<String,Any?>` 给 callback，不污染外部 |
| 按调用顺序自动布局，`newRow()` 切换行 | 完全相同 |
| `dialogShowFullScreen("title")` | `showFullScreen("title") { result -> }` |
| 用 LuaJava 桥到 Java 端 | Kotlin 链式 API，未来桥到 Lua 也只是写个绑定层 |

### 3. HighlightOverlay 的实现要点

- 用 `WindowManager.addView` 创建一个全屏 overlay View，自定义 `onDraw` 画框 + 文字
- Android 8+ 必须用 `TYPE_APPLICATION_OVERLAY`，旧版用 `TYPE_PHONE`
- 必须申请 `SYSTEM_ALERT_WINDOW` 权限并用 `Settings.ACTION_MANAGE_OVERLAY_PERMISSION` 跳转设置页授权
- 设置 `FLAG_NOT_TOUCHABLE` 让触摸穿透到下层

## 编译运行

需要 Android Studio + 安装 SDK 28（"Android 9 Pie"）。Gradle 7.5 + AGP 7.4.2 的组合在 Android Studio Hedgehog（2023.1）以下完美工作。

```bash
# 命令行
./gradlew :app:assembleDebug
# APK 在 app/build/outputs/apk/debug/app-debug.apk
```

如果用更新的 Android Studio（已不内建 SDK 28），需在 SDK Manager 中手动安装"Android 9 (API 28)"。

## 接下来想做什么？

- 把这套 DialogBuilder 桥到 Lua（用 LuaJava 写个 `dialog.add_edit_text` 注册函数）
- 加 `addImageView` / `addProgressBar` / `addSeekBar`（AnkuLua 没做的）
- 加 `dialogModalLoop`：在 dialog 里跑长时任务并实时更新 UI
- 把 HighlightOverlay 升级成可以同时画多个矩形（用一个 list 而不是单个 view）
