@echo off
REM rebuild2.bat - full clean + verbose build
setlocal

set LOG=D:\zhutao\audio_player\build_verbose.log

echo === [%date% %time%] verbose build start === > "%LOG%"

cd /d D:\zhutao\audio_player
call D:\esp\v5.5.3\esp-idf\export.bat > nul 2>&1
set ADF_PATH=D:\esp\esp-adf
set IDF_TOOLS_PATH=C:\Users\zhuta\.espressif

REM 先 fullclean
echo --- fullclean --- >> "%LOG%"
idf.py fullclean >> "%LOG%" 2>&1
if errorlevel 1 (
    echo fullclean failed, continue >> "%LOG%"
)

REM 重新 configure + build（verbose）
echo --- idf.py -v build --- >> "%LOG%"
idf.py -v build >> "%LOG%" 2>&1
set RC=%ERRORLEVEL%

echo. >> "%LOG%"
echo === [%date% %time%] done, exit code = %RC% >> "%LOG%"
exit /b %RC%