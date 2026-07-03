$ErrorActionPreference = 'Continue'

Write-Host '=== 1. 测 gitclone.com 镜像 ===' -ForegroundColor Cyan
try {
    $r = Invoke-WebRequest -Uri 'https://gitclone.com/github.com/espressif/esp-sr' -Method Head -TimeoutSec 15 -UseBasicParsing
    Write-Host "  状态码: $($r.StatusCode)" -ForegroundColor Green
} catch {
    Write-Host "  失败: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host ''
Write-Host '=== 2. 测 ghproxy 镜像 ===' -ForegroundColor Cyan
try {
    $r = Invoke-WebRequest -Uri 'https://ghproxy.com/https://github.com/espressif/esp-sr' -Method Head -TimeoutSec 15 -UseBasicParsing
    Write-Host "  状态码: $($r.StatusCode)" -ForegroundColor Green
} catch {
    Write-Host "  失败: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host ''
Write-Host '=== 3. 测 github.com 直连 ===' -ForegroundColor Cyan
try {
    $r = Invoke-WebRequest -Uri 'https://github.com/espressif/esp-sr' -Method Head -TimeoutSec 15 -UseBasicParsing
    Write-Host "  状态码: $($r.StatusCode)" -ForegroundColor Green
} catch {
    Write-Host "  失败: $($_.Exception.Message)" -ForegroundColor Red
}

Write-Host ''
Write-Host '=== 4. 当前 git insteadOf 配置 ===' -ForegroundColor Cyan
git config --global --get-regexp 'url\..*\.insteadOf'