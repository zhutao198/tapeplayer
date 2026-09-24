@echo off
REM 批量修复 ESP-IDF 所有子模块 .git/config 里的 RUNNER~1 临时路径
set IDF_GIT_MODULES=D:\esp\v5.5.3\esp-idf\.git\modules
set LOG=D:\zhutao\audio_player\fix_idf_submodules.log

echo === [%date% %time%] scan %IDF_GIT_MODULES% > "%LOG%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Get-ChildItem '%IDF_GIT_MODULES%' -Recurse -Filter config | ForEach-Object {" ^
    "    \$f = \$_.FullName;" ^
    "    \$raw = [System.IO.File]::ReadAllText(\$f);" ^
    "    if (\$raw -match 'RUNNER~1') {" ^
    "        \$fixed = \$raw -replace 'worktree = C:\\Users\\RUNNER~1\\AppData\\Local\\Temp\\\.[^\\]+\\v5\.5\.3\\esp-idf\\', 'worktree = D:/esp/v5.5.3/esp-idf/';" ^
    "        if (\$fixed -ne \$raw) {" ^
    "            [System.IO.File]::WriteAllText(\$f, \$fixed, [System.Text.UTF8Encoding]::new(\$false));" ^
    "            Write-Host \"  FIXED \$f\";" ^
    "            Add-Content '%LOG%' \"FIXED \$f\";" ^
    "        }" ^
    "    }" ^
    "}" >> "%LOG%" 2>&1
echo. >> "%LOG%"
powershell -NoProfile -Command "Get-Content '%LOG%' -Tail 60"