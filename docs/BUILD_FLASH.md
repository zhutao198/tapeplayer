# 编译与烧录指南（BUILD & FLASH）

> 供 AI 助手（Claude Code / CodeBuddy 等）与人工参考。
> 最后更新：2026-08-18

## 0. 最关键约束（必读）

**本硬件不能通过串口 DTR/RTS 自动控制 boot 进入下载模式。**
- 必须由用户**手动进下载模式**：按住 `BOOT` → 按 `RESET` → 松开 `BOOT`（保持 RESET 释放后停留在 bootloader，串口打印 `waiting for download`）。
- 烧录**前用户必须关闭串口助手**（否则串口被占用，esptool 连不上，报 `No serial data received`）。
- 因此烧录**只能用 `esptool` 直写**（`--before no_reset`），**禁止** `idf.py flash`（它会尝试复位控 boot，连不上）。
- 工作流：先编译 → 等用户说"就绪"（已手动进下载模式、关串口）→ 再执行 esptool 直写。

串口端口：`COM7`（Windows，换机器需用户确认）。

## 1. 环境

| 项 | 值 |
|---|---|
| IDF | `D:\esp\v5.5.3\esp-idf`（v5.5.3）|
| ADF | `D:\esp\esp-adf`（v2.7，commit `d0493218`）|
| IDF_TOOLS_PATH | `C:\Users\zhuta\.espressif` |
| 芯片 | ESP32-S3-WROOM-1 N16R8（Octal PSRAM 8MB）|
| 目标 | `esp32s3` |

**注意**：`esptool` 模块只存在于 IDF 虚拟 python 环境里，**系统 `python` 没有**。
直接跑 `python -m esptool` 会报 `No module named esptool`。
必须先用 `D:\esp\v5.5.3\esp-idf\export.bat` 激活 IDF 环境。

## 2. 编译

项目用 `build.bat` 封装环境激活（内部 `call export.bat` + 补 `ADF_PATH` / `IDF_TOOLS_PATH`）。

**PowerShell 下** `build.bat` 的 `export.bat`/`call` 写法不兼容，需用 `cmd /c` 包裹：

```powershell
cmd /c "cd /d d:\zhutao\audio_player && call D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1 && idf.py build"
```

或直接在 cmd 里：`build.bat build`。

编译成功（`Project build complete`）后，`build.bat` 会自动：
- 复制 `build/audiobook_player.bin` → `build/TAPEBOOK.BIN`
- 生成 `build/TAPEBOOK.VER`（取自 `main/config.h` 的 `APP_VERSION_STR`）
- 生成 `build/TAPEBOOK.SHA256`（SD 卡 OTA 升级包用）

> 噪音提示：`clang` 检查可能报 `stdio.h not found` 等 include 路径错误，是工具链噪音，**非实错**，`idf.py build` 通过即可。

## 3. 烧录（esptool 直写）

镜像地址取自 `build/flash_args`：

| 偏移 | 文件 |
|---|---|
| `0x0` | `build/bootloader/bootloader.bin` |
| `0x8000` | `build/partition_table/partition-table.bin` |
| `0x10000` | `build/audiobook_player.bin` |
| `0x210000` | `build/ota_data_initial.bin` |

**PowerShell 命令**（等用户"就绪"后执行）：

```powershell
cmd /c "cd /d d:\zhutao\audio_player && call D:\esp\v5.5.3\esp-idf\export.bat >nul 2>&1 && python -m esptool --port COM7 --before no_reset --after no_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/audiobook_player.bin 0x210000 build/ota_data_initial.bin"
```

要点：
- `--before no_reset --after no_reset`：不复位芯片（硬件不能串口控 boot）。
- 烧写结束会打印 `Staying in bootloader` → 此时请用户**松开 BOOT、按一下 RESET** 从 flash 启动。
- 速度约 130–140 kbit/s，app 分区（~1.7MB）约 100 秒，耐心等。
- 每片写完后 `Hash of data verified.` 表示校验通过。

## 4. 其他烧录

| 用途 | 命令 |
|---|---|
| 中文 TTF 字体（font 分区 @0x620000）| `flash_font.bat COM7` |
| SD 卡 OTA 升级包 | `build/TAPEBOOK.BIN` + `.VER` + `.SHA256`，拷到 SD 卡根目录 |

## 5. AI 助手避坑清单

1. 烧录前**必须等用户确认已手动进下载模式 + 关串口**，否则 esptool 连不上。
2. 用 `python -m esptool` 前**必须 `call export.bat` 激活 IDF 环境**，否则 `No module named esptool`。
3. PowerShell 中**不要**用原生 `cd`/`&` 串 IDF 命令，用 `cmd /c "..."` 包裹。
4. **禁止 `idf.py flash`**（会控 boot 失败）。
5. 端口 `COM7`，换机需用户确认。
6. clang 的 `stdio.h not found` 是噪音，别当真。
