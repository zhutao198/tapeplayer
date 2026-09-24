# init_env.ps1 - 在当前 PowerShell 会话激活 ESP-IDF + ESP-ADF 环境
# 用法（在 PowerShell 里）：
#   . D:\zhutao\audio_player\init_env.ps1
#
# 或一行：
#   & D:\zhutao\audio_player\init_env.ps1
#
# 验证：跑完后输入 `where idf.py` / `where xtensa-esp-elf-gcc` 应该能找到

$ErrorActionPreference = 'Stop'

$IDF = 'D:\esp\v5.5.3\esp-idf'
$ADF = 'D:\esp\esp-adf'
$EIM = "$env:USERPROFILE\.espressif"

if (-not (Test-Path "$IDF\export.ps1")) {
    Write-Host "[ERROR] $IDF\export.ps1 not found" -ForegroundColor Red
    exit 1
}

# 1. source IDF
Write-Host "Activating ESP-IDF from $IDF ..." -ForegroundColor Cyan
& "$IDF\export.ps1"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] IDF export.ps1 failed" -ForegroundColor Red
    exit 1
}

# 2. 补 ADF
$env:ADF_PATH = $ADF
$env:IDF_TOOLS_PATH = $EIM

# 3. 校验
if (-not (Test-Path "$ADF\components\esp-adf-libs")) {
    Write-Host "[ERROR] ADF submodules not initialized" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host "  ESP-IDF  : $env:IDF_PATH" -ForegroundColor Green
Write-Host "  ADF      : $env:ADF_PATH" -ForegroundColor Green
Write-Host "  Tools    : $env:IDF_TOOLS_PATH" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
Write-Host ""
Write-Host "Now you can run:  idf.py build" -ForegroundColor Yellow
Write-Host "       or:        build.bat build" -ForegroundColor Yellow