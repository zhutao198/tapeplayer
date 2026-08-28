@echo off
cd /D D:\zhutao\audio_player
call D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1
echo === Flashing via no_reset (assume chip already in ROM download mode) on COM7 ===
echo === Serial monitor (COM7) MUST be closed; chip must be in download mode ===
python -m esptool --port COM7 --baud 921600 --before no_reset --after no_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\audiobook_player.bin 0x210000 build\ota_data_initial.bin

