@echo off
cd /D D:\zhutao\audio_player
call D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1
for /L %%i in (1,1,25) do (
    echo === attempt %%i ===
    python -m esptool --chip esp32s3 --port COM7 --baud 115200 --before no_reset --after no_reset read_mac
    if not errorlevel 1 (
        echo CONNECTED_OK
        goto :done
    )
    timeout /t 2 /nobreak >nul
)
echo ALL_ATTEMPTS_DONE
:done
