$ErrorActionPreference = 'SilentlyContinue'

$root = 'D:\esp\v5.5.3\esp-idf\.git\modules'
$log  = 'D:\zhutao\audio_player\fix_idf_submodules.log'
$pattern = 'worktree = C:\\Users\\RUNNER~1\\AppData\\Local\\Temp\\\.[^\\]+\\v5\.5\.3\\esp-idf\\'
$repl    = 'worktree = D:/esp/v5.5.3/esp-idf/'

$ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
"=== [$ts] scanning $root ===" | Out-File $log -Encoding utf8

$count_total = 0
$count_fixed = 0
$count_skip  = 0

Get-ChildItem $root -Recurse -Filter config -ErrorAction SilentlyContinue | ForEach-Object {
    $count_total++
    $f = $_.FullName
    $raw = [System.IO.File]::ReadAllText($f)
    if ($raw -match 'RUNNER~1') {
        $fixed = $raw -replace $pattern, $repl
        if ($fixed -ne $raw) {
            $utf8 = New-Object System.Text.UTF8Encoding($false)
            [System.IO.File]::WriteAllText($f, $fixed, $utf8)
            $count_fixed++
            "FIXED  $f" | Add-Content $log -Encoding utf8
        } else {
            $count_skip++
            "SKIP   $f (no pattern match)" | Add-Content $log -Encoding utf8
        }
    }
}

"=== summary: total=$count_total fixed=$count_fixed skip=$count_skip ===" | Add-Content $log -Encoding utf8
Get-Content $log -Tail 20