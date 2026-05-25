@echo off
setlocal enabledelayedexpansion

rem ============================================================
rem  build_findpic_test.bat
rem
rem  与 build_stage_i.bat 几乎一致，只是把第 7 步的试跑脚本换成
rem  scripts\stage_findpic_test.lua（绕开 stage_i_test.lua 中
rem  Device.GetBrand() 报错的问题）。
rem
rem  产物（仅 arm64-v8a）：
rem    out_findpic\arm64-v8a\libeasylua.so
rem    out_findpic\dex\easylua.dex
rem
rem  设备目录：/data/local/tmp/easylua/
rem ============================================================

set "SCRIPT_DIR=%~dp0"
set "JAVA_HOME=E:\Android\openjdk\jdk-17.0.12"
set "ANDROID_JAR=D:\android\platforms\android-35\android.jar"
set "D8_JAR=D:\android\build-tools\35.0.0\lib\d8.jar"
set "NDK=C:\Users\Public\android-ndk-r27c"
set "ADB=D:\adb\adb.exe"
set "LUAJIT_BASE=E:\auto_apk\Backport_Project\out"
set "LUAJIT_INC=E:\auto_apk\Backport_Project\src"

set "SRC_JAVA=%SCRIPT_DIR%java"
set "JNI_DIR=%SCRIPT_DIR%jni"
set "EMBED_PY=%JNI_DIR%\embed_lua.py"
set "SRC_LUA=%SCRIPT_DIR%scripts\stage_findpic_test.lua"

set "OUT_DIR=%SCRIPT_DIR%out_findpic"
set "CLASS_DIR=%OUT_DIR%\classes"
set "DEX_OUT=%OUT_DIR%\dex"
set "JAVA_LIST=%OUT_DIR%\java_files.txt"
set "CLASS_LIST=%OUT_DIR%\class_files.txt"

set "DEVICE_DIR=/data/local/tmp/easylua"

if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%CLASS_DIR%"
mkdir "%DEX_OUT%"

echo === 1) embed runtime.lua -^> runtime_lua.h ===
python "%EMBED_PY%" "%JNI_DIR%\runtime.lua" "%JNI_DIR%\runtime_lua.h"
if errorlevel 1 ( echo [失败] embed 失败 & exit /b 1 )

echo.
echo === 2) javac ===
dir /S /B "%SRC_JAVA%\*.java" > "%JAVA_LIST%"
"%JAVA_HOME%\bin\javac" -source 1.8 -target 1.8 -encoding UTF-8 -bootclasspath "%ANDROID_JAR%" -classpath "%ANDROID_JAR%" -d "%CLASS_DIR%" -Xlint:none -nowarn @"%JAVA_LIST%"
if errorlevel 1 ( echo [失败] javac 失败 & exit /b 1 )

echo.
echo === 3) d8 -^> easylua.dex ===
dir /S /B "%CLASS_DIR%\*.class" > "%CLASS_LIST%"
"%JAVA_HOME%\bin\java" -Xmx1024M -cp "%D8_JAR%" com.android.tools.r8.D8 --release --min-api 24 --output "%DEX_OUT%" @"%CLASS_LIST%"
if not exist "%DEX_OUT%\classes.dex" ( echo [失败] d8 失败 & exit /b 1 )
move /Y "%DEX_OUT%\classes.dex" "%DEX_OUT%\easylua.dex" >nul

echo.
echo === 4) NDK clang -^> libeasylua.so (arm64-v8a) ===
call :build_abi arm64-v8a aarch64-linux-android29-clang.cmd
if errorlevel 1 exit /b 1

for %%f in ("%OUT_DIR%\arm64-v8a\libeasylua.so") do echo  libeasylua.so: %%~zf bytes
for %%f in ("%DEX_OUT%\easylua.dex") do echo  easylua.dex 大小: %%~zf bytes

echo.
echo === 5) push 到设备 ===
"%ADB%" shell "mkdir -p %DEVICE_DIR%/scripts"
"%ADB%" push "%DEX_OUT%\easylua.dex" "%DEVICE_DIR%/easylua.dex" >nul
"%ADB%" push "%OUT_DIR%\arm64-v8a\libeasylua.so" "%DEVICE_DIR%/libeasylua.so" >nul
"%ADB%" push "%SRC_LUA%" "%DEVICE_DIR%/scripts/stage_findpic_test.lua" >nul

echo.
echo === 6) 唤醒屏幕 ===
"%ADB%" shell "input keyevent KEYCODE_WAKEUP"
"%ADB%" shell "input keyevent 82"

echo.
echo === 7) 试跑 stage_findpic_test (root) ===
"%ADB%" shell "su -c 'CLASSPATH=%DEVICE_DIR%/easylua.dex app_process /system/bin com.easylua.EasyLuaMain --lua %DEVICE_DIR%/libeasylua.so %DEVICE_DIR%/scripts/stage_findpic_test.lua'"

echo.
echo === 完成 ===
endlocal
exit /b 0


rem ============================================================
rem  :build_abi <abi-name> <clang-bin>
rem
rem  从 %LUAJIT_BASE%\<abi> 取 libluajit.a，编出 %OUT_DIR%\<abi>\libeasylua.so
rem ============================================================
:build_abi
set "ABI=%~1"
set "CC_BIN=%~2"
set "ABI_OUT=%OUT_DIR%\%ABI%"
set "ABI_SO=%ABI_OUT%\libeasylua.so"
set "LUAJIT_A=%LUAJIT_BASE%\%ABI%\libluajit.a"
set "CC=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\%CC_BIN%"

echo.
echo  -- ABI: %ABI% --
echo  CC      = %CC%
echo  LuaJIT  = %LUAJIT_A%

if not exist "%CC%" (
  echo [失败] 找不到 NDK clang: %CC%
  exit /b 1
)
if not exist "%LUAJIT_A%" (
  echo [失败] 找不到 LuaJIT: %LUAJIT_A%
  exit /b 1
)

if not exist "%ABI_OUT%" mkdir "%ABI_OUT%"

call "%CC%" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter ^
  -I"%LUAJIT_INC%" -DLFS_USE_LUAJIT_FILE=1 ^
  "%JNI_DIR%\easylua.c" "%JNI_DIR%\ui.c" "%JNI_DIR%\images.c" "%JNI_DIR%\misc.c" "%JNI_DIR%\net.c" "%JNI_DIR%\lfs.c" ^
  "%LUAJIT_A%" ^
  -lm -ldl -llog -landroid -Wl,--gc-sections ^
  -o "%ABI_SO%"
if errorlevel 1 (
  echo [失败] clang %ABI% 编译失败
  exit /b 1
)
for %%f in ("%ABI_SO%") do echo  -^> %ABI_SO% (%%~zf bytes)
exit /b 0
