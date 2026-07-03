@echo off
REM build.bat - 一键编译 audio_player 项目
REM 用法：在 D:\zhutao\audio_player 目录下执行 build.bat [args...]
REM 例如：build.bat build / build.bat flash / build.bat menuconfig

setlocal

REM 1. 激活 IDF 环境（自动设 PATH、IDF_PATH、IDF_TOOLS_PATH）
call D:\esp\v5.5.3\esp-idf\export.bat
if errorlevel 1 (
    echo [ERROR] IDF export.bat failed
    exit /b 1
)

REM 2. 补 ADF 环境变量
set "ADF_PATH=D:\esp\esp-adf"
set "IDF_TOOLS_PATH=C:\Users\zhuta\.espressif"

REM 3. 校验 ADF 路径
if not exist "%ADF_PATH%\install.bat" (
    echo [ERROR] ADF not found at %ADF_PATH%
    exit /b 1
)
if not exist "%ADF_PATH%\components\esp-adf-libs" (
    echo [ERROR] ADF submodules not initialized: %ADF_PATH%\components\esp-adf-libs missing
    exit /b 1
)
if not exist "%ADF_PATH%\components\esp-sr" (
    echo [ERROR] ADF submodules not initialized: %ADF_PATH%\components\esp-sr missing
    exit /b 1
)

REM 4. 切到项目目录（脚本所在目录）
pushd "%~dp0"

REM 5. 调 idf.py，透传所有参数
echo.
echo ============================================================
echo   Project  : audiobook_player
echo   IDF_PATH : %IDF_PATH%
echo   ADF_PATH : %ADF_PATH%
echo   Target   : (run "idf.py set-target esp32s3" if first build)
echo ============================================================
echo.
idf.py %*

set RC=%ERRORLEVEL%
popd
endlocal & exit /b %RC%