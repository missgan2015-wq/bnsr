# apkbuild

easyLua APK 打包流水线 CLI（独立 Go 单文件分发，所有敏感算法的最外层只在这里）。

## 子命令

```
apkbuild build      [--config <path>] [--workdir <dir>] [--ks-password <pwd>] [--check-only]
apkbuild check      [--config <path>]
apkbuild rotate-seed [--seed <text>]
apkbuild verify     <apk>
```

所有命令默认走 NDJSON 进度协议（每行一个 JSON 事件，写到 stdout），VSIX 端 `apkbuildClient.ts`
解析事件刷新进度条 / 日志面板。

## NDJSON 事件类型

| type | 字段 | 说明 |
| --- | --- | --- |
| `step` | `step / total / name / msg` | 步骤推进 |
| `log` | `level / msg` | 一般日志（info / warn / error） |
| `file` | `action / rel / size?` | 编译过的脚本（compiled / cached / failed） |
| `result` | `ok / msg / apk? / sizeKb?` | 最终结果 |

## 编译

```cmd
go build -o ..\..\..\apkbuild.exe .
```

## 与现有工具链的关系

- `obfus` 子流程 spawn `pwsh easylua/scripts/build-luajit-obfus.ps1`
- `compile` 子流程 spawn `pwsh easylua/scripts/compile-lua.ps1`
- `render` / `gradle` / `sign` / `verify` 直接 Go 原生实现
