@echo off
REM flash_font.bat - 烧录字体分区 (cjk.ttf -> 0x620000) + 应用固件(4段)
REM 用法：先手动进下载模式(BOOT按住+RESET)，再运行本脚本
setlocal
call D:\esp\v5.5.3\esp-idf\export.bat

set ESP=python D:\esp\v5.5.3\esp-idf\components\esptool_py\esptool\esptool.py
set PORT=COM7
set BAUD=921600

echo.
echo [1/2] 烧录字体分区 cjk.ttf -> 0x620000
%ESP% --port %PORT% --before no_reset --baud %BAUD% write_flash 0x620000 tools\cjk.ttf
if errorlevel 1 (
    echo [ERROR] 字体烧录失败 (COM7被占用? 未进下载模式?)
    goto :end
)

echo.
echo [2/2] 烧录应用固件 (bootloader/partition/app/ota_data)
%ESP% --port %PORT% --before no_reset --baud %BAUD% write_flash ^
    0x0 build\bootloader\bootloader.bin ^
    0x8000 build\partition_table\partition-table.bin ^
    0x10000 build\audiobook_player.bin ^
    0x210000 build\ota_data_initial.bin

echo.
echo [DONE] 烧录完成，按 RESET 重启
:end
endlocal
