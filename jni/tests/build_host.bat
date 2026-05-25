@echo off
REM ============================================================
REM build_host.bat
REM
REM Windows host 端编译并运行 Images.FindPic 属性测试。
REM 测试文件 test_findpic_pbt.c 直接 #include "../images.c"，单 TU
REM 拿到 EasyLuaTemplate 完整结构与 stb 实现，并自带 mock 替换
REM EasyLua_FrameDataLocked / FrameUnlock。
REM
REM 用法：
REM   build_host.bat                 -> 默认种子 0xC0FFEE
REM   build_host.bat 12345           -> 自定义种子
REM ============================================================

setlocal

set GCC=D:\mingw64\bin\gcc.exe
if not exist "%GCC%" (
    echo [build_host] 错误：找不到 mingw gcc，路径：%GCC%
    exit /b 1
)

set SRC=%~dp0test_findpic_pbt.c
set OUT=%~dp0test_findpic_pbt.exe

echo [build_host] 编译 %SRC%
"%GCC%" -O2 -std=c11 -Wall -Wno-unused-function -Wno-unused-variable ^
    -I "%~dp0\.." ^
    "%SRC%" -o "%OUT%"
if errorlevel 1 (
    echo [build_host] 编译失败
    exit /b 1
)

echo [build_host] 运行 %OUT% %1
"%OUT%" %1
set RC=%ERRORLEVEL%

if "%RC%"=="0" (
    echo [build_host] 全部通过
) else (
    echo [build_host] 测试失败 退出码=%RC%
)

endlocal & exit /b %RC%
