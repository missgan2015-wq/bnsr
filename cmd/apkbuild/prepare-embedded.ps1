# prepare-embedded.ps1 —— 把打包必需资源 mirror 到 cmd/apkbuild/embedded/
#
# 必须在 `go build` 之前跑（go embed 只能 embed 同包目录下的文件）。
# 与 build-luajit-obfus.ps1 / build-stage-i-obfus.ps1 不同：本脚本不重新编译任何东西，
# 只是把现成产物复制到 embed 目录。
#
# 输入（从 easylua/ 项目根读取）：
#   sampleui_template/                            ← SampleUI 模板（含 .tmpl 占位 + Kotlin 源码）
#   out_i/dex/easylua.dex                         ← Java 桥（架构无关）
#   out_i_obfus/<abi>/libeasylua.so               ← obfus 版 native .so
#   out_obfus/build-info.json                     ← 含 seed sha8
#   out_obfus/host-windows-x86_64/luajit.exe      ← Windows 原生 obfus 编译器（mingw 静态编）
#   out_obfus/host-windows-x86_64/jit/{bcsave,bc,vmdef}.lua ← luajit -b 必需的 jit 模块
#
# 输出（写到 cmd/apkbuild/embedded/）：
#   embedded/runtime-version.txt                  ← seed sha8 + 时间戳
#   embedded/template/...                         ← SampleUI 模板（含 *.tmpl）
#   embedded/dex/easylua.dex
#   embedded/native/<abi>/libeasylua.so
#   embedded/host/luajit.exe                      ← Windows PE
#   embedded/host/jit/{bcsave,bc,vmdef}.lua

[CmdletBinding()]
param(
    [string]$EasyluaRoot = "$PSScriptRoot\..\..",
    [string]$EmbedDir    = "$PSScriptRoot\embedded"
)

$ErrorActionPreference = 'Stop'
$EasyluaRoot = (Resolve-Path -LiteralPath $EasyluaRoot).Path
$EmbedDir    = [System.IO.Path]::GetFullPath($EmbedDir)

Write-Host "[prepare-embedded] easylua root = $EasyluaRoot"
Write-Host "[prepare-embedded] embed dir    = $EmbedDir"

# 必需文件存在性检查
$mustExist = @(
    "$EasyluaRoot\sampleui_template\app\build.gradle.tmpl",
    "$EasyluaRoot\sampleui_template\app\src\main\res\values\strings.xml.tmpl",
    "$EasyluaRoot\sampleui_template\app\src\main\java\com\example\sampleui\ScriptRunner.kt",
    "$EasyluaRoot\out_i\dex\easylua.dex",
    "$EasyluaRoot\out_i_obfus\arm64-v8a\libeasylua.so",
    "$EasyluaRoot\out_i_obfus\x86_64\libeasylua.so",
    "$EasyluaRoot\out_obfus\build-info.json",
    "$EasyluaRoot\out_obfus\host-windows-x86_64\luajit.exe",
    "$EasyluaRoot\out_obfus\host-windows-x86_64\jit\bcsave.lua",
    "$EasyluaRoot\..\Backport_Project\src\lj_obfus_table.h"
)
foreach ($f in $mustExist) {
    if (-not (Test-Path $f)) {
        throw "缺必需文件: $f"
    }
}

# 清空目标目录（保留 .go 文件不动；它们与 embedded/ 同级）
if (Test-Path $EmbedDir) {
    Remove-Item -Recurse -Force $EmbedDir
}
New-Item -ItemType Directory -Path $EmbedDir | Out-Null

# 1) sampleui_template
$tmpl = Join-Path $EmbedDir 'template'
Copy-Item -Recurse -Force "$EasyluaRoot\sampleui_template" $tmpl
Get-ChildItem $tmpl -Recurse -Filter '.gitkeep' | Remove-Item -Force
Get-ChildItem "$tmpl\app\src\main\assets\scripts" -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
Write-Host "[prepare-embedded] sampleui_template -> embedded/template/"

# 2) easylua.dex
$dexDir = Join-Path $EmbedDir 'dex'
New-Item -ItemType Directory -Path $dexDir | Out-Null
Copy-Item -Force "$EasyluaRoot\out_i\dex\easylua.dex" "$dexDir\easylua.dex"
Write-Host "[prepare-embedded] easylua.dex -> embedded/dex/"

# 3) obfus 版 native .so（每个 ABI 一份）
$nativeDir = Join-Path $EmbedDir 'native'
foreach ($abi in @('arm64-v8a', 'x86_64')) {
    $abiDir = Join-Path $nativeDir $abi
    New-Item -ItemType Directory -Force -Path $abiDir | Out-Null
    Copy-Item -Force "$EasyluaRoot\out_i_obfus\$abi\libeasylua.so" "$abiDir\libeasylua.so"
    Write-Host "[prepare-embedded] $abi/libeasylua.so -> embedded/native/$abi/"
}

# 3b) 把 lj_obfus_table.h 也复制到 embedded/native/，apkbuild 启动时从这里抽 RES_KEY
#     与 .so 同 seed 出，PC 加密 / device 解密用同一份 16 字节密钥。
Copy-Item -Force "$EasyluaRoot\..\Backport_Project\src\lj_obfus_table.h" `
    (Join-Path $nativeDir 'lj_obfus_table.h')
Write-Host "[prepare-embedded] lj_obfus_table.h -> embedded/native/"

# 4) host 端 luajit.exe（Windows 原生）+ jit 模块
$hostDir = Join-Path $EmbedDir 'host'
New-Item -ItemType Directory -Force -Path $hostDir | Out-Null
Copy-Item -Force "$EasyluaRoot\out_obfus\host-windows-x86_64\luajit.exe" "$hostDir\luajit.exe"

$jitDst = Join-Path $hostDir 'jit'
New-Item -ItemType Directory -Force -Path $jitDst | Out-Null
foreach ($lua in @('bcsave.lua', 'bc.lua', 'vmdef.lua')) {
    Copy-Item -Force "$EasyluaRoot\out_obfus\host-windows-x86_64\jit\$lua" (Join-Path $jitDst $lua)
}
Write-Host "[prepare-embedded] host luajit.exe + jit/{bcsave,bc,vmdef}.lua -> embedded/host/"

# 5) 写 runtime-version.txt
$buildInfo = Get-Content -Raw -Encoding UTF8 "$EasyluaRoot\out_obfus\build-info.json" | ConvertFrom-Json
$seedSha8 = $buildInfo.seedSha8
if (-not $seedSha8) { $seedSha8 = 'unknown' }
$ts = Get-Date -Format 'yyyyMMdd-HHmmss'
$ver = "$seedSha8-$ts"
Set-Content -Path (Join-Path $EmbedDir 'runtime-version.txt') -Value $ver -Encoding ASCII -NoNewline
Write-Host "[prepare-embedded] runtime-version = $ver"

# 6) 体积统计
$sz = (Get-ChildItem $EmbedDir -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ""
Write-Host ("[prepare-embedded] embed 总大小: {0:N1} MB" -f ($sz / 1MB))
