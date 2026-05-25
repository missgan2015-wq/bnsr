# APK 打包功能路线图

> 目标：在 VS Code 插件里加一个"打包 APK"命令，把当前工作区的脚本 + 必要 native 库 + Lua runtime 自动嵌入一个独立 APK，支持多架构、默认开启字节码混淆。仅面向 root 设备，不考虑无障碍服务 / 应用市场审核。

## 决策总览

| 项 | 决策 | 备注 |
| --- | --- | --- |
| 目标设备 | **仅 root** | 走 `app_process` 起 LuaJIT，不走 AccessibilityService fallback |
| 支持架构 | **arm64-v8a + x86_64** | 覆盖真机 + 模拟器；32 位 armeabi-v7a / x86 砍掉 |
| 默认混淆 | **L2.5（魔改 LuaJIT bytecode）** | 自定义 magic + opcode shuffle + pc-xor 操作数；详见 [`bytecode-obfus-l25.md`](./bytecode-obfus-l25.md) |
| 字符串常量加密 | **L2.5+（KGC_STR / KTAB_STR XOR）** | 中文 / 字面量在 .luac 里彻底找不到，详见 Stage 7 |
| 资源加密 | **ResXor (.enc)** | ui.lua / 任意 assets 资源都加密；运行时透明解密，详见 Stage 7 |
| 可选混淆 | L3a（AES 加密 assets，Stage 5a 占位）| 第二迭代再做 |
| 载体 APK | **极简模板 + 占位渲染** | LauncherActivity 释放 + 起 `app_process` + finish，不复用 SampleUI |
| PC 端打包工具 | **独立 native CLI（apkbuild.exe）** | VSIX 仅负责 UI + 调子进程；所有敏感算法放 native，避免 VSIX 解压即透明 |

---

## 阶段路线

```
Stage 1   多架构 native 产物          ━━━━ 已完成
Stage 2   载体 APK 模板               ━━━━ 已完成
Stage 3   脚本字节码编译                ━━━━ 已完成
Stage 4   VSIX 打包命令 + apkbuild.exe ━━━━ 已完成（脚手架 + IPC + UI 全链路）
Stage 5a  AES 加密 assets             ──────  暂不做（Stage 4 后视需求落地）
Stage 5b  LuaJIT bytecode 魔改 (L2.5) ━━━━ 已完成（脚手架 + 自动化构建链路）
Stage 6   反调试 + 自校验 (L3b)        ──────  暂不做
Stage 7   字符串常量 + 资源加密         ━━━━ 已完成（KGC_STR XOR + ResXor 三端同构）
```

每个阶段都能独立 ship，不阻塞下一阶段。

---

## Stage 1：多架构 native 产物 ✅ 已完成

**目标**：`libeasylua.so` 同时输出 arm64-v8a 与 x86_64 两份产物，分别打包到 APK 的 `jniLibs/` 对应目录。同时为 obfus 链路提供配套版本（`out_i_obfus/`），让 .so 与 obfus 字节码同 seed。

### 完成项

- ✅ **LuaJIT backport 重编 x86_64 版 `libluajit.a`，带 `-fPIC`**
  - 之前 `out/x86_64/libluajit.a` 是旧产物缺 PIC 标志，link `.so` 报 `R_X86_64_TPOFF32` / `R_X86_64_PC32`
  - WSL Ubuntu + Linux NDK r27c：`Backport_Project/scripts/build-android.sh --abi x86_64 --api 29 --mode release`
  - 新产物：`Backport_Project/out/x86_64/libluajit.a` (1.32 MB)，`R_X86_64_TPOFF*` 已消失
- ✅ **`build_stage_i.bat` 多 ABI 改造**
  - 抽出 `:build_abi <abi> <clang-bin>` 子例程，循环编 arm64 / x86_64
  - 产物结构：
    ```
    out_i/arm64-v8a/libeasylua.so   985,376 bytes  (EM_AARCH64)
    out_i/x86_64/libeasylua.so    1,010,280 bytes  (EM_X86_64)
    out_i/dex/easylua.dex             29,340 bytes  (架构无关)
    out_i/libeasylua.so                            ← arm64 副本（兼容 SampleUI 老脚本）
    ```
- ✅ **真机端到端验证**：arm64 push + 跑脚本 OK，stdout 看到 `[easylua] hello from libeasylua.so`
- ✅ **ELF 头校对**：x86_64 .so 的 `e_machine = 0x3E`（EM_X86_64），不再是 arm64 的 0xB7

### 工程坑（已解决）

- **NTFS strip 失败**：`build-android.sh` 在 `/mnt/e/...` 跑到 `llvm-strip` 时报 `Operation not permitted`（drvfs 不允许改 file mode bits）。`.a` 和 `.so` 已生成，只是脚本最后退出非 0；目前手动 cp。频繁重编可整体迁到 WSL ext4 路径。
- **make clean 缺失**：`Backport_Project/src/` 残留 .o 文件，新 ABI 编译时混链。每次切 ABI 前必须 `make clean`。Stage 5b 的 `build-luajit-obfus.ps1` 已显式做了。
- **obfus 链路 .so 不一致**（Stage 4 真机验证时发现）：原 `build_stage_i.bat` 链 `Backport_Project/out/<abi>/libluajit.a`（无 obfus），跑 obfus 字节码时 `script load error: '=' expected near '$'`（被当源码 parse）。新增 `easylua/scripts/build-stage-i-obfus.ps1`：复用同一份 jni/*.c，但链 `out_obfus/<abi>/libluajit.a`，输出到 `out_i_obfus/<abi>/libeasylua.so`。`apkbuild.exe` 在 `cfg.obfus.enabled` 时优先用此目录的 .so，缺失时报警 + fallback。`apkbuild rotate-seed` 重编 luajit-obfus 后会自动级联跑 build-stage-i-obfus。

### 未做项（暂不做）

- ❌ x86_64 模拟器实跑验证：编译过 + ELF 头校对一致就够了，模拟器跑通是 nice-to-have
- ❌ armeabi-v7a / x86：32 位架构都不编，root 设备占比 ≈ 0

---

## Stage 2：载体 APK 模板 ✅ 已完成

**目标**：维护一个最小 Android 工程模板，VSIX 在打包时通过模板变量替换 + 文件填入产生独立 APK。

### 模板项目结构

```
easylua/template_apk/                    ← git 受控
├── app/
│   ├── src/main/
│   │   ├── AndroidManifest.xml.tmpl    ← 占位渲染
│   │   ├── java/com/easylua/runner/
│   │   │   ├── LauncherActivity.kt     ← 极简启动 Activity
│   │   │   └── ScriptDeployer.kt       ← assets 释放 + su 起 app_process
│   │   ├── jniLibs/{arm64-v8a,x86_64}/.gitkeep   ← 由 VSIX 填入 libeasylua.so
│   │   ├── assets/easylua/.gitkeep               ← 由 VSIX 填入 easylua.dex
│   │   ├── assets/scripts/.gitkeep               ← 由 VSIX 填入用户 .luac
│   │   └── res/...
│   ├── build.gradle.tmpl
│   └── proguard-rules.pro
├── settings.gradle / build.gradle / gradle.properties
├── gradle/wrapper/gradle-wrapper.properties
├── gradlew / gradlew.bat               ← gradle-wrapper.jar 不入库
└── README.md
```

### LauncherActivity 关键逻辑

```kotlin
class LauncherActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ScriptDeployer.deployFromAssets(this)   // → /data/local/tmp/<DEPLOY_DIR_NAME>/
        ScriptDeployer.startWithRoot(this)       // su -c app_process ... main.luac
        finish()                                 // 自己直接退场
    }
}
```

### 占位符表（共 8 个）

| 占位符 | 出现位置 | 例子 |
| --- | --- | --- |
| `{{PKG}}` | `applicationId` | `com.foo.myscript` |
| `{{LABEL}}` | `app_name` | `我的脚本` |
| `{{VERSION_NAME}}` / `{{VERSION_CODE}}` | `versionName / versionCode` | `1.0.0 / 1` |
| `{{MIN_SDK}}` / `{{TARGET_SDK}}` | SDK 版本 | `24 / 34` |
| `{{ABI_FILTERS}}` | `ndk.abiFilters` | `'arm64-v8a', 'x86_64'` |
| `{{DEPLOY_DIR_NAME}}` | `ScriptDeployer.kt` 内 `/data/local/tmp/<dir>` | `easylua-myscript` |

> Java 包路径**固定** `com.easylua.runner`，`applicationId` 单独走 `{{PKG}}` —— 模板渲染只是纯文本替换，目录结构不动。

### Manifest 最小化

只保留 `INTERNET`（脚本可能用 socket / curl）+ `WAKE_LOCK`（长跑防熄屏）。

### 完成项要点

- ✅ **包名/路径解耦**：Kotlin 包路径固定，applicationId 单独走 `{{PKG}}`
- ✅ **ScriptDeployer 极简版**：去掉 SampleUI 的 LogPanel / 常驻 root shell / KeepAliveService 依赖，只保留：assets 解到 cacheDir → `su -c "cat > ..."` → `nohup app_process ... &`
- ✅ **入口探测**：`main.luac → main.lua → scripts/main.luac → scripts/main.lua` 顺序探测，bytecode 优先
- ✅ **`scripts/render-template.ps1`**：Stage 4 `templateRender` 的等价 PowerShell 实现，自验证用
- ✅ **端到端编译验证**：渲染后 `gradle -p ... assembleDebug` 成功产出 4.2 MB APK（22 秒）；`aapt dump badging` 确认包名 / 入口 / 多 ABI 正确

### 工程坑（已解决）

- **AGP 7.4.2 + compileSdk 35 不兼容**：自带 aapt2 加载 `android-35/android.jar` 时报 `RES_TABLE_TYPE_TYPE entry offsets overlap`。targetSdk 默认值降到 34
- **`gradle-wrapper.jar` 不入库**：避免二进制污染 git；首次跑可用本机 gradle / VSIX bootstrap / 直接 `gradle wrapper`
- **PowerShell 5.1 中文编码**：`render-template.ps1` 必须 `pwsh` 7 跑；PS5.1 默认按 GBK 解析 UTF-8 脚本会解析错。Stage 4 改 native CLI 后无此问题

---

## Stage 3：脚本字节码编译 ✅ 已完成

**目标**：把工作区里所有 `.lua` 用 obfus host luajit 编成自定义 magic 的 `.luac`，源码不进 APK。

### 完成项

- ✅ **`easylua/scripts/compile-lua.ps1`**（约 200 行）：扫工作区 → WSL 调 `luajit -b -s -t raw` → 输出 `.luac`
  - 入口约定：`<SrcDir>/main.lua` 必须存在
  - 递归扫所有 `*.lua`，自动排除 `.git / .svn / node_modules / .vscode / out / build / dist / __pycache__ / .cache / .gradle`
  - 相对路径保留：`a/b/foo.lua` → `<OutDir>/a/b/foo.luac`
  - **增量缓存**：cache key = `<rel> | size | mtime | seedSha8 | -s|-g`，写到 `<SrcDir>/.cache/easylua-compile.json`
  - **错误聚合**：单文件语法错收集到末尾统一报，不卡死整个 batch；末尾退出码 1
  - 开关：`-DryRun / -Force / -DebugInfo / -IncludeSource`
- ✅ **`easylua/scripts/_compile_one.sh`**：WSL 端单文件编译 helper，配 `LUA_PATH` 让 `luajit -b` 找到 `jit.bcsave`
- ✅ **与 Stage 5b 协同**：默认调 `out_obfus/host-linux-x86_64/luajit`，编出来全是 magic `FB E3 EF 90` 的 obfus 格式；缓存键含 `seedSha8` 让换 seed 自动 invalidate
- ✅ **端到端验证**：Stage 1 + 2 + 3 串通，`gradle assembleDebug` 出 4.2 MB APK，`aapt list` 看到 `assets/scripts/main.luac` + `assets/scripts/libs/greet.luac` 全是 obfus 格式

### 命令示例

```powershell
# 标准用法（VSIX Stage 4 的 apkbuild.exe 内部就这样调）
pwsh easylua/scripts/compile-lua.ps1 `
    -SrcDir <用户工作区> `
    -OutDir <APK_assets/scripts/>

# 调试期保留行号信息（正式打包必关）
pwsh easylua/scripts/compile-lua.ps1 -DebugInfo ...

# L0 fallback 调试用：源码也拷一份
pwsh easylua/scripts/compile-lua.ps1 -IncludeSource ...
```

### 副作用

错误堆栈变成 `?: in function ?`，没行号——这是混淆代价。开发期跑明文 `.lua`，打包时才走 `.luac`。

### 工程坑（已解决）

- **`luajit -b -t b` 报 unknown file type**：`-t` 取 `raw / c / cc / h / obj / o`，不是 `b`；`.luac` 后缀按 LuaJIT 习惯就是 raw
- **`require` 路径**：编完 `.luac` 后 module 名仍按 `.lua` 文件名走，`require("libs.utils")` 自动找 `libs/utils.luac`；runtime 端 `package.path` 加 `?.luac;?/init.luac` 即可

### 未做项（留给 Stage 4）

- ⏭️ 把 PowerShell 脚本改写为 `apkbuild.exe` 子流程（避免 PS 引号转义 + GBK 编码坑）
- ⏭️ 配置文件支持：工作区根可选 `.easylua/build.json` 覆盖默认入口、排除规则等
- ⏭️ 跨平台：当前 `_compile_one.sh` 走 WSL；mac/Linux 宿主下 `apkbuild.exe` 直接调本机 luajit-obfus

---

## Stage 5b：LuaJIT bytecode 魔改 (L2.5) ✅ 已完成

> 详细方案见 [`bytecode-obfus-l25.md`](./bytecode-obfus-l25.md)。

**目标**：让 `.luac` 落盘字节流彻底改造，使 `luajit-decompiler-v2` / 上游 `luajit` 在第一字节就拒绝识别。所有改动集中在 `Backport_Project/src/`，VSIX 端不接触敏感算法。

### 三层混淆

1. **自定义 magic** `OBFUS_HEAD1..3 + OBFUS_VERSION`（默认 `0xFB 0xE3 0xEF 0x90`）—— ljd2 入口 assertion 直接 panic
2. **opcode shuffle** —— 96 个 enum 值映射到 0..255 的 96 个不同字节，剩余 160 字节是陷阱
3. **pc-xor 操作数** —— BCIns 4 字节里 A/B/C/D 槽位按 `(pc * 31 + field_idx) & 15` 取 KCONST_KEY 异或；相同 BCIns 在不同 pc 落盘字节完全不同（验证：13 条相同 KSHORT 重复 4-byte 块 0/35）

> **关键**：enum 数值不动，运行时 dispatch 透明，性能 0 损失。

### 自动化构建链路

`easylua/scripts/build-luajit-obfus.ps1` 一站式：

```powershell
# 默认：复用现有 lj_obfus_table.h，编 arm64-v8a + x86_64 + Linux host
pwsh easylua/scripts/build-luajit-obfus.ps1

# 重新生成 seed（让所有旧 .luac 立即失效）
pwsh easylua/scripts/build-luajit-obfus.ps1 -Seed "release-2026-Q3-private"

# 仅 host 端（开发期跑 round-trip 用）
pwsh easylua/scripts/build-luajit-obfus.ps1 -SkipAndroid
```

输出 `out_obfus/{arm64-v8a, x86_64, host-linux-x86_64}/{libluajit.so, libluajit.a, luajit}` + `build-info.json`。

### Round-trip 验证（6 项全过）

`easylua/scripts/test_obfus_roundtrip.sh`：
1. 编译 `.lua → .luac` 成功
2. 落盘 magic = `FB E3 EF 90`
3. obfus luajit 反向加载业务逻辑正确
4. 上游 luajit 拒绝（错误：被当成源码 parse 失败）
5. ljd2 头部断言失败（`0x1B 'L' 'J'` 检查失败）
6. pc-xor 重复 4-byte 块 0/35

### Baseline 回归

`LUAJIT_ENABLE_OBFUS` 编译开关，**默认关**。关时 LJ_HASOBFUS=0，所有 hook 退化为 no-op；上游 LuaJIT 行为完全一致。

---

## Stage 4：VSIX 打包命令 + apkbuild.exe ✅ 已完成

**目标**：在 VS Code 命令面板加 `easyLua: Build APK...`，弹配置面板（webview）一键产出 APK。所有敏感逻辑放 `apkbuild.exe`（独立 native CLI），VSIX 只负责 UI + 调子进程。

### 安全模型回顾

```
┌────────────────────────────────────────┐
│ VSIX (TypeScript, 易反编译)            │  仅触发打包，UI + 配置；不接触加密 / 混淆
├────────────────────────────────────────┤
│ apkbuild.exe (Go, 单文件 native)       │  ★ 所有敏感逻辑：obfus seed / 签名密钥 / IPC 编排
├────────────────────────────────────────┤
│ luajit-obfus.exe / libluajit.a         │  Stage 5b 产物，apkbuild.exe 调用
│ libeasylua.so (含 obfus reader)        │  Stage 1 产物，链入 APK
└────────────────────────────────────────┘
```

VSIX 解压能看到的只有"调用 apkbuild.exe 的命令行 + 进度 JSON"，看不到任何加密 / shuffle 表 / 签名密钥。

### 模块布局

#### apkbuild.exe（Go，独立单文件）

```
easylua/cmd/apkbuild/         ← Go module 根
├── main.go                   ← CLI 入口：解析参数 + 派发子命令
├── progress.go               ← 进度 JSON 协议（向 stdout 逐行输出）
├── config.go                 ← 读 .easylua/build.json + CLI 参数合并
│
├── obfus/build.go            ← 包装 build-luajit-obfus 流程（直接 spawn WSL bash 或迁到 native）
├── compile/lua.go            ← 包装 compile-lua 流程（同上）
├── tmpl/render.go            ← 模板渲染：纯字符串替换，无外部依赖
├── gradle/run.go             ← 调 gradlew assembleRelease
├── sign/apksigner.go         ← 调 apksigner sign --ks <jks>
└── verify/elf.go             ← APK 内 .so / .luac magic 校验
```

子命令：
- `apkbuild build` —— 串完整流水线
- `apkbuild check`  —— 仅检查环境（JDK / gradle / Android SDK / luajit-obfus）
- `apkbuild rotate-seed` —— 换 obfus seed + 重生成混淆表 + 重编 luajit-obfus
- `apkbuild verify <apk>` —— 验证 APK 里 .luac magic + 签名

#### VSIX 端（TypeScript）

```
easylua-vsix/src/build/
├── buildPanel.ts             ← webview UI（与 examplePanel.ts 同风格）
├── apkbuildClient.ts         ← spawn apkbuild.exe + 解析 stdout JSON 进度
└── buildConfig.ts            ← .easylua/build.json 读写
```

### IPC 协议（apkbuild.exe → VSIX）

`apkbuild.exe` 子进程通过 stdout 逐行输出 JSON 事件（NDJSON）：

```jsonc
{"type":"step","step":1,"total":7,"name":"check-env","msg":"检查工具链..."}
{"type":"log","level":"info","msg":"找到 luajit-obfus: out_obfus/host-linux-x86_64/luajit"}
{"type":"step","step":2,"total":7,"name":"compile-lua","msg":"编译 12 个脚本..."}
{"type":"file","action":"compiled","rel":"main.lua","size":105}
{"type":"step","step":3,"total":7,"name":"render-template"}
{"type":"step","step":4,"total":7,"name":"gradle-assemble"}
{"type":"log","level":"warn","msg":"AGP deprecation 警告（可忽略）"}
{"type":"step","step":5,"total":7,"name":"apksigner"}
{"type":"step","step":6,"total":7,"name":"verify"}
{"type":"step","step":7,"total":7,"name":"done"}
{"type":"result","ok":true,"apk":"./out/myscript-1.0.0.apk","sizeKb":4242}
```

VSIX 端解析后：
- `step` → 进度条 + 当前步骤标题
- `log` → 写到 OutputChannel `easyLua APK Build`
- `file` → 在面板里列编译过的文件（含错误高亮）
- `result` → 弹窗 + "在文件管理器打开"按钮

stderr 透传作为 fallback 日志（捕获到任意非 JSON 输出都视为错误）。

### 命令面板交互

1. 命令面板搜 `easyLua: Build APK`
2. webview 配置面板（与"日志""API 文档"并列的底栏 tab，第三个）：

```
┌─ 打包 APK ─────────────────────────────────────┐
│  应用包名:  com.foo.myscript                  │
│  应用名称:  我的脚本                          │
│  版本号:    1.0.0  (versionCode 1)            │
│  Keystore:  [选择...] release.jks  密码 ****  │
│  输出目录:  [选择...] ./out                   │
│                                               │
│  目标 ABI:  ☑ arm64-v8a  ☑ x86_64             │
│  ☑ L2.5 bytecode 混淆 (默认)                  │
│  ☐ AES 加密 assets (Stage 5a，待落地)         │
│  ☐ 调试模式（保留 .luac 行号信息）            │
│                                               │
│  [开始打包]   [仅检查环境]   [换 obfus seed]  │
│                                               │
│  ────────────────────────────────────         │
│  [▰▰▰▰▱▱▱] 4/7 gradle-assemble                │
│  [info] 编译 12 个脚本...                     │
│  [info] 渲染模板：com.foo.myscript            │
│  ...                                          │
└───────────────────────────────────────────────┘
```

3. 配置保存到工作区 `.easylua/build.json`（明文 JSON，不存 keystore 密码）
4. keystore 密码通过 SecretStorage API 存到 VS Code（机器级加密），不入仓库

### `.easylua/build.json` schema

```jsonc
{
  "$schema": "https://easylua.dev/schemas/build-v1.json",
  "package": "com.foo.myscript",
  "label": "我的脚本",
  "version": { "name": "1.0.0", "code": 1 },
  "abis": ["arm64-v8a", "x86_64"],
  "minSdk": 24,
  "targetSdk": 34,
  "deployDirName": "easylua-myscript",
  "obfus": { "enabled": true, "debug": false },
  "encrypt": { "enabled": false },
  "keystore": {
    "path": "release.jks",
    "alias": "easylua",
    "passwordRef": "vscode-secret://easylua-build/myscript"
  },
  "outputDir": "./out",
  "exclude": [".git", "node_modules", ".cache"]
}
```

### 外部工具依赖（apkbuild.exe 启动时检测）

| 工具 | 检测路径 | 缺失提示 |
| --- | --- | --- |
| JDK 17 | `JAVA_HOME` | "需要 JDK 17，建议安装 OpenJDK 17 或 Microsoft Build of OpenJDK" |
| Gradle | 模板自带 wrapper | wrapper jar 缺失则提示先 `gradle wrapper` bootstrap |
| Android SDK | `ANDROID_HOME` / `local.properties::sdk.dir` | "需要 Android SDK + build-tools 34.0.0+" |
| apksigner | `<sdk>/build-tools/<latest>/apksigner` | 同上 |
| luajit-obfus | `out_obfus/host-linux-x86_64/luajit` | "需要先跑 build-luajit-obfus.ps1" |
| WSL（Windows） | `wsl.exe` | "Windows 宿主下编译 luajit-obfus 需要 WSL2" |

`apkbuild check` 子命令一次性跑全部检测，输出每项的状态 + 修复建议。

### 临时工作目录

`<extensionContext.globalStorageUri>/build/<timestamp>/`（VSIX 调用时透传给 `apkbuild.exe --workdir`），避免污染工作区。

### 工作量预估

- apkbuild.exe Go 项目脚手架（cmd 解析 + IPC 协议 + config 读写）：0.5 天
- 各子流程包装（obfus / compile / render / gradle / sign / verify）：0.75 天
- VSIX 端 buildPanel + IPC client + SecretStorage 集成：0.75 天
- 合计 ≈ **2 天**

### 完成项要点

- ✅ **`easylua/cmd/apkbuild/`**：独立 Go module（`module easylua.dev/apkbuild`），8 个源文件 + 1 个单测：
  - `main.go` —— 子命令派发：`build / check / rotate-seed / verify`
  - `progress.go` —— NDJSON 协议（`step / log / file / result / checkReport`）
  - `config.go` —— `.easylua/build.json` 解析 + 校验 + 默认值合并
  - `paths.go` —— 从 apkbuild 自身位置反推 easylua 项目根（含 template_apk 标志）
  - `check.go` —— 9 项环境检查（JDK / Android SDK / apksigner / pwsh / WSL / Stage 1/5b 产物 / 模板 / 工作区入口）
  - `render.go` —— 模板复制 + 占位符替换 + native 产物填入（Go 原生实现，对齐 `render-template.ps1`）
  - `compile.go` —— 子流程包装：spawn `pwsh compile-lua.ps1` / `build-luajit-obfus.ps1`，按行解析输出转 NDJSON
  - `gradle.go` / `sign.go` / `verify.go` —— 调 `gradlew assembleRelease` / `apksigner sign` / 静态校验 APK 内 .so / .luac magic
  - `render_smoke_test.go` —— 包含 3 个 unit test，端到端验证模板渲染、包名校验、deployDirName 派生
- ✅ **VSIX 端 `easylua-vsix/src/build/`**：
  - `buildConfig.ts` —— `.easylua/build.json` 读写 + SecretStorage key 派生
  - `apkbuildClient.ts` —— spawn 子进程 + 流式 NDJSON 解析 + apkbuild.exe / easylua-root 路径自动定位
  - `buildPanel.ts` —— 第三个底栏 webview，配置面板 + 进度条 + 实时日志
- ✅ **package.json**：注册 `easyLua.buildApk` 命令（图标 `$(package)`）+ `easyLua.buildPanel` 视图 + `easyLua.apkbuildPath` / `easyLua.easyluaRoot` 配置项
- ✅ **`.easylua/build.json` schema**：与 Stage 4 设计章节一致；首次打开面板自动用默认值，保存时带 `$schema` 头方便编辑器智能提示
- ✅ **keystore 密码策略**：通过 VS Code SecretStorage（机器级加密）存储；spawn 子进程时通过环境变量 `APKBUILD_KS_PASS` 传递，不进 argv，避免被 `ps`/任务管理器捕获
- ✅ **进度协议落地**：webview 端实时刷进度条（百分比 + 当前步骤名）+ 分级染色日志（info / warn / error / step / result）+ 完成后渲染"在文件管理器查看 APK"按钮
- ✅ **NDJSON 端到端验证**：用 `apkbuild check` 在 test 工作区运行，9 项检查项 + JSON 结构化报告全部能被前端正确解析渲染（终端 UTF-8 + JSON 字符串中文都正常）

### 工程坑（已解决）

- **Go 字符串字面量内嵌中文双引号**：早期 check.go 把"换 obfus seed"写在 `"..."` 里被 Go 编译器当成嵌套字符串，改成 backtick 原始字符串解决（中文双引号在 backtick 里无歧义）
- **VSCode SecretStorage 与命令行的隔离**：之前一度想用 `--ks-password` 直接 argv 传，被 ps/argv 暴露密码风险大；改用环境变量 + stdin 双重保险（`apksigner sign --ks-pass stdin --key-pass stdin`）
- **gradle-wrapper.jar 缺失自动 bootstrap**：`EnsureGradleWrapperJar` 检测到缺 jar 时自动调宿主 gradle 跑一次 `gradle wrapper --gradle-version 8.5`，与 template_apk/README.md 里"首次使用"流程对齐
- **Stage 1 与 Stage 5b 的 .so 不一致**（真机端到端时暴露）：APK 里 `libeasylua.so` 链入的若是无 obfus 版本的 `libluajit.a`，运行时 `lua_load(<.luac>)` 会拒绝 obfus magic（被当源码 parse 报 `'=' expected near '$'`）。补丁：新增 `out_i_obfus/<abi>/libeasylua.so`（链 obfus libluajit.a），在 `cfg.obfus.enabled=true` 时强制使用

### 未做项（留作后续）

- ⏭️ **apkbuild.exe 内嵌到 vsix 包**：当前从仓库根读 `apkbuild.exe`（开发期 `go build` 输出）；正式发布时打到 `easylua-vsix/resources/` 里，`.vscodeignore` 调整一下即可
- ⏭️ **跨平台 mac/Linux 宿主**：当前 spawn 路径默认 `pwsh` 7 + `wsl`；mac/Linux 上 `compile-lua.ps1` 需要改成直接调本机 luajit-obfus（不走 WSL）。Stage 4 实现了平台无关的 `findPwsh`，第一阶段走包装层先放着
- ⏭️ **Stage 5a AES 加密 assets**：等 Stage 4 上线后用户反馈再决定优先级

---

## Stage 5a：AES 加密 assets ⏸️ 暂不做

> 路线图原 L2 项。Stage 5b 已默认启用更高强度的 L2.5；此项仅作为深度防御选项，看 Stage 4 上线后用户反馈再决定。

设计概要（仅占位，未实施）：

- 编完 `.luac` 后用 AES-128-CTR 加密
- 密钥嵌在 `libeasylua.so` 的 `__data` 段（编译期固化）
- 运行时在 `package.loaders[1]` 加解密 loader，`require("foo.bar")` 透明走解密
- 静态分析门槛抬高，但仍不能彻底防 IDA 下断点 `lua_load`；与 L2.5 叠加才有意义

---

## Stage 6：反调试 + 自校验 (L3b) ⏸️ 暂不做

仅在有真实针对性逆向案例时再处理：

- `libeasylua.so` 启动时 `ptrace(PTRACE_TRACEME)` 占住自身，防 Frida attach
- 校验自身 sha256，比对硬编码值（被改即 abort）
- 计算 device fingerprint（`ANDROID_ID + Build.FINGERPRINT + APK signature`）派生 AES key，绑设备
- 工程量约 1 天，但跟 Stage 5b 收益边际叠加，先不做

---

## Stage 7：字符串常量 + 资源加密 ✅ 已完成

> 解决 Stage 5b 之后的两个明文残留：(1) `.luac` 内 KGC_STR / KTAB_STR 字面量（中文 / 路径 / SQL 等）裸落盘；(2) `ui.lua` DSL 与 `assets/` 资源（json / png / csv / 二进制）原样拷贝进 APK。

### 三层加密统一密钥

`gen-obfus-table.py` 在 `lj_obfus_table.h` 里多出 2 组 16 字节 key（与现有 `KCONST_KEY` 同 seed 派生，新 seed 自动失效）：

| Key | 用途 |
| --- | --- |
| `LJ_OBFUS_STR_KEY[16]` | 方案 A：`bcwrite_kgc` / `bcwrite_ktabk` 写盘后对 string payload 异或；`bcread_*` 读时反向 |
| `LJ_OBFUS_RES_KEY[16]` | 方案 B1 + C：apkbuild 加密 `ui.lua.enc` 与 `<asset>.enc`；native 端 `lib_io.c::io_open` 与 Kotlin 端 `EasyLuaSecret` 解密 |

### 算法（C / Go / Kotlin 三端同构）

```
carry = (len*31 + salt*17 + 0xA5 or 0x5A) & 0xFF
for i in 0..len-1:
    k = KEY[(i + carry) & 15]
    buf[i] ^= k
    carry = (carry + k + i) & 0xFF       // 只用 i + key 推进，与 buf 字节解耦 → 自反
```

salt 区分相同字符串落在不同位置时密文不同；初值常量 `0xA5`/`0x5A` 仅用来区分 STR vs RES 两个 key 域。

### 方案 A：字符串常量加密

- `lj_obfus.h/c` 新增 `lj_obfus_str_xor(buf, len, salt)`
- `lj_bcwrite.c::bcwrite_kgc` BCDUMP_KGC_STR 分支写盘后异或 `(i << 4) | 2`；`bcwrite_ktabk` 增加 `salt` 形参，`(salt << 4) | 1`
- `lj_bcread.c::bcread_kgc` / `bcread_ktabk` 读取后原地反向异或；salt 由 `bcread_ktab` 按写端遍历顺序派生
- LuaJIT runtime 行为透明，dispatch / GC / JIT 全部不感知

验证：`easylua/scripts/test_str_xor_roundtrip.sh` 跑过 6 项检查（magic / strings / hex / 解密执行）。

### 方案 B1：ui.lua 加密

- `apkbuild compile.go::isDslScript` 命中后走 `EncryptFile(salt=0)`，落盘 `ui.lua.enc`
- 模板新增 `EasyLuaSecret.kt.tmpl`，`apkbuild render.go` 渲染期填入 `LJ_OBFUS_RES_KEY` 派生的 `byteArrayOf(0xXX.toByte(), ...)`
- `DialogActivity.kt` 读取时按 `.enc` 后缀分支解密
- `ScriptRunner.kt` `uiPath` 候选列表加 `.enc`；`onDialogResult` / `onHeadlessDialogResult` 自适应回写格式

### 方案 C：assets 资源加密

- `apkbuild compile.go::copyExtraResources` 改 `EncryptFile(salt=0)` 落 `<rel>.enc`
- LuaJIT `lib_io.c::io_open` 加 `#if LJ_HASOBFUS` fallback：明文不存在时尝试 `<path>.enc` → fread → `lj_obfus_res_xor` → `tmpfile` → 返回 FILE\*
- 脚本端 `io.open("assets/foo.png")` 行为不变，PC 调试期读明文 / 真机透明走 `.enc` 解密
- 仅对只读 mode 启用 fallback；写入仍走原路径，让 `io.open(path, "w")` 行为符合最小意外原则
- 64 MB 大小上限做防御（避免恶意 .enc 撑爆内存）
- `tmpfile()` 用完即关、磁盘上无 path，明文不会落盘留痕

验证：`easylua/scripts/test_res_xor_roundtrip.sh` 跑过 4 项检查（明文 / 中文 strings 不可见 / `io.open` 透明解密 / 明文路径回归）。

### apkbuild 端联动

- `prepare-embedded.ps1` 把 `lj_obfus_table.h` 也复制到 `embedded/native/`，让 apkbuild.exe 启动时能从 embed runtime 抽 16 字节 RES_KEY
- `encrypt.go` 实现 Go 版 `ResXor` + `EncryptFile`，并把 RES_KEY 渲染为 Kotlin `byteArrayOf(...)` 灌进模板
- `build.gradle.tmpl` 把 `'enc'` 加入 `androidResources.noCompress`，避免 aapt 压缩破坏 fopen seek

### 端到端验证

`apkbuild build` 在 `test/` 工作区跑通：

```
APK 内 assets/scripts/ 文件清单：
  main.luac                       36 B (FB E3 EF 90 magic)
  ui/ui.lua.enc                  669 B (无中文 strings)
  assets/config.json.enc          49 B (无中文 strings)
  assets/icon.png.enc             35 B (无中文 strings)

unzip -p <apk> 'assets/scripts/*' | strings | grep -E '挂机|账号|测试'
  → 0 命中
```

### Stage 7 改动文件清单

新增：
- `Backport_Project/src/lj_obfus.h/c` 增 `lj_obfus_str_xor / lj_obfus_res_xor`
- `easylua/scripts/test_str_xor_roundtrip.sh`
- `easylua/scripts/test_res_xor_roundtrip.sh`
- `easylua/cmd/apkbuild/encrypt.go`
- `easylua/sampleui_template/.../ui/EasyLuaSecret.kt.tmpl`

修改：
- `Backport_Project/src/lj_bcwrite.c` `bcwrite_ktabk / bcwrite_kgc / bcwrite_ktab / bcwrite_ktab_sorted_hash`
- `Backport_Project/src/lj_bcread.c` `bcread_ktabk / bcread_kgc / bcread_ktab`
- `Backport_Project/src/lib_io.c` `LJLIB_CF(io_open)` 加 `.enc` fallback
- `easylua/scripts/gen-obfus-table.py` 加 `build_str_key / build_res_key`
- `easylua/cmd/apkbuild/compile.go` `CompileLua / copyExtraResources`
- `easylua/cmd/apkbuild/render.go` `RenderTemplate` 加 `{{RES_KEY_BYTES}}` 占位
- `easylua/cmd/apkbuild/prepare-embedded.ps1` 把 lj_obfus_table.h 复制进 embed
- `easylua/sampleui_template/app/build.gradle.tmpl` `noCompress 'enc'`
- `easylua/sampleui_template/.../ScriptRunner.kt` `uiPath` 候选 + 回写自适应
- `easylua/sampleui_template/.../DialogActivity.kt` 读 `.enc` 解密

### 下一步要做的事

- 重编 obfus 版 native 产物（`build-luajit-obfus.ps1` + `build-stage-i-obfus.ps1`），让设备端 `libeasylua.so` 也含新的 `lib_io.c .enc fallback` 与 `lj_obfus_str_xor` 解密路径，否则 APK 在真机跑会找不到 .enc 文件解密路径（即使 host luajit 能跑）。
- 真机端到端跑一遍：安装新 APK，确认 `io.open("assets/icon.png")` 能读到明文 + ui.lua dialog 能弹出。

---

## 工作量总览

| 阶段 | 内容 | 估时 | 状态 |
| --- | --- | --- | --- |
| 1 | 多架构 native 产物 | 0.5 天 | ✅ 完成 |
| 2 | 载体 APK 模板 | 1 天 | ✅ 完成 |
| 3 | LuaJIT bytecode 编译流程 | 0.5 天 | ✅ 完成 |
| 5b | L2.5 bytecode 魔改 + 自动化构建 | 2 天（实际 + 0.5 天调试） | ✅ 完成 |
| 4 | VSIX 打包面板 + apkbuild.exe | 2 天 | ✅ 完成 |
| 5a | AES 加密 assets | 1 天 | ⏸️ 暂不做（被 Stage 7 取代） |
| 6  | 反调试 + 自校验 | 1 天 | ⏸️ 暂不做 |
| 7  | 字符串常量 + 资源加密 | 1 天 | ✅ 完成 |
| **MVP 已交付** | Stage 1+2+3+4+5b+7 | **7 天** | — |

---

## 下一步

MVP 全部交付，已在 mumu12 模拟器（root）端到端验证通过：

```
打包：
  apkbuild build → 4.0 MB APK，签名 Verifies，4 项 magic 校验全过

安装运行：
  adb install / am start →
  EasyLuaLauncher: deploy ok：libeasylua.so + 3 assets
  EasyLuaLauncher: 已起 app_process，入口=main.luac
  /data/local/tmp/easylua-test/run.log:
    [easylua] hello from libeasylua.so
    [01:15:04 main.luac:0] {}      ← print(Config) 输出
    [easylua] script exit code = 0
```

复现步骤：

1. **首次准备**（仅一次）：
   - `pwsh easylua/scripts/build-luajit-obfus.ps1`（出 obfus 产物）
   - `pwsh easylua/scripts/build-stage-i-obfus.ps1`（出 obfus 版 libeasylua.so）
   - `keytool -genkey -v -keystore release.jks -keyalg RSA -keysize 2048 -validity 10000 -alias easylua`（生成测试 keystore）
2. **打开 VS Code 工作区**（含 main.lua），切到底部 panel 的"打包 APK" tab
3. **填配置 → 仅检查环境** → 看 10 项是否全 ✔
4. **开始打包**：apkbuild 串完整流水线（render → compile → gradle → sign → verify），最终 APK 落到 `<工作区>/out/<pkg>-<ver>.apk`
5. **真机安装** + `adb logcat -s EasyLuaLauncher` 看部署日志，确认 `app_process` 起来跑业务脚本
6. **换 seed**（可选）：在面板点"换 obfus seed"→ apkbuild 自动重编 luajit-obfus + obfus libeasylua.so，旧 APK 立即失效
