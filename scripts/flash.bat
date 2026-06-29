@echo off
chcp 65001 >nul
echo ============================================
echo  RLCD Monitor — 烧录固件到设备
echo ============================================

set PROJECT_DIR=%~dp0..\firmware
set IDF_PATH=E:\ESP\v5.5.2\esp-idf

:: 激活 ESP-IDF 环境
call %IDF_PATH%\export.bat

:: 进入固件目录
cd /d %PROJECT_DIR%

:: 烧录
echo 正在烧录到 ESP32-S3...
idf.py -p COM? flash

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ✅ 烧录成功！
    echo.
    echo 启动监视器:
    echo idf.py -p COM? monitor
) else (
    echo.
    echo ❌ 烧录失败，请检查串口号
    echo 用法: idf.py -p COMx flash
)

pause