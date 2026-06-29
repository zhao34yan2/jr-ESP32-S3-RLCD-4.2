@echo off
chcp 65001 >nul
cd /d D:\work\jr-ESP32-S3-RLCD-4.2\firmware
call E:\ESP\v5.5.2\esp-idf\export.bat
echo.
echo ======== 编译开始 ========
idf.py set-target esp32s3
idf.py build
echo.
echo ======== 完成 ========
pause