$ErrorActionPreference = 'SilentlyContinue'
$out = 'D:\zhutao\audio_player\toolchain.log'
$ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'

# Configurable paths
$PROJECT = 'D:\zhutao\audio_player'
$ADF     = 'D:\esp\esp-adf'
$IDF     = 'D:\esp\v5.5.3\esp-idf'

# Toolchain search roots (EIM GUI typical layout)
$TOOL_SEARCH = @(
    'D:\esp\v5.5.3\esp-idf\tools'
    'D:\esp\v5.5.3\tools'
    "$env:USERPROFILE\.espressif\tools"
    "$env:USERPROFILE\.espressif\python_env"
    'C:\Users\zhuta\.espressif\tools'
    'C:\espressif\tools'
)

$lines = New-Object System.Collections.Generic.List[string]

function Add([string]$s) { $lines.Add($s) }

Add '================================================================================'
Add '  audio_player project toolchain & environment log'
Add "  Generated: $ts"
Add '================================================================================'
Add ''

# ---- 1. Workspace ----
Add '## 1. Workspace'
Add "  Project root : $PROJECT"
Add "  Project      : $(if (Test-Path $PROJECT) {'exists'} else {'MISSING'})"
foreach ($f in @('CMakeLists.txt','sdkconfig.defaults','sdkconfig','partitions.csv','README.md','DESIGN.md','PRD.md')) {
    Add "  $((Test-Path (Join-Path $PROJECT $f)).ToString().PadLeft(5))  $f"
}
Add ''

# ---- 2. ADF ----
Add '## 2. ADF (ESP-ADF)'
Add "  ADF_PATH    : $ADF"
Add "  exists      : $(Test-Path $ADF)"
if (Test-Path $ADF) {
    Add "  idf.py      : $(Test-Path "$ADF\install.bat")  (install.bat / export.bat)"
    Add "  HEAD        : $(git -C $ADF rev-parse HEAD 2>$null)"
    Add "  describe    : $(git -C $ADF describe --tags --always --dirty 2>$null)"
    Add "  branch      : $(git -C $ADF branch --show-current 2>$null)"
    Add "  remote      : $(git -C $ADF remote get-url origin 2>$null)"
    Add ''
    Add '  Submodules:'
    $sm = git -C $ADF submodule status 2>$null
    foreach ($s in $sm) {
        if ($s -match '^-')      { Add "    [SKIP] $s" }
        elseif ($s -match '^\+') { Add "    [DIFF] $s" }
        else                     { Add "    [OK]   $s" }
    }
    Add ''
    foreach ($sub in @('esp-adf-libs','esp-sr')) {
        $cnt = (Get-ChildItem "$ADF\components\$sub" -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count
        Add "  components\$sub : $cnt files"
    }
}
Add ''

# ---- 3. ESP-IDF ----
Add '## 3. ESP-IDF'
Add "  IDF_PATH    : $IDF"
Add "  exists      : $(Test-Path $IDF)"
if (Test-Path $IDF) {
    Add "  idf.py      : $(Test-Path "$IDF\tools\idf.py")"
    Add "  export.ps1  : $(Test-Path "$IDF\export.ps1")"
    Add "  export.bat  : $(Test-Path "$IDF\export.bat")"
    Add "  HEAD        : $(git -C $IDF rev-parse HEAD 2>$null)"
    Add "  describe    : $(git -C $IDF describe --tags --always --dirty 2>$null)"
    Add "  branch      : $(git -C $IDF branch --show-current 2>$null)"
    Add "  remote      : $(git -C $IDF remote get-url origin 2>$null)"
    Add "  components  : $((Get-ChildItem "$IDF\components" -Directory | Measure-Object).Count) directories"
}
Add ''

# ---- 4. EIM GUI ----
Add '## 4. EIM GUI (ESP-IDF Installation Manager)'
$eim = "$env:USERPROFILE\.espressif\eim_gui\eim-gui-windows-x64.exe"
Add "  path        : $eim"
Add "  exists      : $(Test-Path $eim)"
$eim_cfg = "$env:USERPROFILE\.espressif\idf_eim_gui.json"
if (Test-Path $eim_cfg) {
    Add "  config      : $eim_cfg (exists)"
}
Add ''

# ---- 5. Toolchain (cross-compiler + tools) ----
Add '## 5. Cross-toolchain (xtensa-esp-elf)'
$found_tools = $false
foreach ($root in $TOOL_SEARCH) {
    if (-not (Test-Path $root)) { continue }
    $gcc = Get-ChildItem $root -Recurse -Filter 'xtensa-esp*-elf-gcc.exe' -ErrorAction SilentlyContinue | Select-Object -First 3
    foreach ($g in $gcc) {
        Add "  found: $($g.FullName)"
        $ver = & $g.FullName --version 2>&1 | Select-Object -First 1
        Add "         $ver"
        $found_tools = $true
    }
    $openocd = Get-ChildItem $root -Recurse -Filter 'openocd.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($openocd) { Add "  found openocd: $($openocd.FullName)" }
    $ccache = Get-ChildItem $root -Recurse -Filter 'ccache.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($ccache) { Add "  found ccache: $($ccache.FullName)" }
}
if (-not $found_tools) {
    Add '  ! xtensa-esp*-elf-gcc NOT FOUND in any search root'
    Add '  ! need to source export.ps1 from IDF, or install via EIM GUI'
}
Add ''

# ---- 6. System tools (PATH-resolved) ----
Add '## 6. System tools (currently in PATH)'
$tools = @(
    'git','python','cmake','ninja','ccache','openocd','make','gperf','dfu-util'
)
foreach ($t in $tools) {
    $cmd = Get-Command $t -ErrorAction SilentlyContinue
    if ($cmd) {
        $ver = & $cmd.Source ('--version') 2>&1 | Select-Object -First 1
        Add ("  {0,-10} : {1}" -f $t, $ver)
        Add ("              {0}" -f $cmd.Source)
    } else {
        Add ("  {0,-10} : NOT IN PATH" -f $t)
    }
}
Add ''

# ---- 7. idf.py invocation test ----
Add '## 7. idf.py (sanity test, runs without source)'
$idfpy = "$IDF\tools\idf.py"
if (Test-Path $idfpy) {
    Add "  command : python $idfpy --version"
    $out7 = & python $idfpy --version 2>&1
    Add ("  result  : {0}" -f ($out7 -join ' | '))
}
Add ''

# ---- 8. git global config (network) ----
Add '## 8. git global config (network-related)'
$gc = git config --global --get-regexp 'url\..*\.insteadOf|http\.' 2>$null
if ($gc) {
    foreach ($l in $gc) { Add "  $l" }
} else {
    Add '  (no network-related git config)'
}
Add ''

# ---- 9. Environment variables ----
Add '## 9. Environment variables (esp-related, current session)'
foreach ($e in @('IDF_PATH','IDF_TOOLS_PATH','ADF_PATH','ADF_LIB_PATH','PYTHONPATH','ESP_ADF_VERSION')) {
    $v = [Environment]::GetEnvironmentVariable($e)
    Add ("  {0,-16} : {1}" -f $e, ($(if ($v) {$v} else {'<unset>'})))
}
Add ''

# ---- 10. Project git status ----
Add '## 10. Project git status'
if (Test-Path "$PROJECT\.git") {
    Add "  HEAD    : $(git -C $PROJECT rev-parse HEAD 2>$null)"
    Add "  branch  : $(git -C $PROJECT branch --show-current 2>$null)"
    Add '  changes :'
    $st = git -C $PROJECT status -s 2>$null
    if ($st) { $st | ForEach-Object { Add "    $_" } } else { Add '    (clean)' }
} else {
    Add '  (project not under git)'
}
Add ''

Add '================================================================================'
Add '  end of log'
Add '================================================================================'

# Write with UTF-8 BOM (so PowerShell, Notepad, etc. all read it correctly)
$content = $lines -join "`r`n"
$utf8Bom = New-Object System.Text.UTF8Encoding $true
[System.IO.File]::WriteAllText($out, $content, $utf8Bom)

Write-Host ''
Write-Host 'Wrote:' $out -ForegroundColor Green
Write-Host ('Size : ' + (Get-Item $out).Length + ' bytes')
Write-Host ('Lines: ' + $lines.Count)