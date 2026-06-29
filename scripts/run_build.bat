@echo off
setlocal enabledelayedexpansion
chcp 437 >nul
set "IDF_PATH=E:\ESP\v5.5.2\esp-idf"
set "IDF_TOOLS_PATH=E:\ESP\Espressif\.espressif"
set "PATH=%IDF_PATH%\tools;%IDF_TOOLS_PATH%\tools\esptool;%IDF_TOOLS_PATH%\python_env\idf5.5_py3.13_env\Scripts;%PATH%"
set "PYTHON=E:\ESP\Espressif\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
cd /d D:\work\jr-ESP32-S3-RLCD-4.2\firmware
echo Building...
%PYTHON% -m idf set-target esp32s3
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
%PYTHON% -m idf build
if %ERRORLEVEL% EQU 0 (echo BUILD OK) else (echo BUILD FAILED)
pause