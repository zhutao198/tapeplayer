@echo off
cd /D D:\zhutao\audio_player
echo === Step 1: export.bat ===
call D:\esp\v5.5.3\esp-idf\export.bat
if errorlevel 1 (
  echo [EXPORT_FAIL] rc=%ERRORLEVEL% >> "D:\zhutao\audio_player\build\flash_progress.log"
  exit /b 1
)
echo === Step 2: esptool write_flash ===
python -m esptool --port COM7 --baud 921600 --before no_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\audiobook_player.bin 0x210000 build\ota_data_initial.bin
echo [FLASH_DONE] rc=%ERRORLEVEL% >> "D:\zhutao\audio_player\build\flash_progress.log"
