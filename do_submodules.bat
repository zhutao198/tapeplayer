@echo off
setlocal

set ADF_DIR=D:\esp\esp-adf
set LOG_DIR=D:\zhutao\audio_player
set PROGRESS=%LOG_DIR%\submodule_init.log

cd /d "%ADF_DIR%"

echo === [%date% %time%] start submodule init in %ADF_DIR% ===
echo === [%date% %time%] start submodule init in %ADF_DIR% > "%PROGRESS%"

REM ---- init esp-adf-libs ----
echo. >> "%PROGRESS%"
echo [%date% %time%] init components\esp-adf-libs ... >> "%PROGRESS%"
echo   init components\esp-adf-libs ...
git submodule update --init --depth=1 components\esp-adf-libs > "%LOG_DIR%\libs.log" 2>&1
if errorlevel 1 (
    echo   FAIL libs >> "%PROGRESS%"
    echo   FAIL libs
    echo --- libs.log tail --- >> "%PROGRESS%"
    powershell -NoProfile -Command "Get-Content '%LOG_DIR%\libs.log' -Tail 20" >> "%PROGRESS%" 2>nul
    goto :FAIL
)
echo   OK libs >> "%PROGRESS%"
echo   OK libs

REM ---- init esp-sr ----
echo. >> "%PROGRESS%"
echo [%date% %time%] init components\esp-sr ... >> "%PROGRESS%"
echo   init components\esp-sr ...
git submodule update --init --depth=1 components\esp-sr > "%LOG_DIR%\sr.log" 2>&1
if errorlevel 1 (
    echo   FAIL sr >> "%PROGRESS%"
    echo   FAIL sr
    echo --- sr.log tail --- >> "%PROGRESS%"
    powershell -NoProfile -Command "Get-Content '%LOG_DIR%\sr.log' -Tail 20" >> "%PROGRESS%" 2>nul
    goto :FAIL
)
echo   OK sr >> "%PROGRESS%"
echo   OK sr

REM ---- verify ----
echo. >> "%PROGRESS%"
echo [%date% %time%] verify submodule status >> "%PROGRESS%"
echo === verify submodule status ===
git submodule status >> "%PROGRESS%" 2>&1
git submodule status

echo. >> "%PROGRESS%"
echo [%date% %time%] DONE >> "%PROGRESS%"
echo === DONE ===
goto :EOF

:FAIL
echo. >> "%PROGRESS%"
echo [%date% %time%] FAILED >> "%PROGRESS%"
echo === FAILED ===
exit /b 1