@echo off
setlocal

set LOG=D:\zhutao\audio_player\build_adf.log
echo === [%date% %time%] ADF-enabled build start === > "%LOG%"

cd /d D:\zhutao\audio_player
call D:\esp\v5.5.3\esp-idf\export.bat > nul 2>&1
set ADF_PATH=D:\esp\esp-adf
set IDF_TOOLS_PATH=C:\Users\zhuta\.espressif

echo --- idf.py fullclean --- >> "%LOG%"
idf.py fullclean >> "%LOG%" 2>&1

echo --- idf.py build --- >> "%LOG%"
idf.py build >> "%LOG%" 2>&1
set RC=%ERRORLEVEL%

echo. >> "%LOG%"
echo === [%date% %time%] done, exit code = %RC% >> "%LOG%"
exit /b %RC%