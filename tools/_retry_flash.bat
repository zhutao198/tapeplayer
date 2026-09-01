@echo off
cd /D D:\zhutao\audio_player
call D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1
echo [%time%] start flash > tools\_flash.log 2>&1

REM < NUL 避免 esptool 读 stdin 报 "Input redirection is not supported"
python -m esptool --chip esp32s3 --port COM7 --baud 921600 --before default_reset --after no_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\audiobook_player.bin 0x210000 build\ota_data_initial.bin >> tools\_flash.log 2>&1 < NUL

if not errorlevel 1 (
    echo [%time%] FLASH_DONE >> tools\_flash.log 2>&1
) else (
    echo [%time%] FLASH_FAIL >> tools\_flash.log 2>&1
)
