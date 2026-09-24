$ErrorActionPreference = 'SilentlyContinue'

Write-Host '=== esp-sr 目录内容 ===' -ForegroundColor Cyan
Get-ChildItem D:\esp\esp-adf\components\esp-sr -Force 2>$null | Select-Object Name, Length | Format-Table -AutoSize

Write-Host '=== esp-adf-libs 目录内容 ===' -ForegroundColor Cyan
Get-ChildItem D:\esp\esp-adf\components\esp-adf-libs -Force 2>$null | Select-Object Name, Length | Format-Table -AutoSize

Write-Host '=== esp-sr/.git 内容（占位文件 vs 真仓库） ===' -ForegroundColor Cyan
if (Test-Path D:\esp\esp-adf\components\esp-sr\.git) {
    Get-Content D:\esp\esp-adf\components\esp-sr\.git
}

Write-Host '=== esp-adf-libs/.git 内容 ===' -ForegroundColor Cyan
if (Test-Path D:\esp\esp-adf\components\esp-adf-libs\.git) {
    Get-Content D:\esp\esp-adf\components\esp-adf-libs\.git
}

Write-Host '=== clone 阶段日志（看 bat 跑时有没有子模块失败信息） ===' -ForegroundColor Cyan
Write-Host '  (bat 没重定向日志，无法回看，只能重跑 update)' -ForegroundColor Yellow