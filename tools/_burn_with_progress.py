#!/usr/bin/env python3
"""_burn_with_progress.py — 实时报告烧录进度，后台跑 esptool。

解决问题：
- esptool 默认进度只在终端打印，自动化时无法看到进度
- 需要"启动烧录后不退出，持续报告"的能力

实现：
- 后台线程跑 esptool subprocess
- 主线程每 0.5 秒读日志文件，解析 Writing 行（百分比）
- 实时打印进度（覆盖式输出）
- 子进程结束/超时退出
"""
import subprocess
import os
import sys
import time
import re
import threading

PORT = "COM7"
BAUD = 460800
PROJECT_DIR = r"D:\zhutao\audio_player"
IDF_DIR = r"D:\esp\v5.5.3\esp-idf"
ADF_DIR = r"D:\esp\esp-adf"
TOOLS_DIR = r"C:\Users\zhuta\.espressif"
BAT_PATH = os.path.join(PROJECT_DIR, "tools", "_clean_cmd_runner_burn.bat")
LOG_PATH = os.path.join(PROJECT_DIR, "build", "flash_progress.log")
DONE_MARK = "[FLASH_DONE]"


def build_bat(cmd_lines: str) -> None:
    with open(BAT_PATH, "w", encoding="utf-8") as f:
        f.write("@echo off\n" + cmd_lines + "\n")


def build_flash_bat() -> str:
    """构造 esptool 烧录命令 (写在 .bat 里)，完成后写 DONE_MARK 到日志"""
    flash_cmds = (
        rf"cd /D {PROJECT_DIR}\r\n"
        rf"call {IDF_DIR}\export.bat >nul 2>&1\r\n"
        f"python -m esptool --port {PORT} --baud {BAUD} --before no_reset "
        f"--after hard_reset write_flash --flash_mode dio --flash_freq 80m "
        f"--flash_size 16MB "
        f"0x0 build\\bootloader\\bootloader.bin "
        f"0x8000 build\\partition_table\\partition-table.bin "
        f"0x10000 build\\audiobook_player.bin "
        f"0x210000 build\\ota_data_initial.bin\r\n"
        f'echo {DONE_MARK} rc=%ERRORLEVEL% >> "{LOG_PATH}"\r\n'
    )
    return flash_cmds


def run_subprocess():
    """后台跑 esptool subprocess"""
    env = os.environ.copy()
    env.pop("MSYSTEM", None)
    env["ADF_PATH"] = ADF_DIR
    env["IDF_TOOLS_PATH"] = TOOLS_DIR

    # 先清理日志
    with open(LOG_PATH, "w", encoding="utf-8") as f:
        f.write(f"=== Flash started at {time.strftime('%H:%M:%S')} ===\n")

    proc = subprocess.Popen(
        ["cmd.exe", "/D", "/C", BAT_PATH],
        env=env,
        stdout=open(LOG_PATH, "a", encoding="utf-8"),
        stderr=subprocess.STDOUT,
    )
    proc.wait()


def monitor_progress():
    """主循环：实时读日志，报告进度"""
    print(f"=== 烧录进度监控 ===")
    print(f"PORT={PORT}, BAUD={BAUD}")
    print(f"日志: {LOG_PATH}")
    sys.stdout.flush()

    last_percent = -1
    last_step = ""
    start_time = time.time()

    while True:
        time.sleep(0.5)

        if not os.path.exists(LOG_PATH):
            continue

        try:
            with open(LOG_PATH, "r", encoding="utf-8", errors="replace") as f:
                content = f.read()
        except Exception:
            continue

        # 检查 DONE_MARK 是否出现
        if DONE_MARK in content:
            # 解析最终返回码
            m = re.search(rf"{DONE_MARK}\s+rc=(\d+)", content)
            rc = int(m.group(1)) if m else -1
            elapsed = int(time.time() - start_time)
            print(f"\n\n[完成] 耗时 {elapsed}s, rc={rc}")
            if rc == 0:
                print("[OK] 烧录成功")
            else:
                print("[FAIL] 烧录失败，请看 build/flash_progress.log 尾部")
            return rc

        # 找最新的 Writing 百分比
        # esptool 输出: "Writing at 0x0004e5b0... (45 %)"
        write_matches = re.findall(
            r"Writing at 0x[0-9a-f]+\.\.\.\s*\((\d+)\s*%\)", content
        )
        if write_matches:
            cur_percent = int(write_matches[-1])
            if cur_percent != last_percent:
                elapsed = int(time.time() - start_time)
                # 找当前正在写哪个段
                if cur_percent <= 5:
                    step = "bootloader"
                elif cur_percent <= 10:
                    step = "partition_table"
                elif cur_percent <= 96:
                    step = "app"
                else:
                    step = "ota_data"
                bar_len = 30
                filled = int(bar_len * cur_percent / 100)
                bar = "#" * filled + "-" * (bar_len - filled)
                print(f"\r[烧录] {cur_percent:3d}% |{bar}| {step:<14} {elapsed}s",
                      end="", flush=True)
                last_percent = cur_percent
                last_step = step

        # 检查致命错误
        if "A fatal error occurred" in content:
            print("\n\n[FAIL] 致命错误：")
            lines = content.splitlines()
            for line in lines[-5:]:
                print(f"  {line}")
            return 1


def main():
    flash_cmd = build_flash_bat()
    build_bat(flash_cmd)

    proc_thread = threading.Thread(target=run_subprocess, daemon=True)
    proc_thread.start()

    try:
        rc = monitor_progress()
    except KeyboardInterrupt:
        print("\n[中断]")
        rc = -1

    proc_thread.join(timeout=2)
    return rc


if __name__ == "__main__":
    sys.exit(main())