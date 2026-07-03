$ErrorActionPreference = 'Continue'

Write-Host '=== 补拉 ADF 子模块 ===' -ForegroundColor Cyan
Write-Host '  仓库: D:\esp\esp-adf'
Write-Host '  命令: git submodule update --init --recursive --depth=1'
Write-Host '  预计: 3-10 分钟（取决于网速）'
Write-Host ''

Set-Location D:\esp\esp-adf

# 优先尝试 depth=1 的浅克隆（更快）
$result = & git submodule update --init --recursive --depth=1 2>&1
$code = $LASTEXITCODE

Write-Host ''
Write-Host "=== 退出码: $code ===" -ForegroundColor $(if ($code -eq 0) {'Green'} else {'Yellow'})

if ($code -ne 0) {
    Write-Host '=== depth=1 失败，重试完整拉取 ===' -ForegroundColor Yellow
    $result2 = & git submodule update --init --recursive 2>&1
    $code2 = $LASTEXITCODE
    Write-Host "=== 二次退出码: $code2 ===" -ForegroundColor $(if ($code2 -eq 0) {'Green'} else {'Red'})
    $result = $result + "`n--- 重试 ---`n" + $result2
    $code = $code2
}

# 输出最后 20 行
Write-Host ''
Write-Host '=== 最后 20 行输出 ===' -ForegroundColor Cyan
$result | Select-Object -Last 20

# 保存完整日志
$result | Out-File D:\zhutao\audio_player\submodule_init.log -Encoding utf8

Write-Host ''
Write-Host '=== 验证 ===' -ForegroundColor Cyan
git submodule status | ForEach-Object {
    $line = $_
    if ($line -match '^-') {
        Write-Host "  UNINIT: $line" -ForegroundColor Yellow
    } elseif ($line -match '^\+') {
        Write-Host "  MISMATCH: $line" -ForegroundColor Yellow
    } else {
        Write-Host "  OK: $line" -ForegroundColor Green
    }
}

Write-Host ''
Write-Host '完整日志: D:\zhutao\audio_player\submodule_init.log' -ForegroundColor Cyan

if ($code -eq 0) {
    Write-Host '[DONE] 子模块初始化完成' -ForegroundColor Green
} else {
    Write-Host '[FAIL] 仍有未初始化的子模块，看上方 UNINIT 行' -ForegroundColor Red
}

# 防止窗口闪退
if ($Host.Name -eq 'ConsoleHost') { Read-Host '按回车关闭' }