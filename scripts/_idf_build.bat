@echo off
chcp 437 >nul
cd /d D:\work\jr-ESP32-S3-RLCD-4.2\firmware
echo Calling export.bat...
call E:\ESP\v5.5.2\esp-idf\export.bat
if errorlevel 1 (
    echo Export failed
    pause
    exit /b 1
)
echo.
echo === BUILD START ===
echo.
idf.py set-target esp32s3
if errorlevel 1 (
    echo Set-target failed
    pause
    exit /b 1
)
idf.py build
if errorlevel 1 (
    echo BUILD FAILED
) else (
    echo BUILD OK
)
echo.
pause