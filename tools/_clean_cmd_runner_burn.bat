@echo off
cd /D D:\zhutao\audio_player\r\ncall D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1\r\npython -m esptool --port COM7 --baud 460800 --before no_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\audiobook_player.bin 0x210000 build\ota_data_initial.bin
echo [FLASH_DONE] rc=%ERRORLEVEL% >> "D:\zhutao\audio_player\build\flash_progress.log"

