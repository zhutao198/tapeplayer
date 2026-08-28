@echo off
cd /D D:\zhutao\audio_player
call D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1
echo [%time%] start retry-flash loop > tools\_flash.log 2>&1
for /L %%i in (1,1,30) do (
    echo [%time%] attempt %%i >> tools\_flash.log 2>&1
    python -m esptool --chip esp32s3 --port COM7 --baud 115200 --before no_reset --after no_reset read_mac >> tools\_flash.log 2>&1
    if not errorlevel 1 (
        echo [%time%] CONNECTED_OK >> tools\_flash.log 2>&1
        goto :flash
    )
    timeout /t 2 /nobreak >nul
)
echo [%time%] CONNECT_TIMEOUT >> tools\_flash.log 2>&1
exit /b

:flash
python -m esptool --chip esp32s3 --port COM7 --baud 460800 --before no_reset --after no_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\audiobook_player.bin 0x210000 build\ota_data_initial.bin >> tools\_flash.log 2>&1
echo [%time%] FLASH_DONE >> tools\_flash.log 2>&1
