@echo off
cd /D D:\zhutao\audio_player
call D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1
idf.py build > D:\zhutao\audio_player\debug\build_coredump.log 2>&1

