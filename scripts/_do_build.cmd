@echo off
start /wait "RLCD Build" cmd /k "cd /d D:\work\jr-ESP32-S3-RLCD-4.2\firmware && call E:\ESP\v5.5.2\esp-idf\export.bat && echo === READY TO BUILD === && idf.py set-target esp32s3 && idf.py build && echo === BUILD DONE === && pause"
echo Build finished.
exit /b 0