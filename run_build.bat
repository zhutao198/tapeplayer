@echo off
REM run_build.bat - 后台跑 build.bat build，日志写到 build_run.log
setlocal

set LOG=D:\zhutao\audio_player\build_run.log
echo === [%date% %time%] start build.bat build === > "%LOG%"

cd /d D:\zhutao\audio_player
call build.bat build >> "%LOG%" 2>&1
set RC=%ERRORLEVEL%

echo. >> "%LOG%"
echo === [%date% %time%] done, exit code = %RC% >> "%LOG%"
exit /b %RC%