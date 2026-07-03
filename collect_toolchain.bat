@echo off
REM collect_toolchain.bat - 在 IDF export.bat 环境下采集工具链信息
setlocal EnableDelayedExpansion

set PROJECT=D:\zhutao\audio_player
set ADF=D:\esp\esp-adf
set IDF=D:\esp\v5.5.3\esp-idf
set EIM=%USERPROFILE%\.espressif
set OUT=D:\zhutao\audio_player\toolchain.log
set TMP=D:\zhutao\audio_player\_toolchain.tmp

REM 1. 激活 IDF 环境
call "%IDF%\export.bat" > nul 2>&1

REM 2. 补 ADF
set ADF_PATH=%ADF%
set IDF_TOOLS_PATH=%EIM%

REM 3. 收集
> "%OUT%" echo ================================================================================
>>"%OUT%" echo   audio_player project toolchain ^& environment log
>>"%OUT%" echo   Generated: %date% %time%
>>"%OUT%" echo ================================================================================
>>"%OUT%" echo.

REM ---- Workspace ----
>>"%OUT%" echo ## 1. Workspace
>>"%OUT%" echo   Project root : %PROJECT%
if exist "%PROJECT%" (>>"%OUT%" echo   Project      : exists) else (>>"%OUT%" echo   Project      : MISSING)
for %%f in (CMakeLists.txt sdkconfig.defaults sdkconfig partitions.csv README.md DESIGN.md PRD.md build.bat init_env.ps1) do (
    if exist "%PROJECT%\%%f" (>>"%OUT%" echo    [OK]    %%f) else (>>"%OUT%" echo    [MISS]  %%f)
)
>>"%OUT%" echo.

REM ---- ADF ----
>>"%OUT%" echo ## 2. ADF (ESP-ADF)
>>"%OUT%" echo   ADF_PATH    : %ADF%
if exist "%ADF%" (
    if exist "%ADF%\install.bat" (>>"%OUT%" echo   install.bat : exists) else (>>"%OUT%" echo   install.bat : MISSING)
    if exist "%ADF%\export.bat"  (>>"%OUT%" echo   export.bat  : exists) else (>>"%OUT%" echo   export.bat  : MISSING)
    for /f "tokens=*" %%h in ('git -C "%ADF%" rev-parse HEAD 2^>nul') do >>"%OUT%" echo   HEAD        : %%h
    for /f "tokens=*" %%d in ('git -C "%ADF%" describe --tags --always --dirty 2^>nul') do >>"%OUT%" echo   describe    : %%d
    for /f "tokens=*" %%b in ('git -C "%ADF%" branch --show-current 2^>nul') do >>"%OUT%" echo   branch      : %%b
    for /f "tokens=*" %%r in ('git -C "%ADF%" remote get-url origin 2^>nul') do >>"%OUT%" echo   remote      : %%r
    >>"%OUT%" echo.
    >>"%OUT%" echo   Submodules:
    for /f "tokens=*" %%l in ('git -C "%ADF%" submodule status 2^>nul') do (
        echo     %%l>> "%OUT%"
    )
    >>"%OUT%" echo.
    for %%s in (esp-adf-libs esp-sr) do (
        for /f %%n in ('dir /s /b /a-d "%ADF%\components\%%s\*" 2^>nul ^| find /c /v ""') do >>"%OUT%" echo   components\%%s : %%n files
    )
) else (
    >>"%OUT%" echo   exists      : MISSING
)
>>"%OUT%" echo.

REM ---- IDF ----
>>"%OUT%" echo ## 3. ESP-IDF
>>"%OUT%" echo   IDF_PATH    : %IDF%
if exist "%IDF%" (
    if exist "%IDF%\tools\idf.py" (>>"%OUT%" echo   idf.py      : exists) else (>>"%OUT%" echo   idf.py      : MISSING)
    if exist "%IDF%\export.bat" (>>"%OUT%" echo   export.bat  : exists) else (>>"%OUT%" echo   export.bat  : MISSING)
    for /f "tokens=*" %%h in ('git -C "%IDF%" rev-parse HEAD 2^>nul') do >>"%OUT%" echo   HEAD        : %%h
    if errorlevel 1 >>"%OUT%" echo   HEAD        : (unreadable, EIM packaging)
    for /f "tokens=*" %%d in ('git -C "%IDF%" describe --tags --always --dirty 2^>nul') do >>"%OUT%" echo   describe    : %%d
    for /f "tokens=*" %%b in ('git -C "%IDF%" branch --show-current 2^>nul') do >>"%OUT%" echo   branch      : %%b
    for /d %%c in ("%IDF%\components\*") do set /a ccount+=1 >nul 2>&1
    >>"%OUT%" echo   components  : !ccount! directories
) else (
    >>"%OUT%" echo   exists      : MISSING
)
>>"%OUT%" echo.

REM ---- idf.py version (sanity) ----
>>"%OUT%" echo ## 4. idf.py sanity
>>"%OUT%" echo   idf.py --version : 
python "%IDF%\tools\idf.py" --version >> "%OUT%" 2>nul
>>"%OUT%" echo.

REM ---- Toolchain in PATH (after export.bat) ----
>>"%OUT%" echo ## 5. Toolchain (PATH-resolved after export.bat)
>>"%OUT%" echo   xtensa-esp-elf-gcc : 
where xtensa-esp-elf-gcc 2>nul >> "%OUT%"
>>"%OUT%" echo   cmake              : 
where cmake 2>nul >> "%OUT%"
>>"%OUT%" echo   ninja              : 
where ninja 2>nul >> "%OUT%"
>>"%OUT%" echo   openocd            : 
where openocd 2>nul >> "%OUT%"
>>"%OUT%" echo   python             : 
where python 2>nul >> "%OUT%"
>>"%OUT%" echo   git                : 
where git 2>nul >> "%OUT%"
>>"%OUT%" echo.

REM ---- Tool versions ----
>>"%OUT%" echo ## 6. Tool versions
>>"%OUT%" echo   xtensa-esp-elf-gcc :
for /f "tokens=*" %%v in ('xtensa-esp-elf-gcc --version 2^>nul') do (
    >>"%OUT%" echo     %%v
    goto :gccver_done
)
:gccver_done
>>"%OUT%" echo   cmake :
for /f "tokens=*" %%v in ('cmake --version 2^>nul') do (
    >>"%OUT%" echo     %%v
    goto :cmakever_done
)
:cmakever_done
>>"%OUT%" echo   ninja :
for /f "tokens=*" %%v in ('ninja --version 2^>nul') do (
    >>"%OUT%" echo     %%v
    goto :ninjaver_done
)
:ninjaver_done
>>"%OUT%" echo.

REM ---- EIM GUI tools dir ----
>>"%OUT%" echo ## 7. EIM GUI installed tools
>>"%OUT%" echo   EIM root : %EIM%
if exist "%EIM%\tools" (
    for /d %%t in ("%EIM%\tools\*") do (
        >>"%OUT%" echo     %%~nxt
        for /d %%v in ("%%t\*") do (
            >>"%OUT%" echo       %%~nxv
        )
    )
) else (
    >>"%OUT%" echo   (not found)
)
>>"%OUT%" echo.

REM ---- Env ----
>>"%OUT%" echo ## 8. Environment (after export.bat)
>>"%OUT%" echo   IDF_PATH        : %IDF_PATH%
>>"%OUT%" echo   IDF_TOOLS_PATH  : %IDF_TOOLS_PATH%
>>"%OUT%" echo   ADF_PATH        : %ADF_PATH%
>>"%OUT%" echo   PATH additions  : %EIM%\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin
>>"%OUT%" echo                    : %EIM%\tools\cmake\3.30.2\bin
>>"%OUT%" echo                    : %EIM%\tools\ninja\1.12.1
>>"%OUT%" echo                    : %EIM%\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin
>>"%OUT%" echo                    : %IDF%\tools
>>"%OUT%" echo.

REM ---- Git global config ----
>>"%OUT%" echo ## 9. git global config (network)
for /f "tokens=*" %%l in ('git config --global --get-regexp "url\..*\.insteadOf^|http\." 2^>nul') do (
    >>"%OUT%" echo   %%l
)
>>"%OUT%" echo.

REM ---- Project git ----
>>"%OUT%" echo ## 10. Project git status
if exist "%PROJECT%\.git" (
    >>"%OUT%" echo   HEAD    : 
    git -C "%PROJECT%" rev-parse HEAD >> "%OUT%" 2>nul
    >>"%OUT%" echo   branch  : 
    git -C "%PROJECT%" branch --show-current >> "%OUT%" 2>nul
    >>"%OUT%" echo   changes :
    git -C "%PROJECT%" status -s >> "%OUT%" 2>nul
) else (
    >>"%OUT%" echo   (project not under git)
)
>>"%OUT%" echo.

>>"%OUT%" echo ================================================================================
>>"%OUT%" echo   end of log
>>"%OUT%" echo ================================================================================

REM 4. 显示摘要
type "%OUT%"
endlocal