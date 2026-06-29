@echo off
chcp 65001 >nul
title RLCD Monitor Bridge

echo ============================================
echo  RLCD Monitor Bridge — 启动守护进程
echo ============================================
echo.

cd /d %~dp0..\bridge

:: 检查依赖
pip show fastapi >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo 正在安装依赖...
    pip install -r requirements.txt
)

echo 启动 bridge (http://localhost:7777)
echo 按 Ctrl+C 停止
echo.

python bridge.py

pause