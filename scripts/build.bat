@echo off
chcp 65001 >nul
echo ============================================
echo  RLCD Monitor — 编译固件
echo ============================================

set PROJECT_DIR=%~dp0..\firmware
set IDF_PATH=E:\ESP\v5.5.2\esp-idf

:: 激活 ESP-IDF 环境
call %IDF_PATH%\export.bat

:: 进入固件目录
cd /d %PROJECT_DIR%

:: 编译
echo 编译中...
idf.py build

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ✅ 编译成功！
    echo 固件位置: %PROJECT_DIR%\build\jr_rlcd_monitor.bin
) else (
    echo.
    echo ❌ 编译失败
)

pause