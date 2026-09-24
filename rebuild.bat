@echo off
REM rebuild.bat - 清 build 目录后重新编译
setlocal

set LOG=D:\zhutao\audio_player\build_run.log

echo === [%date% %time%] rebuild start === > "%LOG%"

cd /d D:\zhutao\audio_player

REM 1. 调用 export.bat
call D:\esp\v5.5.3\esp-idf\export.bat > nul 2>&1
set ADF_PATH=D:\esp\esp-adf
set IDF_TOOLS_PATH=C:\Users\zhuta\.espressif

REM 2. fullclean
echo --- fullclean --- >> "%LOG%"
idf.py fullclean >> "%LOG%" 2>&1
if errorlevel 1 (
    echo fullclean failed, continue anyway >> "%LOG%"
)

REM 3. build
echo --- idf.py build --- >> "%LOG%"
idf.py build >> "%LOG%" 2>&1
set RC=%ERRORLEVEL%

echo. >> "%LOG%"
echo === [%date% %time%] done, exit code = %RC% >> "%LOG%"
exit /b %RC%