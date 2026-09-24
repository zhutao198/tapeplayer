$ErrorActionPreference = 'SilentlyContinue'

Write-Host '=== 1. ADF idf.py ===' -ForegroundColor Cyan
Test-Path D:\esp\esp-adf\idf.py

Write-Host '=== 2. ADF 版本 ===' -ForegroundColor Cyan
git -C D:\esp\esp-adf describe --tags --always --dirty

Write-Host '=== 3. 关键子模块 ===' -ForegroundColor Cyan
foreach ($s in @('esp-sr','esp-dsp','esp_lcd','es8311','esp_codec','esp-adf-libs')) {
    $p = "D:\esp\esp-adf\components\$s"
    if (Test-Path $p) {
        Write-Host "  [OK]   $s" -ForegroundColor Green
    } else {
        Write-Host "  [MISS] $s" -ForegroundColor Yellow
    }
}

Write-Host '=== 4. submodule 状态（前 25 行） ===' -ForegroundColor Cyan
git -C D:\esp\esp-adf submodule status 2>$null | Select-Object -First 25

Write-Host '=== 5. 已初始化子模块统计 ===' -ForegroundColor Cyan
$lines = git -C D:\esp\esp-adf submodule status 2>$null
$total = ($lines | Measure-Object).Count
$inited = ($lines | Where-Object { $_ -match '^\s' }).Count
$uninited = ($lines | Where-Object { $_ -match '^-' }).Count
Write-Host "  总计: $total   已初始化: $inited   未初始化: $uninited"