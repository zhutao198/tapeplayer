@echo off
REM clone_adf.bat
REM 用途：克隆 ESP-ADF v2.8 到 D:\esp\esp-adf（适配 ESP-IDF v5.5.3）
REM 说明：v2.8 是 ESP-ADF 最新稳定版，官方文档明确对应 ESP-IDF v5.5

setlocal EnableDelayedExpansion

set "ADF_REPO=https://github.com/espressif/esp-adf.git"
set "ADF_TAG=v2.8"
set "ADF_DIR=D:\esp\esp-adf"
set "IDF_DIR=D:\esp\v5.5.3\esp-idf"

REM ---- 1. 前置检查 ----
where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] git 未安装，请先安装 Git for Windows
    pause
    exit /b 1
)

if not exist "%IDF_DIR%\tools\idf.py" (
    echo [ERROR] 未找到 %IDF_DIR%\tools\idf.py
    echo         请先在 EIM GUI 中安装 ESP-IDF v5.5.x
    pause
    exit /b 1
)

REM ---- 2. 克隆或更新 ----
if exist "%ADF_DIR%\.git" (
    echo [INFO] %ADF_DIR% 已存在，跳过克隆，仅校验版本...
    cd /d "%ADF_DIR%"
    git fetch --tags --depth=1 origin %ADF_TAG% >nul 2>nul
    git checkout -f %ADF_TAG% 2>nul || (
        echo [ERROR] 无法切到 %ADF_TAG%，请手动检查
        pause
        exit /b 1
    )
) else (
    echo [INFO] 克隆 ESP-ADF %ADF_TAG% 到 %ADF_DIR% ...
    if not exist "D:\esp" mkdir "D:\esp"
    git clone --depth=1 --branch %ADF_TAG% --recursive "%ADF_REPO%" "%ADF_DIR%"
    if errorlevel 1 (
        echo [ERROR] 克隆失败，请检查网络/代理
        pause
        exit /b 1
    )
    cd /d "%ADF_DIR%"
)

REM ---- 3. 显示版本信息 ----
echo.
echo [INFO] 当前 ADF 版本：
git describe --tags --always --dirty 2>nul
echo.

REM ---- 4. 编译提示 ----
echo ============================================================
echo [OK] ESP-ADF 就绪：%ADF_DIR%
echo.
echo 下一步编译项目（在 PowerShell 中执行）：
echo.
echo     cd /d D:\zhutao\audio_player
echo     set ADF_PATH=%ADF_DIR%
echo     %IDF_DIR%\tools\idf.py build
echo ============================================================
echo.
pause
endlocal