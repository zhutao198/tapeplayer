@echo off
REM configure.bat - 一键切换目标模组
REM
REM 用法:
REM   configure.bat                       交互式选择（菜单）
REM   configure.bat wroom-1-n16r8        直接选 WROOM-1 N16R8
REM   configure.bat wroom-1-n16r16va     直接选 WROOM-1 N16R16VA
REM   configure.bat wroom-2-n32r16v      直接选 WROOM-2 N32R16V (默认)

setlocal EnableDelayedExpansion

set "PROJECT_DIR=%~dp0"
set "CONFIGS_DIR=%PROJECT_DIR%configs"

if /I "%~1"=="" goto :menu
if /I "%~1"=="wroom-1-n16r8"     set "TARGET=wroom-1-n16r8"     & goto :apply
if /I "%~1"=="wroom-1-n16r16va"  set "TARGET=wroom-1-n16r16va"  & goto :apply
if /I "%~1"=="wroom-2-n32r16v"   set "TARGET=wroom-2-n32r16v"   & goto :apply
if /I "%~1"=="-h" goto :help
if /I "%~1"=="--help" goto :help

echo [ERROR] Unknown target: %~1
echo Run 'configure.bat --help' for usage.
exit /b 1

:menu
echo ============================================================
echo   Select target ESP32-S3 module
echo ============================================================
echo.
echo   [1] WROOM-1  N16R8     ^(16MB Flash +  8MB PSRAM, 3.3V^) - production
echo   [2] WROOM-1  N16R16VA  ^(16MB Flash + 16MB PSRAM, 1.8V^) - high density
echo   [3] WROOM-2  N32R16V   ^(32MB Flash + 16MB PSRAM, 1.8V^) - dev kit default
echo.
set /p CHOICE="Enter choice [1-3] (default=3): "
if "%CHOICE%"=="" set "CHOICE=3"
if "%CHOICE%"=="1" set "TARGET=wroom-1-n16r8"     & goto :apply
if "%CHOICE%"=="2" set "TARGET=wroom-1-n16r16va"  & goto :apply
if "%CHOICE%"=="3" set "TARGET=wroom-2-n32r16v"   & goto :apply
echo [ERROR] Invalid choice '%CHOICE%'
exit /b 1

:apply
set "TEMPLATE=%CONFIGS_DIR%\sdkconfig.defaults.%TARGET%"
if not exist "%TEMPLATE%" (
    echo [ERROR] Template not found: %TEMPLATE%
    exit /b 1
)

echo ============================================================
echo   Applying target: %TARGET%
echo   Template      : %TEMPLATE%
echo ============================================================

REM 1. 复制对应 sdkconfig 模板为主 sdkconfig.defaults
copy /Y "%TEMPLATE%" "%PROJECT_DIR%sdkconfig.defaults" > nul
if errorlevel 1 (
    echo [ERROR] Failed to copy sdkconfig template
    exit /b 1
)
echo   [OK] sdkconfig.defaults updated

REM 2. 删除旧 sdkconfig 让 menuconfig/build 重新生成
if exist "%PROJECT_DIR%sdkconfig" (
    del /F /Q "%PROJECT_DIR%sdkconfig" > nul 2>&1
    echo   [OK] stale sdkconfig removed (will be regenerated)
)

REM 3. 删除 build 目录的 module 依赖缓存（让 sdkconfig 变化被识别）
if exist "%PROJECT_DIR%build\kconfig_menus.json" (
    del /F /Q "%PROJECT_DIR%build\kconfig_menus.json" > nul 2>&1
)

echo.
echo ============================================================
echo   Target set to: %TARGET%
echo.
echo   Next steps:
echo     1. build.bat menuconfig   -- 确认 BOARD_MODULE 选项
echo     2. build.bat build        -- 编译
echo     3. build.bat flash        -- 烧录
echo ============================================================
exit /b 0

:help
echo Usage: configure.bat [target]
echo.
echo Targets:
echo   wroom-1-n16r8        ESP32-S3-WROOM-1 N16R8
echo   wroom-1-n16r16va     ESP32-S3-WROOM-1 N16R16VA
echo   wroom-2-n32r16v      ESP32-S3-WROOM-2 N32R16V (default)
echo.
echo If no argument is given, an interactive menu is shown.
exit /b 0