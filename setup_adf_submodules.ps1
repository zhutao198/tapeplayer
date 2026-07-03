$ErrorActionPreference = 'Stop'

$ADF = 'D:\esp\esp-adf'
$RELEASE = 'D:\esp\esp-adf-release-v2.x'

function Say([string]$msg, [string]$c = 'Cyan') {
    Write-Host $msg -ForegroundColor $c
}

Say '=== Step 0: remove broken mirror config ==='
git config --global --unset 'url.https://gitclone.com/github.com/.insteadOf' 2>$null
git config --global --get-regexp 'url\..*\.insteadOf'

Say ''
Say '=== Step 1: remove old empty ADF dir (from previous failed clone) ==='
if (Test-Path $ADF) {
    Say "  removing $ADF" -Yellow
    Remove-Item -Recurse -Force $ADF
} else {
    Say "  $ADF not exist, skip"
}

Say ''
Say '=== Step 2: rename release dir to ADF canonical path ==='
if (Test-Path $RELEASE) {
    Rename-Item -Path $RELEASE -NewName 'esp-adf'
    Say "  renamed -> $ADF" -Green
} else {
    Say "  $RELEASE not found" -Red
    exit 1
}

Say ''
Say '=== Step 3: init git repo inside release dir ==='
Set-Location $ADF
git init -b main 2>&1 | Out-Null
git remote add origin https://github.com/espressif/esp-adf.git 2>&1 | Out-Null
git config core.autocrlf false
git config core.filemode false
git config advice.detachedHead false
Say '  init + remote origin OK' -Green

Say ''
Say '=== Step 4: fetch v2.8 tag ==='
git fetch --depth=1 origin v2.8 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Say '  v2.8 fetch failed, trying v2.7' -Yellow
    git fetch --depth=1 origin v2.7 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Say '  v2.7 also failed' -Red
        git ls-remote --tags origin 2>$null | Select-Object -First 10
        exit 1
    }
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
Say '=== Step 6: init only 2 needed submodules (skip esp-idf) ==='
Say '  init components/esp-adf-libs ...'
git submodule update --init --depth=1 components/esp-adf-libs 2>&1 | Out-Null
$code1 = $LASTEXITCODE
Say "  exit code: $code1" $(if ($code1 -eq 0) {'Green'} else {'Red'})

Say '  init components/esp-sr ...'
git submodule update --init --depth=1 components/esp-sr 2>&1 | Out-Null
$code2 = $LASTEXITCODE
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