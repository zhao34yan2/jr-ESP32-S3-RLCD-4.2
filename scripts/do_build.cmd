@echo off
cd /d D:\work\jr-ESP32-S3-RLCD-4.2\firmware
call E:\ESP\v5.5.2\esp-idf\export.bat
echo === COMPILING ===
idf.py set-target esp32s3
idf.py build
echo === DONE ===
exit /b %errorlevel%