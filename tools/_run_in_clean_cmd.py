#!/usr/bin/env python3
"""
_run_in_clean_cmd.py — 从 Git Bash / Claude Code 安全跑 ESP-IDF 命令的 runner。

解决的问题：
  1. MSYSTEM=MINGW64 由 MSYS2 注入，从 Git Bash fork 的 cmd/PowerShell 子进程全部继承，
     导致 ESP-IDF 的 export.bat 第 2 行 `if defined MSYSTEM` 守卫拒绝工作（IDF_PATH 空）。
  2. bash 在命令行解析阶段吞掉 `>` 重定向元字符，导致 `>nul` 这类 cmd 重定向无法传到 Python。

本脚本两步绕过：
  - 用 os.environ.copy() + del MSYSTEM 给 cmd 子进程完全干净的环境
  - 把命令字符串写到临时 .bat 文件，再让 cmd 执行 .bat（避开 bash 元字符干扰）

用法:
    python tools/_run_in_clean_cmd.py "build"      # 跑 idf.py build (内置)
    python tools/_run_in_clean_cmd.py "flash"      # 跑 esptool 烧录 (内置)
    python tools/_run_in_clean_cmd.py "version"    # 跑 idf.py --version (内置)
    python tools/_run_in_clean_cmd.py --raw "<任意 cmd 命令>"  # 原始模式

需要的运行时（用户已具备）：
    - D:\\esp\\v5.5.3\\esp-idf  (ESP-IDF v5.5.3)
    - D:\\esp\\esp-adf           (ESP-ADF v2.7)
    - C:\\Users\\zhuta\\.espressif (IDF_TOOLS_PATH)
"""
import subprocess, os, sys, tempfile, time

PROJECT_DIR = r"D:\zhutao\audio_player"
IDF_DIR     = r"D:\esp\v5.5.3\esp-idf"
ADF_DIR     = r"D:\esp\esp-adf"
TOOLS_DIR   = r"C:\Users\zhuta\.espressif"
BAT_PATH    = os.path.join(PROJECT_DIR, "tools", "_clean_cmd_runner.bat")


def build_bat(cmd_lines: str) -> None:
    """把 cmd 命令字符串写到 .bat 文件"""
    header = "@echo off\n"
    with open(BAT_PATH, "w", encoding="utf-8") as f:
        f.write(header + cmd_lines + "\n")


def run(cmd_lines: str) -> int:
    """通过 .bat 中转执行 cmd 命令"""
    # 1. 构造干净环境（剥 MSYSTEM）
    env = os.environ.copy()
    env.pop("MSYSTEM", None)
    env["ADF_PATH"] = ADF_DIR
    env["IDF_TOOLS_PATH"] = TOOLS_DIR

    # 2. 写 .bat
    build_bat(cmd_lines)

    # 3. 调 cmd /D /C 跑 .bat
    r = subprocess.run(["cmd.exe", "/D", "/C", BAT_PATH], env=env)
    return r.returncode


# === 预定义常用操作 ===
def cmd_build() -> int:
    return run(rf"""cd /D {PROJECT_DIR}
call {IDF_DIR}\export.bat >nul 2>&1
idf.py build > {PROJECT_DIR}\debug\build_coredump.log 2>&1
""")

def cmd_flash() -> int:
    """esptool 烧录（RTS 未接，必须手动进下载模式，故 --before no_reset）"""
    return run(rf"""cd /D {PROJECT_DIR}
call {IDF_DIR}\export.bat >nul 2>&1
echo === Flashing via no_reset (assume chip already in ROM download mode) on COM7 ===
echo === Serial monitor (COM7) MUST be closed; chip must be in download mode ===
python -m esptool --port COM7 --baud 921600 --before no_reset --after no_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\audiobook_player.bin 0x210000 build\ota_data_initial.bin
""")

def cmd_version() -> int:
    return run(rf"""cd /D {PROJECT_DIR}
call {IDF_DIR}\export.bat >nul 2>&1
idf.py --version
""")

def cmd_monitor() -> int:
    return run(rf"""cd /D {PROJECT_DIR}
call {IDF_DIR}\export.bat >nul 2>&1
idf.py -p COM7 monitor
""")

def cmd_coredump() -> int:
    """从板子 flash 读 coredump 并反解符号（板子需手动进下载模式；RTS 未接，手动进下载模式）"""
    return run(rf"""cd /D {PROJECT_DIR}
call {IDF_DIR}\export.bat >nul 2>&1
python {IDF_DIR}\components\espcoredump\espcoredump.py --port COM7 --baud 921600 info_corefile build\audiobook_player.elf > {PROJECT_DIR}\debug\coredump_parse.log 2>&1
""")

def cmd_addr2line() -> int:
    """反解崩溃 PC 0x403743c0 到函数/行（IRAM 地址，用 elf 直接 addr2line）"""
    return run(rf"""cd /D {PROJECT_DIR}
call {IDF_DIR}\export.bat >nul 2>&1
xtensa-esp32s3-elf-addr2line.exe -e build\audiobook_player.elf -a -f 0x403743c0 > {PROJECT_DIR}\debug\addr2line.log 2>&1
xtensa-esp32s3-elf-addr2line.exe -e build\audiobook_player.elf -f -C -p 0x403743c0 >> {PROJECT_DIR}\debug\addr2line.log 2>&1
""")

def cmd_coredump_save() -> int:
    """用 esptool 直接 dump coredump 分区原始数据到文件（绕过 espcoredump 无 --save-corefile 的限制），分区 coredump@0xE20000 size 0x40000"""
    return run(rf"""cd /D {PROJECT_DIR}
call {IDF_DIR}\export.bat >nul 2>&1
python -m esptool --port COM7 --baud 921600 --before no_reset --after no_reset read_flash 0xE20000 0x40000 debug\coredump_raw.bin > {PROJECT_DIR}\debug\coredump_dump.log 2>&1
""")


def main():
    if len(sys.argv) < 2:
        print("用法:", file=sys.stderr)
        print("  python tools/_run_in_clean_cmd.py build      # 编译", file=sys.stderr)
        print("  python tools/_run_in_clean_cmd.py flash      # 烧录 (需用户就绪)", file=sys.stderr)
        print("  python tools/_run_in_clean_cmd.py version    # 验证 IDF 环境", file=sys.stderr)
        print("  python tools/_run_in_clean_cmd.py monitor    # 监视串口", file=sys.stderr)
        print("  python tools/_run_in_clean_cmd.py --raw \"<任意 cmd 命令>\"", file=sys.stderr)
        sys.exit(2)

    arg = sys.argv[1]
    if arg == "build":
        sys.exit(cmd_build())
    elif arg == "flash":
        sys.exit(cmd_flash())
    elif arg == "version":
        sys.exit(cmd_version())
    elif arg == "monitor":
        sys.exit(cmd_monitor())
    elif arg == "coredump":
        sys.exit(cmd_coredump())
    elif arg == "addr2line":
        sys.exit(cmd_addr2line())
    elif arg == "coredump_save":
        sys.exit(cmd_coredump_save())
    elif arg == "--raw":
        if len(sys.argv) < 3:
            print("--raw 需要命令字符串参数", file=sys.stderr)
            sys.exit(2)
        sys.exit(run(sys.argv[2]))
    else:
        print(f"未知命令: {arg}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()