@echo off
cd /d D:\work\jr-ESP32-S3-RLCD-4.2\firmware
call E:\ESP\v5.5.2\esp-idf\export.bat >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Export failed. Trying direct python...
    E:\ESP\Espressif\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe -m idf
    if %ERRORLEVEL% NEQ 0 (
        echo Cannot find idf.py. Check ESP-IDF installation.
        pause
        exit /b 1
    )
)
echo Building...
idf.py set-target esp32s3
idf.py build
echo Done.
pause