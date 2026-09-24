@echo off
cd /d D:\zhutao\audio_player
call D:\esp\v5.5.3\esp-idf\export.bat > nul 2>&1
set ADF_PATH=D:\esp\esp-adf
idf.py --version 2>&1
echo.
echo === PATH check ===
where xtensa-esp-elf-gcc
where cmake
where ninja
where openocd
echo.
echo === ADF path check ===
if exist "%ADF_PATH%\components\esp-adf-libs" (echo ADF submodules OK) else (echo ADF submodules MISSING)