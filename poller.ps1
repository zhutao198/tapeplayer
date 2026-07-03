$ErrorActionPreference = 'SilentlyContinue'

$pid_target = 23932
$out_log = 'D:\zhutao\audio_player\incr.out'
$err_log = 'D:\zhutao\audio_player\incr.err'
$build_log = 'D:\zhutao\audio_player\build_verbose.log'
$prog_log = 'D:\zhutao\audio_player\progress.log'
$interval_sec = 120

# 重置 progress.log
'' | Out-File $prog_log -Encoding utf8

$tick = 0
while ($true) {
    $tick++
    $alive = Get-Process -Id $pid_target -ErrorAction SilentlyContinue
    $alive_str = if ($alive) { "RUNNING (PID=$pid_target, CPU=$($alive.CPU)s)" } else { "EXITED" }

    $out_size = if (Test-Path $out_log) { (Get-Item $out_log).Length } else { 0 }
    $err_size = if (Test-Path $err_log) { (Get-Item $err_log).Length } else { 0 }
    $last_line = ''
    if ($out_size -gt 0) {
        $last_line = (Get-Content $out_log -Tail 1 -Encoding UTF8 -ErrorAction SilentlyContinue)
    }

    # 取 build_run.log 倒数第 5 行（更详细的编译进度）
    $build_tail = ''
    if (Test-Path $build_log) {
        $bl = (Get-Item $build_log).Length
        if ($bl -gt 0) {
            $build_tail = (Get-Content $build_log -Tail 5 -Encoding UTF8 -ErrorAction SilentlyContinue) -join ' | '
            if ($build_tail.Length -gt 200) { $build_tail = $build_tail.Substring(0, 200) + '...' }
        }
    }

    $ts = Get-Date -Format 'HH:mm:ss'
    $line = "[$ts] tick=$tick  $alive_str  out=${out_size}B  err=${err_size}B"
    $line += "`n         build_log last: $build_tail"
    Write-Host $line
    Add-Content -Path $prog_log -Value $line -Encoding utf8

    # 进程退出后，再多看 1 次就停
    if (-not $alive) {
        Write-Host 'target process gone, poller exiting'
        Add-Content -Path $prog_log -Value 'poller EXIT (target gone)' -Encoding utf8
        break
    }

    Start-Sleep -Seconds $interval_sec
}