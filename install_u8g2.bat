@echo off
REM install_u8g2.bat - 安装 u8g2 + u8g2_esp32_hal 作为本地 component
REM
REM 用法：install_u8g2.bat
REM
REM 完成后需要在 sdkconfig.defaults 加：
REM   CONFIG_USE_U8G2=y
REM 或者在 menuconfig 里勾选 "Target Module Selection -> Enable U8G2 OLED"

setlocal EnableDelayedExpansion

set "PROJECT=%~dp0"
set "COMPONENTS=%PROJECT%components"

echo ============================================================
echo   Installing u8g2 + u8g2_esp32_hal as local components
echo ============================================================
echo.

if not exist "%COMPONENTS%" mkdir "%COMPONENTS%"

REM 1. 下载 u8g2 (olikraus/u8g2, master branch)
echo [1/2] Downloading u8g2...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "try { Invoke-WebRequest -Uri 'https://github.com/olikraus/u8g2/archive/refs/heads/master.zip' -OutFile '%TEMP%\u8g2.zip' -UseBasicParsing; Write-Host '  Downloaded' } catch { Write-Host \"  FAILED: \$_\" -ForegroundColor Red; exit 1 }"
if not exist "%TEMP%\u8g2.zip" (
    echo [ERROR] u8g2 download failed
    exit /b 1
)

echo [2/2] Extracting u8g2...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Expand-Archive -Path '%TEMP%\u8g2.zip' -DestinationPath '%COMPONENTS%' -Force"
if exist "%COMPONENTS%\u8g2" rmdir /S /Q "%COMPONENTS%\u8g2"
ren "%COMPONENTS%\u8g2-master" "u8g2"
echo   Extracted to %COMPONENTS%\u8g2

REM 2. 创建 u8g2_esp32_hal component (ESP32 HAL glue)
echo [3/3] Creating u8g2_esp32_hal wrapper...
set "HAL=%COMPONENTS%\u8g2_esp32_hal"
if not exist "%HAL%" mkdir "%HAL%"
if not exist "%HAL%\include" mkdir "%HAL%\include"

REM CMakeLists.txt for u8g2_esp32_hal
> "%HAL%\CMakeLists.txt" echo idf_component_register( SRCS "u8g2_esp32_hal.c" INCLUDE_DIRS "include" REQUIRES driver )

REM Public header (minimal HAL interface)
> "%HAL%\include\u8g2_esp32_hal.h" echo #pragma once
>>"%HAL%\include\u8g2_esp32_hal.h" echo #include "u8g2.h"
>>"%HAL%\include\u8g2_esp32_hal.h" echo.
>>"%HAL%\include\u8g2_esp32_hal.h" echo typedef struct { int sda; int scl; } u8g2_esp32_hal_t;
>>"%HAL%\include\u8g2_esp32_hal.h" echo.
>>"%HAL%\include\u8g2_esp32_hal.h" echo void u8g2_esp32_hal_init(u8g2_esp32_hal_t cfg);
>>"%HAL%\include\u8g2_esp32_hal.h" echo uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
>>"%HAL%\include\u8g2_esp32_hal.h" echo uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

REM Minimal HAL implementation (I2C + GPIO delay)
> "%HAL%\u8g2_esp32_hal.c" echo // Minimal u8g2 ESP32 HAL stub - adapt as needed
>>"%HAL%\u8g2_esp32_hal.c" echo #include "u8g2_esp32_hal.h"
>>"%HAL%\u8g2_esp32_hal.c" echo #include "driver/i2c.h"
>>"%HAL%\u8g2_esp32_hal.c" echo #include "driver/gpio.h"
>>"%HAL%\u8g2_esp32_hal.c" echo #include "esp_log.h"
>>"%HAL%\u8g2_esp32_hal.c" echo #include "freertos/FreeRTOS.h"
>>"%HAL%\u8g2_esp32_hal.c" echo #include "freertos/task.h"
>>"%HAL%\u8g2_esp32_hal.c" echo.
>>"%HAL%\u8g2_esp32_hal.c" echo #define TAG "u8g2_hal"
>>"%HAL%\u8g2_esp32_hal.c" echo #define I2C_PORT I2C_NUM_0
>>"%HAL%\u8g2_esp32_hal.c" echo #define I2C_FREQ_HZ 400000
>>"%HAL%\u8g2_esp32_hal.c" echo #define ACK_CHECK_EN 1
>>"%HAL%\u8g2_esp32_hal.c" echo.
>>"%HAL%\u8g2_esp32_hal.c" echo static int s_sda = -1, s_scl = -1;
>>"%HAL%\u8g2_esp32_hal.c" echo.
>>"%HAL%\u8g2_esp32_hal.c" echo void u8g2_esp32_hal_init(u8g2_esp32_hal_t cfg) {
>>"%HAL%\u8g2_esp32_hal.c" echo     s_sda = cfg.sda; s_scl = cfg.scl;
>>"%HAL%\u8g2_esp32_hal.c" echo     i2c_config_t conf = {
>>"%HAL%\u8g2_esp32_hal.c" echo         .mode = I2C_MODE_MASTER,
>>"%HAL%\u8g2_esp32_hal.c" echo         .sda_io_num = s_sda,
>>"%HAL%\u8g2_esp32_hal.c" echo         .scl_io_num = s_scl,
>>"%HAL%\u8g2_esp32_hal.c" echo         .sda_pullup_en = GPIO_PULLUP_ENABLE,
>>"%HAL%\u8g2_esp32_hal.c" echo         .master.clk_speed = I2C_FREQ_HZ,
>>"%HAL%\u8g2_esp32_hal.c" echo     };
>>"%HAL%\u8g2_esp32_hal.c" echo     i2c_param_config(I2C_PORT, ^&conf);
>>"%HAL%\u8g2_esp32_hal.c" echo     i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
>>"%HAL%\u8g2_esp32_hal.c" echo     ESP_LOGI(TAG, "u8g2 I2C init sda=%d scl=%d", s_sda, s_scl);
>>"%HAL%\u8g2_esp32_hal.c" echo }
>>"%HAL%\u8g2_esp32_hal.c" echo.
>>"%HAL%\u8g2_esp32_hal.c" echo uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
>>"%HAL%\u8g2_esp32_hal.c" echo     static uint8_t buf[128]; static uint8_t buf_idx; static uint8_t data;
>>"%HAL%\u8g2_esp32_hal.c" echo     switch(msg) {
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_BYTE_INIT: buf_idx = 0; break;
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_BYTE_SEND: buf[buf_idx++] = arg_int; break;
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_BYTE_SET_DC: data = arg_int; break;
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_BYTE_START_TRANSFER:
>>"%HAL%\u8g2_esp32_hal.c" echo             buf_idx = 0; i2c_master_write_to_device(I2C_PORT, u8x8_GetI2CAddress(u8x8)^>^>1, NULL, 0, 1000/portTICK_PERIOD_MS);
>>"%HAL%\u8g2_esp32_hal.c" echo             break;
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_BYTE_END_TRANSFER:
>>"%HAL%\u8g2_esp32_hal.c" echo             i2c_master_write_to_device(I2C_PORT, u8x8_GetI2CAddress(u8x8)^>^>1, buf, buf_idx, 1000/portTICK_PERIOD_MS);
>>"%HAL%\u8g2_esp32_hal.c" echo             break;
>>"%HAL%\u8g2_esp32_hal.c" echo         default: return 0;
>>"%HAL%\u8g2_esp32_hal.c" echo     }
>>"%HAL%\u8g2_esp32_hal.c" echo     return 1;
>>"%HAL%\u8g2_esp32_hal.c" echo }
>>"%HAL%\u8g2_esp32_hal.c" echo.
>>"%HAL%\u8g2_esp32_hal.c" echo uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
>>"%HAL%\u8g2_esp32_hal.c" echo     switch(msg) {
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_GPIO_RESET: break;
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_DELAY_MILLI: vTaskDelay(arg_int/portTICK_PERIOD_MS); break;
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_DELAY_10MICRO: ets_delay_us(10*arg_int); break;
>>"%HAL%\u8g2_esp32_hal.c" echo         case U8X8_MSG_DELAY_100NANO: ets_delay_us(1); break;
>>"%HAL%\u8g2_esp32_hal.c" echo         default: return 0;
>>"%HAL%\u8g2_esp32_hal.c" echo     }
>>"%HAL%\u8g2_esp32_hal.c" echo     return 1;
>>"%HAL%\u8g2_esp32_hal.c" echo }

echo   u8g2_esp32_hal created at %HAL%

echo.
echo ============================================================
echo   Done.
echo   Components installed:
echo     - %COMPONENTS%\u8g2\
echo     - %COMPONENTS%\u8g2_esp32_hal\
echo.
echo   Next steps:
echo     1. Add to sdkconfig.defaults:
echo        CONFIG_USE_U8G2=y
echo     2. Rebuild: build.bat build
echo ============================================================
endlocal