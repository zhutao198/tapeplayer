$ErrorActionPreference = 'Stop'

$ADF = 'D:\esp\esp-adf'

function Say([string]$msg, [string]$c = 'Cyan') {
    Write-Host $msg -ForegroundColor $c
}

Say "=== Resume: ADF=$ADF ==="
Set-Location $ADF

Say '=== Step 4 (retry): fetch v2.8 with retry ==='
$ok = $false
for ($i=1; $i -le 3; $i++) {
    Say "  attempt $i ..."
    git fetch --depth=1 origin v2.8 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        $ok = $true
        break
    }
    Say "  attempt $i failed, wait 5s and retry" -Yellow
    Start-Sleep -Seconds 5
}
if (-not $ok) {
    Say '  fetch failed after 3 attempts' -Red
    exit 1
}
$ref = git rev-parse FETCH_HEAD 2>$null
$tag = git describe --tags --exact-match FETCH_HEAD 2>$null
if (-not $tag) { $tag = 'FETCH_HEAD' }
Say "  fetch OK ref=$ref tag=$tag" -Green

Say ''
Say '=== Step 5: reset index to fetched commit ==='
git reset --hard $ref 2>&1 | Out-Null
Say "  HEAD: $(git describe --tags --always --dirty)" -Green

Say ''
Say '=== Step 6: init only 2 needed submodules ==='
Say '  init components/esp-adf-libs ...'
$code1 = 1
for ($i=1; $i -le 3; $i++) {
    git submodule update --init --depth=1 components/esp-adf-libs 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { $code1 = 0; break }
    Say "  attempt $i failed, wait 5s" -Yellow
    Start-Sleep -Seconds 5
}
Say "  exit code: $code1" $(if ($code1 -eq 0) {'Green'} else {'Red'})

Say '  init components/esp-sr ...'
$code2 = 1
for ($i=1; $i -le 3; $i++) {
    git submodule update --init --depth=1 components/esp-sr 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { $code2 = 0; break }
    Say "  attempt $i failed, wait 5s" -Yellow
    Start-Sleep -Seconds 5
}
Say "  exit code: $code2" $(if ($code2 -eq 0) {'Green'} else {'Red'})

Say ''
Say '=== Step 7: verify ==='
git submodule status

if ($code1 -eq 0 -and $code2 -eq 0) {
    Say ''
    Say '[DONE] submodules initialized' -Green
    Say "  ADF path: $ADF"
    Say "  ADF version: $(git describe --tags --always)"
    Say "  IDF path: D:\esp\v5.5.3\esp-idf (local, not via submodule)"
} else {
    Say ''
    Say '[FAIL] see error above' -Red
}